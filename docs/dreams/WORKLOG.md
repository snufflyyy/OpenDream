# Work log (newest last; append, don't rewrite)

## 2026-11-15

- Researched options. PS4Recomp (wie-project) is macOS Apple-Silicon only, so it can't target this
  machine. Picked the bbport model instead (decision D1).
- Found the Dreams shadPS4 fork (JesusDaBest/Dreams-ShadPS4). Per its notes it reaches offline
  menus, DreamShaping and edit mode. It's a repo of per-build `.patch` files + binaries targeting
  CUSA04301 2.64. Upstream shadPS4 issues #2812/#3141 show stock shadPS4 fails on Dreams' shaders
  and compute pipelines.
- Extracted the user's fpkg → `game/CUSA08010` with our own extractor (`tools/pkg_extract.py`),
  fully verified (see RUNNING.md). Deleted the .pkg at the user's request after verification.
- Import analysis (`notes/dreams-imports.md`): 812 eboot imports. bbport covers 510; 269 of the
  302 missing have shadPS4 HLE implementations to borrow.
- bbport reuse audit (`notes/bbport-reuse-plan.md`): keep CPU side, replace GPU side (D2).
- Milestone 1: built bbport on Fedora (four environment fixes, see BUILD.md). The Vulkan smoke test
  passes on RX 7800 XT RADV.
- Milestone 2: prepare.py + link_modules.py (with libSceJobManager.prx) run unmodified on Dreams
  with `BB_SKIP_GAME_CHECK=1`.
- Created `port/` (git clone of ref/bbport, branch `dreams`) with `build-dreams.sh` and
  `run-dreams.sh`.
- First `--cpu-only` run: image maps, relocations apply, libc.prx init starts, then it stops at
  `_sceKernelSetThreadDtors` (rNhWz+lvOMU) called from libc.prx+0x790. 416 imports were
  unresolved at load.
- Found two systemic reasons implemented functions didn't bind:
  1. NID→name tables only listed Bloodborne's imports → added generated
     `src/import_names_dreams.inc` (`tools/gen_import_names.py`, 921 names);
  2. `runtime_thread_resolve` matched whole `NID#lib#mod` strings with Bloodborne's letter codes
     → now matches on the NID only.
- Implemented `_sceKernelSetThreadDtors`; the registered hook runs at guest thread exit, before
  TSD destructors (shadPS4 order).
- Learned that shadPS4 binds every unimplemented import to a log-and-return-0 stub
  (`core/aerolib/stubs.cpp`), and Dreams is in-game that way on the fork. Gave our loader the same
  fallback (`STUB:` lines, log once); `DREAMS_STRICT_IMPORTS=1` restores stop-at-first.
- Dreams `main()` runs natively. Next stop was Ajm AAC → added a silent MP3/AAC decoder for now.
- `--cpu-only` then idles in the main loop: GPU fences never signal.
- Ran with bbport's GPU library (`BB_GAME_PROFILE=none`). The Vulkan presenter comes up, the
  game submits real GPU work, and the first compute shader fails on `DS_ORDERED_COUNT`, the opcode
  the Dreams fork implements. This confirms the next step is milestone 6/7 (vcore from
  `ref/shadps4` branch `dreams`).
- Tools added: `tools/unresolved_report.py` (group `BB_LIST_UNRESOLVED` output by library with
  shadPS4 refs), `tools/stub_report.py` (→ `docs/dreams/STUBS.md`).
- Patch-stack agent finished: `ref/shadps4` branch `dreams` = upstream `f3687cc4` (2026-11-15) + the
  Dreams fork's 46 incremental patches, ported per subsystem (10 commits; full report
  `notes/shadps4-dreams-rebase.md`). `5d41108e` makes the fork's ~160 `CUSA04301` gates also accept
  CUSA08010 / `SHADPS4_DREAMS_TITLE_IDS`. The `DS_ORDERED_COUNT` translation itself is generic
  (`frontend/translate/data_share.cpp`), but ~60 per-shader paths are matched by shader hash
  (v2.64 hashes) and the eboot patches check exact bytes, so on 2.52 those fall back or skip.
