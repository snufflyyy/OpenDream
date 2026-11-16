# Architecture and decisions

## Execution model (inherited from bbport)

1. **Offline prep** (`scripts/`, Python, no game modification on disk):
   - `prepare.py` parses the plaintext SELF (`eboot.bin`), lays out loadable segments into a flat
     image, records relocations/imports → `work/out/boot.bin`, `analysis.json`.
   - `link_modules.py` places the game's own bundled modules after the eboot
     (libc.prx @0xb040000, libSceFios2.prx @0xb160000, libSceJobManager.prx @0xb2f0000), binds
     imports to their native exports where they exist (235 bindings), and leaves the rest
     (704 imports) to the host runtime → `boot-linked.bin` (format BBPROBE5), `link.json`.
   - `content_profile.py` writes AppContent metadata from param.sfo → `content.bin`.
2. **Loader** (`src/probe.c`): maps the image (it must sit low: 40-bit pointer packing), applies
   relocations, resolves each remaining import through `runtime_resolve()` (by NID) or points it at
   an `unresolved()` trap that prints `STOP: first unsupported PS4 import`, runs module inits
   (libc's `_init_env` etc.), then jumps to the game entry. Game code executes natively.
3. **Runtime** (`src/runtime_*.c`, C11): implements the PS4 OS functions the game calls: kernel
   memory (memfd pool, VMA table, write traps), threads/sync, files (app0/savedata mapping),
   Ajm (ATRAC9), audio out, pad, savedata, AppContent, net stubs.
4. **GPU library** (`libbbgpu.so` today): PM4 command processor, GCN→SPIR-V shader recompiler,
   buffer/texture caches, Vulkan presenter. Called through the seam in `gpu/shim/bbgpu.cpp` /
   `gpu/bbgpu.h`.

## Decisions (with reasons)

- **D1. bbport-style native execution, not PS4Recomp or emulation.** PS4Recomp is Apple-Silicon only;
  PS4 and PC share x86-64, so the game's code can run as-is. bbport proved this for Bloodborne.
- **D2. Keep bbport's CPU side (loader, prep scripts, C runtime); replace its GPU side.**
  bbport's `gpu/` is a ~218k-line rewrite of an older shadPS4 renderer; the user requires latest
  upstream shadPS4 + Dreams fork patches. Upstream video_core needs little from the rest of
  shadPS4 (≈7 MemoryManager methods, ≈31 config getters, interrupt/VideoOut types, a few clocks),
  so an adapter (~1.5–2.5k lines) modelled on `gpu/shim/core/*.h` is the cheaper path than porting
  every Dreams fork fix into bbport's diverged renderer. Analysis: `notes/bbport-reuse-plan.md` §4.
- **D3. Until vcore exists, link bbport's libbbgpu.so unchanged** (built in `ref/bbport`), so the
  CPU-side work (milestones 3–5) isn't blocked on the GPU rewrite.
- **D4. Link `libSceJobManager.prx` as a native module** like libc/Fios2. Dreams ships it in
  `sce_module/`; Bloodborne didn't.
- **D5. Offline only.** Np/PSN/Http report "not signed in / no network". Dreams' online endpoints
  (`https://prod.puprod.indreams.me/puserver.v1.*`, see https://archivingdreams.mgik.dev/) are out
  of scope.
- **D6. Peripherals absent by default.** PS VR (Hmd/VrTracker), Move, Camera, AudioIn report
  not-connected. Motion (DualShock gyro) matters: Dreams' default control scheme is "Motion";
  bbport's `runtime_pad/motion.c` is the starting point.
- **D7. Game check is title-based, not image-hash-based.** bbport pins one Bloodborne executable
  hash. Dreams versions will change (2.52 → 2.64), so don't hash-pin.

- **D8. Lantern Engine is the template for the GPU glue, not the base** (decided 2026-11-15).
  Lantern (github.com/Awakenchan/LanternEngine, sparse clone in `ref/lantern/engine`) is a bbport
  descendant that the user finds much smoother. Its `gpu/shadps4/` vendors upstream shadPS4
  `b4e7ae7` (2026-10-02) in upstream layout (`video_core` 221 files with ~52k changed lines vs
  upstream; `shader_recompiler` 130 files with ~3k changed lines; local edits marked `bbport:`).
  `gpu/shim/core/*.h` replaces the shadPS4 core headers video_core needs. Why not rebase onto
  Lantern: its renderer lacks the Dreams fork's DS_ORDERED_COUNT work, it's 337 upstream commits
  behind, its 52k-line video_core changes overlap the files the Dreams patches touch, and much of
  its speed-up is Bloodborne-specific. So vcore = `ref/shadps4` branch `dreams` sources + glue
  modelled on Lantern's shim; Lantern's generic performance work (parallel draw pipeline, pipeline
  compiles, memory model; see `ref/lantern/engine/docs/`) gets ported later (milestone 9).
  **License:** Lantern is GPL-3.0-or-later with additional terms (keep the credit "Lantern Engine
  by Awaken", mark modified versions, don't use the "Lantern Engine" name). Any file derived from
  Lantern code must say so in its header; the combined port then becomes GPL-3.0-or-later.
- **Reference result (2026-11-15):** the rebased shadPS4 *emulator* (`tools/run-shadps4-reference.sh`)
  boots the user's CUSA08010 v2.52 to the controller-introduction menus (user-confirmed visually).
  That's the behaviour vcore must reproduce natively.

## Facts about the Dreams binary (CUSA08010 v2.52)

- PIE eboot, 176 MiB image span (≈155 MB zero-initialised), no static TLS, no JIT, no
  `sceKernelLoadStartModule`/dlsym. `DT_TEXTREL` set, but no relocation touches code.
- Requires FW 8.50 (SDK 8.508.001), so the fpkg's "10.01 backport" label doesn't matter here.
- 812 imports from 63 libraries; 103 GnmDriver functions (libGnm is statically linked into the game).
- The game replaces malloc/new with its own allocator; requests 212 MiB of flexible memory.
- `sceSysmoduleLoadModule` is called from 33 sites for ~30 modules; all of them are also imported
  directly, so returning success is enough.
- `sceKernelIsNeoMode` (PS4 Pro path) is imported.
Full tables: `notes/dreams-imports.md`.
