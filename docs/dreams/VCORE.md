# vcore: the shadPS4-based GPU library of the native Dreams port

Status 2026-11-15: **builds and boots Dreams natively to its first-run preferences menu**
("Dreams comes with a host of preferences..., ✕ CONTINUE"), past the old `DS_ORDERED_COUNT`
stop, with intro screens and the AvPlayer intro video in between. This is the same point the
reference emulator reaches (controller-introduction/first menus).

vcore is `port/vcore/`: a CMake project that produces `libvcore.so`, the GPU library the native
loader (`port/src/probe.c`) and C runtime (`port/src/runtime_*.c`) link against instead of bbport's
`libbbgpu.so`. It implements bbport 0.7's runtime<->GPU C API (`port/gpu/bbgpu.h`) using the GPU
code of **ref/shadps4 branch `vcore`** = branch `dreams` (latest upstream shadPS4 `f3687cc4` + all
Dreams-ShadPS4 fork patches, `notes/shadps4-dreams-rebase.md`) + 1 commit of `// vcore:` edits.

## 1. Design

```
 port/src/probe.c + runtime_*.c   (C: loader, kernel, files, pad, audio, ...)
        |  bbgpu_init / bbgpu_register_kernel / bbgpu_resolve(NID) / bbgpu_handle_fault
        v                                   ^ runtime_memory_*, runtime_process_time_*, runtime_errno
 port/vcore/shim/*.cpp            (glue: C API, MemoryManager facade, signals, log, window, stubs)
 port/vcore/shim/include/**       (headers that SHADOW shadPS4 core headers)
        |
 ref/shadps4/src (compiled IN PLACE, branch vcore):
   video_core/**  shader_recompiler/**  common/{subset}  core/emulator_settings.cpp
   core/libraries/{gnmdriver/gnmdriver.cpp, videoout/{driver,video_out}.cpp, kernel/equeue.cpp}
 port/gpu/platform/avplayer/*.cpp (bbport's native AvPlayer, compiled in place)
```

- **In place, not vendored.** `port/vcore/CMakeLists.txt` globs `${SHADPS4_DIR}/src/video_core`
  and `shader_recompiler` and lists the few other files. Re-syncing upstream = rebasing the
  `vcore` branch in ref/shadps4 and rebuilding. `SHADPS4_DIR` (CMake cache var) points elsewhere if
  needed.
- **Header shadowing.** `shim/include` is first on the include path. A quoted include such as
  `"core/memory.h"` from `src/video_core/...` is looked up next to the including file (not found),
  then in `shim/include` (found) before `ref/shadps4/src`. So upstream files compile unmodified
  against vcore's replacements. Only headers that would drag in the emulator are shadowed.
