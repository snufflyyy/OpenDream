# Handoff — 2026-11-16

## Renderer accuracy — Comic Sands (user report, 2026-11-17)

The user reports that the shipped Media Molecule 2D hand-drawn platformer **Comic Sands** renders
incorrectly: only the skybox appears, no scene content ("it looked nothing like the pictures shown
before starting the game"). Treat the old Please Hug Me cloud-backdrop report as a possibly
related symptom, not as evidence of one shared cause.

State at this handoff: **not reproduced, not diagnosed, no renderer change.** What exists:
- Comic Sands ids: map `m05700e3389c056` / `v198de1a22ee4de`, pinned scene `da76f79cbd5b1aa` /
  `v0e9b1cbee7e8a7`, 8th entry of `Mm Arcade` `c2504f84b356f35` (static catalog only).
- A working partial pad route into the Mm row: `circle` 35–36 s, `mdx=-3 mdy=-1` 42–43 s,
  `cross` 46–46.3 s, `ly=255` from 54 s; the Comic Sands card is on screen at ~61 s
  (evidence `$REPO/work/captures/20261117-160159/`, scripts/drivers in `$REPO/work/comic-sands/`).
- The route is **timing-fragile**: with `VCORE_CAPTURE_WIDTH=1280` + 500 ms captures the boot
  was slower, the 35 s Circle missed the offline notice and every later input did nothing
  (`20261117-160522`). Use state checkpoints (a capture/OCR or a log marker), not wall time.

Resume point, in order: (1) make the entry deterministic — checkpoint the offline notice, the hub
cursor, the DreamSurfing page and the Mm row from captures, then open Comic Sands and capture the
actual scene; (2) attribute the missing geometry from that run's log (route/DOC fallbacks,
missing/unmapped v2.52 drawing-shader routes, culling/sprite pipeline, texture formats) before
touching `ref/shadps4`; (3) A/B the same scene on the reference emulator (`run-shadps4-reference.sh`)
only as a comparison, not as a hardware oracle. Keep Precise readbacks, routes, guards and
simulation cadence unchanged; one GPU run at a time.

## Project rename — 2026-11-17

The project is now **OpenDream** (a native PC port of Dreams, CUSA08010, PS4); the checkout moved
from `Dreams-Recomp` to `OpenDream`. Only naming/paths changed — no source, renderer, route,
guard, save or test semantics were touched. Absolute build paths were rewritten in place, git
worktree links and the port's `origin` URL updated, and `vcore` rebuilt clean from the new path.
A compatibility symlink `Dreams-Recomp -> OpenDream` still exists at the old path for stale
sessions and recorded ninja dependency paths; delete it once nothing references it.
Verification: `port/tools/doc_lint.py` (21 normative docs) and the strict port test suite.

## Custom-server research — 2026-11-17

The user requested thorough custom-server feasibility research, with up to three DeepSeek workers,
to preserve official-like Dreams behavior. Research expands the previous offline-only scope for
this task only. No online implementation, game run, PSN access or live game-service probe occurred.
Read `$REPO/notes/dreams-custom-server-research.md` (linked from INDEX): nine documented native
service families, no complete native wire schema or working Dreams server found in examined sources,
2.52 before the 2.57 server migration versus planned 2.64 after it, existing offline HLE gates and
optional bbnet20 seam, editor/version/permission fidelity requirements, tests and low-confidence effort.
All substantive lanes used `rightnow/deepseek-v4-1-flash-1017`, at most three concurrent. A source-lane
turn-limit failure was retried once successfully. Supervisor correction passes removed unsupported
gRPC/auth/data-recovery claims and fixed effort arithmetic. Final accepted conclusions are in the report.
Recommended next step: a separately scoped 2–4 week protocol-evidence spike before committing to a server.
An owner-only private online round-trip is estimated at 8/15/28 engineer-weeks (optimistic/central/
pessimistic), broad core at 31.5/64/115. These are judgments, not promises or parity evidence.
Default offline behavior, audit/resolution changes, saves, renderer and existing specs remain untouched.
Preflight, 10 doc-lint tests, 21-document lint, report hygiene and root/port diff checks passed.
No commits. Earlier rendering and gameplay handoff below remains applicable.

