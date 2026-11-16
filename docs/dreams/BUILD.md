# Building (Fedora 44, x86-64)

## 1. System packages

Installed by the user on 2026-11-15 (needs sudo):

```
sudo dnf install vulkan-loader-devel SDL3-devel libdecor-devel boost-devel fmt-devel \
  magic_enum-devel robin-map-devel xxhash-devel VulkanMemoryAllocator-devel spirv-headers-devel \
  xbyak-devel zydis-devel miniz-devel
```
Already present: gcc/g++ 16, cmake, ninja-build, glslang, ffmpeg-devel (RPM Fusion), X11/xcb/
wayland/alsa devel, vulkan-headers, vulkan-tools. Clang is at `~/.local/bin/clang`.

## 2. GPU library (temporary: bbport's libbbgpu.so in ref/bbport)

Fedora needed four workarounds versus bbport's nix environment. All are outside `port/`:

| Problem | Fix | Where |
|---|---|---|
| `find_package(Zydis)` fails: Fedora's zydis-devel ships only `zydis.pc`, no CMake config | `cmake/FindZydis.cmake` creates `Zydis::Zydis` from pkg-config | `-DCMAKE_MODULE_PATH=<root>/cmake` |
| `#include <magic_enum/magic_enum.hpp>` fails: Fedora puts headers flat in `/usr/include` | symlinks in `cmake/compat-include/magic_enum/` | `-isystem <root>/cmake/compat-include` |
| `vk::PhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE` missing: Fedora Vulkan headers are 1.4.341 | latest Khronos headers (1.4.368) in `ref/vulkan-headers` | `-isystem <root>/ref/vulkan-headers/include` (must come first) |
| link error: `ffxVkShaderOverride` / `ffxVkJobMark` undefined (pinned FSR-Vulkan submodule + bbport patches don't define them) | null definitions in `ref/bbport/gpu/upscale/zz_ffx_hook_defs.cpp` (local, uncommitted file in the ref clone) | file picked up by `upscale/*.cpp` glob |

Commands (run once; CMake caches the flags, so `build.sh` reuses them):

```
cd ref/bbport
R=/home/braden/Documents/Programming/Dreams-Recomp
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_NET=OFF -DBB_LTO=OFF \
  -DBB_PGO=off -DCMAKE_MODULE_PATH=$R/cmake \
  -DCMAKE_CXX_FLAGS="-isystem $R/ref/vulkan-headers/include -isystem $R/cmake/compat-include" -Wno-dev
BB_LTO=OFF bash build.sh          # builds out/gpu/libbbgpu.so and out/bb-probe
out/bb-probe --vulkan-only        # expect: "AMD Radeon RX 7800 XT (RADV NAVI32) ... PASS"
```
`BB_NET=OFF` drops the online module (Dreams is offline-only). LTO is off only to speed up
iteration.

## 3. The port (loader + C runtime) + GPU library

```
cd port
./build-dreams.sh                 # default DREAMS_GPU=vcore: builds vcore/build/libvcore.so,
                                  # ./build-deps.sh (codec library), then out/dreams-probe
DREAMS_GPU=bbport ./build-dreams.sh   # links ../ref/bbport/out/gpu/libbbgpu.so instead (section 2)
```
`out/gpu-library.txt` records which one was linked (run-dreams.sh reads it). Override bbport's
library location with `BBGPU_DIR=...`. The C side must stay `-no-pie`.

## 4. vcore (milestone 6, done 2026-11-15)

`port/vcore/` compiles the GPU sources of `ref/shadps4` branch **`vcore`** (= `dreams` + 2 native
edits) **in place**, plus glue in `port/vcore/shim/`. Full design: `docs/dreams/VCORE.md`.

- First `./build-dreams.sh` configures it:
  `CC=~/.local/bin/clang CXX=~/.local/bin/clang++ cmake -S vcore -B vcore/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo`
  (clang 20 like the reference shadPS4 build; override with `VCORE_CC`/`VCORE_CXX`), then
  `ninja -C vcore/build vcore` (`VCORE_JOBS`, default 15). Clean build ≈ 6 min, ~300 MB.
- Third-party code comes from `ref/shadps4/externals` (pinned versions, no extra packages). System:
  Vulkan loader, SDL3, FFmpeg dev packages (RPM Fusion, already installed), glslang.
- None of the bbport Fedora workarounds (FindZydis, magic_enum compat, ref/vulkan-headers) are
  needed: vcore uses shadPS4's own copies.
- CMake options: `SHADPS4_DIR` (default `../ref/shadps4`), `VCORE_MARCH` (default `x86-64-v3`),
  `VCORE_BBPORT_AVPLAYER` (default ON, interim AvPlayer).
- `libvcore.so` leaves `runtime_*` undefined on purpose (`--allow-shlib-undefined`); they come from
  `dreams-probe` (`-rdynamic`). Check: `nm -D -u vcore/build/libvcore.so | grep -v @` should list
  only `runtime_*` (+ weak `__gmon_start__`/`_ITM_*`).