- **Third-party** comes from shadPS4's own pinned `externals/` (fmt 12, Vulkan-Headers 1.4.330,
  VMA, sirit, abseil, robin-map, xbyak, magic_enum, toml11, nlohmann-json, zydis, miniz, xxhash,
  ext-boost, gcn, half, stb, tracy headers), i.e. the same versions as the reference emulator build.
  System: Vulkan loader, SDL3 (must be the same SDL3 the runtime links), FFmpeg (AvPlayer),
  glslang (host shaders, via shadPS4's own `src/video_core/host_shaders/CMakeLists.txt`).
- **Compiler:** `~/.local/bin/clang++` (clang 20, as the reference build), `-march=x86-64-v3`,
  `-ftls-model=initial-exec` (game threads call in; same as bbport/Lantern), C++23. The runtime
  stays GCC/C11. `-Wl,--allow-shlib-undefined`: `runtime_*` symbols resolve against `dreams-probe`
  (`-rdynamic`).
- **Initialisation order** (`shim/bbgpu.cpp`, mirrors shadPS4 `Emulator::Run`):
  1. `bbgpu_register_kernel()` (before the image is loaded): upstream `RegisterEventQueue`
     (equeue/kqueue/user/timer events) + bbport AvPlayer into vcore's NID table.
  2. `bbgpu_init(BbGpuConfig)`: `EmulatorSettings.Load()` + `Load(serial)` (per-game config),
     `ElfInfo` filled (serial, title, SDK, PSF attributes, APP_VER/SYSTEM_VER from
     `$VCORE_GAME_DIR/sce_sys/param.sfo`), SDL window on its own thread (`g_window`), then
     `GnmDriver::RegisterLib` (creates `Liverpool` + `Vulkan::Presenter`, i.e. instance, rasterizer,
     caches) and `VideoOut::RegisterLib` (VideoOutDriver + present/vblank thread).
  3. The loader resolves imports through `bbgpu_resolve(NID)` (309-335 HLE symbols).
  4. The runtime's SIGSEGV handler calls `bbgpu_handle_fault()` first ->
     `Core::Signals::DispatchAccessViolation()` -> upstream `page_manager.cpp`'s handler.

## 2. How each shadPS4 dependency is mapped

| shadPS4 dependency (used by GPU code) | vcore mapping | file |
|---|---|---|
| `core/memory.h` `MemoryManager` (`IsValidMapping` x158 in the fork's rasterizer, `ClampRangeSize`, `CopySparseMemory`, `TryWriteBacking`, `TryReadBacking`, `TryStoreBacking`, `SetRasterizer`, `GetAddressSpace`) | shadowed facade over `runtime_memory_*`: mapping queries with a per-thread region cache (bbport), backing-view reads/writes (`runtime_memory_read/write_backing`, never fault on GPU-protected pages), `SetRasterizer` installs `runtime_memory_set_gpu_hooks`: map -> `Rasterizer::RegisterMemory`+`MapMemory`, unmap -> `UnmapMemory`, host writes (file reads) -> `InvalidateMemory` | `shim/include/core/memory.h`, `shim/memory.cpp` |
| `core/address_space.h` (`Protect`, `BackingPages().find`, `SystemReservedVirtualBase/Size`, `GetUsableRegions`) | shadowed: `Protect` -> `runtime_memory_gpu_protect`; `BackingPages().find(page)` returns a per-thread copy of the 16 KiB page read through the backing view (only used for hashing) | same |
| `core/signals.h` | upstream header kept; `SignalDispatch` implemented as a pure registry (no OS handlers: the loader owns them) | `shim/common_glue.cpp` |
| `core/emulator_settings.h/.cpp` | **upstream compiled as is**; reads `$SHADPS4_USER_DIR/config.json` + `custom_configs/<serial>.json` exactly like the emulator | (upstream) |
| `common/path_util.cpp` user dir | upstream + `// vcore:` edit: `SHADPS4_USER_DIR` overrides `./user` / `~/.local/share/shadPS4` | branch `vcore` |
| `common/logging/log.cpp` (spdlog, log files) | replaced: stdout lines `gpu <level> (<Class>): msg [file:line]`, filter `VCORE_LOG_FILTER` (shadPS4 syntax, default `*:warning`) | `shim/log.cpp` |
| `common/assert.cpp` (`Core::Emulator::Shutdown`) | replaced: backtrace + `STOP: GPU library assertion failed`, exit 23 (bbport behaviour) | `shim/common_glue.cpp` |
| `common/thread.cpp` (guest pthread names) | replaced: Linux pthread names/priorities/sleep | `shim/common_glue.cpp` |
| `core/debug_state.h` (devtools) | upstream header; non-inline methods are no-ops | `shim/kernel_glue.cpp` |
| `core/libraries/libs.h` (`LIB_FUNCTION`, loader `SymbolsResolver`) | shadowed: fills vcore's NID table read by `bbgpu_resolve`; `SymbolsResolver::AddSymbol` kept for bbport's AvPlayer | `shim/include/core/libraries/libs.h`, `shim/bbgpu.cpp` |
| `core/libraries/kernel/kernel.h` (pulls the linker) | shadowed: `__Error()` only -> `runtime_errno()` | `shim/include/.../kernel/kernel.h`, `shim/kernel_glue.cpp` |
| kernel `process.h`/`time.h` (`sceKernelGetTscFrequency`, `ReadTsc`, `GetProcessTime*`, `IsNeoMode`=0, `GetCompiledSdkVersion`, `Usleep`) | upstream headers; implemented on the runtime's clock (`runtime_tsc_frequency`, `runtime_process_time_*`) | `shim/kernel_glue.cpp` |
| `kernel/equeue.cpp` | **upstream compiled as is**. Its `io_context`/`KernelSignalRequest` (timer events) get a lazily started service thread; its file/socket handle table is a shadowed mini `HandleTable` (handles start at `0x40000`, never collide with runtime fds) | `shim/include/core/file_sys/fs.h`, `shim/include/core/net/guest_net.h`, `shim/kernel_glue.cpp` |
| `kernel/threads.h` (`Libraries::Kernel::Thread`) | shadowed with bbport's host thread that calls `runtime_thread_attach_host` first (guest TCB for guest callbacks) | `shim/include/core/libraries/kernel/threads.h` |
| `system/systemservice.h` `IsSplashVisible` | `false` (splash is the runtime's) | `shim/kernel_glue.cpp` |
| `sdl_window.h` (`Frontend::WindowSDL` + input) | shadowed: window/surface/resize/close + IME text entry only; input stays in `runtime_pad` | `shim/include/sdl_window.h`, `shim/window.cpp` |
| `imgui/renderer/{imgui_core,texture_manager}.h`, `imgui/imgui_texture.h`, `imgui/notifications_layer.h` | shadowed no-ops (no Dear ImGui in vcore) | `shim/include/imgui/...` |
| presenter drawing the game through ImGui | `// vcore:` edit in `vk_presenter.cpp`: blit (letterboxed, black clear) into the swapchain under `VCORE_NATIVE` | branch `vcore` |
| `video_core/renderdoc.cpp` (RenderDoc + screenshot requests) | replaced; RenderDoc off; screenshot requests kept, plus `VCORE_SCREENSHOT_EVERY=<s>` timer | `shim/kernel_glue.cpp` |
| `core/file_sys/ifile.h` (`IsZArchiveFile`, `ReadGameFile`, used by path_util game discovery) | `false` / `nullopt` | `shim/kernel_glue.cpp` |
| `common/elf_info.h` `ElfInfo` (GameSerial gates!) | filled by a vcore-defined `Core::Emulator` (ElfInfo's friend) from `BbGpuConfig` + param.sfo | `shim/bbgpu.cpp` |
| AvPlayer (`libSceAvPlayer`) | **interim**: bbport 0.7's native AvPlayer, compiled in place from `port/gpu/platform/avplayer` (FFmpeg). CMake option `VCORE_BBPORT_AVPLAYER` (default ON). The lead plans a CPU-side AvPlayer in the runtime; the loader asks the runtime before `bbgpu_resolve()`, so that one overrides this automatically; then configure with `-DVCORE_BBPORT_AVPLAYER=OFF` | CMakeLists.txt |

## 3. bbgpu.h API: implemented vs stubbed

Implemented: `bbgpu_init`, `bbgpu_register_kernel`, `bbgpu_resolve`, `bbgpu_symbol_count`,
`bbgpu_handle_fault`, `bbgpu_text_input_begin/poll`, `bbgpu_video_active/skip` (bbport AvPlayer;
stubs returning 0 when `VCORE_BBPORT_AVPLAYER=OFF`),
`bbgpu_frame_aspect` (16:9), `bbgpu_overlay_banner` (prints).

Stubs (Bloodborne/bbport product features or diagnostics): `bbgpu_dump_guest_writes`,
`overlay_captures_input` (0), `mouse_take/captured/capture/look_enable`, `add_event_filter`,
`world_active/world_us` (0), `add_pointer_query`, `pointer_wanted` (0), `add_pad_filter`,
`filter_pad`, `trophy_setup`, `trophy_unlock` (-1), `camera_look/free_camera/camera_state` (0),
`patch_image`, `rebuild_shader_caches` (0), shader-recipe/cache tools (`rebuild_shader_cache`,
`predict_shaders_test`, `merge_shader_metas/packs`, `meta_reconstruct_test`,
`export/import_shader_recipes`: return 1 "not in vcore"), and the two non-header functions
`port/src/runtime_memory/kernel_api.c` calls: `bbgpu_gpu_touched` (0), `bbgpu_straddle_report`.

Other stubs/limitations inside the shim:
- Equeue read/write/vnode kevents on runtime file descriptors or sockets report EBADF.
- `sceGnmGetTheTessellationFactorRingBufferBaseAddress` returns upstream's address in the PS4
  system-reserved range, which the native runtime does not map (only matters if Dreams tessellates).
- No ImGui overlay (FPS, devtools, notifications are printed instead).
- RenderDoc capture is off.
- `IsValidMapping` means "really mapped" (upstream: "inside the reserved guest space").

## 4. ref/shadps4 branch `vcore` (local commits, never pushed)

`git -C ref/shadps4 log dreams..vcore`: one commit `5f10ad43` "vcore: native-port edits".

| File | Edit (marked `// vcore:`) | Why |
|---|---|---|
| `src/common/path_util.cpp` | `SHADPS4_USER_DIR` env overrides the user directory | the library must not create/read `./user` or `~/.local/share/shadPS4` (an installed emulator's data) |
| `src/video_core/renderer_vulkan/vk_presenter.cpp` | `#ifndef VCORE_NATIVE` around ImGui/devtools/shadNet includes, layer registration, ImGui texture add/remove; `#ifdef VCORE_NATIVE` blit of the frame image into the swapchain instead of the ImGui "Display" window | upstream draws the game frame only through Dear ImGui; vcore has no ImGui |

Everything else is upstream/fork code byte for byte. To re-sync: rebase `vcore` onto a new `dreams`.

## 5. Build and run

```
cd port
./build-dreams.sh                 # DREAMS_GPU=vcore is the default: configures vcore/build once
                                  # (clang from ~/.local/bin), ninja libvcore.so, ./build-deps.sh
                                  # (codec library), links dreams-probe
DREAMS_GPU=bbport ./build-dreams.sh   # old path: ../ref/bbport/out/gpu/libbbgpu.so
cd port && BB_GAME_PROFILE=none DREAMS_TIMEOUT=0 timeout -s KILL 150 ./run-dreams.sh
VCORE_SCREENSHOT_EVERY=10 ...     # PNGs of the presented frame in work/vcore-user/screenshots/
VCORE_LOG_FILTER="*:info" ...     # more GPU log
```
A clean vcore build is ~2,300 compile steps incl. abseil/zydis/sirit (~6 min on 16 cores);
`vcore/build` is ~300 MB. Manual configure:
`CC=~/.local/bin/clang CXX=~/.local/bin/clang++ cmake -S vcore -B vcore/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo`.

`run-dreams.sh` (when `out/gpu-library.txt` says vcore) reproduces the Dreams fork launcher's
setup (`tools/run-shadps4-reference.sh`):
- `work/vcore-user/custom_configs/<SERIAL>.json` = `notes/shadps4-dreams-runtime/CUSA04301.json`
  (readbacks_mode=2 Precise, copy_gpu_buffers=false, direct_memory_access=false,
  patch_shaders=false, pipeline_cache=false, vblank 60, Immediate present, ...). The serial is read
  from param.sfo, so v2.64 / CUSA04301 copies work the same way.
- the fork's `SHADPS4_DREAMS_*` switches (16 on, 5 off), `SHADPS4_ENABLE_IPC=false`,
  `SHADPS4_DREAMS_PRIMARY_QUEUE_PROBE_DIR=work/vcore-probe` (mode.txt = native),
  `SHADPS4_DREAMS_PRIVATE_SHADER_CACHE=work/vcore-shader-cache`; other `SHADPS4_*` from the caller
  are removed unless `VCORE_KEEP_SHADPS4_ENV=1`.
- `SHADPS4_USER_DIR=work/vcore-user`, `VCORE_GAME_DIR=<game>`.

The first log lines confirm it: `vcore: ... per-game config custom_configs/CUSA08010.json loaded`,
`readbacks_mode=2 ...`, `GameSerial()=CUSA08010 (Dreams gates ON)`.

## 6. Current result (2026-11-15, CUSA08010 v2.52)

Run log `work/vcore-run3.log` (150 s, killed by `timeout`). Sequence on screen (timed screenshots):
"Sony Interactive Entertainment presents" -> the indreams.me companion card -> the first-run
**preferences screen with ✕ CONTINUE**, which then animates waiting for input (all frames correct).
GPU log matches the reference emulator's profile: `Failed to compute offset for SRT walker`
(also in the reference), 15 `Dreams DOC atomic fallback ... kernel shape/feature gate` (DOC
routes keyed on v2.64 shader hashes don't match v2.52), one
`image format Bc3UnormBlock type 3D is not supported` (also in the reference), `Dreams private
shader cache preloaded 149 of 149 pipelines` on later runs, `Dreams precise WRITE_DATA active`.
No assertion, no STOP.

Without AvPlayer (first attempt) the game sat on the SIE card forever: Dreams waits for its intro
video. bbport's AvPlayer (interim, see §2) fixed it. Re-checked on the merged tree (runtime
790883e + build-deps codecs): `work/vcore-run4.log`, same menu after ~60 s.

For a runtime-side AvPlayer that writes decoded frames into guest memory: the GPU side only needs
to hear of those CPU writes like any other. Writes made by guest-attached host threads with plain
stores fault on GPU-protected pages and are handled by page tracking. Writes the kernel makes
(read() into guest memory) or writes into pages the runtime writes through its backing view must
call `runtime_memory_note_write(address, size)` first (which reaches `Rasterizer::InvalidateMemory`
through the hook vcore installs). bbport's AvPlayer calls `Core::Memory::Instance()->InvalidateMemory`
after each frame for the same reason.

## 7. Next steps / known gaps

1. Input: press ✕ with the connected pad (runtime_pad samples SDL gamepads; the window pumps their
   events) and follow the menus to homespace; compare with the reference emulator.
2. v2.64 (CUSA04301 hashes): most `DreamsCompat` DOC routes and the eboot byte patches only engage
   there (`notes/shadps4-dreams-rebase.md` §4). Or remap the ~60 shader hashes for v2.52.
3. Eboot patches (`ApplyDreamsEbootPatches`, scene gate / slowdown guard) live in shadPS4's
   module loader, which vcore doesn't compile. If needed on v2.64, port them into `bbgpu_patch_image`
   (the loader already calls it before protecting the image).
4. Performance: Lantern Engine's parallel draw pipeline / memory model (milestone 9).
5. ImGui-less presenter: no FPS counter; add one through `bbgpu_overlay_banner`-style output if wanted.

## 8. Licensing / provenance

- shadPS4 sources: GPL-2.0-or-later (compiled in place, not copied).
- vcore glue (`port/vcore/**`): GPL-2.0-or-later. Files modelled on bbport 0.7 (`gpu/shim/bbgpu.cpp`,
  `window.cpp`, `core/memory.h`, `common/assert.cpp`, `core/libraries/kernel/threads.h`; all
  GPL-2.0-or-later, our own `port/` repo) say so in their headers. bbport's AvPlayer is compiled from
  `port/gpu/platform/avplayer` unchanged.
- **No file in vcore is derived from Lantern Engine code.** Lantern (`ref/lantern/engine/gpu/shim`)
  was only studied for the overall pattern (shadowing shim headers + bbgpu C API), which is also
  bbport's. If Lantern code is ported later, mark the file "derived from Lantern Engine by Awaken"
  and follow its extra terms (D8 in ARCHITECTURE.md).
