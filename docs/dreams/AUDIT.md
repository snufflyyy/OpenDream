# Port audit — cleanliness, extendability, performance

Snapshot: 2026-11-16. Read-only audit of `port/`, its seam to `ref/shadps4`, and the project-root
`tools/`/`notes/`. No game, GPU or build run was used for the findings; the CPU suite is green.
This is a **snapshot**, not a status doc: current state lives in `HANDOFF.md`/`STATUS.md`.

## Addressed in the cleanup that followed this snapshot

- **S1** — the fault-bitmap GPU fix is now committed on `ref/shadps4` `vcore` (`0ca8d051`) and the
  backup bundle is refreshed.
- **S2/S3/S4** — `build-dreams.sh` records `out/gpu-revision.txt` (`branch`, `head`, tracked
  `dirty`, fault-manager blob) and warns when not a clean `vcore`; it also runs an advisory `nm`
  seam check. A compile-time `tests/test_abi_contract.cpp` pins the runtime↔vcore signatures.
- **T1/T2/T3/T4** — route-table and import-name sync tests, the loop-guard test, and the
  game-check test are in `tests/run-dreams-tests.sh`; optional skips are counted, and
  `DREAMS_TESTS_STRICT=1` fails on them.
- **L6/E5** — host-independent docs + `doc_lint.py`, `INDEX.md`, `CONVENTIONS.md`,
  `TEST_HARDWARE.md`, `README.DREAMS.md`; `preflight.sh` is the agent gate.
- **S8** — the runner now reaps a timed-out game by its private `--user` dir, so a survivor cannot
  hold the single-instance lock.
- **L1/L3** — Dreams banners on `port/README*.md`; stale backend/branch statements corrected.

Still open from the list below: behavioural scene gate (T5), measured submit/readback counters
(P2), capture-default cost (P1), and the cleanup of stale perf worktrees (B4, needs owner
approval).

## Executive summary

The port is in good shape: a small readable runtime, a genuinely thin vcore shim over upstream
shadPS4, honest evidence-oriented docs, and a better agent sandbox than most ports. The main risks
are **state and provenance**, not game code: the GPU fix behind the current performance baseline was
uncommitted in a nested repo, generated/config inputs straddle three repositories, two GPU backends
still coexist without an ownership marker, and some existing regression tests were not wired into
the single test entry point.

Highest-value small fixes: commit and bundle the fault-bitmap fix; record/assert the GPU revision in
`build-dreams.sh`; add ABI/forbidden-symbol checks for the vcore seam; wire the existing
loop-guard/game-check tests; sync checks for generated tables; and measured counters before more GPU
optimization.

## What is already good (preserve)

1. **A narrow, real GPU seam.** `gpu/bbgpu.h` declares 37 C entry points; `vcore/shim/` implements
   them in ~2k LOC while upstream `video_core`/`shader_recompiler` compile unmodified.
2. **Honest docs.** `ACCURACY.md`, `SCENE_TESTS.md`, `PERF.md`, `HANDOFF.md` separate measured facts
   from hypotheses and unverified claims. Do not water this down.
3. **Real guard tests.** Loop guard, indirect clamp, fault buffer, backing pages have unit tests.
4. **Agent run discipline.** `tools/agent-run.sh` (headless, sandboxed, one GPU instance) plus
   `capture_sheet.py` and `clean_work.py`.
5. **Route matching by code hash / DOC signature**, not game-version hashes.

## Findings

Severity: blocker / high / medium / low. Effort: S / M / L.
Paths are repository-relative. `[verified]` = inspected, `[hypothesis]` = risk analysis.

### State and provenance

