# Building (Linux x86-64)

Working directory: `$REPO` is the repository root of your OpenDream checkout
(`REPO=/path/to/OpenDream`). Unless noted, run commands from `$REPO`. Use a Git clone, not a
source ZIP. You do not need the game to build the port. See `TEST_HARDWARE.md` for runtime
requirements and `RUNNING.md` for the game folder.

## 1. System packages

For the default build, install Git, CMake 3.24+, Ninja, pkg-config, glslang/`glslangValidator`,
GCC/G++ and Clang with C++23 support (Clang 20 was tested). Install the development packages
for the Vulkan loader and headers, SDL3, and OpenSSL. Install FFmpeg development files to
build the video decoder. The build can omit that decoder, but your game may then skip videos.
Launching also requires Python 3, `flock` (util-linux), and a working Vulkan driver.

Fedora example:

```sh
sudo dnf install git cmake ninja-build pkgconf-pkg-config glslang gcc gcc-c++ clang \
  vulkan-loader-devel vulkan-headers SDL3-devel openssl-devel ffmpeg-free-devel python3 util-linux
```

Use `ffmpeg-devel` instead if you use FFmpeg from RPM Fusion. Package names differ on other
distributions (for example `libvulkan-dev`, `libsdl3-dev`, `libssl-dev`, and `glslang-tools`).
Some older distributions do not package SDL3 or a recent enough Clang. SDL2 is not a substitute.
The renderer's other libraries come from pinned Git submodules, not system packages.

For the default build in a fresh checkout, fetch them once before building:

```sh
port/tools/setup-build.sh
```

This downloads public sources from GitHub and the Android Open Source Project. No GitHub login
is needed. It checks out the pinned shadPS4 base, applies the tracked vcore patch, checks the
result against `port/vcore/source-lock.sh`, and initializes only the needed submodules. Rerun it
if a dependency download fails. It does not replace an existing developer renderer checkout.
If you already have the local `vcore` branch and its submodules, keep it; initialize the port's
`LibAtrac9` separately with `git submodule update --init -- port/third_party/LibAtrac9`.

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
port/build-dreams.sh               # GPU library, codecs and port/out/dreams-probe
# If the compiler runs out of memory, use fewer jobs:
VCORE_JOBS=2 JOBS=2 port/build-dreams.sh
# Legacy backend only, after section 2:
DREAMS_GPU=bbport port/build-dreams.sh
```
`out/gpu-library.txt` records which one was linked (run-dreams.sh reads it). Override bbport's
library location with `BBGPU_DIR=...`. The C side must stay `-no-pie`.

## 4. vcore (milestone 6, done 2026-11-15)

`port/vcore/` compiles the pinned patched shadPS4 renderer **in place** in `ref/shadps4`,
plus glue in `port/vcore/shim/`. The public setup does not need the maintainer's local `vcore`
branch. Full design: `VCORE.md`. `port/out/gpu-revision.txt` records the upstream commit and
verified overlay tree for that setup.

- The first `./build-dreams.sh` configures it with the equivalent of
  `CC=<clang> CXX=<clang++> cmake -S vcore -B vcore/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo`
  (it prefers `~/.local/bin/clang{,++}` when present, otherwise uses Clang from `PATH`;
  override with `VCORE_CC`/`VCORE_CXX` and use a fresh `VCORE_BUILD_DIR` to change compilers), then
  `ninja -C vcore/build vcore` (`VCORE_JOBS`, default 15). Clean build ≈ 6 min on the reference
  machine (`TEST_HARDWARE.md`), ~300 MB.
- The pinned third-party code comes from `ref/shadps4/externals`. The required system packages
  are listed in section 1.
- None of the bbport Fedora workarounds (FindZydis, magic_enum compat, ref/vulkan-headers) are
  needed: vcore uses shadPS4's own copies.
- CMake options: `SHADPS4_DIR` (default `../ref/shadps4`), `VCORE_MARCH` (default `x86-64-v3`),
  `VCORE_BBPORT_AVPLAYER` (default OFF, optional legacy fallback). The runtime supplies AvPlayer
  through `src/runtime_avplayer.c` and `out/libdreams-av.so`.
- `libvcore.so` leaves `runtime_*` undefined on purpose (`--allow-shlib-undefined`); they come from
  `dreams-probe` (`-rdynamic`). Check: `nm -D -u vcore/build/libvcore.so | grep -v @` should list
  only `runtime_*` (+ weak `__gmon_start__`/`_ITM_*`).
