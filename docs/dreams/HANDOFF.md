# Handoff — 2026-11-16

## Audit and cleanup (later this session)

Read `AUDIT.md` first; it is the whole-port snapshot and guardrail list.

**Committed in `port` (branch `dreams`, tree clean):** host-independent docs + `INDEX`/
`CONVENTIONS`/`TEST_HARDWARE`/`README.DREAMS`/`AUDIT` (`844f179`); doc/route/import/ABI/build-
provenance gates + `preflight.sh` (`09eeda6`, `40df4ea`); shared-backing pages (`c5f373c`);
content tools + `DREAMS_VCORE_CONFIG` (`ef691dc`); pad/fps/fault tests (`5f63807`); audit notes
(`c2ac4ba`, `b1a794f`).
**`ref/shadps4` `vcore`** commits the fault-bitmap fix (`0ca8d051`); the backup bundle is refreshed.
**Root repo** is on feature branch `cleanup/audit-20261116` (`5c18d77`, `e46e159`) because `master`
is commit-protected; `notes/*` edits and `specs/` are left untouched.

Guardrails now run: `port/tools/preflight.sh` (syntax, doc_lint, route-table + import-name sync,
no absolute home paths, diff check) and `tests/run-dreams-tests.sh` (CPU suite; loop guard, ABI
contract; `DREAMS_TESTS_STRICT=1` fails on optional skips). `build-dreams.sh` records
`out/gpu-revision.txt` and warns on a non-reproducible GPU tree.

### Visual bug — current resume point

The artifact scene is the tutorial (`RemixTutorial` / Dream Shaping), not a DreamSurfing launch.
Deterministic headless entry (no real pad): copy `work/user`, restore
`…/SYSTEM/savedata.json.before-tutorial-skip-20261115-210501`, then `circle` at 36 s,
`mdx=3 mdy=-1` at ~42 s, `cross` at ~46 s, `DREAMS_SKIP_INTRO=1`, `DREAMS_CAPTURE=250`,
`agent-run.sh --seconds 130`. Capture: `work/visual-fix/captures/native-intro-tut2/`.
The camera-pivot tutorial step renders correctly; the white-quad/black-grid artifact is a
**later tutorial step** (sculpt/paint) or the Please Hug Me scene. `0x252ccde6` is still the only
ordered-count fallback (`reason=252 unaudited translated binary`); its dumped current module
(`work/visual-fix/private/dump/`) differs from the audited golden only by translator-rebase
codegen, and the fork's `DataOrderedCount` lowering is present (same guest code hash, counter
0x400). Do not re-bless the gate until a capture ties the artifact to 252.

Also fixed this session: `agent-run.sh` now reaps a timed-out game by its private `--user` dir, so
a survivor cannot hold the single-instance lock (`e46e159`).

---

The user asked to wrap up after a connection loss and usage exhaustion.
**No game, agent runner, input script, or background task was running at handoff.**
All changes remain uncommitted. The current native build includes the two fixes below.
Read `AGENTS.md`, then this file, `ACCURACY.md`, and `SCENE_TESTS.md` before continuing.

## Current direction — rendering speed and visual accuracy

The user asked to wrap the diagnostic phase and begin performance and visual-accuracy work.
DeepSeek remains the primary implementer. GPT assigns tasks and reviews approaches, patches, and evidence.
At most three DeepSeek workers run concurrently, with one exclusive bounded GPU lease.
Check the fleet and saved lease before reconnect recovery. Do not restart completed workflows.

The metadata-visible closure tool is integrated: `tools/dreams_content_closure.py` and 17 CPU tests.
Independent review required malformed-reference and special-hash coverage fixes before acceptance.
The complete main-checkout suite passed, including fault-buffer, codecs, and AvPlayer tests.
Two real Please Hug Me reports were byte-identical: 2 dreams, 2 versions, 217 unique declared assets, no missing assets.
This proves only metadata-visible presence, not opaque binary dependency closure or playability.
Evidence: `work/content-closure-worker/integrated-suite.log` and `integrated-phm-1.json` (project-root relative).