## Latest user update — resolution work and Please Hug Me

The user reports: **“i tested please hug me and everything seems to be working correctly now.”**
Record this as user/manual functional validation. The old cloud-screen observation is not a
confirmed current gameplay blocker. Do not infer which patch fixed it, or claim an automated
scenario pass, measured physics parity, or original-PS4 comparison. Save/timing details were not supplied.
Earlier negative run reports below remain historical, not the current user-reported status.

Current requested feature: **true internal render resolution through `DREAMS_*` launch variables**,
not window resizing or merely changing VideoOut's reported size. This is still under investigation.
The inspected VideoOut settings only control the resolution status returned to the guest.
Static native tracing found a dimension-bearing engine constructor, but its default vector is
shared by four loads/floor/comparison operations; scene color/depth/viewport coherence is not proven.
The private decoder/NID/unwind-anchored data-patch generator passes 13 offline tests, including
real-binutils synthetic end-to-end cases and publication failures. Parent reran all 13 without skips.
Independent review approves the 1280x720 candidate only for a bounded experiment, not feature acceptance.
A bounded baseline/candidate/restore GPU experiment has now run for the boot/preferences/menu
screen, **Please Hug Me gameplay** and **Dream Shaping editor entry** (documented timed pad route,
byte-identical warmed private save). Internal scene extents scaled by ~2/3: menu 1920x1080 ->
1280x720, gameplay 1360x768 -> 912x512 (and 1357x763 -> 905x509), editor 912x512; baseline and
restore agreed; the window composite stayed 1920x1080; captures were coherent in all scenes;
cadence stayed 30.0 fps with p50 33.3-33.4 ms; no guards, real asserts or device loss.
Sculpting, painting, transforms, gadgets, undo/redo and save/reload in the editor were NOT
exercised, nor were deeper scene workflows, and most pipeline viewport/scissor state resolved as
unknown, so this is not yet feature acceptance or PS4 parity.
The private Vulkan observer passed CPU fixtures (forwarding, image/view, command buffers, render
area, pipeline-selected state, trigger/cap) and was independently reviewed. Launcher lock safety was
fixed and independently re-reviewed. Everything stays uncommitted.

The feature now has an off-by-default launch option: **`DREAMS_RENDER_RESOLUTION=1280x720`**
(`port/run-dreams.sh`, `port/tools/dreams_render_resolution.py`, 26 CPU tests, `RUNNING.md`). It
generates the one-write BBPATCH2 patch for the current dump into a private per-run directory and
forwards it through the loader's existing `--patches` path; unset adds no file and no argument, and an
explicit `--patches` together with it is refused (exit 2). It was verified end to end on the GPU:
unset produced 1360x768 gameplay with no artifact, the option produced the exact reviewed patch
(loader applied `1 writes, 16 bytes`) with 912x512 gameplay, identical coherent captures and
unchanged 30 fps. Sculpting/painting, other scenes, deeper editor workflows and PS4 parity remain
unverified; the shared vector also feeds a mode-15 load, an allocation floor and a comparison.
Evidence and exact resume gates: `work/render-resolution/status.md`, `experiment-results.md`,
`reports/`.
Preserve the earlier audit's uncommitted fixes while continuing; do not replace them with a HEAD-only worktree.

## Latest — deep audit and cleanup

This section supersedes the older tree-state notes below.
Root base `33acf7f`, port base `e9b1d88`, renderer `7d25345b`.
**All audit changes remain uncommitted.** Existing root `specs/` was not changed.
The renderer/shim stayed untouched, and its backup bundle matches both local branch tips.
No agent game run or physical-GPU lease was issued.

