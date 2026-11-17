# Building (Linux x86-64)

Working directory: `$REPO` is the repository root of your OpenDream checkout
(`REPO=/path/to/OpenDream`). Commands below use `$REPO`; the build steps themselves run from
`$REPO/ref/bbport`, `$REPO/port` and `$REPO/port/vcore` as stated. Hardware and distribution
requirements: `TEST_HARDWARE.md`. The package list is a **Fedora example**; other distributions
have different package names but need the same libraries.

## 1. System packages

Fedora example (needs sudo; installed 2026-11-15 on the reference machine):

```
sudo dnf install vulkan-loader-devel SDL3-devel libdecor-devel boost-devel fmt-devel \
  magic_enum-devel robin-map-devel xxhash-devel VulkanMemoryAllocator-devel spirv-headers-devel \
  xbyak-devel zydis-devel miniz-devel
```
Already present on that machine: gcc/g++ 16, cmake, ninja-build, glslang, ffmpeg-devel (RPM Fusion),
X11/xcb/wayland/alsa devel, vulkan-headers, vulkan-tools, clang (in `~/.local/bin` there; any
Clang recent enough for C++23 works — override with `CC`/`CXX` or `VCORE_CC`/`VCORE_CXX`).

Generic equivalent: a Vulkan loader and headers, SDL3, libdecor, boost, fmt, magic_enum,
robin-map, xxhash, VulkanMemoryAllocator, SPIRV-Headers, xbyak, Zydis, miniz, FFmpeg development
files, glslang, CMake, Ninja, GCC and Clang. On Debian-family systems the names are e.g.
`libvulkan-dev`, `libsdl3-dev`, `libfmt-dev`, `libmagic-enum-dev`, `libspirv-headers-dev`;
check your distribution's package search.

## 2. Optional legacy GPU library (bbport's libbbgpu.so in ref/bbport)

Four workarounds were needed on Fedora versus bbport's nix environment. All are outside `port/`;
they are distro-layout fixes, so a different host may need none, some, or analogous ones:

| Problem | Fix | Where |
|---|---|---|
| `find_package(Zydis)` fails: Fedora's zydis-devel ships only `zydis.pc`, no CMake config | `cmake/FindZydis.cmake` creates `Zydis::Zydis` from pkg-config | `-DCMAKE_MODULE_PATH=$REPO/cmake` |
| `#include <magic_enum/magic_enum.hpp>` fails: Fedora puts headers flat in `/usr/include` | symlinks in `cmake/compat-include/magic_enum/` | `-isystem $REPO/cmake/compat-include` |
| `vk::PhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE` missing: Fedora Vulkan headers are 1.4.341 | latest Khronos headers (1.4.368) in `ref/vulkan-headers` | `-isystem $REPO/ref/vulkan-headers/include` (must come first) |
| link error: `ffxVkShaderOverride` / `ffxVkJobMark` undefined (pinned FSR-Vulkan submodule + bbport patches don't define them) | null definitions in `ref/bbport/gpu/upscale/zz_ffx_hook_defs.cpp` (local, uncommitted file in the ref clone) | file picked up by `upscale/*.cpp` glob |

Skip this section for the default vcore build. Keep this backend for comparison runs.
Commands (run once; CMake caches the flags, so `build.sh` reuses them):

```
REPO=/path/to/OpenDream
cd $REPO/ref/bbport
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_NET=OFF -DBB_LTO=OFF \
  -DBB_PGO=off -DCMAKE_MODULE_PATH=$REPO/cmake \
  -DCMAKE_CXX_FLAGS="-isystem $REPO/ref/vulkan-headers/include -isystem $REPO/cmake/compat-include" -Wno-dev
BB_LTO=OFF bash build.sh          # builds out/gpu/libbbgpu.so and out/bb-probe
out/bb-probe --vulkan-only        # expect a line naming your GPU and driver, ending in PASS
```
`BB_NET=OFF` drops the online module (Dreams is offline-only). LTO is off only to speed up
iteration.

## 3. The port (loader + C runtime) + GPU library

```
cd $REPO/port
./build-dreams.sh                 # default DREAMS_GPU=vcore: builds vcore/build/libvcore.so,
                                  # ./build-deps.sh (codec library), then out/dreams-probe
DREAMS_GPU=bbport ./build-dreams.sh   # links $REPO/ref/bbport/out/gpu/libbbgpu.so instead (section 2)
```
`out/gpu-library.txt` records which one was linked (run-dreams.sh reads it). Override bbport's
library location with `BBGPU_DIR=...`. The C side must stay `-no-pie`.

## 4. vcore (milestone 6, done 2026-11-15)

`port/vcore/` compiles the GPU sources of `ref/shadps4` branch **`vcore`** (= `dreams` + 2 native
edits) **in place**, plus glue in `port/vcore/shim/`. Full design: `docs/dreams/VCORE.md`.

- The first `./build-dreams.sh` configures it with the equivalent of
  `CC=<clang> CXX=<clang++> cmake -S vcore -B vcore/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo`
  (it prefers `~/.local/bin/clang{,++}` when present: clang 20, the reference shadPS4 compiler;
  override with `VCORE_CC`/`VCORE_CXX`), then
  `ninja -C vcore/build vcore` (`VCORE_JOBS`, default 15). Clean build ≈ 6 min on the reference
  machine (`TEST_HARDWARE.md`), ~300 MB.
- Third-party code comes from `ref/shadps4/externals` (pinned versions, no extra packages). System:
  Vulkan loader, SDL3, FFmpeg dev packages (RPM Fusion on Fedora; installed already), glslang.
- None of the bbport Fedora workarounds (FindZydis, magic_enum compat, ref/vulkan-headers) are
  needed: vcore uses shadPS4's own copies.
- CMake options: `SHADPS4_DIR` (default `../ref/shadps4`), `VCORE_MARCH` (default `x86-64-v3`),
  `VCORE_BBPORT_AVPLAYER` (default OFF, optional legacy fallback). The runtime supplies AvPlayer
  through `src/runtime_avplayer.c` and `out/libdreams-av.so`.
- `libvcore.so` leaves `runtime_*` undefined on purpose (`--allow-shlib-undefined`); they come from
  `dreams-probe` (`-rdynamic`). Check: `nm -D -u vcore/build/libvcore.so | grep -v @` should list
  only `runtime_*` (+ weak `__gmon_start__`/`_ITM_*`).
