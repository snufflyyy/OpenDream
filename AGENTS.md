# Dreams-Recomp — start here

Native Linux x86-64 port of **Dreams** (Media Molecule, PS4) in the style of **bbport**
(deadinside28's native Bloodborne port): the game's own x86-64 code runs directly on the host CPU,
a C runtime replaces the PS4 system libraries, and a Vulkan renderer derived from **shadPS4** runs
the GPU side. No CPU emulation, no JIT, no static recompilation.

Read in this order:
1. This file (layout, rules, current state in one paragraph).
2. `port/docs/dreams/STATUS.md` — milestone checklist, what works, the exact current blocker.
3. `port/docs/dreams/ARCHITECTURE.md` — how the pieces fit and *why* each decision was made.
4. `port/docs/dreams/BUILD.md` and `port/docs/dreams/RUNNING.md` — reproducible commands.
5. `port/docs/dreams/WORKLOG.md` — dated log of what was done and learned (append to it).
6. `port/docs/dreams/IMPORTS_LOG.md` (what we implemented) and `STUBS.md` (what still returns 0).

Tools (`tools/`, run with `python3 -I`): `pkg_extract.py` (fpkg → folder), `ps4_imports.py` +
`imports_report.py` (static import/coverage analysis → `notes/dreams-imports.*`),
`gen_import_names.py` (→ `port/src/import_names_dreams.inc`), `unresolved_report.py` (group a
`BB_LIST_UNRESOLVED=1` run log by library), `stub_report.py` (run log → `STUBS.md`),
`find_import_calls.py` (call sites of an import in the eboot), `run-shadps4-reference.sh` (runs the
rebased shadPS4 *emulator* on the game with the Dreams fork's config: the GPU reference),
`capture_sheet.py` (review a `DREAMS_CAPTURE=1` run: timeline + contact sheets, see
`port/docs/dreams/CAPTURE.md`). `install_game.sh` (pkg → verified game folder), `collect_crash.sh` (GPU-hang evidence → `work/crash/`), `clean_work.py` (prune `work/`, dry run by default), `dreams_shader_routes.py` (port/tools: route
table for new game versions, VERSIONS.md), `spirv_val.c`/`loop_guard_test.cpp`/`fps_sample.py` (port/tools, GPU_HANG.md). **To see what a native run shows, use DREAMS_CAPTURE.**

## Goal and user requirements (do not drift from these)

- Primary goal: **Dreams boots natively on the user's PC** (Fedora 44, x86-64, AMD RX 7800 XT RADV,
  16 cores, 31 GB RAM).
- Priority feature: the **Dreams editor / creation tools**. **Offline only** — no PSN/server work.
- GPU base must be **latest official shadPS4** + **all patches from the Dreams fork**
  (https://github.com/JesusDaBest/Dreams-ShadPS4). Not bbport's older diverged renderer copy.
- The user's copy today: **CUSA08010 v2.52** (US, CONTENT_ID UP9000-CUSA08010; fpkg). The user plans to move to **v2.64** soon —
  that is the version the Dreams fork targets (its fixes are gated on CUSA04301 + 2.64 shader
  hashes). Avoid hard-coding anything specific to the 2.52 executable.
- Keep docs detailed and current for future agents (this file + `port/docs/dreams/`).
- Subagent budget set by the user: at most 2 in parallel.

## Directory layout

| Path | What | Tracked? |
|---|---|---|
| `port/` | **The Dreams port** (git repo, branch `dreams`, forked from bbport 0.7 `0fc2921`) | own git |
| `port/docs/dreams/` | Dreams-specific docs (bbport's own docs in `port/docs/` are upstream, mostly Russian) | yes |
| `game/CUSA08010/` | Extracted game (13 GB, 1,591 PFS files + sce_sys). The user's own copy — never distribute | no |
| `game/CUSA08010.extract.json` | Per-file SHA-256 manifest of the extraction | no |
| `work/` | Run outputs: `work/out/` (boot-linked.bin, link.json, content.bin), `work/user/` (saves), run logs | no |
| `ref/bbport/` | Pristine-ish clone of deadinside28/bloodborne_pc (one local build fix, see BUILD.md); its `out/gpu/libbbgpu.so` is linked by the port for now | upstream clone |
| `ref/shadps4/` | Official shadPS4 clone; branch `dreams` = latest upstream + Dreams fork patches (see `notes/shadps4-dreams-rebase.md`) | upstream clone |
| `ref/dreams-fork/` | Blobless clone of the Dreams fork (patch files + builds; do NOT full-checkout, it is GBs of .exe) | upstream clone |
| `ref/vulkan-headers/` | Latest Khronos Vulkan-Headers (Fedora's 1.4.341 is too old for bbport) | upstream clone |
| `cmake/` | Build-environment shims for Fedora (FindZydis.cmake, magic_enum include compat) | — |
| `tools/` | Our scripts: `pkg_extract.py` (fpkg extractor), `ps4_imports.py` (import/coverage analysis), ... | — |
| `notes/` | Research reports: `dreams-imports.md/.json`, `bbport-reuse-plan.md`, `shadps4-dreams-rebase.md`, tutorial-skip save | — |

## Current state (keep this paragraph short and current)

**Dreams boots and plays natively** (2026-11-15, port `dreams` e741924, shadPS4 `vcore` 17e99fee).
Loader + C runtime run the game's own code; GPU library **vcore** = latest shadPS4 + Dreams fork
patches. Done: boot to main menu/homespace, DS4 motion + keyboard/mouse (INPUT.md), AvPlayer, IME,
offline NP/sign-in/sockets, intro skip and 60 Hz/high-fps switches (FPS.md), Mailbox presentation,
**fork shader routes matched by code on any game version** (VERSIONS.md: fixes the holes/blocks in
objects on 2.52), **GPU hang guard** (loop guard + indirect clamps, GPU_HANG.md: the intro hang scene
now survives; root cause lead = indirect draws with 1,949,696 instances), frame stats in every run
(PERF.md). Next: find the writer of that bad instance count; performance (readback stalls, submit
batching, allocation churn: PERF.md); editor testing; v2.64 when the user has it.

**GPU hang risk:** a native run once hung the GPU and amdgpu's full reset killed the desktop
(2026-11-15, see WORKLOG incident). `run-dreams.sh` allows one instance at a time, logs every run
to `work/logs/` and captures frames to `work/captures/`. Commit/save before long GPU runs, and never
start a run while the user may be playing (the lock will refuse it anyway).

**Fragile dependency:** `ref/shadps4` branch `vcore` (and `dreams`) exist only as local commits in
this clone. Don't delete/re-clone `ref/shadps4`. Backup: `backups/shadps4-dreams-vcore.bundle`
(commits `main..dreams` + `main..vcore`, prerequisite upstream `f3687cc4`; restore with
`git fetch backups/shadps4-dreams-vcore.bundle dreams:dreams vcore:vcore` in a shadPS4 clone).
Re-create the bundle after new commits on those branches.

## Rules for agents

- **Run the game only via `tools/agent-run.sh`** (headless, sandboxed, user-priority; see
  `port/docs/dreams/AGENT_RUNS.md`). Never open windows, play audio or touch the user's devices:
  the user works on this PC while agents test.

- Don't commit game files, extracted data, or anything from `game/` / `work/`.
- Implement PS4 functions by NID; use shadPS4 (`ref/shadps4/src/core/libraries/`) as the reference
  for semantics, cite the file in a comment, and log each one in `port/docs/dreams/IMPORTS_LOG.md`.
- Peripherals Dreams can live without offline (PS VR/Hmd, Move, Camera, PSN/Np, sharing) should
  report "not connected / not signed in" with the documented PS4 error codes, never crash.
- `/tmp` is a 16 GB RAM tmpfs and `/home` has limited free space (~29 GB at last check): keep big
  data under the project and check `df -h /home` before large operations.
