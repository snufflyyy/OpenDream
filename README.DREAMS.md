# OpenDream: the Dreams port in this directory

**OpenDream** — a native PC port of Dreams (CUSA08010) for the PS4.

This directory (`port/`) is the **native Linux x86-64 port of Dreams** (CUSA08010, PS4). It is a
fork of bbport 0.7, deadinside28's native Bloodborne port: the game's own x86-64 code runs on the
host CPU, a C runtime replaces the PS4 system libraries, and a Vulkan renderer derived from
shadPS4 runs the GPU side. No CPU emulation, no JIT, no static recompilation.

Start with the documentation map, not this file:

- `docs/dreams/INDEX.md` — every Dreams doc: purpose, when to read it, normative vs historical.
- `$REPO/AGENTS.md` — project layout, goal, rules for agents (`$REPO` = the OpenDream
  checkout root, one level up from here).
- `docs/dreams/HANDOFF.md` — what the last session changed and where to resume.

## What is what here

| Path | What it is |
|---|---|
| `src/` | The loader (`probe.c`), the C runtime (`runtime_*.c`) and the guest-facing service implementations |
| `vcore/` | The GPU library used by default: shadPS4's `video_core`/`shader_recompiler` (from `../ref/shadps4` branch `vcore`) compiled in place behind bbport's C API (`docs/dreams/VCORE.md`) |
| `gpu/` | **Legacy, non-default backend**: bbport's own renderer (`libbbgpu.so`) plus its AvPlayer sources. Selected with `DREAMS_GPU=bbport`; it stops at Dreams' `DS_ORDERED_COUNT` compute passes. Kept for A/B reference and because the runtime's AvPlayer sources and the `bbgpu.h` seam live here. |
| `run-dreams.sh`, `build-dreams.sh`, `build-deps.sh` | Run and build entry points (`docs/dreams/RUNNING.md`, `docs/dreams/BUILD.md`) |
| `tools/` | Port-specific tooling: route table, content inventory/closure, SPIR-V helpers, capture/config helpers. Project-wide tools are one level up in `$REPO/tools/`. |
| `tests/` | Unit tests and `run-dreams-tests.sh`; `tools/preflight.sh` is the cheap pre-submit gate (`docs/dreams/CONVENTIONS.md`) |
| `docs/dreams/` | The Dreams documentation (`docs/dreams/INDEX.md`) |
| `docs/` (other files) | Upstream bbport documentation (mostly Russian) — Bloodborne, not Dreams |

`README.md` in this directory is **upstream bbport's README** (Bloodborne, CUSA03173, Windows and
Linux, FSR/DLSS upscaling). It documents bbport's product, not Dreams. Where the two differ,
the Dreams docs under `docs/dreams/` are authoritative for this port.

## Minimum path to a running port

```
$REPO/AGENTS.md                     # layout, goal, rules
port/docs/dreams/STATUS.md          # what works today, current blockers
port/docs/dreams/BUILD.md           # build (Linux x86-64; Fedora example for packages)
port/docs/dreams/RUNNING.md         # run, switches, reading a STOP
port/docs/dreams/TEST_HARDWARE.md   # the reference test machine; real requirements
```

Agents: game runs only through `$REPO/tools/agent-run.sh` (headless, sandboxed, user-priority;
`docs/dreams/AGENT_RUNS.md`), and `port/tools/preflight.sh` before handing work off.
