# GPU hang in the first-time intro (2026-11-15): investigation notes

Status: **root cause not yet confirmed; no fix merged.** Work stopped early (usage limit). This file
records what is known so the next agent can continue without repeating it.

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

## 4. Proposed fix (not implemented yet)

A bounded-iteration loop guard in the SPIR-V backend
(`ref/shadps4/src/shader_recompiler/backend/spirv/emit_spirv.cpp`, `Traverse`):
- At the `Repeat` node (back edge: `OpBranchConditional(cond, loop_header, merge)`), replace `cond`
  with `cond && (++counter < N)`. Counter = one `Function`-storage u32 variable per invocation
  (declared in the entry block), shared by all loops of the shader (bounds nested loops too).
- `N` from env, e.g. `VCORE_LOOP_GUARD=<iterations>` (0 = off); suggested 1<<20 (a stuck wave then
  exits after well under a second, far below amdgpu's 10 s gfx timeout).
- **The guard changes SPIR-V: it must be part of the pipeline/private-shader-cache key** (check how
  `vk_pipeline_serialization.cpp` derives the `build-<hash>-cache-v4` dir; otherwise old unguarded
  SPIR-V is served from `work/vcore-shader-cache`). Simplest: different cache dir when the guard
  is on.
- Reporting: when the guard fires, atomically increment a counter in a host-visible buffer (a new
  `BufferType` like `FaultBuffer`, see resource.h:85 / spirv_emit_context.cpp:904 and
  fault_manager), read it each frame and log `loop guard fired in shader <pgm_hash>`; this also
  names the hanging shader. Without the counter, at least log which shaders were guarded.
- Measure overhead with the guard on vs off (one extra add+compare per back edge).

Also cheap and worth adding: a clamp + log for indirect dispatch/draw dimensions read from GPU memory
(e.g. > 65535 groups per dimension), since a garbage indirect dispatch hangs the ring the same way.

## 5. Software GPU (lavapipe) attempt: dropped by the user

Lavapipe failure analysed before the user asked to stop the software-GPU path: shadPS4's buffer
cache creates 4 GiB sparse arenas (`ARENA_PAGE_BITS = 32`), and lavapipe's `maxBufferSize` is
0xffffffff (4 GiB - 1): `vkCreateBuffer` of exactly 4 GiB returns ErrorOutOfDeviceMemory, while
2 GiB/1 GiB sparse buffers work (tested with a small Vulkan program). A run-time arena page size
(1 GiB when maxBufferSize < 4 GiB) fixes the creation; the change was written and then **discarded**
on request. Lavapipe also reports `sparseAddressSpaceSize` 2 GiB and `maxStorageBufferRange` 128 MiB,
so more failures may follow.

## 6. Repro recipe (only with the guard in place)

The scene is reached with a save that hasn't done the first-time intro: `cp -r work/user
work/hang-user` (never run against work/user), then
`DREAMS_USER_DIR=$PWD/work/hang-user DREAMS_SKIP_INTRO=0 DREAMS_PORT_DIR=$PWD/port-hang
tools/agent-run.sh --seconds 300` (+ ✕ presses via BB_PAD_FILE if the preferences screen appears).
The hang point is ~133-165 s in, at the black transition after "The Dreamiverse teems with their
energy...". One attempt at a time; after a hang run tools/collect_crash.sh and stop.