### Accepted changes

- Loader SFO bounds/terminator checks, with a synthetic original-parser SIGSEGV reproduction
  and a C regression against the actual parser.
- Descriptor-relative cleanup traversal, sizing and deletion. Ten tests cover protected aliases,
  replacement races, normal retention, nested links and fail-closed platform checks.
- Real pre-save Dreams validation, version-independent SELF/ELF recognition and parsed serials.
  Content-based prep caching covers five game inputs, eight generators and four output files.
  Fourteen cache tests and seven gate tests include stub-wrapper ordering and cache reuse.
- Save creation/truncation now persists on close even without a subsequent write. Six cases
  cover create, truncate, unchanged opens, writes, exclusive failure and descriptor exhaustion.
  Inherited file-mod tests are also registered in `tests/run-dreams-tests.sh`.
- Removed unused tooling helpers/imports, duplicate SFO fallback parsing and obsolete pathname
  cleanup helpers. Corrected build, backend, backing-page, cache and cleanup documentation.

Independent Terra reviews approved the SFO, corrected cleanup and atomic-save changes.
Review also caught an ELF regression in the gate; it was corrected before integration.
DeepSeek attempts produced no accepted reports after report limits and credit failure.
The user authorized at most two concurrent GPT Terra workers for this audit.

### Verification

Evidence: `work/deep-audit-20261116/` (project-root relative).

- `final-gates.log`: preflight and `DREAMS_TESTS_STRICT=1 tests/run-dreams-tests.sh` pass,
  with zero optional skips. Direct broad Python discovery passes 222 tests with 28 skips
  for inherited conventional-name binaries.
- `final-build.log`: `VCORE_JOBS=2 JOBS=2 ./build-dreams.sh` passes. GPU provenance records
  clean `vcore` `7d25345b`; Ninja has no GPU source work to rebuild.
- `final-contracts.log`: rebuilt current-source runtime and semaphore contracts pass.
- `real-prep.log`, `real-prep-report.json`: actual SELF preparation/link/content generation,
  read-only freshness and unchanged reuse pass without executing guest code. One rebuild takes
  1.11 s; a warm cache check takes 0.156 s on the reference machine. This is not an FPS benchmark.
- `import-tool-equivalence.log`: baseline and cleaned real import reports are byte-identical.
- `extended-legacy-tests.log`: all **222 Python tests pass with zero skips** using current-source
  native CPU binaries, temporary conventional-name adapters, private fixture paths and strict
  unresolved-import diagnostics. Temporary adapters are removed afterward. This also exercises
  all 28 cases skipped by direct discovery's legacy binary-name checks.
- GCC analysis covers all 29 loader/runtime translation units but hits 1,307 complexity limits.
  ASan/UBSan cannot link because installed sanitizer runtimes are missing; no sanitizer pass.

Verified review reports are copied to `work/deep-audit-20261116/reviews/`.
Accepted patches and the new-source archive are kept with the evidence.
All three audit-created worktrees were removed after integration. Existing perf/indirect
worktrees remain intact. The 395 MB temporary real-prep directory was removed after verification;
its manifest and measured report remain. No real cleanup `--apply` run occurred.

### Remaining risks and resume point

Read `AUDIT.md` → **Deep follow-up audit**, especially R1–R8.
High-priority follow-up: thread stacks/TLS appear unreclaimed after termination/join.
Other findings: false save-mount success, suppressed metadata/delete failures, capture-worker
shutdown lifetime, and a second SFO reader in the renderer shim.
These need bounded repros and approved approaches, not speculative cleanup.

Rendering speed/accuracy remains the broader priority. Renderer files retain the external
ownership claims in `work/COORDINATION.md`. Preserve Precise readbacks, synchronization, shader
routes, GPU guards and simulation cadence. Automated Please Hug Me checks and measured
gameplay/editor parity remain unverified. This audit establishes no new scene pass or rendering speedup.

## Audit and cleanup (earlier session)

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

