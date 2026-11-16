# PS4 behavior and accuracy

Working directory: `$REPO/port` (`$REPO` = your Dreams-Recomp checkout). Measurements below come
from the reference machine (`TEST_HARDWARE.md`).

The user requires original-PS4 behavior (2026-11-16). Dreams is an engine: rendering alone does not prove correctness.
Performance changes must preserve game logic, physics, sculpt data, animation timing, audio timing, and saved creations.

## Rules

- Keep Precise readbacks and the matched Dreams shader routes enabled.
- Fix undefined state, incorrect synchronization, or incorrect API semantics before changing limits.
- Keep GPU hang guards enabled during agent tests. A guard hit is a correctness failure to investigate, not a successful render.
- Do not force empty draws, reduce simulation work, or remove synchronization to increase FPS.
- Use the default PS4 timing as the control. High-FPS rendering remains opt-in.
- Test only through `$REPO/tools/agent-run.sh`. Use copied saves, synthetic input, and frame captures.
- Never change the player's save for a test.

## Evidence and limits

The PS4 executable runs natively. The renderer translates GPU programs, and the runtime replaces PS4 services.
Both replacements can introduce errors. A successful boot or a stable frame rate does not establish PS4 parity.

The forked shadPS4 emulator is a useful comparison, but it is not a hardware oracle.
The native port and emulator share GPU code, so matching images can contain the same error.
Original-PS4 captures and repeatable creation tests are needed for a hardware comparison.

Current evidence: menu tests measured 30 gameplay updates per second at 30, 60, and approximately 90–100 render FPS (`FPS.md`).
The 2026-11-16 repeat measured 30.0 gameplay updates/s at 30.0 and 94.4 render FPS (`PERF.md`).
This does not prove equivalent physics, logic, animation interpolation, or editor behavior.
Those checks remain open. No hardware comparison was performed in this session.

## Verification matrix

| Area | Required check | Current evidence |
|---|---|---|
| Timing | Default rendering at 30 FPS; gameplay at 30 updates/s. High render FPS must not accelerate gameplay or audio. | Post-fix menu repeat: 30.0/30.0 and 94.4/30.0 render/game rates. Audio and creation behavior remain unverified. |
| Sculpts | Same visible geometry, surface detail, material, shadows, and transparency for identical creations. | Homespace route-matching captures improved. Editor scenes remain unverified. |
| Physics and logic | Same elapsed-time outcome for collisions, timers, counters, signals, and logic circuits. | Not verified against PS4. |
| Editing | Create, transform, sculpt, paint, wire, undo, redo, save, quit, and reload without data loss. | Not yet verified end to end. |
| Saves | Reopened creations preserve object properties and logic connections. | Private test saves only. No PS4 round-trip claim. |
| Input | Pad timestamps, motion, deadbands, and button behavior match the expected game clock. | Unit tests and synthetic-motion captures. Physical-pad confirmation remains with the user. |
| GPU safety | No guard trips, malformed indirect arguments, assertions, or device loss in the tested scene. | Guards prevent known hangs. The indirect-argument producer is still unresolved. |

## Comparison procedure

1. Copy one known save into separate control and candidate directories.
2. Use the same game version, scene, input sequence, capture interval, and run duration.
3. Test default PS4 timing first.
4. Compare captures and log errors before comparing FPS.
5. Repeat at 60 FPS and high render FPS.
6. Measure gameplay updates and elapsed game time separately from render FPS.
7. Record the tested scene and untested areas with every result.

For physics and logic, compare elapsed-time outcomes rather than exact pixel hashes.
For images, align scene state and camera position before judging differences.
Use repeated runs to separate random scene variation from a rendering regression.
