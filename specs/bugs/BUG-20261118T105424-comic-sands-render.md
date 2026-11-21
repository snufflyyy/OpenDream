---
bug_id: BUG-20261118T105424
status: open
severity: high
scope: renderer
title: Comic Sands opens with only the sky visible
---

## Problem

The shipped Media Molecule game Comic Sands is reported to show the sky but not the hand-drawn opening scene. The game should show the character, ground, and drawn scene at startup.

Security impact: NONE. No security exploit path identified.

## Reproduce

Use Dreams v2.52 (CUSA08010), the default timing and GPU settings, and the headless agent runner with a copy of a private save. In DreamSurfing, open Comic Sands from the Media Molecule row and select Play. A menu card or background is not evidence of reaching the scene. Capture the asset-load log and opening frames.

Historical baseline: `work/logs/run-20261116-171852.log` requests the pinned playable scene (`da76f79cbd5b1aa/v0e9b1cbee7e8a7`). In `work/captures/20261116-171852/`, `f00185_t00195.8.png` shows the Comic Sands title and a transient effect; `f00188_t00198.9.png` through `f00194_t00205.0.png` show only the cloud background. The older `20261116-144105` run also entered Comic Sands, but exhibited distinct checkerboard/triangle corruption and showed part of the cyan character. The comparison is not a controlled A/B.

## Isolate, hypothesize, verify

In progress. All 533 metadata-declared assets in the local map and scene are present in the package; successful runtime loads and asset semantics are not established. A known ordered-count fallback runs before scene entry and is not yet causal evidence. The current build reproduces in `work/comic-sands/trace-3/`: pinned scene preload at line 1372; title+pencil at 54.9 s; sky and jump prompt at 67.9–84.0 s. The graphics trace logs new pairs after load but deduplicates steady draws, so it cannot distinguish missing submission from draws that make no visible contribution. An input run (`work/comic-sands/gameplay-2/`) confirms that the jump prompt disappears after Cross; rightward input and a second jump do not restore the character or ground. The computation window records sprite culling and indirect-argument generation in this state; dispatch records do not establish their results. The private draw-window observer (`work/comic-sands/observer-1/runner.log`) sees the known proxy and fleck draw keys in Homespace but not at the jump prompt. The sculpt indirect draw is attempted in both scenes; the observer does not read its GPU-written count. The existing sprite readback tracer stalls in Homespace (`sprite-1`), so a smaller, one-shot snapshot of the effective cull inputs and populated commands is needed. No production renderer patch is approved until the missing content can be attributed to a specific failure.

### Paused with evidence (2026-11-20)

The defect reproduces at both 30 and 60 Hz. All 21 declared scene members were read in full; their interpretation is unverified. The tested draw uses a normal viewport and valid atlas formats. An atlas mip-zero experiment removed the hint text and did not recover the artwork; it is not a fix. The final composite already receives a layer without the artwork. The world pass's per-pixel hit flag disappears after Cross and material IDs collapse to one sentinel (`work/comic-sands/attachment-probe/bits-1/`). The next bounded check is the draw's flattened scene-record selector and bounds versus actual guest user data (`work/comic-sands/handoff/6948d1-record-gate.md`). That check was not run. Private diagnostics and reports are saved; no production patch was applied.

## Acceptance

- [x] Current-build baseline capture shows the failure after verified Comic Sands scene entry.
- [ ] A controlled comparison ties the failure to the proposed cause.
- [ ] A candidate from the same save and game version renders the expected player and scene; the test also observes movement.
- [ ] Precise readbacks, matched shaders, timing and GPU guards remain enabled; no guard trips, malformed draws, or device loss are accepted.
- [ ] Focused regression tests and the repository preflight pass.