Next: establish matched performance/capture baselines, select evidence-backed fixes, and rerun correctness tests.
Keep simulation timing, Precise readbacks, shader routes, and guards unchanged.
Do not trade geometry, draw work, synchronization, or simulation accuracy for FPS.
All changes remain uncommitted. No gameplay, physics, or editor parity claim is new.

## Previous diagnostic addendum — DeepSeek test integration

DeepSeek extended `tests/test_pad.c` and added it to `tests/run-dreams-tests.sh`.
Coverage now includes held/released buttons, exact/clamped sticks, analog triggers, and per-read mouse deltas/release.
Independent review found an inherited mouse-mode failure. The author fixed it before approval.
The test explicitly selects camera mode and clears external virtual-pad injection.

GPT integrated the two reviewed files. The complete main-checkout suite passed without codec or AvPlayer skips.
Separate `DREAMS_MOUSE_MODE=imp` and `BB_TEST_VPAD=1` pad runs also passed.
Evidence: `work/ai-pad-worker/integrated-suite.log` (project-root relative).
No runtime behavior changed. All changes remain uncommitted.

Gameplay remains blocked/unverified. Valid runs `20261116-115546`, `-120157`, and `-120811`
log a launch request for the correct Please Hug Me scene, then show a title card and cloud backdrop.
A launch request is not proof of playable-scene entry. Run `-120517` is void because its input environment was missing.
The original sampler missed the renamed process. Its replacement passed 21 CPU tests, including a real renamed-process test.
Run `20261116-122339` matched the executable, private save, and private pad, producing 60 observed samples.
During the cloud window, render/game-update work continued while named streaming/cache threads had zero sampled CPU ticks.
This does not establish a root cause or prove that the scene failed to load.
Static catalog inspection found no explicit missing Please Hug Me dependency. Binary-payload references remain uninspectable.
The valid file/draw-profile run `20261116-124209` is complete; all workers have stopped at this handoff.
It still showed the cloud backdrop, continuing compute work, and one expensive indirect draw.
Fewer draw pipelines than the menu do not prove that the scene never loaded. The heavy pass's exact role remains unknown.
Evidence: `work/deepseek-hug/probe5-report.md`. The gameplay blocker is deferred, not fixed.
The old workflow parent exited on connection loss; its finished child evidence was recovered without rerunning it.
Use real hooks: `BB_FILE_TRACE`, `DREAMS_PAD_READ_TRACE`, `SHADPS4_DREAMS_PERF_PROFILE`, and `SHADPS4_DREAMS_PERF_PROFILE_ALL_DRAW`.
The profiler needs `VCORE_KEEP_SHADPS4_ENV=1`, with unrelated inherited `SHADPS4_*` variables removed first.
`Lib.SceFios2` is not a vcore logging category. Fios2 is linked guest code, not a host preload-completion hook.
The older navigation notes below are historical. See `AI_DEVELOPMENT.md` for the current supervisor-first policy.

## User priorities

1. Consistent, smooth native execution with high FPS and correct graphics.
2. **Original-PS4 accuracy is mandatory.** Dreams is an engine. Physics, logic, progression,
   editor operations, and saves matter more than FPS. Guards are not correctness fixes.
3. Build automated tests inside every locally available built-in Media Molecule game **and the editor**.
   Profile, fix confirmed bugs, then rerun the affected and previous scenarios.
4. Make the tests easy for future agents: one documented entry point, named scenarios,
   automatic sandbox/private saves, stable JSON reports, useful exit codes and artifacts.
   Distinguish passed/failed/blocked/unverified. A menu-only run must not pass a gameplay test.
5. First gameplay target: **Please Hug Me**. The user suspects both interaction/progression
   and movement/physics problems. This is **not yet reproduced or diagnosed**.

## Implemented and verified

### 1. Progressive slowdown: GPU fault bitmap initialization

`ref/shadps4/src/video_core/buffer_cache/fault_manager.cpp`:
- The device-local fault bitmap had undefined initial contents.
- Stale bits became phantom sparse-residency requests. Overflow recovery kept those requests
  alive, causing repeated allocations and increasingly slow libdrm VA searches.
- The constructor now records a zero fill and a transfer-write → shader-read/write barrier.
- Precise readbacks, shader routes, fault recovery, and hang guards remain enabled.