- Reference run of the rebased **emulator** (`tools/run-shadps4-reference.sh`, fork launcher's
  config + env, state in `work/shadps4-ref/user/`): Dreams runs 150 s with no crash, signs in
  offline, and the GPU thread is busy in Dreams DOC / texture-alias / inline-table paths. Logged
  "DOC atomic fallback ... feature gate" for some hashes, and both eboot patches "did not match
  this executable" (expected on 2.52). What is on screen isn't verified yet (needs the user's eyes).
  Gotcha: seeding a save under the old `user/savedata/` layout makes current shadPS4 open a
  blocking "Save Migration" SDL dialog. The current layout is `user/home/1000/savedata/<serial>/`.
- Lantern Engine review (decision D8 in ARCHITECTURE.md): it vendors near-upstream shadPS4
  video_core + shader_recompiler with a shim, which is the template for vcore. Its renderer lacks
  the Dreams fork's work and is 337 commits behind, so it's not the base. Dispatched an agent to
  build vcore (port/vcore/, see VCORE.md when it lands).
- The user ran `tools/run-shadps4-reference.sh` and confirmed Dreams boots to the
  controller-introduction menus in the rebased emulator.
- Replaced semantically wrong return-0 stubs with real behaviour in new `src/runtime_dreams.c`
  (error dialog, Move, Hmd info, Np account, random, time zones, AudioIn, camera, savedata mount
  info) plus flexible-memory size and thread name/id. All verified to bind in a `--cpu-only` run
  (`work/cpu-only-run6.log`). Full list: IMPORTS_LOG.md.
- CPU-side audit of the return-0 fallback (agent "cpu-runtime"). Baseline `--cpu-only` run
  (`work/cpu-audit-base.log`): 54 `STUB:` lines, 258 non-GPU imports unresolved at load. Decided
  each non-GPU import of STUBS.md (cpu-only and gpu-run1 lists) and implemented them, shadPS4
  branch `dreams` as reference (rows in IMPORTS_LOG.md):
  - new `src/runtime_dreams_services.c`: OpenPsId (fork bytes), libc heap trace info (libc.prx
    read uninitialised pointers before), user colour, privacy setting, TemporaryDataMount2
    ("/temp0" + FORMAT), ContentSearch/ContentExport, NP word filter (SanitizeComment used to leave
    its output untouched), reachability callback, coredump handler, Hmd init/open/reprojection
    queries (Dreams sized buffers from return value 0), VrTracker memory query/init, Ime keyboard,
    At9Enc (the fork's artswap encoder), sceKernelSleep, plus explicit `ok` no-ops for profiler,
    streaming/sharing, sanitizer and Neo/Prospero queries;
  - new `src/runtime_eventflag.c`: kernel event flags (libSceJobManager.prx imports them);
  - new `src/runtime_audio3d.c`: shadPS4's software Audio3d routed to our AudioOut (port type 126);
    AudioOut mastering + pad-speaker mix level in `runtime_audio.c`;
  - PlayGo language mask/progress/to-do list (`runtime_services.c`), `sceKernelGetdents`
    (`runtime_file.c`), POSIX `pthread_rwlock_*` for libc.prx (`runtime_rwlock.c`), Hmd
    PARAMETER_NULL code fix (`runtime_dreams.c`).
- Real AAC decoding: `./build-deps.sh` builds `out/libdreams-codecs.so` (fdk-aac decoder sources
  and the artswap ATRAC9 encoder from `ref/shadps4/externals`, plus `src/runtime_codecs/codecs_glue.c`)
  in ~6 s on 16 cores; `src/runtime_codecs.c` dlopens it (no change to `build-dreams.sh` needed;
  `DREAMS_CODECS_LIB` overrides the path). `runtime_ajm.c` decodes AAC jobs like shadPS4's
  ajm_aac.cpp. Learned: fdk's RAW transport takes exactly one access unit per fill (concatenated
  raw AUs fail with AAC_DEC_UNKNOWN), so RAW jobs must carry one AU each, as in shadPS4.
- Tests: `tests/run-dreams-tests.sh` (event flags, Audio3d with a fake AudioOut, services, codecs
  incl. an Ajm AAC job over an ffmpeg-made ADTS sine). bbport's runtime/sema tests still pass.
- After (`work/cpu-audit-2.log`): 17 `STUB:` lines, all GnmDriver/VideoOut; 154 non-GPU imports
  unresolved at load. The codec library loads, Dreams creates its two ATRAC9 encoders and AAC
  instances with the real decoder. The cpu-only run still ends at the GPU-fence watchdog.

## 2026-11-15 (vcore agent): milestone 6/7, the shadPS4-based GPU library

