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

## 3. The port (loader + C runtime)

```
cd port
./build-dreams.sh                 # -> out/dreams-probe, links ../ref/bbport/out/gpu/libbbgpu.so
```
Override the GPU library location with `BBGPU_DIR=...`. The C side must stay `-no-pie`.

## 4. Later: vcore (milestone 6)

The GPU library will be rebuilt from `ref/shadps4` branch `dreams` (latest upstream + Dreams fork
patches). See `notes/shadps4-dreams-rebase.md` for how that branch was produced and
`notes/bbport-reuse-plan.md` §3–4 for the seam and the adapter design.