| ID | Finding | Sev | Effort | Action |
|---|---|---|---|---|
| S1 | The GPU fix behind the ~94 FPS baseline was uncommitted in `ref/shadps4` (`fault_manager.cpp`); a clean checkout silently restores the regressing build | High | S | Commit on `vcore`, refresh `backups/shadps4-dreams-vcore.bundle` |
| S2 | `build-dreams.sh` never records/asserts the shadPS4 branch, HEAD or dirty state | High | S | Write `out/gpu-revision.txt`, print it, warn on dirty, `DREAMS_REQUIRE_CLEAN_GPU=1` to fail |
| S3 | Header shadowing (`vcore/shim/include/**` before `${SRC}`) is the fragile part of the seam; no check that emulator code did not leak in | High | S | Advisory `nm` forbidden-symbol check after linking vcore |
| S4 | Runtime↔vcore C ABI has no compile-time contract (`runtime_api.h` says signatures were "copied from port/src") | High | S | Compile-time `test_abi_contract.cpp` (pointer type-checks + struct asserts) |
| S5 | Generated/config inputs live outside `port/`: vcore config and `import_names_dreams.inc` come from root `notes/`; route table is generated into `ref/shadps4` | Medium | S | Document a repo map + source-of-truth; prefer a `port/config/` template with a notes fallback |
| S6 | The fork switch set is duplicated in `run-dreams.sh` and `tools/run-shadps4-reference.sh` | Medium | S | One sourced env module + a test that both agree |
| S7 | `ref/shadps4/i-didnt-check-my-work.md` is a generated marker required by that tree's AGENTS.md, not stray noise | Low | — | Keep; do not delete (contrary to a naive `git status` cleanup) |
| S8 | `tools/agent-run.sh`'s timeout could kill the wrapper while the game process survived and held the single-instance `flock`, blocking later runs | Medium | S | **Done**: after the timeout, kill only a `dreams-probe` whose `--user` is this run's private dir (never the user's run) |
| S9 | MangoHud's Vulkan layer could leave the window on an older frame while the game rendered the newest one (frame capture showed the imp/doodles; the screen did not) | Medium | S | **Done**: `run-dreams.sh` disables MangoHud by default (`DREAMS_MANGOHUD=1` keeps it), preserving other `LD_PRELOAD` entries; `tests/test_run_mangohud.py` |

### Identity, legacy, and docs drift

| ID | Finding | Sev | Effort | Action |
|---|---|---|---|---|
| L1 | `port/README.md`/`README.ru.md` are bbport/Bloodborne; a `port`-only reader sees the wrong project | High | S | `port/README.DREAMS.md` + a short banner/link; keep upstream text for provenance |
| L2 | `port/gpu/` is 952 tracked files / ~301k LOC; **not dead** for bbport but not in the Dreams default | Medium | S | Mark "bbport legacy/reference, not built for Dreams"; do not delete while `DREAMS_GPU=bbport` is supported |
| L3 | Stale statements: `ARCHITECTURE.md` "libbbgpu.so today"/"until vcore exists"; `VCORE.md`/`GPU_HANG.md` "not merged"; `VERSIONS.md` "branch docmatch" | Medium | S | Correct them; historical branch names only in `WORKLOG.md` |
| L4 | `docs/dreams/logs-gpu-run1.txt` is a tracked stale run log | Low | S | Keep only an excerpt in docs; move full logs to untracked `work/` |
| L5 | bbport-only CLI and `BB_LOW_INFO_TEST` remain in `src/probe.c` (vcore stubs return 1) | Low | S | Leave for `DREAMS_GPU=bbport` compatibility; mark as bbport-only in a comment, do not delete blindly |
| L6 | Normative docs hard-coded one host (`/home/<user>`, distro, CPU/GPU model) | High | S | `TEST_HARDWARE.md` + `doc_lint.py` (now enforced) |

### Tests

| ID | Finding | Sev | Effort | Action |
|---|---|---|---|---|
| T1 | Route table generated into `ref/shadps4` had no sync check | Medium | S | **Done**: `tests/test_route_table.py` (verified identical, 26 routes; skips without the pack) |
| T2 | `src/import_names_dreams.inc` generated from root `notes/`; no sync test; no resolver regression test | Medium | M | Generator `--check` + a GPU-free NID-resolve test |
| T3 | `tools/loop_guard_test.cpp` existed but was not in the suite | Medium | S | Wire into the lavapipe conditional block |
| T4 | `scripts/dreams_game_check.py` + its test existed but were not wired into `run-dreams.sh`/the suite | Medium | S | Call the check; keep `BB_SKIP_GAME_CHECK` as an explicit override |
| T5 | No behavioural gate for the accuracy requirement (30 gameplay updates/s, no guard trips) | Medium | L | Bounded scene report + one scripted scenario; mark blocked/unverified honestly |
| T6 | Optional test skips still print "passed" with less coverage | Low | S | Print a skip summary; `DREAMS_TESTS_STRICT=1` fails on skips |

### Performance

| ID | Finding | Sev | Effort | Action |
|---|---|---|---|---|
| P1 | `DREAMS_CAPTURE` defaults to 1000 ms for user runs; capture adds an unconditional GPU readback to the path being optimized | Medium | S | Default off (or 5 s) for user runs; keep 500 ms for agents — measure the cost |
| P2 | No per-frame submit/readback/allocation counters with timestamps | Medium | M | Env-gated counters before changing sync/alloc behavior |
| P3 | `GpuComm` is the ~90–100 fps ceiling; `Main` stalls in Precise fault readbacks | Known | — | Documented; no speculative fix without P2 |
| P4 | A/B captures are compared while animation state differs | Low | M | Record a fixed scene/camera/save pair for comparisons |
| P5 | `fps_sample.py` is v2.52-only (signatures + offsets) | Low | S | Add a v2.64 signature when the user has the build |

### Extendability for agents

| ID | Finding | Sev | Effort | Action |
|---|---|---|---|---|
| E1 | No single "start here" for a `port`-only checkout | Medium | S | `README.DREAMS.md` + `docs/dreams/INDEX.md` |
| E2 | Documentation drift (merged branches described as pending) is the main agent trap | Medium | S | Branch-status lines; `WORKLOG.md` owns history |
| E3 | No repo map for the root/`port`/`ref`/`game`/`work` split | Low | S | One table in `AI_DEVELOPMENT.md`/`ARCHITECTURE.md` |
| E4 | No evidence-artifact naming convention | Low | S | `work/<topic>/<date>-<what>.{md,json,log}` |
| E5 | No `CONVENTIONS.md`; ownership, one-writer, generated-vs-tracked and test gates were implicit | High | S | **Done**: `CONVENTIONS.md` + `INDEX.md` |

### Top 5 coupling risks

1. Uncommitted GPU fix + no revision assertion → the tested binary is not reproducible (S1, S2).
2. Header shadowing against an in-place upstream tree (S3).
3. Unchecked runtime↔vcore C ABI (S4).
4. Three-repository source-of-truth split (S5).
5. Legacy backend coexisting without an ownership marker (L2, S6).

## Ranked actions

1. Commit the fault-bitmap fix on `ref/shadps4` `vcore`; refresh the backup bundle. (S1, S)
2. Record/assert GPU revision + dirty state in `build-dreams.sh` and the run log. (S2, S)
3. Add the vcore forbidden-symbol check and the runtime ABI contract test. (S3/S4, S)
4. Wire `dreams_game_check.py` into the run and the suite. (T4, S)
5. Keep the new sync/regression gates: route table (**done**), import names, loop guard, config. (T1–T3, S/M)
6. Make capture opt-in for user runs; measure the cost. (P1, S)
7. Add env-gated submit/readback/allocation counters. (P2, S/M)
8. Correct stale docs and add the repo map + Dreams entry point. (L1/L3/E1–E3, S)
9. Mark `gpu/` and bbport-only CLI clearly; do not delete while `DREAMS_GPU=bbport` works. (L2/L5, S)
10. Build one bounded scene-regression report for the accuracy requirement. (T5, L)

## Guardrails (keep the project clean/extendable/performant)

- **G1** `ref/shadps4` must be committed before performance/visual evidence is accepted; builds
  record `branch@HEAD (dirty?)` and the fault-manager blob.
- **G2** Never hand-edit a generated file (`dreams_route_table.inc`, `import_names_dreams.inc`);
  regenerate and run the sync check.
- **G3** Every completeness/accuracy claim names game version, save origin, scene, timing mode,
  build revision, and whether a guard trip/clamp occurred (`ACCURACY.md`).
- **G4** A new shadowed header or `// vcore:` edit states why, which upstream file it shadows/edits,
  and gets a boot check after the next rebase.
- **G5** Changes to `runtime_api.h`/`runtime.h`/`bbgpu.h` pass the ABI test and are reviewed as a
  shared-contract change.
- **G6** One writer per shared file; one GPU lease at a time; preserve the lock and agent isolation.
- **G7** Every fix gets a CPU regression test when practical; every guard/route change gets a
  lavapipe/unit test; every tool gets a `--check` mode.
- **G8** `tests/run-dreams-tests.sh` is the single entry point; add new tests in the same change.
  Conditional skips are summarized; `DREAMS_TESTS_STRICT=1` fails on them.
- **G9** Measure before/after on the same scene/save; report p50/p99 and gameplay updates/s, not
  only FPS. No performance patch without a control run.
- **G10** Never weaken Precise readbacks, routes, synchronization or guards for FPS; a guard
  trip/clamp is a correctness failure.
- **G11** `HANDOFF.md` is the only "current state" doc; `WORKLOG.md` is append-only history; merged
  branches get a status line, not a new doc.
- **G12** Run `port/tools/preflight.sh` before handoff/commit.

## Verify

```sh
cd port
./tools/preflight.sh
tests/run-dreams-tests.sh
```
