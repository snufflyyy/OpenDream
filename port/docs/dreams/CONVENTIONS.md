# Conventions for agents (OpenDream docs and code)

Rules that keep the documentation and tooling usable by agents that work on different machines
and in different sessions. `INDEX.md` maps the documents; `TEST_HARDWARE.md` holds the one
machine description.

Working directory: `$REPO` is your OpenDream checkout. The port is `$REPO/port`, project-root
tools are `$REPO/tools/`, port tools are `$REPO/port/tools/`.

## Documentation freshness

- A normative doc describes the code and commands as they are **now**. When a change makes a doc
  wrong, update the doc in the same task; do not leave the correction to the next session.
- Historical docs are append-only or frozen: `WORKLOG.md` (append entries), `HANDOFF.md` (written
  per session), `logs-gpu-run1.txt` (raw evidence). Do not rewrite them to stay current.
- Update `STATUS.md` when a milestone, a blocker or a measured baseline changes.
- Add a row to `IMPORTS_LOG.md` when an import is implemented or changed. Regenerate `STUBS.md`
  (`python3 -I tools/stub_report.py <run log> --md port/docs/dreams/STUBS.md`) when the fallback
  set changes.
- Add new documents to the `INDEX.md` table (purpose, when to read, kind, last-updated date). The
  doc lint fails when a normative doc is missing from the index.

## Host-independent wording (normative docs)

- No absolute user paths. Write `$REPO` for the checkout root and define it once per document
  (`REPO=/path/to/OpenDream`); otherwise use paths relative to the cwd the doc states. The
  only allowed home-relative form is a portable one such as `~/.local/bin/clang`.
- Every document states its working directory near the top and uses it consistently
  (`$REPO`, `$REPO/port`, `$REPO/port/vcore`, ...).
- No host hardware as a requirement. Say "the reference machine (`TEST_HARDWARE.md`)" when a
  measurement depends on it, and state the real requirement generically (Linux x86-64, a Vulkan
  GPU, enough VRAM, 16 GB+ RAM recommended).
- No distribution-specific instructions without a label. Give the generic step, then a
  "Fedora example" (or other distro) block for package names and file paths.
- Historical logs may keep machine specifics. `WORKLOG.md` and `HANDOFF.md` are exempt.
- Report numbers from another machine with that machine's details; never present them as the
  reference baseline (`TEST_HARDWARE.md`, "Reporting results from a different machine").

### Normative docs must pass doc_lint

- Normative docs are the ones listed in `INDEX.md` except `HANDOFF.md`, `WORKLOG.md` and raw log
  files. The explicit allowlist lives in `port/tools/doc_lint.py`.
- `python3 -I port/tools/doc_lint.py` must exit 0. It reports `file:line:rule` and exits 2 on
  violations. Rules: absolute home paths (the checkout written as an absolute user path), the
  host user's name, hardcoded CPU/GPU models of the reference machine, its distribution-version
  and desktop-compositor names, and a normative doc missing from `INDEX.md`. The exact patterns
  are in the script.
- A line that genuinely must name the machine (a driver limit, a distro file path) may be exempted
  with the marker `doc-lint:allow` on that line, or inside a block delimited by
  `doc-lint:allow-begin` / `doc-lint:allow-end`. Use it rarely and preferably only in
  `TEST_HARDWARE.md`, which is exempt as a whole.
- The lint is deterministic and stdlib-only; its fixtures are `port/tests/test_doc_lint.py`.

## Ownership and edits

- One writer per file. The orchestrator assigns files; do not edit a file another worker owns,
  and do not resolve a conflict by overwriting: keep both versions and reconcile first.
- Stay inside the assigned scope. Read anything; edit only what was assigned.
- Before editing, inspect the working trees (`$REPO`, `$REPO/port`, `$REPO/ref/shadps4`): the
  project carries uncommitted work and local branches. Never reset or discard it.
- Nested repositories: `port/` is its own git repo, `ref/shadps4` carries local branches
  (`dreams`, `vcore`, `docmatch`, `loopguard`). Backups for them are in `$REPO/backups/`.

## Generated vs tracked files

Tracked, generated, guarded — regenerate, never hand-edit:

| File | Generator | Guard |
|---|---|---|
| `ref/shadps4/src/shader_recompiler/dreams_route_table.inc` | `port/tools/dreams_shader_routes.py emit` (source of truth: `port/tools/dreams_routes.json` + a game shader pack) | `port/tests/test_route_table.py` (registered in `tests/run-dreams-tests.sh`; skips when the pack or the shadPS4 tree is absent) |
| `port/src/import_names_dreams.inc` | `tools/gen_import_names.py` | build-time use |
| `port/docs/dreams/STUBS.md` | `tools/stub_report.py` (from a run log) | none: review the diff |
| `notes/dreams-imports.md`, `notes/dreams-imports.json` | `tools/ps4_imports.py`, `tools/imports_report.py` | none: review the diff |