Two pre-fix sandbox runs degraded from ~5 FPS to below 2 FPS (one below 1 FPS).
Stacks: `work/smooth-profile-stacks.out`, showing `ProcessFaultBuffer -> ObtainBuffer ->
EnsureResident -> radv_alloc_memory -> amdgpu_vamgr_find_va`.
After initialization, repeated menu runs sustain ~94 FPS instead of progressively slowing.

Backups:
- `backups/shadps4-vcore-fault-bitmap-init.patch` (GPU-source patch).
- `backups/session-20261116-handoff.tar.gz` (modified/new port files, root instructions/impact note,
  and the GPU fault-manager source; no game files or run artifacts).

The existing shadPS4 bundle still covers the committed base. No new commits were made.
Do not delete/re-clone the reference tree.

### 2. Texture backing-page lookup

- New runtime API `runtime_memory_backing_span` lends contiguous bytes from the permanent shared view.
- `port/vcore/shim/backing_pages.cpp` now implements `BackingPageTable::find`.
- Complete pages avoid a redundant 16 KiB copy. Split mappings and private direct allocations
  keep the original copy path. Never lend the protected guest view or retain pointers across remaps.
- `VCORE_BACKING_COPY=1` restores the old path for A/B tests.
- One paired menu comparison: **94.4 FPS borrowed vs 90.5 FPS copying** (~4.3%).
  This is one scene comparison, not a general benchmark.

### Timing and test evidence

All game runs used `tools/agent-run.sh`: private headless Wayland, bubblewrap, no network,
no audio devices, no real input devices, copied saves, one GPU instance.

| Run | Evidence |
|---|---|
| `20261116-100826`, `-101443` | Pre-initialization progressive slowdown |
| `-101934` | Fixed 90 s run: steady-state ~94 FPS, p50 ~10.1 ms, p99 ~14 ms |
| `-102341` | Default timing: 30.0 render FPS / 30.0 gameplay updates/s |
| `-102752` | High FPS: 94.4 render FPS / 30.0 gameplay updates/s |
| `-103044` | Copy control: 90.5 render FPS / 30.0 gameplay updates/s |
| `-103421` | 60 FPS: 60.0 render FPS / 30.0 gameplay updates/s, steady p50/p99 ~16.7 ms |

These are **menu/offline-notice measurements**, not physics/editor parity tests.
No guard trip, malformed-draw clamp report, fault-overflow warning, or device loss appeared
in these fixed runs. Known bad indirect arguments in other scenes remain unresolved.

`port/tests/run-dreams-tests.sh` passes, including:
- Shared backing and adapter tests.
- Timing-sampler tests (original and high-FPS v2.52 signatures).
- Four content-inventory tests.
- CPU-Vulkan fault-parser regression: dirty-memory negative control, initialization,
  real boundary-page faults, and lossless/duplicate-free overflow draining.
- Existing Dreams runtime tests.

Inherited runtime and semaphore contracts also pass, rebuilt against the current runtime/vcore.
Evidence: `work/smooth-tests-integrated.out`, `work/scene-inventory-tests.out`,
`work/smooth-runtime-contracts.out`, `work/smooth-sema-contracts.out`.
Builds, shell syntax, Python compilation, and final `git diff --check` passed.

## Scene test work — incomplete, continue here

### Available inventory tool

From `port/`:

```sh
python3 -I tools/dreams_content_inventory.py ../game/CUSA08010 --out ../work/content-inventory.json
python3 -I tests/test_content_inventory.py
tests/run-dreams-tests.sh
```

This reads local ZIP/JSON catalogs without extracting binary assets. The report has 1,111
registry entries and pinned child-scene/version relationships. Every runtime status is `not_run`.
Direct asset presence is not recursive dependency closure or proof of gameplay.
`SCENE_TESTS.md` documents the future harness contract. **The gameplay/editor harness is not built yet.**

The Mm Arcade collection (`c2504f84b356f35`) has 12 nonempty game references and two empty placeholders.
Art's Dream and Ancient Dangers are separate roots. This gives an initial 14-game candidate suite,
not an exhaustive verified coverage claim. The actual DreamSurfing row also displays
**Dreams VR – Inside The Box**, so classify additional built-in/tutorial/VR content explicitly.

### Please Hug Me IDs