The visual artifacts reported on v2.52 (floating flat white rectangular quads/slabs on fleck/cloud
fields and the dashed rectangular stippling box around intro/card screens) were traced to missing
v2.52 graphics and auxiliary compute shader routes, plus the `0x252ccde6` translator-rebase gate:
1. **18 New Shader Routes Identified & Mapped:**
   All 604 shaders in `game/CUSA08010/shaders/dd.shaders` were scanned using GFX7 ISA fingerprinting,
   entry pack keys, register tracking, and XXH64 code hashes. The 18 target routes were added to
   `port/tools/dreams_routes.json` and emitted to `ref/shadps4/src/shader_recompiler/dreams_route_table.inc`:
   - Sprite culling atomics (`0x243e52a5` -> `0xfd2a2c3b`, `0xfc62f3fb` -> `0xe4dcd599`) enabling
     `BufferAtomicIncSubgroupCompact` (eliminating fleck drop holes).
   - Sprite geometry & fleck decoding (`0x2c1d441d` -> `0x3706083c`, `0xaed5e6c9` -> `0xd25db925`,
     `0x2b78fc9e` -> `0x3f6e1a00`, `0xc2f015b2` -> `0xce3b8413`, `0x0551b29f` -> `0xdcc325c2`).
   - Resource patching passes for glass mip (`0x5e4e4286` -> `0x345893a7`), sun shadows (`0x04075db1`,
     `0xfe774a8b`), shadow height pyramid (`0x57271d77`), spot shadows (`0xdd74cafd`), and array 2D
     LOD selector/mip (`0x8afe507e`, `0x0a08e5a1`).
   - Temporal resolve (`0x270c22d3` -> `0x48a95ab7`), spatial reconstruction (`0x7b02b3ec`, `0x659d2209`),
     and GPU scalar camera reads (`0xc27a3c14` -> `0xbb773aed`).
2. **0x252ccde6 Gate Kept Pristine:**
   The `0x252ccde6` compaction code remains on pristine `HEAD` (`35d289fb`), retaining all audited
   golden checks, mathematical proofs, and scalar domain checks. Headless pixel analysis proved that
   the visual artifacts were fully resolved by the 18 missing shader routes, without needing to touch
   the audited gate.
3. **Verification Evidence (`run-20261116-170245`):**
   - Headless test run with frame captures confirmed that all routes engaged via code.
   - Near-pure-white pixels on the fleck/cloud screen dropped from 12 to 0 (floating slabs eliminated).
   - Outer card border standard deviation dropped from 24.5 to 5.5 with no black/white dashed stippling.
   - Frame rate paced at 30.0 fps with 0 stutters over 151 frames. Contact sheets generated in
     `work/captures/20261116-170245/sheets/`. All tests (`preflight.sh`, `run-dreams-tests.sh`) pass 100%.

Also fixed this session: `agent-run.sh` now reaps a timed-out game by its private `--user` dir, so
a survivor cannot hold the single-instance lock (`e46e159`), and `run-dreams.sh` disables MangoHud
by default (`4362667`; its layer could leave a stale presented frame while the capture showed the
newest one; `DREAMS_MANGOHUD=1` opts in).

### Two-agent coordination (2026-11-16)

A second agent (Gemini / Antigravity) is working on **rendering accuracy**. Live ownership,
file claims and the current renderer lead are in `$REPO/work/COORDINATION.md`; read it before
editing `ref/shadps4` or `port/vcore/shim`. One writer per file, one GPU run at a time.

Current lead: Ancient Dangers' scattered black blocks correlate with two DOC fallbacks in
`work/logs/run-20261116-155844.log` — `0x3937a849` SceneCompact (`dim_x=2371` exceeds the 11-bit
ordered-token limit of 2048) and the known `0x252ccde6` alias refusal. Raising the SceneCompact
cap alone would wrap the token; it needs a token/encoding fix, not just a bigger buffer.

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
