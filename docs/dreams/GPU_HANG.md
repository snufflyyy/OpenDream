# GPU hang in the first-time intro (2026-11-15): investigation notes

Status (2026-11-15, evening): **root cause not confirmed; a hang guard is implemented** (section 4,
ref/shadps4 branch `loopguard`, port branch `loopguard`, not merged yet): bounded loops in translated
shaders, clamped indirect dispatch/draw arguments, a copy-extent fix. Validated offline (SPIR-V
validation, lavapipe unit test) and in two guarded boots on the real GPU. **Still open:** overhead
numbers and the single guarded repro of the intro hang (section 6), postponed because the GPU was
needed for other work.

## 1. Symptom and evidence

- Two hangs (19:07 and 20:58), same kernel signature: `ring gfx_0.0.0 timeout ... Process Main ...
  thread shadPS4:Present`, MES ring reset failed, MODE1 full reset, VRAM lost, desktop died.
- Evidence of hang 2: `work/crash/20261115-205951/` (kernel log, captures), full run log
  `work/logs/run-20261115-205625.log` (user's own save, DREAMS_60HZ=1).
- No GPU VM page fault in the kernel log: a job ran > the gfx timeout without faulting. Candidates:
  a shader that never terminates, or a dispatch/draw so large it can't finish (indirect arguments
  read from garbage GPU memory).

## 2. Where exactly it hangs (new)

The hang is **not in the middle of the imp swarm** but at the **scene transition after it**:

- Capture 120 (t=127.5 s): imp swarm, subtitle "The Dreamiverse teems with their energy...".
- Capture 126 (t=133.6 s): black screen with the Dreams watermark (transition).
- Then the log shows `save data 'SYSTEM' mounted` + three `Coercing copy source layers 1->2, 2->3,
  3->4 ... D32Sfloat 512x512` (layered depth/shadow copy), and the next submit fails
  (`Device lost during submit`, from `BufferCache::DownloadMemory` <- `Scheduler::Finish`).
- The same sequence (SYSTEM mount + D32 layer coercion) happens at boot (~21 s) and survives there.

A run that **passed** the scene (`work/logs/run-20261115-191330.log`, 30 fps) shows what runs right
after the transition: at t=164.4 s, `Skipping lowering for null buffer sharp`
(lower_buffer_format_to_raw.cpp:216), i.e. a pipeline compiled for the first time. The private shader
cache file times (work/vcore-shader-cache/.../CUSA08010) match it to the second: only
**compute shader 0x75d8db76 (permutation `_8`, local size 8x8x1)** was created at 19:16:15. In the hang
run that pipeline came preloaded from the cache (271 pipelines), so its first dispatch is the first
new GPU work at the hang point. Its SPIR-V has **no loops and no atomics**, so it can't spin by
itself; suspects are therefore (a) its dispatch size / its null buffer sharp (reads/writes through a
null or wrong buffer), (b) other already-known shaders dispatched with new (possibly indirect, GPU-
owned) dimensions in the new scene, (c) the layered D32 copy whose layer counts are coerced.

The 22 "Dreams DOC atomic fallback" shaders all log their first dispatch before t=21 s (boot), not
at the hang, so they are not new in that scene (they may still produce bad data consumed there).

## 3. Static scan of all cached shaders for spin loops

Tools (this branch): `tools/spirv_dis.c` (SPIR-V disassembler over Fedora's
libSPIRV-Tools-shared.so; no spirv-dis on the system) and `tools/spirv_spin_scan.py` (flags loops
whose exit condition depends on a memory read made inside the loop).

```
gcc tools/spirv_dis.c -o out/spirv-dis /usr/lib64/libSPIRV-Tools-shared.so
C=../work/vcore-shader-cache/build-*/CUSA08010; mkdir -p ../work/spvasm
for f in $C/*.spv; do out/spirv-dis $f > ../work/spvasm/$(basename $f .spv).spvasm; done
python3 -I tools/spirv_spin_scan.py ../work/spvasm/*.spvasm
```

Result on the 300 cached SPIR-V modules: 79 loops flagged in ~30 shaders. Two kinds:
- 3-block `OpAtomicLoad` + `OpAtomicCompareExchange` loops (0x001492d9, 0x0f4f418d, 0x1b307f28,
  0x6ed945b1, 0xccf96f29): the float min/max CAS emulation; always progresses (ruled out before).
