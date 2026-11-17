# AI development architecture

Working directory: `$REPO` (your OpenDream checkout). This policy is host-independent: it
applies to agents on any machine. The reference machine for measurements is
`TEST_HARDWARE.md`.

## Purpose and scope

This document defines responsibility, delegation, and acceptance rules for AI work on OpenDream.
The user assigns the main Pi assistant as orchestrator and authorizes it to dispatch DeepSeek subagents.
The user now assigns most implementation and debugging to DeepSeek to reduce inference cost.
The orchestrator advises, assigns work, reviews evidence, and keeps agents on track.
This supervisor-first policy replaces the earlier lead-as-primary-implementer arrangement.

This is an operating policy, not an installed model-routing configuration.
This document does not launch agents or claim that any model endpoint is configured.

OpenDream runs the PS4 executable natively on x86-64.
Its main compatibility work concerns PS4 services, GPU shader translation, memory, synchronization, and engine behavior.
Refer to [ARCHITECTURE.md](ARCHITECTURE.md) for the port design.

## Model roles and routing

| Role | Requested model | Responsibility |
|---|---|---|
| Orchestrator | GPT 6.1 Sol | Assign bounded tasks, advise on architecture, review evidence, and own integration and acceptance. |
| Implementation worker | DeepSeek V4.1 Flash | Own most diagnosis, runtime/GPU fixes, tests, tooling, and documentation within the assigned scope. |
| Investigator | DeepSeek V4.1 Flash | Trace source paths, compare reference implementations, and analyze existing evidence. |
| Reviewer | DeepSeek V4.1 Flash | Independently review patches and test evidence. Escalate unresolved risks to the orchestrator. |

Confirmed session routes:
- Orchestrator: `openai-codex/gpt-6.1-sol`.
- DeepSeek workers: `rightnow/deepseek-v4-1-flash-1017`.

These routes establish model identity, not benchmarked capability.
Recheck availability before dispatch after a configuration change.

### Available RightNow models (provider `rightnow`)

The `rightnow` provider is configured once in the agent-level `models.json` (outside this repo).
It reads the key from the `RIGHTNOW_API_KEY` environment variable; never copy the key into this
repo, a task prompt, a report, or a log.

- Endpoint: OpenAI-compatible `https://api.rightnowai.co/v1` (`openai-completions`).
  The same models also answer Anthropic-compatible Messages at `https://api.rightnowai.co`.
- Live list: `GET https://api.rightnowai.co/v1/models`; use an id whose `availability` is
  `available` and whose `supported_endpoints` includes `chat_completions`.
- Configured chat models (verified 2026-11-16, all tool-capable, 262K-1M context):
  `deepseek-v4-1-flash`, `deepseek-v4-1-flash-1017` (worker route, text+image), `glm-5-3`,
  `glm-5-3-flash` (text+image), `nemotron-3-5-lightning-30b` (no thinking),
  `ornith-1-5-35b` (text+image, no thinking), `qwen3-8-27b` (text+image), `step-5-preview`
  (text+image, thinking always on, no effort control).
- Not configured: `system-zero` / `rightnow-decision`. They serve the `decisions` endpoint, not
  chat, so they are not coding-agent models.
- Thinking levels map to the gateway's `reasoning_effort` values; `glm-5-3*` cannot disable
  reasoning, so `off` clamps to `low`. `step-5-preview` accepts no effort value.
- Cost in `models.json` comes from the live model list, including the cached-input price.
  RightNow also publishes a cheaper "flex" tier that the config does not model.

### Image budget (8 images per request)

RightNow hosted inference rejects a request that carries more than 8 images across the whole
conversation, including images from earlier turns:

```
400 ... "Hosted inference accepts at most 8 images per request."
code: hosted_parameter_not_supported
```

Pi does not prune history for a model's `inputLimits.images.maxPerRequest`, so the agent-level
extension `~/.pi/agent/extensions/rightnow-image-budget.ts` trims older images on every request to
RightNow image models. It keeps the newest 6 (headroom under 8), replaces each dropped image
with a short text placeholder, and leaves the stored transcript untouched — the trim is
request-local. `RIGHTNOW_MAX_IMAGES=<n>` overrides the budget.

- Verified 2026-11-16: 9 images in one prompt fail with the 400 without the extension and
  succeed with it (6 images reach the provider); 10 images across two turns also succeed.
