# Gameplay and editor regression tests

Working directory: `$REPO/port` for the commands in "Commands available now" (`$REPO` = your
OpenDream checkout); the game runner is `$REPO/tools/agent-run.sh`.

## Required scope

The user requires real scene tests across locally included Media Molecule games and the editor.
Menu FPS and process survival are not gameplay passes. Correctness takes priority over FPS (`ACCURACY.md`).

**First target: Please Hug Me.** The user now reports that it works correctly in manual testing.
The earlier movement/progression and cloud-screen reports are not confirmed current blockers.
The responsible change is unknown. Automated scenario coverage and original-PS4 comparison remain unverified.
Do not turn this manual report into an automated or physics-parity pass.

Editor coverage must include sculpting, painting, transforms, physics/logic gadgets, play/edit transitions,
undo/redo, and save/quit/reload. Opening the editor alone is insufficient.

## Commands available now

Run from `port/`:

```sh
python3 -I tools/dreams_content_inventory.py $REPO/game/CUSA08010 --out $REPO/work/content-inventory.json
python3 -I tests/test_content_inventory.py
python3 -I tools/dreams_content_closure.py $REPO/game/CUSA08010 \
  --root-dream m326284ee5d6145 --root-version vd6c7c0f6c4ad37 \
  --out $REPO/work/please-hug-me-closure.json
python3 -I tests/test_content_closure.py
tests/run-dreams-tests.sh
```

The inventory reads ZIP directories and JSON metadata without extracting binary assets.
Its report contains content IDs, versions, pinned child scenes, and directly declared asset counts.
All entries have `runtime_status: not_run`. Presence on disk is not a gameplay result.
The inventory does not verify recursive asset dependencies.
The closure tool traverses explicit JSON relationships and verifies declared ZIP members, including special hashes.
It does not parse opaque scene/audio payload references or establish playable-scene entry.
Exit 0 means metadata-visible completeness, 2 means missing/malformed references or assets, and 3 means invalid input.
Its 17 CPU tests run in the primary suite. Please Hug Me currently resolves to 2 dreams, 2 versions, and 217 unique assets.

Run the game only through `../tools/agent-run.sh`. Use private saves and synthetic input.
Never use the player's save as a test output directory. Never override the single-instance lock.

## Installed Please Hug Me identifiers

Evidence: ordinary Python ZIP/JSON inspection of the user's local v2.52 package.
These are static catalog observations, not runtime evidence.

| Item | ID / version |
|---|---|
| Registry usage | `MmContentPleaseHugMe` |
| Map/collection | `m326284ee5d6145` / `vd6c7c0f6c4ad37` |
| Pinned playable scene | `dbe74bfb376195e` / `v1627df79cf02a2` |
| Direct scene assets | 9 scene blobs and 204 audio blobs, all present across the two installed archives |

The map version pins that scene and connects its entrance portal to the scene's entrance.
A reliable runtime route from the hub to this scene is still needed.
A scripted Cross or R2 press did not dismiss the offline notice.
**Circle does:** `run-20261116-104813` pressed Circle at 35–36 s and reached the main hub.
Pad traces confirm the injected button reached the game. The captured hub shows Dream Surfing and Dream Shaping.

**Correction to earlier notes:** `m326284ee5d6145` is Please Hug Me, not the homespace.
Earlier worklog entries inferred a homespace from saved progress containing this map.
Do not reuse that inference as a scene checkpoint. `DefaultHomespace` is separately registered as `d683140ca95a287`.

## Please Hug Me behavior reference