- Built `port/vcore/` (CMake → `libvcore.so`): the bbport 0.7 `bbgpu.h` API over ref/shadps4
  branch `vcore` (= `dreams` + commit `5f10ad43` with 2 `// vcore:` edits), compiled **in place**
  (video_core, shader_recompiler, common subset, emulator_settings, gnmdriver, videoout, equeue).
  Glue in `port/vcore/shim/`: header shadowing for core/memory.h, address_space.h, libs.h,
  kernel.h, threads.h, file_sys/fs.h, net/guest_net.h, sdl_window.h, imgui headers; .cpp for the C
  API, MemoryManager over `runtime_memory_*`, signal registry, stdout logging, window, kernel clocks,
  stubs. Third-party from shadPS4's pinned externals. Design and mapping table: `VCORE.md`.
- Surprise: upstream video_core needed less adapter than estimated (~1.9k lines incl. CMake and
  comments, low end of the 1.5-2.5k estimate). After two compile rounds the only missing symbols were `io_context`/`KernelSignalRequest`
  (equeue timers).
- The emulator draws the game frame only through Dear ImGui; vcore has no ImGui, so
  `vk_presenter.cpp` got a `VCORE_NATIVE` blit into the swapchain. `path_util.cpp` got
  `SHADPS4_USER_DIR` so the library never touches `./user` or `~/.local/share/shadPS4`.