- Foreground pi-subagents children do not load ambient parent extensions, so a subagent that must
  read screenshots should list this extension in its agent definition; background children load
  ambient extensions by default.
- If a task needs more than 8 distinct images at once, split it across turns (older images are
  dropped) or ask the human to resend the few that still matter.

To use one, pin the exact route, for example `rightnow/glm-5-3-flash`.

Before dispatch, resolve the requested DeepSeek model to an exact configured provider/model ID.
Pin that route for each subagent.
Do not silently substitute another model or provider.
If the route is unavailable, report the blocker and continue independent lead work.
Never put API keys in task prompts, reports, or project documentation.

## Concurrency and ownership

**At most three DeepSeek subagents run concurrently.** The orchestrator does not count toward this limit.
The user explicitly increased the earlier two-worker limit.
Use the third slot for independent implementation, investigation, or review.
Do not create concurrent work merely to fill the available slots.
Only the user can authorize a further increase.

After connection loss, inspect the subagent fleet and saved GPU lease before dispatch.
Resume existing owners instead of repeating their assignments.
If another owner changed shared state, stop conflicting work before any overwrite.
Preserve both versions of conflicting files and reconcile them before tests or integration.

- Give each subagent one bounded deliverable.
- Default investigation and review tasks to an inspection-only tool profile.
- Do not rely on prompt wording alone to prohibit execution or mutation.
- Require separate scope approval for binary analysis, helper builds, or new diagnostic tooling.
- Permit source edits only within explicitly assigned files and an isolated worktree.
- Give each runtime worker its own scratch scripts, reports, and copied saves.
- Assign one writer per file and one owner per output directory.
- Reserve shared architecture decisions and final integration for the orchestrator.
- Do not let subagents delegate further, merge work, or commit without explicit authorization.
- Bound CPU tests and builds to preserve resources for the user's work.

The project contains nested repositories and existing uncommitted work.
Before edits, inspect the root, `port/`, and `ref/shadps4/` working trees.
Do not reset existing work or assume that a new worktree contains uncommitted fixes.
Record the base revision and any required local patch for each task.
Preserve the local shadPS4 branches and backups described in the root `AGENTS.md`.

## Implementation and escalation

Assign most remaining port work to DeepSeek, including difficult fixes with a clear scope and verification plan.
Do not reserve all runtime/GPU implementation for the orchestrator.
Do not delegate an open-ended instruction such as "fix the renderer."

Require an orchestrator review of the proposed approach before changes to:

- GPU synchronization, barriers, readbacks, and command submission order.
- Shader translation and ordered-count behavior.
- Memory mapping, sparse residency, shared backing, and pointer lifetime.
- Simulation timing, physics, logic, and complex PS4 service semantics.
- Shared runtime/vcore contracts or test acceptance criteria.

After approach approval, DeepSeek can implement the change and its regression tests.
Require verified evidence before accepting any root-cause claim.
Escalate conflicting evidence, repeated failed attempts, and architectural uncertainty to the orchestrator.
The orchestrator implements directly only for a justified escalation or an explicitly assigned task.
Keep all work within the project's offline-only scope.

## Dispatch contract

Every task must specify:

1. Objective: one question or implementation result.
2. Context: relevant requirements, confirmed facts, and unresolved hypotheses.
3. Route: the exact provider/model ID.
4. Scope: allowed files, prohibited changes, and read-only or edit access.
5. Baseline: repository, revision, worktree, and required local changes.
6. Deliverable: report location or patch, plus explicit acceptance criteria.
7. Verification: bounded commands, expected outcomes, and evidence locations.
8. Stop conditions: time/resource limits, missing evidence, conflicting edits, or unsafe behavior.

Require this return format:

```text
Task and route:
Base revision and local changes:
Files inspected or changed:
Findings, with source locations:
Evidence and commands executed:
Tests passed / failed / not run:
Hypotheses and remaining uncertainties:
Patch or report path:
Recommended next action:
```

Require subagents to distinguish observation from inference.
Do not accept "looks correct" or a successful build as proof of gameplay accuracy.
If a subagent reaches a stop condition, require a partial report instead of expanded scope.

## Development cycle