Never tracked: `game/` (the user's own game data), `work/` (run output, saves, captures, crash
dumps), build output (`port/out/`, `port/vcore/build/`), Python caches, generated SPIR-V, local
patch backups (`*.orig`), and any API key or service credential. `port/.gitignore` covers these;
keep it deduplicated and do not re-add build output patterns.

## Verification gates

Run the cheapest gate that covers the change, and report what you ran:

1. `port/tools/preflight.sh` — seconds, no GPU, no game: shell syntax, `py_compile` of
   `tools/ scripts/ tests/`, `doc_lint`, shader-route-table sync, no absolute home paths,
   `git diff --check`. **Run it before handoff or commit.**
2. Focused unit tests: `python3 -I port/tests/test_<x>.py`, or the C tests in
   `port/tests/` (see `tests/run-dreams-tests.sh` for build lines).
3. `port/tests/run-dreams-tests.sh` — the primary suite; add a new test here when it is cheap and
   deterministic.
4. `python3 -I port/tools/doc_lint.py` — required for a normative-doc change.
5. Game runs only through `$REPO/tools/agent-run.sh`, under a GPU run lease
   (`AI_DEVELOPMENT.md`), with private saves (`AGENT_RUNS.md`). A boot or a menu screen is not a
   gameplay or editor pass (`SCENE_TESTS.md`, `ACCURACY.md`).

A change is not verified because it builds or "looks correct". State what was tested, on which
machine, and what remains unverified.

## Commit and uncommitted-work policy

- Leave changes uncommitted unless the user or the orchestrator explicitly asks for a commit.
  Sessions end with a written handoff (`HANDOFF.md`) recording the exact resume point.
- If you do commit (only when asked): one coherent change, no game files, no `work/` output, no
  build output, no credentials, no unrelated reformatting. Run the preflight first and keep
  `git -C port diff --check` clean.
- Do not rewrite history, force-push, or move the local shadPS4 branches. Re-create
  `backups/shadps4-dreams-vcore.bundle` after new commits on `dreams`/`vcore`
  (`$REPO/AGENTS.md`).

## How to add things

**An import.** Implement it by NID in the right `port/src/runtime_*` file, with the semantics
copied from the shadPS4 reference (`ref/shadps4/src/core/libraries/...`) and that file cited in a
comment. Add a unit test in `port/tests/` when the behaviour is testable without the game. Add one
row to `IMPORTS_LOG.md` (NID, name, library, where, semantics/reference, date). If the import only
needs "not connected / not signed in" semantics, use the documented PS4 error codes, not a crash.
Never let an unimplemented import crash the game (`RUNNING.md`, `STUBS.md`).

**A shader route or a new game version.** Follow `VERSIONS.md` §5: `dreams_shader_routes.py check`
for every pack, add the route to `port/tools/dreams_routes.json` (source of truth), then
`emit` the table. Run `port/tests/test_route_table.py`; it fails when the tracked
`dreams_route_table.inc` no longer matches the generator output. Gate any route change on
orchestrator approval (`AI_DEVELOPMENT.md`: shader translation and ordered-count behaviour).

**A scene test.** Read `SCENE_TESTS.md` first: the scenario needs a real scene entry checkpoint,
recorded setup and input, explicit behavioural checkpoints, private saves, and separate
`passed`/`failed`/`blocked`/`unverified` outcomes. Run through `tools/agent-run.sh`; a capture
alone never proves physics, logic, progression or save integrity. Add the scenario name and its
evidence to the harness report, not only to a worklog entry.

**A document.** Add it under `port/docs/dreams/`, state its cwd, keep it host-independent, add the
`INDEX.md` row, and run `doc_lint.py`. Historical/dated material belongs in `WORKLOG.md` instead.

## Performance changes

- Measure a control run and a candidate run on the **same scene, save and timing mode**; report
  render FPS (p50/p99) **and** gameplay updates/s. A change that moves gameplay cadence away from
  30 updates/s is a correctness regression, not an optimisation (`ACCURACY.md`).
- Never weaken Precise readbacks, shader routes, synchronization or hang guards to gain FPS. A
  guard trip or an indirect clamp is a failure to investigate (`GPU_HANG.md`).
- Follow `PERF.md`: use `tools/fps_sample.py` and `VCORE_FRAME_STATS`, and say which threads were
  sampled. A capture-only run is not an FPS measurement (capture adds readbacks).
- Do not claim a speedup from one paired comparison without stating the scene and the machine
  (`TEST_HARDWARE.md`).
