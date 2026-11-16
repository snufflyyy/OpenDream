# High frame rate (60 / 120+ fps) for the native Dreams port: research

Date: 2026-11-15. Game: CUSA08010 v2.52 (the user is moving to v2.64, the version the Dreams fork targets).
This is research only. No code was changed. Addresses are **v2.52 eboot image offsets** (text segment at
file offset 0x4000 of `work/out/eboot.elf`, image vaddr 0). Disassembly was done with
`objdump -D -b binary -mi386:x86-64` on that segment, and call sites were found with `tools/find_import_calls.py`.

## TL;DR

1. **Dreams asks for 30 fps itself.** On every frame it calls `sceVideoOutSetFlipRate(handle, rate)` from
   eboot `+0xe34da7`, with `rate` = 1 (every 2nd vblank, so 30 fps at the emulated 60 Hz vblank) or 0 (every vblank, 60 fps).
   The choice is a small **30/60 Hz state machine** in the game (§1.2). It picks 60 Hz on its own only in PS VR display
   modes and, since update 2.28, **on PS5 in the Dreamiverse UI** (official notes: "we upped the UI to 60fps!",
   https://docs.indreams.me/en/updates/release-notes/dreams/v228). It detects PS5 with
   `sceKernelIsProspero` (eboot `+0xe56900`). Our runtime returns 0 for that call (`port/src/runtime_dreams_services.c:437`).
2. **Rendering and gameplay run on separate clocks.** The render/presentation delta is *measured* wall time
   (`sceKernelGetProcessTimeCounter`), smoothed, and clamped to **at least 1/30 s in 30 Hz mode or 1/60 s in 60 Hz mode**
   (eboot `+0xe72d81`…`+0xe72e22`, global float `0x26e4f90`, read in >50 places). The Dreams fork says gameplay runs on an
   independent **30 Hz worker**. Its overlay shows "render FPS" and "game FPS" separately ("20 render FPS / 11 game FPS",
   fork README).
3. **The fork's `SHADPS4_DREAMS_FORCE_60HZ_RENDERING`** patches exactly those two places: the flip-rate pick → 0, and
   the dt floor → 1/60. Both patch sites exist in v2.52 with identical opcodes and different displacements (`+0xe34d73`,
   `+0xe72d81`). vcore does not apply any eboot patches yet (`bbgpu_patch_image` is a stub), so the switch currently does nothing in our port.
4. **For 120 fps** the same mechanism needs three things:
   - flip rate 0;
   - a display/vblank side that can show 120 flips per second;
   - the dt floor lowered to 1/120 or below. If the floor stays at 1/60, the render clock runs 2× fast at 120 fps.

   Gameplay (30 Hz) is not touched by this, so **game logic speed-up is not expected**. Whether the render side
   *interpolates* gameplay states, so that the extra frames show new motion and not repeats, is the main open question.
5. **Hardware headroom.** The baseline shows the hardware mostly idle at 30 fps: GPU busy about 22–26% at a low clock of about 1.6 GHz.
   The dips to 15 fps are **GPU-bound**: GPU load median 89% in samples below 28 fps. Light scenes (menus, homespace)
   probably reach 60, and maybe 120. Heavy scenes need vcore performance work first: the DOC atomic fallbacks on v2.52, Precise readbacks, and the single-threaded command processor.

## 0. Baseline numbers

`work/mangohud/dreams-probe_2026-11-15_19-13-38.csv` (user session, 560 s, 5,599 samples at 100 ms), re-analysed:

| | p1 | p5 | p50 | p95 | p99 |
|---|---|---|---|---|---|
| fps | 4.3 | 15.0 | 30.0 | 30.02 | 30.03 |
| frametime ms | 33.3 | 33.3 | 33.3 | 66.7 | 233 |
| GPU load % | 10 | 13 | 22 | 84 | 92 |
| CPU load % (16 threads) | 8.3 | 9.0 | 12.9 | 15.5 | 22.3 |

- Frametime buckets: 5,305 samples at 30–36 ms, 46 at 50–70 ms, 248 at ≥70 ms. **No sample is faster than 30 fps**, which is a hard cap.
- The 294 samples below 28 fps have a **GPU load median of 89%** and a CPU median of 8.8%, so the dips are GPU-bound.
- At 30 fps the GPU load is 22% median (p90 32%) at a median core clock of 1,598 MHz. The RX 7800 XT boosts to about 2.4 GHz,
  so the real headroom is larger than the percentage suggests.
- A second, short run made for this research (`work/logs/run-20261115-194622.log`,
  `work/mangohud/research/dreams-probe_2026-11-15_19-46-29.csv`, 110 s, boot → logo → menus, no input) also held
  **exactly 30.00 fps from t=5 s on**, including the Sony card and logo video while the GPU sat at 10% busy and 150–290 MHz.
  So the cap is a requested flip rate, not load.

## 1. How Dreams paces frames

### 1.1 PS4 APIs Dreams imports (from `notes/dreams-imports.json`)

| API | Call sites (v2.52 eboot) | Notes |
|---|---|---|
| `sceVideoOutSetFlipRate` (CBiu4mCE1DA) | `+0xe34da7` (one site) | Called only when the rate changes (`cmp ebx,[0x8ca807c]; je`) |
| `sceGnmSubmitAndFlipCommandBuffers(ForWorkload)` | via libGnm / JobManager (no direct `call` found) | Flips are EOP flips: the GPU flips when the frame's command buffer completes |
| `sceGnmInsertWaitFlipDone` | `+0x17799`, `+0x826f6` | The GPU waits on the display buffer's label (shadPS4 `gnmdriver.cpp:1185`): **this is what throttles the game to the flip rate**. The next frame cannot render into a buffer still on screen |
| `sceVideoOutGetFlipStatus`, `sceVideoOutSubmitFlip` | `+0x1453033`, `+0x1453055`, `+0x1453073` | Teardown path (blank flip + wait for pending flips with `usleep(1000)`), not the main loop |
| `sceVideoOutGetVblankStatus` | `+0x145268f`, `+0xe771e7` | `+0xe771e7` is in a VR-only pacing block (`+0xe75b9c` skips it unless display mode is 0x11/0x12): it sleeps (`usleep(200)` loop at `+0xe75bfe`) until last vblank + 2000 µs + 16,667 µs × (rate+1) |
| `sceKernelUsleep` | ~40 sites, mostly 1000 µs polls | No 33 ms sleep-based limiter found in the 2D path |
| `sceKernelGetProcessTimeCounter(+Frequency)`, `sceKernelGetProcessTime`, `sceKernelReadTsc` | render dt at `+0xe72d43`/`+0xe72d6f` | Measured time, not vblank counts |
| `sceKernelIsNeoMode` | ~25 sites (e.g. `+0xe568b6` → cached in `0x452a288`) | PS4 Pro paths (minimum value of the hysteresis score, §1.2) |
| `sceKernelIsProspero` | `+0xe56900`, `+0x1146947` → cached in `0x452a298` | PS5 detection, used for the 60 fps UI |
| `sceVideoOutAddFlipEvent`/`AddVblankEvent` | **not imported** | Dreams does not wait on flip/vblank equeue events. Pacing is the flip rate plus the GPU's WaitFlipDone |

### 1.2 The 30/60 Hz decision (eboot `+0xe34a5e` … `+0xe34dac`, called once per frame)

Reconstructed (confidence: high for the data flow, medium for the meaning of the state globals):

```
dispmode = [0x8ca7d28]            // 0x11/0x12/0x13 = PS VR display modes (budget N/120 or 1/60)
budget   = dispmode==0x13 ? 1/60 : 1/30          // table 0x16fefa8 = {1/30, 1/60}; VR 0x11: N/120
gpu_t    = [0x8d755ec] - [0x8d755f0]             // GPU frame time from the game's timing struct
score    = [0x26cc368]  *0.95 if gpu_t > budget, *1.01 if gpu_t < 0.96*budget   (constants 0x16f7aa0/aa4/aa8)
score    = clamp(score, Neo ? {0.5625|1.0} : 0.5, 1.0)
flag30   = [0x58aa968] = (score < 1.0) || (gpu_t > 0.9*budget)       // +0xe34c79..+0xe34c8c
if ([0x58aa964] || r15b /* [0xa8a50d8] || [obj+0x27b099] != 3 */) and 2D display mode:
    flag30 = 1; score = 0.1                                           // +0xe34cfc: forced 30 Hz, hysteresis reset
if (UI mode r14b /* [0xa8a50d8] && [0x79c92ec] */ && IsProspero()) flag30 = 0      // +0xe34d0d..+0xe34d29
if ([0x8c83464] != 0) { flag30 = 0; score = 1.0 }                    // +0xe34d30
rate = ([0x8ca8074]==1) ? (VR modes ? 0 : flag30) : 0                // +0xe34d51..+0xe34d94
if (rate != last_rate [0x8ca807c]) sceVideoOutSetFlipRate(handle [0x8ca7d38], rate)   // +0xe34da7
```

So on a base PS4 in 2D the game mostly forces `flag30 = 1` → flip rate 1 → 30 fps. That matches what we see.
The paths to 60 Hz that ship in the game are:
- **PS5 + UI mode**, which needs `sceKernelIsProspero() == 1`. This is the official 2.28 "UI at 60 fps on PS5".
- The dynamic path when the force conditions are false. The hysteresis score (0.1 → ×1.01 per good frame, about 230
  frames to climb back to 1.0) suggests the engine can climb to 60 Hz on its own when the GPU is fast.
- `[0x8c83464] != 0`: a state enum written with values 0–5 around `+0x13f8a4a`…`+0x13f9ca3`. Meaning unknown.
- The VR display modes.

### 1.3 The render clock (eboot `+0xe72d40` … `+0xe72e22`)

```
now = sceKernelGetProcessTimeCounter() / sceKernelGetProcessTimeCounterFrequency()
dt_raw = now - prev   (double, 0x73a1188/0x73a1190)
dt = dt_prev + (dt_raw - dt_prev) * 0.2          // 0x16f7c6c = 0.2 (exponential smoothing)
dt = max(dt, table_0x16ff098[flag30 == 0])       // {1/30, 1/60}: the FLOOR
dt = min(dt, 0.1)                                // 0x16f7c70
-> [0x26e4f90] (float), read by >50 render/animation sites (0x132f3e3 …, 0x148924d …)
```

Consequences:
- Running *faster* than the floor makes the render clock run fast. At 60 fps with the 1/30 floor, render-side motion is 2× too
  fast. At 120 fps with the 1/60 floor it is also 2× too fast.
- Running *slower* than the floor slows motion down, as on PS4.
- The fork's comment on its patch ("Both readers select the game's existing fast presentation/render-clock branch; neither
  changes physics timing", `ref/shadps4/src/core/module.cpp:855-882`) agrees with this reading.

### 1.4 How our runtime / vcore implement the pacing APIs

- `sceVideoOutSetFlipRate` stores `port->flip_rate` (`ref/shadps4/src/core/libraries/videoout/video_out.cpp:142-146`). The
  log call there is `LOG_TRACE`, which is compiled out in release builds (`common/logging/log.h:62`), so the requested rate is never logged.
- The present/vblank thread (`ref/shadps4/src/core/libraries/videoout/driver.cpp:330-405`) ticks at
  `vblank_frequency` (60, from `notes/shadps4-dreams-runtime/CUSA04301.json`). It takes a flip request only when
  `vblank_count % (flip_rate+1) == 0`, and **presents on the same thread** (`presenter->Present`, `driver.cpp:255`). If
  presenting blocks, the vblank tick is delayed. bbport moved presenting to a separate thread for this reason
  (`ref/bbport/gpu/platform/videoout/display.h:3-8`).
- Flip completion resets the previous buffer's label (`driver.cpp:283-288`). That is what `sceGnmInsertWaitFlipDone` waits for in the
  next frame's command buffer, and it is how the game ends up throttled.
- Present mode is Immediate (`vk_swapchain.cpp:211-229`), so there is no extra vsync wait on the host.
- `sceKernelIsNeoMode` and `sceKernelIsProspero` are answered by the runtime (`port/src/runtime_dreams_services.c:437`, the `ok` function = 0).
  The vcore copy (`port/vcore/shim/kernel_glue.cpp:50`) is shadowed by the runtime.

## 2. The Dreams fork's frame-rate switches

Out of 199 `SHADPS4_DREAMS_*` names in `ref/shadps4` branch `vcore`, only one is about frame rate:
**`SHADPS4_DREAMS_FORCE_60HZ_RENDERING`** (`ref/shadps4/src/core/module.cpp:853-882`). It is off (=0) in the fork's
launcher profile (`notes/shadps4-dreams-rebase.md` §6).

| | v2.64 offset (fork) | v2.64 bytes | patch | v2.52 equivalent (found by pattern) |
|---|---|---|---|---|
| Site 0: flip-rate pick | `+0xc2075e` | `0f b6 05 <d32> eb 09 31 c0 c6 05` = `movzx eax, byte [flag30]; jmp; xor eax,eax; mov byte…` | `31 c0 90 90 90 90 90` (rate 0) | `+0xe34d73` (`0f b6 05 ee 5b a7 04 eb 09 31 c0 c6 05…`) |
| Site 1: dt floor | `+0xc502d1` | `80 3d <d32> 00 48 8d 0d <d32> c7 05` = `cmp byte [flag30],0; lea rcx,[{1/30,1/60}]` | `39 c0 90 90 90 90 90` (`cmp eax,eax` → ZF → index 1 → 1/60) | `+0xe72d81` (`80 3d e0 7b a3 04 00 48 8d 0d 09 c3 88 00 c7 05`) |

- The fork applies the two patches as a pair, or neither ("a version mismatch must not mix 30/60 Hz paths"), and logs
  "Dreams 60 Hz rendering enabled; native 30 Hz gameplay worker retained".
- What it does *not* touch:
  - the `flag30` global itself, which keeps feeding the hysteresis and other readers (`+0xe70d78`, `+0xe4f286` read `0x58aa96c`);
  - the VR pacing;
  - the GPU budget (1/30), so its dynamic-resolution-like scoring still targets 30.
- Results claimed in the fork docs: only that the 60 Hz rendering mode is "retained" in builds since 2026-10-19
  (`ref/dreams-fork` `docs/SHADER_INSTRUCTIONS_BUILD_20261019.md:11`). **No fps numbers or visual-correctness claims for it.**
  Every validated fork result is "30 FPS in the recorded view". In heavy scenes the fork reports "20 render FPS / 11 game FPS" (Tren, README:23) and
  "21 render / 22.6 game" (`docs/updates/DOC_PREFIX_20261112.md:13`), which shows that gameplay updates and rendering are counted separately.
- The fork's "Game FPS" counter (`ref/shadps4/src/core/devtools/layer.cpp:291-357`, Windows-only) reads the
  "completed gameplay update" counter at v2.64 `+0x4558688`, signature `inc qword [rip+…]` at `+0x8fd90b`. That signature
  **does not match v2.52** (searched with wildcards). The counter must be located again for 2.52, or used directly on 2.64.
- Other fork eboot patches matter for high fps:
  - the **slowdown guard** at v2.64 `+0x948810` forces the game's "slowdown estimate" to 1.0, so Preferences stops reopening on long frames;
  - the scene gate at `+0x9c44f7`.

  vcore does not apply any of these (`port/docs/dreams/VCORE.md` §7.3).

## 3. Is the simulation tied to frame rate?

Evidence:
- **Render clock:** measured, smoothed, floor-clamped (§1.3). Not tied to the frame count as long as frames are not faster than the floor.
- **Gameplay:** the fork describes "the independent 30 Hz gameplay worker" (module.cpp:853-855), and its overlay counts gameplay updates
  separately. A heavy scene showing 11 game FPS against 20 render FPS means gameplay **slows down** when it can't keep 30 (no catch-up).
  This fits a fixed 30 Hz step that runs once per update and is decoupled from flips.
- **PS VR:** Dreams renders at 60 (reprojected to 120 for the headset) and has a "frame-rate threshold" option
  (2.16 notes: https://www.pushsquare.com/news/2020/07/dreams_update_2_16_adds_psvr_support_heres_all_the_patch_notes,
  https://www.thevrgrid.com/dreams). The VR display modes 0x11/0x12 use an N/120 budget in the code above.
  So the engine already supports rendering at a different rate from its 30 Hz gameplay.
- **PS5:** Dreams has no PS5 version; only the UI goes to 60 fps on PS5 (2.28). Gameplay and edit mode stay 30 on PS5.
  This is the strongest hint that Media Molecule did not consider 60 Hz gameplay or edit-mode rendering safe or worthwhile on PS4-era content.
  It may also only have been a GPU-budget call.
- **Patches:** no GoldHEN or PS5 fps patches for Dreams (CUSA04301/CUSA08010) were found
  (https://florinsdistortedvision.github.io/orbisunjailed/fps-patches-mods lists none). The fork's patch is the only known one.

Open questions (must be measured, see §6 E1/E2):
- (a) Does the render side **interpolate** between 30 Hz gameplay states? If it does, 60/120 fps shows smooth motion.
  If it doesn't, frames repeat and the camera/UI is smooth but objects judder at 30.
- (b) Which per-frame code still uses a fixed 1/30 or 1/60 instead of `0x26e4f90`, for example per-frame eases like Bloodborne's?
  Bloodborne had 211 such sites (`ref/lantern/engine/docs/high-fps-audit.md`).
- (c) Does the gameplay worker really stay at 30 when flips go faster, or is it paced by the render loop?

## 4. How bbport and Lantern unlocked Bloodborne

Bloodborne is a *lockstep* 30 fps game: its main loop steps by a fixed 1/30 per frame. Dreams is different, because its
render clock is already measured. So most of their work is Bloodborne-specific, but the display-side techniques are generic.

| Technique | Where | Generic? | Use for Dreams |
|---|---|---|---|
| Override the game's flip rate (`port->flip_rate = Unlocked() ? 0 : rate`; `BB_FLIP_RATE=n` forces it) | `ref/bbport/gpu/platform/videoout/videoout.cpp:135-155` | **Generic** | Yes. It is the display half of 60/120 (the dt floor must change with it) |
| Immediate flips between vblanks when uncapped or limited; frame limiter `BB_FPS_LIMIT` with paced slots | `ref/bbport/gpu/platform/videoout/display.cpp:390-480` | **Generic** | Yes. It decouples the flip rate from the 60 Hz vblank emulation, so vblank events keep their PS4 rate |
| Present on its own thread so vblank timing does not jitter when the GPU is busy | `display.h:3-8` (`BB_PRESENT_THREAD`) | **Generic** | Yes. vcore presents on the vblank thread (`driver.cpp:330-405`) |
| Hook the game's frame pacer and step by the measured dt; redirect ~dozens of 1/30 constants to a live page (decay^frames, approach weights, cloth stiffness) | `ref/bbport/gpu/shim/bbport_frame_rate.cpp:1-140` | Game-specific | The *method* applies to Dreams' render-side constants if E2 finds 1/30 assumptions. Dreams' pacer is already measured |
| Engine-side frame-rate lists; steps follow the held rate with hysteresis (up after 30 frames, down after 120) | `ref/lantern/engine/docs/NATIVE_FRAME_RATE.md` | Game-specific values; the follow-the-rate idea is generic | The Dreams floor (1/30 vs 1/60) should follow the rate held, the way Lantern's steps do |
| Audit of 30 fps constants (`fps_constants.py`); "a frame counter compared with an authored count is invisible to a constant scan" | `high-fps-audit.md` | Method generic | Reuse for Dreams' render-side code |
| Frame generation: FSR 3.1 FI (vendored FireBurn/FSR-Vulkan), DLSS-G; needs depth + motion vectors + HUD-less copy found in the game's frame | `ref/lantern/engine/docs/dlss_frame_generation.md`, `gpu/shadps4/video_core/renderer_vulkan/vk_frame_generation.h` | Integration is game-specific (needs Dreams' depth, motion vectors, UI point) | Later. A zero-code alternative for experiments: **lsfg-vk** (Lossless Scaling FG as a Vulkan layer on Linux; needs the paid Windows app's DLL; https://github.com/PancakeTAS/lsfg-vk) |
| Parallel GPU command processing (two-stage draw pipe, +18–32%); write-fault windows (+22%); pipeline-compile handling; memory model | `parallel_gpu.md`, `pipeline_compiles.md`, `memory_model_fps_sink.md` | **Generic** (shadPS4 renderer) | Needed for heavy Dreams scenes. Note that Lantern's memory model *sank* fps over time before its fix |
| Hard cap 480 fps: Bloodborne's loaders broke above it ("main loop outran its worker threads") | `bbport_frame_rate.cpp:47-51` | Lesson | Keep a cap (e.g. 240) when uncapping Dreams |

## 5. Where the time goes after uncapping

- **GPU:**
  - At 30 fps in menus and homespace the GPU is about 25% busy at about 1.6 GHz → roughly 8 ms of GPU work per frame at that clock.
    At full clock that is maybe 5–6 ms → **~120 fps is plausible in light scenes, 60 is easy**.
  - Heavy scenes already run at 15 fps or lower with the GPU at 89% → >60 ms per frame. Uncapping doesn't help there.
    The cost sits in vcore/shadPS4: 15 `Dreams DOC atomic fallback ... kernel shape/feature gate` routes on v2.52 (the slow, known-broken path),
    BC3 3D aliasing, and copies coerced between layers (seen in both run logs).
- **CPU:**
  - 13% of 16 threads ≈ 2 cores busy at 30 fps. The per-thread split was **not measured**: the `top -H` sample failed.
  - Lantern found shadPS4's GPU command-processor thread at ~100% of one core at 70 fps (`parallel_gpu.md:1-20`).
    At 4× the frame rate, that thread and the game's main/render job threads are the likely CPU limit.
- **Readbacks (Precise, `readbacks_mode=2`):**
  - CPU reads of GPU-written memory wait for the GPU. That serialises CPU and GPU (frame time ≈ CPU + GPU instead of the max of the two).
  - The fork calls Precise mandatory for correct sculpts (`docs/Dreams-runtime-setting-fix-20260904.md`). Even the faster Relaxed mode showed a median
    130 ms aggregate wait per reported interval.
  - The cost scales with the number of frames, so it hurts more at 120 than at 30.
  - `PREFETCH_CPU_READBACKS` is already on.
- **Presentation:** the present runs on the vblank thread. At 120 Hz each tick allows 8.3 ms; a slow `Present` delays vblanks
  and the game's pacing (bbport's reason for a present thread).
- **Monitor:** the user's display refresh rate is unknown (MangoHud didn't log it). 120 fps needs a ≥120 Hz display or VRR,
  otherwise Immediate mode tears.

## 6. Plan

### Stage A: measurement and cheap experiments (no or tiny code changes)

All runs must use the isolated agent-run mode the lead is building (`tools/agent-run.sh`). For each: MangoHud CSV
(`MANGOHUD_CONFIG=autostart_log=1,log_interval=100,output_folder=work/mangohud/<name>`; this worked in run
20261115-194622) plus `DREAMS_CAPTURE=200` for motion checks, compared with the baseline at the same screen.

- **E0. Confirm the decision state (read-only, no code change).**
  - Sample the globals of §1.2/§1.3 from `/proc/<pid>/mem` every 500 ms (the script is in the appendix; `ptrace_scope=0` on this host).
    My first attempt failed only because `pgrep` found no PID at the time of the attach.
  - Expected: `rate_now [0x8ca807c] = 1`, `flag30 = 1`, `render_dt ≈ 0.0333`.
  - Also record which force condition holds (`[0x58aa964]`, `[0xa8a50d8]`), the GPU time `[0x8d755ec]-[0x8d755f0]`, and
    `top -H -b -n 2 -d 2 -p <pid>` for per-thread CPU.
- **E1. Official PS5 UI path: `sceKernelIsProspero` → 1** (one line in `runtime_dreams_services.c`, behind an env switch such as `DREAMS_PROSPERO=1`).
  - Expected: menus/Dreamiverse at 60 fps the way the game intends (flip rate 0, dt floor 1/60), with edit and play modes still at 30.
  - Risk: the second call site (`+0x1146947`) and any other PS5 branches are unknown, so check the log and captures.
  - It is low risk because it is a shipped game path.
- **E2. Fork-equivalent 60 Hz everywhere:** port `FORCE_60HZ_RENDERING` into vcore's `bbgpu_patch_image`.
  - Use **wildcard signatures** (opcodes fixed, displacements wildcarded) so it matches both 2.52 (`+0xe34d73`, `+0xe72d81`) and 2.64 (`+0xc2075e`, `+0xc502d1`).
  - Keep the fork's pair-or-nothing rule.
  - Expected: 60 fps in light scenes, with real-time render motion (dt floor 1/60).
  - Check, with 100–200 ms captures plus the user's eyes: imp/camera speed and UI tween speed against 30 Hz, and whether gameplay objects move smoothly or in 30 Hz steps (open question 3a).
  - Also find the 2.52 "completed gameplay update" counter, or test on 2.64 with `+0x4558688`, to confirm gameplay stays at 30/s.
- **E3. 120 fps proof of concept.** On top of E2:
  - point the floor table's second entry (1/60 at image `0x16ff09c`, only referenced from `+0xe72d88`) at 1/240,
    or better, redirect the `lea` to a table in a page next to the image that the shim updates;
  - `vblank_frequency=120`, or bbport-style immediate flips with `BB_FPS_LIMIT`-like pacing.

  Note: the per-game config is overwritten from `notes/shadps4-dreams-runtime/CUSA04301.json` on every run (`port/run-dreams.sh:56`), so this needs a
  run-dreams.sh override switch rather than editing work/.

  Expected: 100–120 fps on menus/homespace if the GPU-time estimate holds.

  **Don't** change `vblank_frequency` without the E2 patches. With flip rate 1 and a 120 Hz vblank, Dreams would flip at 60 fps
  while its dt floor stays 1/30, so render-side motion would run 2× fast.
- **E4. lsfg-vk** (if the user owns Lossless Scaling): generic 2–4× frame generation at the Vulkan layer on top of 30 or 60 fps.
  No code. It shows what interpolation feels like with Dreams' UI (expect UI smearing: no HUD-less input). It adds latency.

### Stage B: bigger work

1. **Engine-side frame-rate module** (like Lantern's `frame_rate.cpp`, not byte lists):
   - flip rate override;
   - dt floor that follows the held rate (hysteresis, like Lantern: up after N frames, down after M);
   - fps limit (30/60/120/144/uncapped capped at 240);
   - the fork's slowdown guard ported, so a long frame does not trigger the Preferences warning;
   - all version-checked by signature (2.52 and 2.64).
2. **Display path:** a present thread separate from the vblank thread and immediate flips with paced slots (port of bbport
   `display.cpp`/`display.h` logic into vcore's VideoOut driver, as a `// vcore:` edit or a shim). Vblank events stay at 60 Hz so any
   vblank-counting code keeps PS4 timing.
3. **Audit of render-side 1/30 assumptions** (Lantern `fps_constants.py` approach on Dreams' eboot). Constants read per frame
   besides `0x26e4f90`: eases, damping, particle steps. Validate with paired captures at 30 vs 120.
4. **If gameplay is not interpolated:** options in order of cost:
   - (a) accept 30 Hz gameplay with 120 Hz camera/UI (what PS VR does);
   - (b) frame generation integrated in vcore, Lantern style (FSR 3.1 FI works on RADV). This needs Dreams' depth, motion vectors and the
     pre-UI point identified (Dreams has TAA, so motion vectors probably exist);
   - (c) raising the gameplay worker's rate: high risk, because physics, creations and logic gadgets are authored against 30 Hz.
     Not recommended.
5. **vcore performance for heavy scenes** (the real limit for 60+ everywhere):
   - move to v2.64 so the fork's exact DOC routes match (or remap the 58 shader hashes);
   - port Lantern's two-stage draw pipeline, write-fault windows and pipeline-compile handling (`parallel_gpu.md`, `pipeline_compiles.md`);
   - reduce Precise readback waits (per-frame readback counters first).

### Risks

- **Game speed:** render-side speed-up if the dt floor and the flip rate are not changed together (§1.3). Gameplay speed-up is
  *not* expected (30 Hz worker), but that is unproven on our port.
- **Audio:** AudioOut is paced by the audio device and AvPlayer by wall clock, so no frame-rate coupling was found.
  Watch audio-visual sync in videos and creations.
- **GPU hangs:** a native run has hung the GPU once (gfx ring timeout in Present, STATUS.md "GPU hang risk"). 2–4× more
  submissions and presents per second raise the exposure. Use agent-run isolation, short runs and a frame-rate cap.
- **Correctness of fork fixes:** some fork paths key on frame fences and readbacks per frame. More frames means more
  sync points: re-check the sculpt/cube correctness the fork validated at 30.
- **Version drift:** every patch must be signature-based (2.52 now, 2.64 soon; the fork's offsets are 2.64).
- **Dreams' own warnings:** the slowdown estimate (fork patch at v2.64 `+0x948810`) and the VR frame-rate threshold might behave
  differently with unusual frame times.

## Appendix: read-only sampler for E0

Used (unsuccessfully attached) in run 20261115-194622. Copy is in the session scratchpad. Usage: `python3 -I sample.py <pid> <seconds> out.csv`.

```python
import sys, struct, time
pid, dur, out = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
SIG_OFF, SIG = 0xe34d73, bytes.fromhex('0fb605ee5ba704eb0931c0')   # v2.52 flip-rate pick
mem = open(f'/proc/{pid}/mem', 'rb', 0)
def rd(a, n): mem.seek(a); return mem.read(n)
base = None
for line in open(f'/proc/{pid}/maps'):
    lo = int(line.split('-')[0], 16)
    try:
        if rd(lo + SIG_OFF, len(SIG)) == SIG: base = lo; break
    except OSError: pass
V = {'flag30': (0x58aa968,'<B'), 'force_964': (0x58aa964,'<B'), 'ratemode': (0x8ca8074,'<i'),
     'rate_now': (0x8ca807c,'<i'), 'dispmode': (0x8ca7d28,'<i'), 'score': (0x26cc368,'<f'),
     'render_dt': (0x26e4f90,'<f'), 'gpu_t1': (0x8d755ec,'<f'), 'gpu_t0': (0x8d755f0,'<f'),
     'neo': (0x452a288,'<i'), 'prospero': (0x452a298,'<i'), 'ui_a8a50d8': (0xa8a50d8,'<B'),
     'ui_79c92ec': (0x79c92ec,'<B'), 'state_8c83464': (0x8c83464,'<i')}
with open(out, 'w') as f:
    f.write('t,' + ','.join(V) + '\n'); t0 = time.time()
    while time.time() - t0 < dur:
        row = [struct.unpack(fmt, rd(base + o, struct.calcsize(fmt)))[0] for o, fmt in V.values()]
        f.write(f'{time.time()-t0:.2f},' + ','.join(map(str, row)) + '\n'); f.flush(); time.sleep(0.5)
```

## Sources

- Local: files cited inline (`ref/shadps4` branch `vcore`, `ref/bbport`, `ref/lantern/engine/docs`, `ref/dreams-fork` via
  `git show HEAD:<path>`, `port/`, `notes/`, `work/logs/run-20261115-191330.log`, `work/logs/run-20261115-194622.log`).
- Dreams 2.28 release notes (PS5 UI 60 fps): https://docs.indreams.me/en/updates/release-notes/dreams/v228 ;
  coverage: https://www.psu.com/news/dreams-update-2-28-adds-ps5-60-fps-ui-support-mercedes-benz-adventure/
- Dreams on PS5 (backwards compatible, no native version): https://www.pushsquare.com/news/2020/11/dreams_is_backwards_compatible_on_ps5_creations_will_get_performance_boost
- Dreams VR (2.16, frame-rate threshold option): https://www.pushsquare.com/news/2020/07/dreams_update_2_16_adds_psvr_support_heres_all_the_patch_notes ,
  https://www.thevrgrid.com/dreams
- PS4 fps-patch list (no Dreams entry): https://florinsdistortedvision.github.io/orbisunjailed/fps-patches-mods
- lsfg-vk: https://github.com/PancakeTAS/lsfg-vk , https://www.gamingonlinux.com/2026/09/lossless-scalings-frame-generation-for-linux-lsfg-vk-2-0-is-out-now/