1. Read the root `AGENTS.md`, [HANDOFF.md](HANDOFF.md), and [STATUS.md](STATUS.md).
2. Identify one reproducible failure or one missing test capability.
3. Define correctness criteria before dispatch.
4. Dispatch bounded investigation, implementation, and test tasks to DeepSeek.
5. Review high-risk approaches before the worker implements them.
6. Review all returned patches before integration.
7. Assign a reviewer that did not author the change.
8. Run applicable tests and inspect the actual evidence.
9. Rerun the affected scenario and earlier regression scenarios.
10. Update the handoff, status, and worklog with results and limits.

Do not mark incomplete work as complete.
Record failed tests and missing scenario access as blockers.
The reviewer advises. The orchestrator owns the final acceptance decision.

## GPU runs and acceptance gates

**Only the orchestrator grants a GPU run lease.**
Assign at most one named runtime worker a lease at a time.
The lease must specify the allowed wrapper, timing mode, run count, duration, and private output paths.
The designated DeepSeek worker can launch the authorized runs and inspect their results.
All other workers inspect existing artifacts or perform CPU-only work.
Do not transfer the lease until the previous run stops.

Use the project-root `tools/agent-run.sh` entry point for every agent game run.
Keep runs bounded, headless, and isolated with private saves and synthetic input.
Never override the single-instance lock or use the player's saves as test output.
Respect exit 75 and user-priority termination. Do not compete with the user's game.

CAUTION: Save work before GPU runs. A GPU hang can reset the user's entire desktop.
Headless isolation does not isolate the physical GPU.
See [AGENT_RUNS.md](AGENT_RUNS.md) for the run policy.

Acceptance requires evidence appropriate to the change:

- Add a regression test that exposes the original defect where practical.
- Run applicable unit and contract tests, then the relevant build.
- Verify scene identity and behavioral checkpoints for gameplay claims.
- Compare default PS4 timing before high-FPS modes.
- Check captures, error logs, guard trips, and simulation cadence separately from render FPS.
- Preserve precise readbacks, shader routes, synchronization, and hang guards.
- Treat guard trips and malformed-draw clamps as correctness failures, not successful fixes.
- Report `passed`, `failed`, `blocked`, and `unverified` outcomes separately.

Menu FPS does not prove physics, editor, progression, or save correctness.
Agreement with shadPS4 does not prove hardware parity because both paths share GPU code.
Use original-PS4 evidence for accuracy claims.
Refer to [ACCURACY.md](ACCURACY.md) and [SCENE_TESTS.md](SCENE_TESTS.md).

## Initial assignments

These are proposed task boundaries, not completed or running tasks.
Recheck the latest handoff before dispatch.

| Owner | First assignment | Required result |
|---|---|---|
| Orchestrator | Assign tasks, approve risky approaches, and review returned evidence | Clear next tasks, accepted patches, and explicit unresolved blockers. |
| Runtime worker | Establish Please Hug Me entry, then reproduce movement/progression behavior under a GPU lease | Confirmed scene checkpoint and evidence before a fix. |
| Test/tooling worker | Add missing scripted-input coverage and build bounded scenario utilities | Tests and reports that distinguish real passes from blocked or unverified outcomes. |
| Reviewer | Review worker patches and returned test evidence | Source-based findings, missing cases, and explicit verification limits. |

Assign the malformed indirect-draw diagnosis and implementation to a bounded DeepSeek task.
Require orchestrator approval for synchronization or shader changes before implementation.
Keep editor creation, logic, physics, undo/redo, and save/reload in the regression scope.

## Verification

Run these documentation checks from the project root (`$REPO`):

```sh
test -f port/docs/dreams/AI_DEVELOPMENT.md
grep -F 'AI_DEVELOPMENT.md' AGENTS.md
grep -F 'At most three DeepSeek subagents run concurrently.' port/docs/dreams/AI_DEVELOPMENT.md
grep -F 'Only the orchestrator grants a GPU run lease.' port/docs/dreams/AI_DEVELOPMENT.md
python3 -I port/tools/doc_lint.py
git diff --check
git -C port diff --check
```

These checks verify documentation structure and whitespace, not model routing or game behavior.
`doc_lint.py` is the normative-doc gate; see `CONVENTIONS.md`.
Next: assign the next bounded implementation task to DeepSeek and review its evidence before integration.