- Loops whose exit depends on a storage-buffer load inside the loop (list/tree walks, counts read
  per iteration): e.g. 0xb521ee25 (28 loops, also AtomicUMax), 0x1e4b6169 (7), 0xc2b93a79 (4,
  compiled at 19:22, i.e. in the "choose your imp" scene), 0xff923586, 0x99a1439a, 0x48ff18f4,
  0x3655c8be (a DOC-fallback shader), 0x9b14d57b, 0xa0a32846, 0x1239a108, 0x3391fe2e, 0x9322a819,
  0x85992425, 0x28ed40dd, 0xc7b340b1 (exit on an OpAtomicIAdd result). These terminate only if the
  data they walk is consistent; a cycle or a never-set terminator (data produced by a DOC-fallback
  shader in the wrong order, a stale readback, a null sharp) would loop forever. The scanner's
  induction-variable detection did not recognise any counter, so all of these are "data-bounded".
  No shader was found with the classic "wait for another workgroup's flag" (decoupled look-back)
  pattern with a loop-invariant address.

## 4. The hang guard (implemented 2026-11-15, ref/shadps4 branch `loopguard`)

Three guards against GPU work that never ends, all `// vcore:`-marked in ref/shadps4 branch
`loopguard` (on top of `vcore`). All are ON by default.

| Env | Default | Effect |
|---|---|---|
| `VCORE_LOOP_GUARD=<n>` | `1048576` (1<<20) | back-edge budget per invocation and outermost loop entry; `0`/`off` = no guard (byte-identical SPIR-V to before, old cache dir) |
| `VCORE_INDIRECT_CLAMP=0` | on | disables the indirect-argument clamp |
| `VCORE_INDIRECT_MAX_GROUPS=<n>` | `4194304` (1<<22) | workgroup cap of one indirect dispatch |

### 4.1 Loop guard (shader translator)

Files: `src/shader_recompiler/loop_guard.h` (env, constants, report layout),
`src/shader_recompiler/backend/spirv/emit_spirv_loop_guard.{h,cpp}` (emission), hooks in
`emit_spirv.cpp` (`Traverse`, `EmitSPIRV`), 4 fields in `spirv_emit_context.h`, the ordered-count
turnstile loop in `emit_spirv_atomic.cpp`.

- Every shader with a reachable loop (`Repeat` node) gets one `Function` u32 counter, declared at the
  top of the entry block. At each back edge (`Repeat`: `OpBranchConditional(cond, header, merge)`)
  the emitter adds `++counter` and replaces `cond` with `cond && counter < N`. Nested loops share the
  counter, so the whole loop nest is bounded by N back edges. The counter is reset (after the phis)
  in the merge block of each **outermost** loop, so separate top-level loops each get N and the
  counter becomes wave-uniform again after a divergent loop.
- N = 1<<20: a wave spinning on a memory load exits after roughly 0.1-1 s, below amdgpu's 10 s gfx
  timeout. Legit Dreams loops are far shorter (PS4 frame budget), so a trip means bad data.
- The fork's optional ordered-count turnstile (`SHADPS4_DREAMS_ORDERED_COUNT_TURNSTILE=1`, off) is
  guarded the same way. The float min/max CAS loop is not (a CAS loop always has a winner).
- **Report.** When a back edge wanted to repeat but the budget is gone, the invocation does
  `atomicAdd(slot.count, 1)` and writes the shader hash into its slot of a 64 KiB report buffer
  (4096 slots of 4 dwords, slot = hash of `pgm_hash`). The buffer address reaches the shader as a
  **u64 specialization constant** (SpecId 0x4c47) through a `PhysicalStorageBuffer` pointer, so the
  SPIR-V stays address independent and cacheable, and no descriptor layout changes (the fork has
  ~60 places that match shaders by their exact buffer list). Pipelines that don't pass the constant
  (fork replay pipelines) keep the sentinel default and skip the report, but are still guarded.
  The constant is emitted as `OpConstant` with a unique sentinel and patched to `OpSpecConstant`
  after assembly, like the fork's ordered-phase constant.
- Host side `src/video_core/renderer_vulkan/vk_loop_guard.{h,cpp}`: the report buffer
  (device-local host-visible, created in the `PipelineCache` constructor), the specialization info
  (compute: combined with the DOC phase constant in `vk_compute_pipeline.cpp`; graphics: every
  stage without its own specialization, `vk_graphics_pipeline.cpp`; Vulkan ignores unused ids),
  and a thread that polls the buffer every 0.5 s and logs
  `gpu error (Render.Vulkan): vcore loop guard: TRIPPED in shader 0x... (N loop exits forced so far ...)`
  once per shader (then at most every 10 s while it keeps tripping). The startup line is
  `vcore loop guard ON: 1048576 iterations per outermost loop entry; indirect clamp ON; ...`.
