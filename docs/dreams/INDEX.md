# Docs index (Dreams-Recomp, `port/docs/dreams/`)

Entry point for agents. Read the rows you need; do not read the whole directory.

Working directory: `$REPO` is your Dreams-Recomp checkout. Docs state their own cwd; project-root
tools are `$REPO/tools/`, port tools are `$REPO/port/tools/`.

Two kinds of document:

- **Normative** — current rules, facts, commands and measurements. Kept host-independent and
  must pass `python3 -I port/tools/doc_lint.py` (`CONVENTIONS.md`).
- **Historical** — dated logs and session handoffs. They may name the machine, paths and
  branches as they were at the time. Never edited to stay current, never cited as a requirement.

| Document | Purpose | Read it when | Kind | Last updated |
|---|---|---|---|---|
| `INDEX.md` | This map | Starting any task | Normative | 2026-11-16 |
| `CONVENTIONS.md` | Agent rules: freshness, path/hardware-neutral phrasing, ownership, gates, how to add an import/route/scene test/doc | Before editing or adding a doc, test or route | Normative | 2026-11-16 |
| `TEST_HARDWARE.md` | The current reference test machine and what is a real requirement vs a convenience; how to report results from another host | A number or a hardware claim needs context; testing on a different machine | Normative (hardware facts) | 2026-11-16 |
| `AI_DEVELOPMENT.md` | Operating policy: model roles, concurrency, ownership, dispatch contract, GPU run lease, acceptance gates | Delegating, reviewing, or accepting AI work | Normative | 2026-11-16 |
| `AUDIT.md` | Whole-port audit snapshot: cleanliness, extendability, performance, coupling risks, ranked actions, guardrails | Planning cleanup or a structural change; onboarding a new agent | Snapshot (dated) | 2026-11-16 |
| `STATUS.md` | Milestone checklist, what works, current blockers | First orientation after `HANDOFF.md` | Normative (kept current) | 2026-11-16 |
| `HANDOFF.md` | Latest session: what changed, evidence, exact resume point | First, every session | Historical | 2026-11-16 |
| `WORKLOG.md` | Dated log of work, measurements and incidents (append only) | Looking up why something was done; incident history | Historical | append-only |
| `ARCHITECTURE.md` | How the pieces fit and why (execution model, decisions D1-D8, binary facts) | Understanding the design before a structural change | Normative | 2026-11-16 |
| `BUILD.md` | Build steps: packages, bbport GPU library, `port/`, vcore | Building anything | Normative | 2026-11-16 |
| `RUNNING.md` | Running the port, game files, STOP handling, runtime switches, helper tools | Running the game, adding a switch | Normative | 2026-11-16 |
| `AGENT_RUNS.md` | The isolated headless agent runner (`tools/agent-run.sh`) and its sandbox | Any agent game run | Normative | 2026-11-16 |
| `CAPTURE.md` | Frame capture (`DREAMS_CAPTURE`), review tooling, how the sink works | Verifying a visual change without a screen | Normative | 2026-11-16 |
| `INPUT.md` | DualShock/DualSense, keyboard/mouse mapping, pad motion, IME | Input behaviour or a new binding | Normative | 2026-11-16 |
| `FPS.md` | 60/120/240 fps switches, pacing research, skip-intro boot | Frame rate or boot-time work | Normative | 2026-11-16 |
| `PERF.md` | Where the frame time goes, measurements, ordered perf plan | Performance work | Normative | 2026-11-16 |
| `VCORE.md` | The shadPS4-based GPU library: design, shadPS4 dependency map, API surface, build/run, gaps | Any GPU-library change | Normative | 2026-11-16 |
| `VERSIONS.md` | Matching the fork's shader routes on any game version; adding a version | Shader routes, a new game version | Normative | 2026-11-16 |
| `GPU_HANG.md` | The intro hang investigation, loop guard, indirect clamp, repro recipe | GPU guards, hangs, indirect draws | Normative | 2026-11-16 |
| `ACCURACY.md` | The accuracy requirement, verification matrix, comparison procedure | Before claiming any behaviour fix | Normative | 2026-11-16 |
| `SCENE_TESTS.md` | Gameplay/editor regression scope, Please Hug Me target, harness contract | Gameplay or editor testing | Normative | 2026-11-16 |
| `IMPORTS_LOG.md` | One row per implemented/changed PS4 import (NID, semantics, reference, date) | Implementing an import; keeping the log in sync | Normative (append a row) | 2026-11-16 |
| `STUBS.md` | Imports still served by the return-0 fallback | Something behaves oddly; import audit | Normative (regenerate) | 2026-11-16 |
| `logs-gpu-run1.txt` | Raw GPU-run log kept for reference (bbport backend stop) | Only when reproducing the old bbport-backend stop | Historical data | 2026-11-15 |

Related entry points outside this directory:

- `$REPO/AGENTS.md` — project layout, goal, rules for agents.
- `$REPO/port/README.DREAMS.md` — short Dreams entry point inside `port/`.
- `$REPO/port/README.md` — upstream bbport (Bloodborne) README, not Dreams documentation.
- `$REPO/notes/` — research reports (imports, rebase, input, high-fps, lantern).
- `$REPO/port/docs/` — bbport's own upstream docs (mostly Russian), not Dreams documentation.

## Documentation gates

- `python3 -I port/tools/doc_lint.py` — normative docs: no absolute home paths, no host hardware
  claims outside `TEST_HARDWARE.md`, every normative doc listed here. Exit 0 clean, 2 violations.
- `port/tools/preflight.sh` — cheap host-independent preflight an agent runs before handoff or
  commit: shell syntax, `py_compile`, `doc_lint`, shader-route-table sync, no absolute home paths,
  `git diff --check`.
