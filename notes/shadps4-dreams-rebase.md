# shadPS4 + Dreams-ShadPS4 fork fixes, rebased onto latest upstream

Date: 2026-11-15. Tree: `ref/shadps4`, branch **`dreams`**. All commits are local and nothing was pushed.

## 1. Upstream base used

- Upstream: https://github.com/shadps4-emu/shadPS4 `main` at
  **`f3687cc400d77736b2a7ef1904a1302e17f9441b`** (2026-11-15 22:08 +01:00, "shader_recompiler: Implement S_BFM_B64 (#5724)").
- Submodules were initialised recursively with `--depth 1`. Sirit is at upstream `fc2b42d5`, which already has
  `OpGroupNonUniformShuffle`, so the fork's sirit patch is not needed.

Branches in `ref/shadps4`:

| branch | what |
|---|---|
| `origin/main` | upstream f3687cc4 |
| `dreamsfork/dreams-dev-20260829-full-covered-filled` | fork's published source base `b84847d1` (orphan history, 4 snapshot commits) |
| `dreams-fork-reconstructed` | b84847d1 + the 46 fork patches, one commit per patch ("fork: dreams-<name>"). This is the exact fork source. |
| `dreams-fork-squashed` | one synthetic commit: tree of `dreams-fork-reconstructed`, parent = upstream `555c458c` (the fork's real upstream base) |
| **`dreams`** | upstream f3687cc4 + the fork's fixes, rebased (10 commits, see section 3) |

## 2. The fork's patch model

- The repo (JesusDaBest/Dreams-ShadPS4) is not a source fork on `main`. It is a release repo with `builds/cusa04301-<name>-<date>/`
  folders, a `patches/` folder (the same patches are also copied into each build folder), and docs.
- **Base:** every current build manifest says `public_base_commit = b84847d1b6ca14d349bf9e11e91d14ae182e008d`.
  - That commit is not in upstream. It sits on the fork repo's branch `dreams-dev-20260829-full-covered-filled`, an orphan
    history whose tree is upstream **`555c458c`** (2026-08-15) + the Aug 17–29 Dreams investigation checkpoints.
  - It is the same content as `patches/dreams-focused-20260829-full-covered-filled.patch` + `sirit-group-nonuniform-shuffle-20260829.patch`.
  - Its diff against `555c458c`: 156 files, +54.6k lines, mostly Dreams DOC/ordered-count replay and diagnostics.
    `vk_rasterizer.cpp` alone grew by about 26k lines.
- **The patches are incremental, applied in order.** `builds/cusa04301-ordered-guards-20261113/source-manifest.json` (the newest
  build, 2026-11-13) lists all 46 patches, from `dreams-source-delta-20261016-performance` through
  `dreams-ordered-guards-20261113`. Each build's manifest is the previous list plus one new patch.
- **Cumulative exceptions:** `dreams-source-delta-20261016-performance.patch` (400 KB) is itself cumulative. It contains the
  Oct 2–15 work: save-mount lifecycle, host-entropy random, raw-image clear cache refresh, align zero-offset, selector reset
  and shape-density.
- **Verified:** the stack applies cleanly to b84847d1. Three patches needed CRLF→LF normalisation. All 61 `frozen_overlay_files`
  SHA-256 hashes in the latest manifest match the reconstructed tree.
- **Older exe-only builds** (Oct 4–6 sculpt alignment, selector reset, imp-smush, tweak-menu blur; Aug 29 full-covered;
  Oct 15 shape density; Oct 16–21 performance, occlusion, ordered-count, shader-instructions, graphical-launcher, image-cache;
  Oct 25 clean-main) have no patch of their own in the build folder. Their sources are in `patches/` and in the manifest
  chain. Check results against the reconstructed tree:
  - host-entropy-random, save-mount-lifecycle, raw-image-clear-cache-refresh, generic-align-zero-offset, selector-reset: the
    patch reverse-applies, so they are included verbatim.
  - tweak-menu-blur (glass mip, shader `0x345893a7`) and shape-density inline tables: included, reworked in later patches.
  - The Sept 4/7 research patches (generic-shift-constant-fold, research-generic-gpu-corrections, upstream-quad-swizzle,
    upstream-buffer-synchronization) and the Sept 2 WIP are research-only. The fork's SOURCE_BUILD.md says not to stack them,
    so they were not applied. The `diagnostic-source-*` snapshots were not applied either.
- **Caveat from the fork itself:** the manifests say "fresh full-build reproducibility not established" and "production
  compiler objects are reused unchanged". The shipped Windows exe may contain objects built from private sources. The
  published patches are the only source.

## 3. What was applied on `dreams`

Method: cherry-pick of `dreams-fork-squashed` onto upstream main, then per-file conflict resolution.
- 84 files conflicted, about 400 hunks.
- Upstream's 467 commits since 555c458c include major rewrites:
  - net (`network/` moved to `net/`);
  - thread-mask model (64-bit Ballot masks);
  - resource tracking split into discover and patching passes;
  - post-SSA control-flow structurizer;
  - sparse-arena buffer cache;
  - image copy and resolve code moved to `vk_runtime.cpp`;
  - per-buffer barriers replaced by Runtime range tracking;
  - host shaders embedded as SPIR-V.

The fork's intent was ported onto the new code everywhere possible. The result was then committed by subsystem:

```
09a01d40 dreams: software ATRAC9 encoder for microphone recording (libSceAt9Enc)
ff8a7b01 dreams: core/HLE changes from the Dreams-ShadPS4 fork
301e9392 dreams: shader recompiler fixes and DS_ORDERED_COUNT lowering
1c6b051e dreams: liverpool, buffer cache and page manager fixes
cfc91de0 dreams: texture cache fixes (mip, copy, readback, eviction, sampler)
be8efb2c dreams: rasterizer ordered-count collect/prefix/replay and Dreams paths
32cbc367 dreams: Vulkan pipeline/shader cache, scheduler, presenter changes
06ab3f46 dreams: build system wiring for fork sources and host shaders
2f212014 dreams: fork development notes and offline replay tools
5d41108e dreams: accept CUSA08010 (and SHADPS4_DREAMS_TITLE_IDS) where the fork checked CUSA04301
```

The commits are split by subsystem, not one commit per fork patch. Only the final tree is known to build; the intermediate
commits are not individually buildable. For per-fix provenance use `git log dreams-fork-reconstructed -- <file>`.

Total `origin/main..dreams`: 212 files, +69.6k / −1.3k lines.

### Ported (all fork fix areas)
- **DS_ORDERED_COUNT / DOC:**
  - frontend translation;
  - IR op `DataOrderedCount`, with the GDS handle packed in control bits 16+;
  - SPIR-V lowering keyed on Dreams shader hashes (`dreams_compat.h`, `dreams_stateful_order.h`);
  - rasterizer collect → parallel exclusive prefix (`dreams_ordered_prefix.comp`) → replay;
  - the Nov 12 parallel prefix and the Nov 13 guarded `fff20d00` / `2bfebd3c` / `252ccde6` paths (`alias252_*`, `audited252_spv.h`).
- **Shader instruction fixes:**
  - V_RCP_LEGACY_F32 (Tren collision);
  - V_BFE_U32 mask and clamp;
  - V_CNDMASK_B32 integer select;
  - V_CMP_CLASS lowering (`float_class_lowering.h`);
  - V_CMP results AND EXEC;
  - all F32/F64/I64/U64 CMP/CMPX variants;
  - V_CVT_U32_F64;
  - S_WQM_B64;
  - per-shader uniform SCC;
  - 64-bit DS atomics, DS_MIN/MAX_F32, GDS M0 base, DS_SWIZZLE inactive lanes;
  - S_SETPC jump tables (reimplemented for the post-SSA structurizer);
  - inline scalar tables (shape density);
  - min-LOD / offset-LOD;
  - glass-mip coordinates;
  - GPU scalar table reads;
  - sculpt checkpoint;
  - LDS barriers for Dreams wave64 shaders.
- **Texture cache:**
  - mip rendering, fallbacks, depth, capacity, refresh and readback;
  - array range;
  - copy depth, width, resolve, mixed and compressed;
  - storage multisample;
  - dynamic and single-mip writes;
  - depth growth and depth-copy buffer;
  - partial and packed GPU image writes;
  - eviction writeback and padding;
  - sampler border and GC accounting;
  - flicker repair;
  - partial raw clear.
- **Buffer cache / liverpool:**
  - precise readbacks, readback prefetch cache, readback correctness and lifetime;
  - backing write segments;
  - precise WRITE_DATA, fair GPU queues, GDS export awaiter;
  - 52 MB GDS;
  - clean CPU-write and SRT-read fault handling;
  - tiling.comp padding and `num_texels`;
  - post-process sRGB passthrough.
- **Pipeline cache:** shader-cache preloading (`WarmUp`), per-shader cache keys with revisions, private shader cache dir.
- **Core:**
  - save-mount lifecycle, host random, OpenPsId bytes, ContentSearch HLE;
  - offline NP identity for Dreams;
  - Dreams service hostname blocked (`prod.puprod.indreams.me` → ENODNS, or 127.0.0.1 with
    `SHADPS4_DREAMS_SERVICE_PROXY=1`), ported into the live `net/net_resolver.cpp` `Lookup()`;
  - eboot byte patches (`module.cpp` → `ApplyDreamsEbootPatches`);
  - F10 FPS toggle;
  - ATRAC9 software encoder for the microphone (artswap_at9, GPL-3.0-or-later);
  - `--dreams-draw-replay` tool.
- **Kept although Windows-only in the fork:** the 4.7k-line int3 trace and scene-readiness handlers in `signals.cpp`, and the
  game-FPS counter in `layer.cpp`. They still compile only under `_WIN32`, so on Linux they do nothing.

### Skipped because upstream already has it
- **Shader recompiler:**
  - V_ALIGNBIT/BYTE zero offset (the sculpt-alignment fix, upstream #5186);
  - VCC / thread-mask fixes (upstream #4972/#4995 mask redesign);
  - S_ADDC/SUBB/ADDK carry, S_BITSET/FF1_B64, 64-bit shifts;
  - V_BFM, V_CVT_PK_U8, V_MBCNT;
  - NaN-aware compares;
  - SharedAtomicFMin/FMax, ShuffleXor ops;
  - sampler `is_compare` (upstream: `is_depth`);
  - dynamic GDS append address;
  - runtime sharp-from-composite;
  - `emit_spirv_integer` fix (b2ad718f).
- **Indirect draws:** base vertex/instance (a5d307ee / 643c7997, `DrawIndirectParams`). The fork's own mechanism was removed so
  it isn't applied twice.
- **Display:** the 10-bit sRGB decode (part of display-colour, 473633c3), so `pp_pass` was kept as upstream.
- **Texture cache:** texture-synchronization barriers, readback lifetime and staging, ExpandImage stale-copy guard, XThick
  thickness, depth-growth refresh, ResolveDepthOverlap check.
- **Core and buffers:**
  - StreamBuffer watch cursor;
  - TryWriteBacking advance;
  - `sceKernelSync` (upstream posix_sync);
  - lru_cache;
  - PrepareBlankFrame;
  - sceKernelGetOpenPsId (upstream function kept, with the fork's ID bytes);
  - PngDec (upstream uses LLE now).

### Superseded or approximated
These are the places where behaviour can differ from the fork. Check them first if a regression shows up.
- **Buffer cache:**
  - large-image admission (fork: 64–128 MB per buffer) is now capped at 256 MB total plus a device budget check, because
    arenas have no GC;
  - JoinOverlap, CreateBuffer admission and DeleteBuffer no longer exist upstream.
- **Rasterizer:**
  - removed: the fork's per-buffer dynamic-read, indirect-arg and descriptor barriers (upstream Runtime range tracking
    replaces them);
  - `DreamsBufferBarrier` is conservative and always issues a full barrier;
  - raw storage-buffer writes now invalidate overlapping images on every write (upstream rule).
- **Fork's subgroup-compacted GDS append:** replaced by upstream's per-lane add with `RemoveAppendBufferLaneOffset`, which
  should give the same result. The backend's compacted path is still used for sprite-cull.
- **Pooled tiling allocations** (`SHADPS4_DREAMS_POOLED_TILING_ALLOCATIONS`): superseded by the upstream staging pool, so the
  env var is now a no-op.
- **`pp_pass`:** the fork's `input_srgb` push constant was dropped because upstream's `srgb_input` does the same thing.
- **Network:** resolver destroy, error-code and SO_ERROR conversions are moot after the net rewrite.
- **Shader cache formats:** versions bumped (ShaderBinary 21, Meta 8/9/10, PipelineKey 5). Old fork caches are invalid.

### Left out or unverified (`grep -rn DREAMS-REBASE src`)
- `vk_rasterizer.cpp` (TODO): the integer-format sampler downgrade to point filtering is not ported. It needs
  `SamplerResource::associated_image`, which upstream removed.
- `emit_spirv_atomic.cpp` (3 TODOs): the fork asserted exact GDS buffer slots (2bf = 5, 692 = 7, 7aa = 3, 016 = 2). These are
  relaxed to "buffer is GDS", because upstream's flatbuffer user data shifts binding slots.
- `emit_spirv.cpp` (TODO): the 2bf audit binding numbers (load 3/4, store 2) need re-checking.
- `emit_spirv_context_get_set.cpp` (TODO): the 2bf `handle == 2` store gate needs re-checking.
- **84aa replay:** its setup asserts a specific outer if/merge layout in the structured block list. Upstream now structures
  control flow later, so this needs a real run.
- `resource_pass` (DREAMS-REBASE note): the unbounded inline-table fallback.
- `PushData` host_data offset is now 72 (fork: 120). The backend uses `PushDataHostDataOffset`.
- **Not tested:**
  - nothing was run against the game;
  - the `_WIN32`-only fork code was never compiled on this Linux host.

## 4. Title- and version-specific code (important: the user has CUSA08010 v2.52; the fork targets CUSA04301 VERSION 02.64)

1. **Serial gates.** The fork gates about 160 sites on `GameSerial() == "CUSA04301"`:
   - 116 are in `vk_rasterizer.cpp`, and almost every Dreams path is behind one;
   - the rest are in the scheduler, presenter, pipeline cache, buffer cache prefetch, fair queues, GDS export, np_manager
     offline identity, layer.cpp and export.cpp;
   - plus the window title.

   The last commit, `5d41108e`, routes all of them through `common/dreams_title.h`. That accepts CUSA04301, CUSA08010, and
   anything listed in `SHADPS4_DREAMS_TITLE_IDS`. Drop that commit to restore the fork's exact CUSA04301-only gating.
2. **Shader hashes.** These matter most. `pgm_hash` is the hash embedded in the game's shader binary
   (`bininfo.shader_hash`, regs_shader.h), not a hash of the code.
   - The fork's own inspection (`ref/dreams-fork/docs/GAME_FILES_INSPECTION_20260904.md`) found that a CUSA08010 02.65 build
     has the same 604 `dd.shaders` pack keys, but every embedded hash is different. 546 of the 604 shaders have identical
     code; 58 differ. Examples: `cbac06d2` ↔ `fff5a423`, `4ebeffd2` ↔ `c1a85bbd`, `84aa3dc9` ↔ `e0887e2d`.
   - The fork keys about 60 shaders. They include `DreamsCompat::*` (b535c6c8, 2bfebd3c, 692f0f7f, 3937a849, 81cf58cf, d8b4ddb5,
     95c28c88, 7aa925e9, 016b9f6a, 90272fc4, 7ba4de5d, ce3b8413, 3706083c, …), fff20d00, 252ccde6, 345893a7, 85f67046,
     5650b8f7, 44c189d5, 2b085c6c, 15f114ff and 619002ef. These are spread across `dreams_compat.h`, `vk_rasterizer.cpp`,
     `resource_patching_pass.cpp`, `scalar_alu.cpp`, `vk_pipeline_cache.cpp` and `vk_pipeline_serialization.cpp`.
   - **On CUSA08010 v2.52 essentially none of these hash-gated paths will activate**, so DOC falls back to the plain
     atomic path, which is the known-broken behaviour.
   - Fix: build a pack-key → hash remap from the user's `dd.shaders` against CUSA04301 02.64. Or, better, key on a hash of
     the instruction bytes. 64-bit code fingerprints and XXH3 checks already guard some rasterizer paths (the linear-write
     guard and the dynamic-read tables), and those only match identical code.
3. **Guest executable patches** (`module.cpp`, `ApplyDreamsEbootPatches`). All are keyed on the serial and on an exact byte
   match at an eboot offset. On v2.52 they will almost certainly report "did not match" and be skipped, which is safe.
   - Always on:
     - scene gate at `+0x9c44f7` (36 bytes);
     - slowdown guard at `+0x948810` (forces the slowdown estimate to 1.0 so Preferences stops reopening).
   - Env-gated:
     - `FORCE_60HZ_RENDERING`: `+0xc2075e` / `+0xc502d1`;
     - `RETIREMENT_FALLBACK`: `+0x9aaf4a`;
     - `GUARD_UNRESOLVED_RESOURCE`: `+0xebadea`;
     - `SKIP_UNRESOLVED_BUILDER_RESOURCE`: `+0x8b7bc0`;
     - `DEFER_ACTIVE_RETIREMENT`: `+0x729fac`;
     - `UNSAFE_SCENE_GATE_BYPASS`: `+0x9c44f7`.
   - About 12 Windows-only int3 trace sets (offsets 0x5xxxxx–0x15cxxxx), with handlers in `signals.cpp`.
   - Guest data-structure offsets in `src/core/dreams_scene_readiness.h` (`root+0x10b58c0`, `base+0x44d1730`, `+0x28ca538`, …).
4. **Other fixed addresses and constants:**
   - `layer.cpp` game-FPS counter: signature at eboot `+0x8fd90b`, counter at eboot `+0x4558688` (Windows-only);
   - file_system traces: `/app0/data/dev_levels/dev_levels_0{0,1}.zip` and main-image range 0x05770000–0x07400000;
   - rasterizer trace addresses: 0x231c0061c, 0x23b4fe900, 0x2e3ae0000;
   - frame-fence load sites relative to `MemoryPatcher::g_eboot_address`;
   - GDS dwords 320/321/322, 324, 327, 515/516 and 0xa6–0xaa; DOC control words;
   - queue-producer store offsets 0x140–0x18c;
   - BC3 3D alias and the R16G16B16A16 partial raw clear (shader 85f67046).
5. **Hard-coded paths and IDs:**
   - the private shader cache folder name `build-248ff74e…-cache-v4`;
   - the tutorial helper only accepts `CUSA04301/SYSTEM/savedata.json`;
   - the per-game config must be named after the serial, i.e. `custom_configs/CUSA08010.json` for the user's copy.

## 5. Build

Built successfully: `ref/shadps4/build/shadps4` (96 MB ELF, Release, clang 20 ROCm). `--help` runs, and `ldd` shows no
missing libraries. It has not been run against the game.

```sh
cd ref/shadps4
# Workaround deps (no sudo on this host): see below
CC=gcc CXX=g++ cmake -S externals/openal-soft -B build-deps/openal-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLIBTYPE=STATIC -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF -DALSOFT_INSTALL=ON \
  -DALSOFT_INSTALL_CONFIG=OFF -DALSOFT_INSTALL_HRTF_DATA=OFF -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF \
  -DALSOFT_ENABLE_MODULES=OFF -DALSOFT_BACKEND_PIPEWIRE=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX=$PWD/build-deps/openal
ninja -C build-deps/openal-build install
CC=~/.local/bin/clang CXX=~/.local/bin/clang++ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DLIBUSB_ENABLE_UDEV=OFF \
  -DCMAKE_PROJECT_INCLUDE=$PWD/build-deps/host-workarounds.cmake
ninja -C build -j15 shadps4
```

Host workarounds live in `ref/shadps4/build-deps/`, which is excluded via `.git/info/exclude` and is not committed:
- **openal-soft:** the bundled copy doesn't compile with clang 20 + libstdc++ 16 (`operator<=>` constraint error in
  `externals/openal-soft/common/altypes.hpp`). It was prebuilt with GCC 16 and injected via `host-workarounds.cmake`
  (`find_package(OpenAL CONFIG)`). Backends: PulseAudio, ALSA, OSS. A newer clang might make this unnecessary.
- **libudev headers missing:** worked around with `-DLIBUSB_ENABLE_UDEV=OFF`.
- **libuuid headers missing:** worked around with a minimal `uuid/uuid.h` shim plus a `libuuid.so` symlink to the system
  `libuuid.so.1`.

To drop the workarounds, install (needs sudo): `sudo dnf install systemd-devel libuuid-devel`. The other packages in upstream's
Fedora list are not required for the bundled-deps build: pipewire-jack-audio-connection-kit-devel, openal-soft-devel,
libevdev-devel, vulkan-devel / vulkan-loader-devel, vulkan-validation-layers, libpng-devel. Installing
`vulkan-validation-layers` is still recommended for debugging.

## 6. Runtime configuration the fork uses

Copied to `notes/shadps4-dreams-runtime/`:
- `environment.json`, `CUSA04301.json`, `versions.json`, `validation.json`, extracted from the Nov 13 release zip;
- `DreamsCore.cs`, `SkipDreamsTutorial.cs`, `source-manifest.json`.

- **Per-game config** (`CUSA04301.json`; for the user's copy, save it as `custom_configs/CUSA08010.json`). Key settings:
  - `GPU.readbacks_mode = 2` (Precise). The fork treats this as essential: Disabled/Relaxed gives the wrong cube and sculpts.
  - `readback_linear_images_enabled = false`, `copy_gpu_buffers = false`, `direct_memory_access_enabled = false`,
    `patch_shaders = false`.
  - `present_mode = "Immediate"`, `vblank_frequency = 60`, `fsr_enabled = false`, `rcas_enabled = true`.
  - `Vulkan.pipeline_cache_enabled = false`: the fork uses its own private shader cache (`SHADPS4_DREAMS_PRIVATE_SHADER_CACHE`).
  - `General.connected_to_network = true` and `shad_net_enabled = false`, so play is offline. The `indreams.me` service host is
    forced to fail DNS, and NP returns the offline identity "DreamsOffline".
  - Log filter `*:warning Kernel.Vmm:critical`.
  - Upstream's settings still read the same keys (`readbacks_mode` etc.) from `custom_configs/<serial>.json`.
- **Environment variables** set by the launcher (`DreamsCore.cs` strips any other `SHADPS4_*` first). These are the
  feature switches, so reproduce them exactly. Set to `1`:
  - `ADDITIONAL_EXACT_DOC`, `SCENE_COMPACT_EXACT_DOC`, `SELECTION_LIST_EXACT_DOC`, `STATEFUL_EXACT_DOC`
  - `BLOCK_TEXEL_ALIAS`, `EXTENDED_READBACK_CACHE`, `FAIR_GPU_QUEUES`, `GPU_NUMERIC_TABLE`, `LINEAR_WRITE_RANGES`
  - `POOLED_TILING_ALLOCATIONS` (now a no-op)
  - `PRECISE_FRAME_FENCE_READS`, `PRECISE_SRT_READS`, `PRECISE_WRITE_DATA`
  - `PREFETCH_CPU_READBACKS`, `PREFETCH_CPU_WRITE_READBACKS`
  - `STRICT_SCENE_READINESS` (its int3 part is Windows-only)

  Set to `0`: `BACKGROUND_CONTROL`, `DEFER_PREFETCH_WAITS`, `FORCE_60HZ_RENDERING`, `PRECISE_CPU_WRITES`, `SYNC_GDS_EXPORT`.

  Also: `SHADPS4_ENABLE_IPC=false`, `SHADPS4_DREAMS_PRIMARY_QUEUE_PROBE_DIR=<writable dir containing mode.txt = "native">`, and
  `SHADPS4_DREAMS_PRIVATE_SHADER_CACHE=<dir>`. The code knows 199 `SHADPS4_DREAMS_*` names in total; most are diagnostics.
- **Launch:** windowed (`--fullscreen false`), no portable `user/` folder next to the exe, stdin closed.
- **Firmware modules:** the fork adds HLE for `libSceAt9Enc` (microphone) and `libSceContentSearch`. Its docs never list
  required `sys_modules`, and the release says it ships "no firmware".
  - Upstream loads a game's own `sce_module/` first.
  - Otherwise it allows LLE from the firmware `sys_modules` only for a fixed list (`ModulesToLoad` in
    `src/core/libraries/sysmodule/sysmodule_internal.cpp`).
  - The modules on that list with no HLE fallback are: libSceUlt, libSceJson, libSceJson2, libSceCesCs, libSceAudiodec*,
    libSceFreeTypeOt/Ol/OptOl, libSceXml, libSceDepth, libScePadTracker, libSceMoveTracker, libSceAvPlayerStreaming and a
    few others. Supply these from a real PS4 firmware dump if Dreams requests them.
  - I don't know which ones Dreams actually loads. The shadPS4 log reports "Allowed LLE that isn't present" on the first run.
- **Tutorial skip** (`SkipDreamsTutorial.cs`, a Windows Forms GUI, easy to reimplement):
  - Requires Dreams to be closed.
  - Opens `<profile>/…/CUSA04301/SYSTEM/savedata.json`; on the user's copy the folder is `CUSA08010`.
  - In `player_progress` it sets every `false` to `true` in three arrays, and refuses the save if any array length differs:
    - `played_tutorial_vo` (exactly 158 flags; indices 85–87 are left unchanged);
    - `played_tutorial_cutscene` (6 flags);
    - `progress_unlocked` (56 flags).
  - Text-level regex edit that preserves the BOM and formatting. It writes a timestamped `.before-tutorial-skip-*.bak`
    backup via `File.Replace` and aborts if the file changed meanwhile.
  - The array lengths may differ on v2.52; check before porting.
- **`DreamsCore.cs`:** a launcher adapter only.
  - Detects Dreams by the param.sfo `TITLE_ID == CUSA04301`.
  - Verifies the core, config and env SHA-256 hashes.
  - Copies `CUSA04301.json` into `%APPDATA%/shadPS4/custom_configs/`, backing up any existing file.
  - Sets the environment variables above and launches `core/shadps4.exe`.