- **Cache key.** The private cache (`SHADPS4_DREAMS_PRIVATE_SHADER_CACHE`) and the regular pipeline
  cache use the subdirectory `loop-guard-<N>` when the guard is on
  (`vk_pipeline_serialization.cpp`, `WarmUp`), so unguarded SPIR-V is never reused and the old cache
  stays valid for `VCORE_LOOP_GUARD=0`. The first guarded run recompiles everything (more stutter
  once).
- **Validation.** `tools/spirv_val.c` (spvValidate over libSPIRV-Tools-shared, Vulkan 1.3 env):
  of the 263 SPIR-V modules a guarded boot produced, 79 carry the guard; 256 validate, and the 7
  failures are the same pre-existing ones as in the unguarded cache (64-bit `OpBitFieldInsert`,
  `Offset` on a non-gather sample: need maintenance8/9), none in guard code.

```
gcc tools/spirv_val.c -o out/spirv-val /usr/lib64/libSPIRV-Tools-shared.so
out/spirv-val ../work/vcore-shader-cache/build-*-cache-v4/loop-guard-1048576/CUSA08010/*.spv | grep -v ': OK'
```

### 4.2 Indirect-argument clamp

Files: `src/video_core/renderer_vulkan/vk_indirect_clamp.{h,cpp}`,
`src/video_core/host_shaders/vcore_indirect_clamp.comp`, call sites in `vk_rasterizer.cpp`.

- Indirect arguments are GPU-written guest memory; the host can't read them without a stall. So
  before the indirect call a tiny compute pass (64 lanes per workgroup, BDA + push constants, no
  descriptors) copies the command(s) into a 4 MiB device-local ring with the sizes clamped, and the
  call reads the copy. Barriers: all prior writes -> clamp shader; clamp -> indirect read.
- Dispatch (`DispatchIndirect`, all 13 `dispatchIndirect` sites incl. the fork's DOC
  collect/replay paths, plus the diagnostic descriptors that read the args): each dimension to the
  device limit (RADV: x 2^32-1, y/z 65535) and the product to `VCORE_INDIRECT_MAX_GROUPS` (1<<22).
  The clamp runs after the barrier flush and before `BindPipelineResources`, so the guest's compute
  bindings are untouched. CPU-read dims of the ordered traversal path (`have_ordered_dims`, a
  diagnostic mode) get the same clamp on the host.
- Draw (`DrawIndirect`, indexed or not, with or without count buffer, and the VS370 shadow replay):
  count per command <= 1<<26, instances <= 1<<24, count x instances <= 1<<28. The pass must run
  outside dynamic rendering: when a scope is open it is ended (a split), **except** when the draw's
  render state clears attachments on begin (restarting would clear them again); then that draw is
  not clamped. After the pass the draw's resources are re-bound (RADV keeps push constants in one
  array for all bind points). The fork's native volume draw (own command buffer) is not touched.
- A clamp logs once: `vcore indirect clamp: dispatch arguments AxBxC clamped ...` /
  `... draw arguments count=... instances=... clamped ...` (report area after the loop-guard slots).

### 4.3 Tests so far

