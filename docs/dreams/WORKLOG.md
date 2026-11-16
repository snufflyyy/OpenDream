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