- Registry: `MmContentPleaseHugMe`.
- Map: `m326284ee5d6145` / `vd6c7c0f6c4ad37`.
- Pinned scene: `dbe74bfb376195e` / `v1627df79cf02a2`.
- All 9 directly declared scene blobs and 204 audio blobs are installed.

**Important correction:** prior notes called `m326284ee5d6145` the homespace.
The shipped catalog identifies it as Please Hug Me. `DefaultHomespace` is `d683140ca95a287`.
A saved map/history entry is not proof of the currently displayed scene.

### Verified navigation and latest capture

At **default 30 FPS** with a fresh private tutorial-skip save:
1. Circle at ~35–36 s dismisses the offline notice. Cross and R2 alone did not.
2. `mdx=-3 mdy=-1` at ~42–43 s moves toward Dream Surfing.
3. Cross at ~46–46.3 s opens DreamSurfing.
4. `ly=255` at ~55–55.5 s scrolls down to the Media Molecule games row.

Latest run: **`20261116-105830`**.
Latest reviewed capture: `work/captures/20261116-105830/f00131_t00074.5.png`.
It shows the Mm row with Ancient Dangers, Dreams VR – Inside The Box, Art's Dream,
Welcome Garden Adventure, and more cards to the right. **Please Hug Me has not been entered yet.**

Exploratory scripts and outputs:
- `work/scene-row-input.py`, `work/scene-row-pad.txt`, `work/scene-row-run.out`.
- `work/scene-surf-input.py`, `work/scene-surf-run.out` (DreamSurfing entry).
- `work/scene-entry-input.py`, `work/scene-entry-run.out` (Circle dismissal, pad traces).

`mdx`/`mdy` apply per pad read, so displacement depends on read frequency.
Do not reuse this calibration unchanged across FPS modes. The trace measured ~90 pad reads/s at 30 FPS.
Use scene/cursor checkpoints, not blind wall-time scripts, for the reusable harness.
The next navigation experiment was going to scroll the Mm row horizontally (perhaps `rx=255`)
and locate Please Hug Me. No such experiment was launched.

### Behavior reference

Public PS4-labeled video: https://www.youtube.com/watch?v=n3af8AZ63Nc
Six frames sampled at 15, 27, 39, 51, 63, 75 s show populated platform, fewer NPCs at the edge,
empty-platform camera pullback, post-game replay controls, and a populated replay.
Version and hardware provenance are not independently verified. Sparse frames do not establish exact speeds.

A contemporary PS4 article describes NPCs fleeing on approach, falling off the platform, and
completion after none remain:
https://www.gamespew.com/2020/03/dream-surfing-in-dreams-is-delightfully-hit-and-miss/

Candidate checks: controlled player movement, NPC response, falling/removal, completion,
and reset on replay. Not receiving a hug is the premise, not by itself a defect.
Do not dismiss the user's movement/progression report based on that premise.

## Next actions

1. Finish reliable entry into Please Hug Me at default PS4 timing and verify the actual scene.
2. Reproduce movement/physics and progression behavior against the reference observations.
3. Build the agent-facing scenario runner with real checkpoints and honest report statuses.
4. Add editor workflows: sculpt/paint, transforms, physics/logic gadgets, play/edit,
   undo/redo, save/quit/reload.
5. Profile confirmed defects, fix causes, and rerun prior scenarios. Expand across built-in games.
6. Continue tracing malformed indirect arguments; guards remain safety nets.

## Repository/worktree cautions

- `port/` and `ref/shadps4/` both have uncommitted changes from this session.
- `port-perf`, `ref/shadps4-perf`, and `ref/shadps4-indirect` contain previous unfinished work.
  They were left intact. Do not blindly cherry-pick the readback experiments.
- Root `notes/high-fps-research.md` and `notes/input-research.md` were already modified at start.
- `ref/shadps4/i-didnt-check-my-work.md` is a generated marker required by that tree's AGENTS.md,
  not the verification report. Actual verification is above.
- Root `.pi/` contains harness/task artifacts. Do not commit them or anything from `game/`/`work/`.
- Approximately 26 GB free on `/home` at handoff. Do not extract the ~4.8 GB dev-level packs unnecessarily.