- **Unit test on lavapipe** (`tools/loop_guard_test.cpp`, Mesa's CPU Vulkan driver, never the GPU):
  runs the real guarded cache module of compute shader `0x5c35f8ee` (loop bound = flatbuffer dword 6)
  and the clamp shader. Bound 64: 0.03 s, no report. Bound 2^31-1 (minutes unguarded): ends in
  0.97 s, report slot 2858 counts 64 trips (one per invocation) and holds the hash. Without the
  specialization constant: ends, no report. Clamp: a sane dispatch passes unchanged;
  `0xffffffff x 0x3f800000 x 7` becomes `4194304x1x1` and is reported with its original dims; x=0
  stays empty; a sane indexed draw is copied unchanged and a garbage one is clamped with the other
  words kept. All 11 checks pass.
  ```
  g++ -std=c++20 -O1 tools/loop_guard_test.cpp -o out/loop-guard-test -lvulkan -I vcore/build/host_shaders/include
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json out/loop-guard-test \
      ../work/vcore-shader-cache/build-*-cache-v4/loop-guard-1048576/CUSA08010/0x000000005c35f8ee_0.spv
  ```
- **Guarded boot, real GPU** (`run-20261115-214308`, agent run, 90 s, 30 fps, skip-intro save):
  main menu as before (captures every 0.5 s to the end), startup lines present, no trip, no clamp,
  no device loss; the new cache dir `loop-guard-1048576` filled with 263 modules (79 guarded, all
  validating as described in 4.1); the old cache dir untouched.
- `run-20261115-215208` (guarded, `DREAMS_FPS=240`, meant as overhead run A) **ended at 40 s** with
  `ERROR: Wayland display connection closed by server (fatal)` (the headless weston went away; its
  log is deleted by agent-run.sh). No GPU error, no trip. Unknown whether related to the guard: the
  unguarded comparison run (B) was not made. Check this first when the GPU is free (run B, then A
  again).

### 4.4 The layered D32 copy before the hang ("Coercing copy source layers N and destination layers N+1")

`Runtime::CopyImage` (vk_runtime.cpp) at the hang is an image *growing* by one layer (same guest
address `0x10e8428000`, 512x512 D32, 1 mip, src 1..3 layers -> dst 2..4): `SanitizeCopyLayers` copies
`min(src, dst)` layers, so this copy is in bounds and it also happens at boot without harm. Not a
hang cause. One real hole was found next to it: for two images of the same block-ness the region
height was the *source* height unchecked (width was already the minimum), so a shorter destination
got an out-of-bounds copy region. Now width and height are clamped to the destination's mip size.

## 5. Software GPU (lavapipe) attempt: dropped by the user

Lavapipe failure analysed before the user asked to stop the software-GPU path: shadPS4's buffer
cache creates 4 GiB sparse arenas (`ARENA_PAGE_BITS = 32`), and lavapipe's `maxBufferSize` is
0xffffffff (4 GiB - 1): `vkCreateBuffer` of exactly 4 GiB returns ErrorOutOfDeviceMemory, while
2 GiB/1 GiB sparse buffers work (tested with a small Vulkan program). A run-time arena page size
(1 GiB when maxBufferSize < 4 GiB) fixes the creation; the change was written and then **discarded**
on request. Lavapipe also reports `sparseAddressSpaceSize` 2 GiB and `maxStorageBufferRange` 128 MiB,
so more failures may follow.

## 6. Remaining GPU work (postponed: the GPU went to the hole-fixing work)

1. Overhead, 4 agent runs of `port-loopguard` with the sampler (`tools/fps_sample.py OUT.csv 40 85
   port-loopguard/out/dreams-probe` started next to `tools/agent-run.sh --seconds 90`):
   A guard+clamp `DREAMS_FPS=240`; B `VCORE_LOOP_GUARD=0 VCORE_INDIRECT_CLAMP=0 DREAMS_FPS=240`
   (render fps there is CPU-bound, so it shows the clamp pass / re-bind cost); C/D the same at the
   default 30 fps (compare gpu_busy_percent). Summarise with `tools/fps_sample.py --summary`.
   If the clamp's draw splits cost too much, `VCORE_INDIRECT_CLAMP=0` keeps the loop guard alone.
2. The 240 fps early exit above: does B reproduce it?
3. One guarded repro of the intro hang: `work/hang-user` is prepared (copy of `work/user` with the
   pre-skip save `savedata.json.before-tutorial-skip-20261115-210501` restored, tutorial flags
   false). `DREAMS_USER_DIR=$PWD/work/hang-user DREAMS_SKIP_INTRO=0 DREAMS_PORT_DIR=$PWD/port-loopguard
   tools/agent-run.sh --seconds 240`, with ✕ via `BB_PAD_FILE` at the indreams.me card, the
   preferences screen and the control-scheme popup (watch the captures; don't press during the
   cutscene). Expected outcome with the guard: the scene passes, and the log names the culprit
   (`vcore loop guard: TRIPPED in shader 0x...` or `vcore indirect clamp: ...`). If the GPU still
   hangs: `tools/collect_crash.sh`, stop, report.

## 7. Repro recipe (only with the guard in place)

The scene is reached with a save that hasn't done the first-time intro: `cp -r work/user
work/hang-user` (never run against work/user), then
`DREAMS_USER_DIR=$PWD/work/hang-user DREAMS_SKIP_INTRO=0 DREAMS_PORT_DIR=$PWD/port-hang
tools/agent-run.sh --seconds 300` (+ ✕ presses via BB_PAD_FILE if the preferences screen appears).
The hang point is ~133-165 s in, at the black transition after "The Dreamiverse teems with their
energy...". One attempt at a time; after a hang run tools/collect_crash.sh and stop.