Public reference: [PS4 gameplay video](https://www.youtube.com/watch?v=n3af8AZ63Nc).
Six sampled frames at 0:15, 0:27, 0:39, 0:51, 1:03, and 1:15 show:

- The cuboid player on a platform with many cone-shaped NPCs.
- Fewer NPCs near the platform edge during play.
- An empty platform and a zoomed-out camera.
- A post-game card with replay/play controls.
- Another populated run after the post-game card.

The sparse frames do not establish exact speed, controls, collision rules, or game version.
The video is labeled PS4 gameplay, not hardware-attested evidence from this session.
A [contemporary PS4 article](https://www.gamespew.com/2020/03/dream-surfing-in-dreams-is-delightfully-hit-and-miss/)
describes NPCs fleeing when approached, falling off the platform, and the game ending when none remain.

Candidate behavioral checks: controllable player movement, NPC reaction to approach, NPC removal at the edge,
completion after the final NPC, and reset on replay. Confirm these inside the native run before defining thresholds.
NPCs refusing hugs is the intended premise, not by itself a defect.
The latest user report is positive manual evidence, not a measured reference comparison.
Keep these checks for regression coverage. Reproduce any renewed movement or progression problem
before diagnosing it, and do not dismiss it based on the game's premise.

## Initial game candidates

The shipped `MmContentCollection1` registry entry is **Mm Arcade** (`c2504f84b356f35`).
Its map metadata references 12 nonempty game entries plus two empty placeholders:

- Vecta Majoris, Please Hug Me, Hammer Panic, Ferovium.
- Treasure Hunt, Cone Ball, Dreamiverse Dash, Comic Sands, Combo Crush!.
- Ancient Temple Game, Welcome Home Game, Welcome Garden Adventure.

The registry separately identifies Art's Dream and Ancient Dangers: A Bat's Tale.
These 14 games are the initial suite inventory, not verified runtime passes.
The catalog also includes Welcome Garden showcases, Box Maze/Box Blaster, tutorials, and editor templates.
Those need separate coverage classification rather than automatic exclusion.
`Mm Showcases` contains 11 additional scenes useful for graphical tests.

The registry has 1,111 entries, including objects, collections, tools, tutorials, and other resources.
Do not treat all entries as games or infer runtime availability from a title alone.

## Verified navigation so far

At default PS4 timing with a private tutorial-skip save, `run-20261116-105342` reached DreamSurfing:

1. Press Circle at approximately 35 s for one second to dismiss the offline notice.
2. At approximately 42 s, inject `mdx=-3 mdy=-1` for one second to move toward Dream Surfing.
3. At approximately 46 s, press Cross for 0.3 s.
4. Check the captured DreamSurfing page, not just the run timeout.

The page showed Ancient Dangers as the featured item and the Media Molecule games row below it.
This route is exploratory, not yet a reusable scenario or a specific game pass.
The current `mdx`/`mdy` tokens apply per pad read, so their displacement depends on read frequency.
Do not reuse those values unchanged across FPS modes without checking the resulting cursor position.

## Agent-facing harness contract (not yet implemented)

The next implementation must provide:

- One entry point with `list`, single-scenario, and suite commands.
- Stable names for each game scene and editor workflow.
- Automatic sandboxing, bounded runs, and private save setup.
- Recorded setup and input sequences with explicit behavioral checkpoints.
- A reproducible source build, game version, timing mode, and warm/cold cache label in each report.
- Captures, frame-time percentiles, gameplay cadence, guard/error counts, and bounded profiling output.
- Stable JSON reports plus concise terminal summaries and documented exit codes.
- Distinct `passed`, `failed`, `blocked`, and `unverified` outcomes.
- A documented procedure for adding a scenario and rerunning previous scenarios after a fix.

A missing entry route, missing assets, or unavailable baseline must not produce a pass.
A run that stays at the offline notice must not pass a Please Hug Me or editor scenario.
Guard trips, clamped malformed draws, crashes, and failed behavioral checkpoints must fail the relevant scenario.
A screenshot alone cannot establish physics, logic, progression, or save integrity.

## Test order

1. Establish an actual scene entry checkpoint.
2. Reproduce the reported behavior at default PS4 timing.
3. Define observable movement, interaction, and progression checks.
4. Profile the failing scenario.
5. Fix one confirmed cause.
6. Rerun that scenario and previous game/editor scenarios.
7. Repeat at 60 and high render FPS without changing simulation speed.

Original-PS4 evidence remains the accuracy reference. The forked emulator shares GPU code with
this port, so agreement between them is useful but not sufficient proof of correct behavior.