- Settings/serial: upstream `emulator_settings.cpp` is compiled as is and loads
  `work/vcore-user/custom_configs/CUSA08010.json` (the fork's CUSA04301.json); log shows
  `readbacks_mode=2`, `GameSerial()=CUSA08010 (Dreams gates ON)`. `run-dreams.sh` exports the fork
  launcher's `SHADPS4_DREAMS_*` set (same as `tools/run-shadps4-reference.sh`).
- `build-dreams.sh`: `DREAMS_GPU=vcore` (default) / `bbport`; also runs `./build-deps.sh`.
- Run 1: past `DS_ORDERED_COUNT` (DOC routes active, "DOC atomic fallback" for v2.52 hashes),
  but the screen stayed on "Sony Interactive Entertainment presents" (found by adding
  `VCORE_SCREENSHOT_EVERY`, which drives shadPS4's own screenshot readback). Cause: all 18
  `libSceAvPlayer` imports were `STUB:`s (bbport's libbbgpu used to provide AvPlayer), so the intro
  video never ended.
- Compiled bbport's native AvPlayer (`port/gpu/platform/avplayer`, FFmpeg, runtime file paths,
  runtime-attached host threads) into vcore in place (CMake option `VCORE_BBPORT_AVPLAYER`, interim:
  the lead routes a runtime-side AvPlayer to another agent; the runtime resolves before
  `bbgpu_resolve`, so it will override this automatically).
- Run 3/4: **Dreams boots natively to the first-run preferences screen ("✕ CONTINUE")** via the
  indreams.me card, ~40-60 s after launch, rendered correctly, no GPU assertion in 150 s. GPU log
  matches the reference emulator's (SRT-walker errors, DOC fallbacks, one BC3-3D format error).
- CPU runtime: AvPlayer, pad motion, IME (agent "cpu-runtime-2", 2026-11-15).
  - **libSceAvPlayer** moved to the runtime: `src/runtime_avplayer.c` (C, pthreads; shadPS4
    avplayer_state/source semantics, Dreams fork logo rule) + FFmpeg glue
    `src/runtime_avplayer/av_glue.c` built by `build-deps.sh` into `out/libdreams-av.so`
    (dlopen, `DREAMS_AV_LIB` overrides; no build-dreams.sh change). Frames: decode thread ->
    bounded host queues -> copied on the game's Get*Data call into a ring of the game's own
    buffers (`num_output_video_framebuffers`), with runtime_memory_note_write/prepare/note_cpu_write
    around the copy. Learned: x264 clips seek to the previous keyframe, so JumpToTime drops decoded
    frames before the target; a test that pulls audio greedily runs the audio clock ahead and
    video frames then go stale (the game's audio thread paces it, as on PS4). GPU runs: logo plays
    (300 frames, 1920x1088 pitch 1920), indreams.me card, preferences screen; same with vcore built
    `-DVCORE_BBPORT_AVPLAYER=OFF` in `vcore/build-noavp` (`work/avp-gpu-noavp2.log`, real pad
    hidden with `SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000` because a button press
    on the user's DS4 made Dreams skip the logo in `work/avp-gpu-noavp.log`).
  - **Pad motion** (user report: motion did nothing): bbport's runtime filled only a synthetic
    "lying still" pose (or its spray-can emulation with BB_PAD_MOTION). New
    `src/runtime_pad/sensors.c`: SDL3 gamepad gyro/accel enabled on the chosen pad, "PadMotion"
    thread integrates at ~500 Hz, tilt/bias/deadband settings from the game,
    scePadSetLightBar/ResetOrientation. Only ONE SDL gamepad exists on the user's machine (HIDAPI
    `/dev/hidraw11`, gyro+accel 250 Hz: `tools/sdl_pads.c`); the second "gamepad connected" line
    in `work/user-play-1.log` is the same pad being reopened after SDL dropped it once. Sensors now
    track the pad by SDL id (a reopened pad may get the same pointer back). Dreams calls
    scePadSetTiltCorrectionState(1) and toggles bias correction often. Verified with the real DS4
    in a GPU run (`work/motion-gpu1.log`).
  - **IME**: `src/runtime_ime.c` (keyboard functions moved out of runtime_dreams_services.c):
    sceImeOpen via the window's title-bar text entry, USB keyboard events via an SDL event watch.
  - Tests added to `tests/run-dreams-tests.sh`: avplayer-test (ffmpeg-made 2 s 200x120 H.264+AAC
    clip: sync, NV12 content/padding, audio, jump, file callbacks, auto start, logo wall clock),
    pad-motion-test (integration, tilt convergence, SDL virtual pad with sensors), ime-test.
    `--cpu-only` run: `work/cpu-avp-ime-motion.log` (17 STUB lines, all GnmDriver/VideoOut).
  - Non-GPU STUBs seen in GPU runs now: sceNpHasSignedUp, sceKernelSync, sceSigninDialog{Initialize,
    Open,UpdateStatus,Terminate} (next audit).
- Test frame capture for agents (CAPTURE.md): `DREAMS_CAPTURE=1 ./run-dreams.sh` saves 480 px PNGs
  of changed frames every 500 ms via a weak `VideoCore::CaptureSink` hook (ref/shadps4 `vcore`
  75c80f94) and `vcore/shim/capture.cpp` (background thread, never blocks the presenter), plus
  `index.csv` and `CAPTURE #n` markers in the run log. `tools/capture_sheet.py` gives a text
  timeline and 4×4 contact sheets. First use: a 70 s boot gave 122 captures, 66 saved (3.2 MB),
  0 dropped, and one sheet showing Sony card → Media Molecule logo video (runtime AvPlayer) →
  indreams.me card → preferences screen.
- **Incident 2026-11-15 19:07: GPU hang took the whole desktop down.** The kernel log (`journalctl -b -1 -k`)
  shows `ring gfx_0.0.0 timeout ... Process Main pid 148458 thread shadPS4:Present`. pid 148458 was
  `port/out/dreams-probe` (a user-launched native run, started ~19:04:57). The per-queue reset via MES
  failed, so amdgpu did a MODE1 full GPU reset ("VRAM is lost"), Hyprland and all GPU clients died and
  the machine rebooted. From 19:05:34 to 19:06:43 an agent capture test ran *at the same time* (two
  Dreams instances on one GPU). The hang came ~45 s after the test ended. No log or devcoredump survived
  (the user run's output went only to the terminal; devcoredump is lost on reboot). Suspects: a
  GPU-side wait that never completes (fork's ordered-count emulation, PRECISE readback waits),
  interference from the parallel instance, or the user's input path. Unconfirmed.
- Safeguards added to `run-dreams.sh`: a single-instance `flock` (`work/.dreams-run.lock`, exit 75 when
  busy, `DREAMS_ALLOW_PARALLEL=1` to override), an always-on per-run log `work/logs/run-<RUN_ID>.log`
  (`DREAMS_LOG=0` off), and frame capture ON by default at 1000 ms (`DREAMS_CAPTURE=0` off) into
  `work/captures/<RUN_ID>`, so the next hang leaves a log plus pictures. After a hang, also copy
  `/sys/class/drm/card1/device/devcoredump/data` *before rebooting* if the desktop still allows it.
- Gyro debugging (user: imp doesn't follow DS4 motion). `tools/sdl_gyro_watch.c` (waits for Cross,
  records 10 s) showed SDL reads the DS4 USB gyro fine outside the game (peak 9.1 rad/s). The
  in-game `DREAMS_PAD_SENSOR_TRACE=1` log also shows real motion reaching our PadData once the user
  moved it (samples #134304+ in work/logs/run-20261115-192439.log; earlier samples were the pad at
  rest, which I first misread as stale data). So the data reaches scePadRead. Found a semantic bug:
  the PadData `timestamp` the game reads was CLOCK_MONOTONIC-since-boot µs, while PS4/shadPS4 use
  the process clock (sceKernelGetProcessTime). Now converted at the hand-off in pad_read_state
  (`runtime_monotonic_to_process_us`, runtime_kernel.c). Candidate fix for the imp; pending
  user test.
- Performance baseline (MangoHud CSV `work/mangohud/dreams-probe_2026-11-15_19-13-38.csv`, 9-min user
  session, RX 7800 XT + Ryzen 7 5700X, v2.52): fps median 30.0 (p5 15), frametime median 33.3 ms,
  GPU load median 22% (p95 84%), CPU load median 13%. Dreams paces itself at 30 fps like a base PS4;
  hardware mostly idle. The user wants 120 fps+ eventually, plus basic keyboard & mouse. Research first
  (user request): notes/high-fps-research.md and notes/input-research.md (agents, read-only / diagnostic
  worktree only).

## 2026-11-15 (input agent): imp motion root cause confirmed, keyboard & mouse

- Root cause of "the imp doesn't follow the DS4" (disassembly, `notes/input-research.md`): Dreams
  keeps every scePadRead sample in a 256-entry ring and the imp looks its orientation up at
  `sceKernelGetProcessTime() - 1.5 sample periods` (eboot 0x771ac0), falling back to the identity
  quaternion when no sample is that old. Our boot-monotonic timestamps were always "newer", so the
  imp got identity every frame and, moving by orientation *changes*, never moved. `3a31dd3` (process
  clock) is the fix.
- Verified headless (`tools/agent-run.sh`, worktree `port-diag`, branch `input`) with a synthetic
  figure-eight motion (`DREAMS_PAD_SYNTH_MOTION=25,4,0`) and a tutorial-skip copy of the save
  (`work/agent-user-input`, seeded with port-fps's `scripts/dreams_seed_save.py`, passed as
  `--user`): at the "offline mode" notice after the main menu the imp sweeps the screen with the 4 s
  period (`work/captures/20261115-201803`, sheet 60-64 s); with the old timestamps (temporary
  `DREAMS_PAD_TS_LEGACY=1`, since removed) it stays still on CONTINUE
  (`work/captures/20261115-202118`). Without the tutorial-skip save, the first-run path is
  preferences → control-scheme screen → a ~100 s intro film before the imp appears (~180 s).
- Keyboard & mouse (INPUT.md): vcore's window takes the mouse on click (relative mode), F1 toggles,
  focus loss releases; `bbgpu_mouse_*` are real; `bbgpu_text_input_end` added. Runtime: Dreams input
  profile (bbport's Bloodborne layout stays as `DREAMS_INPUT_PROFILE=bbport`), mouse motion = a
  yaw/pitch rotation composed after the sensors' orientation (DS4 and mouse together), keys and mouse
  buttons pause while the IME text box is open and until all keys are released. `BB_PAD_FILE`
  `mdx=/mdy=` now go through the same path: in a headless run, mdx=+6 moved the imp right, -6 left,
  mdy=-6 up, +6 down (`work/captures/20261115-202429`). Sensitivity calibrated from the synthetic run
  (10 degrees of yaw ≈ 480 px of imp at Imp Sensitivity 6): default 0.0004 rad/px ≈ 1:1 at 1080p.
- Tests: `tests/test_pad_input.c` (new, in run-dreams-tests.sh); bbport's `tests/test_pad.c` didn't
  link before (no AvPlayer stubs), now fixed and run with the bbport profile; pad-motion and IME tests
  pass.
- Learned: the imp is visible on the offline-mode notice right after the main menu loads (~40 s with
  the tutorial-skip save): the quickest place for imp tests. A Cross press doesn't hit CONTINUE unless
  the imp is over it.

- Frame rate (branch `fps`, worktree `port-fps`; research `notes/high-fps-research.md`, results
  `docs/dreams/FPS.md`).
  - Dreams picks flip rate 1 (30 fps) itself every frame and floors its measured render clock at
    1/30. Ported the Dreams fork's 60 Hz pair into `bbgpu_patch_image`
    (`vcore/shim/frame_rate.cpp`), found by signature (v2.52 `+0xe34d73`/`+0xe72d81`, v2.64 the
    fork's offsets): `DREAMS_60HZ=1`.
  - `DREAMS_FPS=n` (61..240) adds a 1/n floor and an n Hz vblank.
  - Measured in game memory (read-only `/proc/<pid>/mem` sampler, sandboxed runs):
    - render frames 30.0 → 60.0 (60HZ) → 90.4 (FPS=120) → 97.8 (FPS=240);
    - the game's render dt equals the real frame time at every rate;
    - the gameplay update counter (`0x6f738e8`) stays at 30/s;
    - above ~90 fps the shadPS4 GPU command thread is the limit (78% of a core at 98 fps), with the GPU 44% busy.
  - `DREAMS_PROSPERO=1` (sceKernelIsProspero = 1, the game's own PS5 60 fps UI) is plumbed, but only
    consulted while browsing the Dreamiverse: user test pending.
  - Found: guest thread `OSC` (remote-signals listener) at 99% of a core at any rate.
- Skip intro: the preferences screen and the control-scheme popup are first-run boot flow, gated by
  the save's tutorial flags (no ini or game switch).
  - `DREAMS_SKIP_INTRO=1` ends the logo video at once (fork switch, previously stripped by
    run-dreams.sh's `SHADPS4_*` clean-up). In a separate `DREAMS_USER_DIR` (seeded from a copy of
    `work/user`) it also sets the tutorial flags (`scripts/dreams_seed_save.py`, same edit as the
    fork's SkipDreamsTutorial).
  - Result: main menu at ~23 s with no input. The offline notice after it still needs ✕.
  - The player's own save is only edited with `DREAMS_TUTORIAL_SKIP_MY_SAVE=1` (backup first).
  - Note: until now agent runs used `work/user` too, and the game writes its SYSTEM save during runs
    (e.g. 19:47 by a research run).

- **Incident 2 (2026-11-15 20:58): second GPU hang**, same signature as 19:07 (`ring gfx_0.0.0 timeout`,
  `Process Main ... thread shadPS4:Present`, ring reset failed, MODE1 reset, VRAM lost; desktop died,
  kernel recovered without reboot). User run `run-20261115-205625` (DREAMS_60HZ=1, DREAMS_SKIP_INTRO=1,
  user's own save) hung 133 s in. Evidence: `work/crash/20261115-205951/` (tools/collect_crash.sh).
  Captures show both hangs happened in the **first-time intro cutscene** (imps swarming the camera before
  "Waiting for you to choose your very own"); the first hang (30 fps, before 60 Hz existed) was at the
  same point (~150 s), so it isn't 60 Hz-specific. A 9-minute 30 fps session earlier passed the same
  scene, so it's intermittent. Checked: the fork's unbounded "ordered turnstile" spin loop is only
  emitted with SHADPS4_DREAMS_ORDERED_COUNT_TURNSTILE=1 (not set); the float min/max CAS loop always
  makes progress. Open hypotheses: (1) a Dreams shader's own wait loop (translated, emit_spirv.cpp loop)
  that relies on PS4 ordered-count forward-progress guarantees, broken on 2.52 where 22 DOC shaders use
  the generic atomic fallback; (2) driver/timing. Safe repro path: `DREAMS_SOFTWARE_GPU=1` (lavapipe, no
  /dev/dri) in agent-run.sh, not working yet (ErrorOutOfDeviceMemory in BufferCache::GetArena).
  Workaround for the user: skip the first-time intro (DREAMS_SKIP_INTRO=1 DREAMS_TUTORIAL_SKIP_MY_SAVE=1,
  backs up the save first).
- 2026-11-15 21:08: fixed skip-my-save launch crash (FaultManager read past its buffer on >1023 page faults per batch; shadPS4 vcore commit). Verified 100 s sandbox run with a copy of the flagged save.

## 2026-11-15 (hang agent): GPU hang investigation, partial (docs/dreams/GPU_HANG.md)

- The hang is at the **transition after the imp swarm** (black frame at 133.6 s), not mid-swarm. In a
  passing run the first new pipeline at that moment is compute 0x75d8db76_8 ("Skipping lowering for
  null buffer sharp", matched by shader-cache file mtime); it has no loops/atomics. The 22 DOC-fallback
  shaders are all first dispatched during boot.
- Static scan of the 300 cached SPIR-V modules (tools/spirv_dis.c + tools/spirv_spin_scan.py): no
  look-back style wait-for-other-workgroup loop; ~30 shaders have data-bounded loops (list/tree walks)
  that would spin forever on inconsistent data.
- Lavapipe: 4 GiB sparse arena > maxBufferSize (4 GiB - 1); fix written then discarded (user dropped
  the software-GPU path).
- Next: loop guard with env switch + cache-key change + host-visible hit counter, indirect-dims clamp;
  then one guarded repro with work/hang-user.
- **OSC thread 100% CPU fixed** (2026-11-15). gdb on the spinning `OSC` thread (Dreams' Open Sound
  Control remote-signals listener) showed it looping in Dreams code around eboot +0xda2621 calling
  sceKernelGetProcessTimeCounter and scePthreadGetthreadid: a receive loop whose socket calls all
  failed instantly offline (sceNetSocket/Recvfrom/EpollWait returned ENETUNREACH), so it never slept.
  runtime_services.c now models an unplugged network: socket/bind/listen/setsockopt/getsockname
  succeed on a local socket table; recv/recvfrom/accept wait for SO_RCVTIMEO (0x1106, µs; 0 = until
  abort/close) then EAGAIN (non-blocking SO_NBIO/MSG_DONTWAIT: EAGAIN at once); sceNetEpollWait waits
  for its timeout; Abort/Close wake waiters (EINTR); send/sendto/connect stay ENETUNREACH; NetCtl/Np
  still offline. Measured in a sandbox run: OSC 103.8% -> 0.4% of a core.

## 2026-11-15 (docmatch agent): fork shader routes on any Dreams version (docs/dreams/VERSIONS.md)

- Cause of the blocky/holey sculpts on v2.52: the fork keys its fixes on v2.64 embedded shader
  hashes, so all 22 DOC kernels used the atomic fallback (wrong prefix order -> broken sculpt
  compaction lists).
- `game/CUSA08010/shaders/dd.shaders` is a 604-entry pack whose entry keys are stable across builds:
  the three keys the fork published for v2.64 (cbac06d2, 84aa3dc9, 4ebeffd2) exist in v2.52 with the
  same code sizes. Wrote `tools/dreams_shader_routes.py` (pack parser, GCN walker validated on all 604
  shaders, XXH64 code hash, DOC signature, census/check/emit) and `tools/dreams_routes.json`.
- Mapped 22 DOC routes by DS_ORDERED_COUNT control + GDS counter (all equal the fork's constants)
  and 4 non-DOC sculpt shaders (GatherVoxels, B1 transform, A3 lookup, ModelFinalize) by the fork's
  trace PCs; b535/GatherVoxels/etc. anchors land on the documented instructions in v2.52, so the
  code is the same as v2.64. f030/5ac and 252/fff2 share signatures: assigned by resources/dispatch
  shape; the emitter's exact-control asserts (GDS binding) pass for f030/5ac.
- shadPS4 branch `docmatch` (4c19e203): `Shader::DreamsRoute::Resolve` in `Info`'s constructor
  (SHADPS4_DREAMS_SIGNATURE_MATCH=1) sets pgm_hash to the route's v2.64 hash (code hash, else unique
  DOC signature), logs `Dreams route:` once per shader; route-matched programs are not cached and
  preloads of match candidates are skipped.
- A/B (same script, offline notice -> main menu): on `run-20261115-215358`: 26 routes via code,
  1 fallback (252: its audited-SPIR-V guard; same on v2.64 after the rebase); off
  `run-20261115-215739`: 22 fallbacks. The homespace behind the main menu loses its white blocky
  patches (`work/captures/20261115-215358/sheets/compare-20261115-215739.png`). No assertion/STOP.
  Inside the homespace (runs 220022 on / 220312 off, full-width captures): painterly cloud flecks and
  a solid puppet with matching, flat white blocks without.
- run-dreams.sh: `DREAMS_SIGNATURE_MATCH` default 1 (=0 for the old behaviour).
- Open: 66 non-DOC fork hashes (glass/shadow mip coordinates, linear-write ranges, sculpt/fleck
  diagnostics) need a v2.64 pack (`check`/`emit` with both packs; add keys) or per-shader work.
