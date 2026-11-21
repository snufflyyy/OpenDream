# OpenDream — start here

**OpenDream** — a native PC port of Dreams (CUSA08010) for the PS4.

Native Linux x86-64 port of **Dreams** (Media Molecule, PS4) in the style of **bbport**
(deadinside28's native Bloodborne port): the game's own x86-64 code runs directly on the host CPU,
a C runtime replaces the PS4 system libraries, and a Vulkan renderer derived from **shadPS4** runs
the GPU side. No CPU emulation, no JIT, no static recompilation.

Read in this order:
1. This file (layout, rules, current state in one paragraph).
2. `port/docs/dreams/INDEX.md` — the full doc map (what to read, normative vs historical).
3. `port/docs/dreams/CONVENTIONS.md` — agent rules for docs, ownership, tests and commits.
4. `port/docs/dreams/HANDOFF.md` — latest session changes, test evidence, unfinished scene harness, exact resume point.
5. `port/docs/dreams/STATUS.md` — milestone checklist, what works, the exact current blocker.
6. `port/docs/dreams/ARCHITECTURE.md` — how the pieces fit and *why* each decision was made.
7. `port/docs/dreams/BUILD.md` and `port/docs/dreams/RUNNING.md` — reproducible commands.
8. `port/docs/dreams/WORKLOG.md` — dated log of what was done and learned (append to it).
9. `port/docs/dreams/IMPORTS_LOG.md` (what we implemented) and `STUBS.md` (what still returns 0).

The normative docs are host-independent. The current test machine is recorded only in
`port/docs/dreams/TEST_HARDWARE.md`; it is a reference, not a requirement. `port/tools/doc_lint.py`
enforces this (no absolute home paths, no host hardware claims outside `TEST_HARDWARE.md`).

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

- Primary goal: **Dreams boots natively on a Linux x86-64 PC with a Vulkan GPU**. The machine used
  for current measurements is recorded in `port/docs/dreams/TEST_HARDWARE.md`; do not treat it as a
  requirement or hard-code its hardware.
- Priority feature: the **Dreams editor / creation tools**. **Offline only** — no PSN/server work.
- **Accuracy is a hard requirement** (user, 2026-11-16): Dreams is an engine, so gameplay,
  physics, logic, sculpts, saves and creation tools must behave as on an original PS4.
  Performance changes must preserve those semantics. High render FPS is not permission to
  change simulation speed. Treat hang guards as safety nets, not accuracy fixes.
- **Regression coverage:** test real scenes in every locally available built-in Mm game **and the
  editor**, with profiling, behavioral checks, fixes, and reruns. First gameplay target:
  **Please Hug Me** (original movement/progression report; the user now reports it working).
  Keep the harness easy for future agents: one documented command, private save setup, stable
  machine-readable reports, and separate pass/fail/blocked/unverified outcomes. See `SCENE_TESTS.md`.
- GPU base must be **latest official shadPS4** + **all patches from the Dreams fork**
  (https://github.com/JesusDaBest/Dreams-ShadPS4). Not bbport's older diverged renderer copy.
- The user's copy today: **CUSA08010 v2.52** (US, CONTENT_ID UP9000-CUSA08010; fpkg). The user plans to move to **v2.64** soon —
  that is the version the Dreams fork targets (its fixes are gated on CUSA04301 + 2.64 shader
  hashes). Avoid hard-coding anything specific to the 2.52 executable.
- **Current work priority:** improve rendering speed and visual accuracy. Preserve engine semantics,
  Precise readbacks, shader routes, and GPU guards. The user now reports Please Hug Me working
  correctly in manual testing; the earlier cloud screen is not a confirmed current blocker.
  Automated scene coverage and original-PS4 parity remain unverified. Read `HANDOFF.md` first.
- Keep docs detailed and current for future agents (this file + `port/docs/dreams/`). New/changed
  normative docs must pass `python3 -I port/tools/doc_lint.py` and must not hard-code a host, user,
  distro or GPU model. Put machine facts in `TEST_HARDWARE.md` and historical facts in `WORKLOG.md`.
- Subagent budget set by the user: at most 3 DeepSeek agents in parallel. **DeepSeek does most implementation,
  debugging, tests, and tooling to reduce cost.** The main GPT Pi assistant supervises, advises,
  assigns new tasks, reviews evidence, and accepts patches. It does not reserve difficult fixes
  for itself by default. Read `port/docs/dreams/AI_DEVELOPMENT.md` before delegation for routes,
  ownership, review gates, and the exclusive GPU run lease. For the current session, the user
  also authorizes at most **two concurrent GPT Terra workers** (`openai-codex/gpt-6.1-terra`).
  The same ownership, review and single-GPU rules apply.

## Directory layout

| Path | What | Tracked? |
|---|---|---|
| `port/` | **The Dreams port** (forked from bbport 0.7 `0fc2921`); tracked in this repository, not a separate repo | yes |
| `port/docs/dreams/` | Dreams-specific docs (bbport's own docs in `port/docs/` are upstream, mostly Russian) | yes |
| `game/CUSA08010/` | Extracted game (13 GB, 1,591 PFS files + sce_sys). The user's own copy — never distribute | no |
| `game/CUSA08010.extract.json` | Per-file SHA-256 manifest of the extraction | no |
| `work/` | Run outputs: `work/out/` (boot-linked.bin, link.json, content.bin), `work/user/` (saves), run logs | no |
| `ref/bbport/` | Legacy/reference Bloodborne port (one local build fix, see BUILD.md); its `out/gpu/libbbgpu.so` is used only with `DREAMS_GPU=bbport` | upstream clone |
| `ref/shadps4/` | Pinned build sources from `port/tools/setup-build.sh`, or the maintainer's existing shadPS4 clone with local renderer branches | separate checkout |
| `ref/dreams-fork/` | Blobless clone of the Dreams fork (patch files + builds; do NOT full-checkout, it is GBs of .exe) | upstream clone |
| `ref/vulkan-headers/` | Latest Khronos Vulkan-Headers (distro packages can be too old for bbport) | upstream clone |
| `cmake/` | Build-environment shims (FindZydis.cmake, magic_enum include compat); distro-independent | — |
| `tools/` | Our scripts: `pkg_extract.py` (fpkg extractor), `ps4_imports.py` (import/coverage analysis), ... | — |
| `notes/` | Research reports: `dreams-imports.md/.json`, `bbport-reuse-plan.md`, `shadps4-dreams-rebase.md`, tutorial-skip save | — |

## Current state (keep this paragraph short and current)

**Dreams boots natively** (port sources in this repository, renderer `vcore` 7d25345b).
The loader/runtime execute guest x86-64 code; vcore supplies the shadPS4/Dreams-fork GPU path.
Boot/menu/homespace, input, AvPlayer, offline services, shader routes and hang guards work.
Fault-bitmap initialization and shared backing prevent the earlier progressive slowdown;
previous menu measurements reach ~94 render FPS with gameplay still at 30 Hz (`PERF.md`).
**Current audit changes are uncommitted:** bounded SFO parsing, content-based preparation cache,
pre-save game validation, race-resistant cleanup and empty-save create/truncate persistence.
CPU regressions do not prove gameplay/editor parity. Please Hug Me is user-reported working;
automated scenario and original-PS4 comparison coverage remain unverified.
Read `HANDOFF.md` and `AUDIT.md` for evidence, remaining thread/savedata/renderer risks, and resume
instructions. Rendering performance/accuracy remains the priority; preserve Precise readbacks,
routes, synchronization, guards and simulation cadence.

**GPU hang risk:** a native run once hung the GPU and amdgpu's full reset killed the desktop
(2026-11-15, see WORKLOG incident). `run-dreams.sh` allows one instance at a time, logs every run
to `work/logs/` and captures frames to `work/captures/`. Commit/save before long GPU runs, and never
start a run while the user may be playing (the lock will refuse it anyway).

**Fresh builds:** see `port/docs/dreams/BUILD.md`. `port/tools/setup-build.sh` reconstructs the
pinned source overlay from public dependencies without the maintainer's local branches.

**Maintainer history:** the `vcore` and `dreams` branches exist only in the maintainer's clone.
Don't replace that clone with the public build checkout. Backup: `backups/shadps4-dreams-vcore.bundle`
(commits `main..dreams` + `main..vcore`, prerequisite upstream `f3687cc4`; restore with
`git fetch backups/shadps4-dreams-vcore.bundle dreams:dreams vcore:vcore` in a shadPS4 clone).
Re-create the bundle after new commits on those branches. The bundle matches the current branch
heads. The fault-bitmap fix is committed (`0ca8d051`); its older patch backup remains at
`backups/shadps4-vcore-fault-bitmap-init.patch`.

## Rules for agents

- **Run the game only via `tools/agent-run.sh`** (headless, sandboxed, user-priority; see
  `port/docs/dreams/AGENT_RUNS.md`). Never open windows, play audio or touch the user's devices:
  the user works on this PC while agents test.

- Don't commit game files, extracted data, or anything from `game/` / `work/`.
- Implement PS4 functions by NID; use shadPS4 (`ref/shadps4/src/core/libraries/`) as the reference
  for semantics, cite the file in a comment, and log each one in `port/docs/dreams/IMPORTS_LOG.md`.
- Peripherals Dreams can live without offline (PS VR/Hmd, Move, Camera, PSN/Np, sharing) should
  report "not connected / not signed in" with the documented PS4 error codes, never crash.
- Disk/temp space is limited and host-dependent. Keep large data under the project tree, check
  free space before builds/extractions, and do not assume `/tmp` is large (it may be a RAM tmpfs).
