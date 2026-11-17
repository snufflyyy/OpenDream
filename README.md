# OpenDream

A native Linux x86-64 port of **Dreams** (Media Molecule, PS4 — `CUSA08010`), in the style of
bbport: the game's own code runs directly on the host CPU, a C runtime replaces the PS4 system
libraries, and a Vulkan renderer derived from [shadPS4](https://github.com/shadps4-emu/shadPS4)
(with the Dreams fork's patches) runs the GPU side. No CPU emulation, no JIT, no static
recompilation.

It boots natively to the menu and homespace, runs offline services, input, audio and video, and
can reach built-in Media Molecule games and the Dream Shaping editor. It is a work in progress:
rendering accuracy, scene coverage and original-PS4 parity are still being worked on, and many
scene workflows are unverified. The priority feature is the **offline editor / creation tools**.

Start reading at [`AGENTS.md`](AGENTS.md), then `port/docs/dreams/INDEX.md`. No game data is
included or distributed here — you need your own copy of Dreams.

---

## AI disclaimer

**This project was completely done by AI.** Every source file, shader route, test, tool and
document in this repository was written by large language model agents running in an autonomous
coding harness, under human direction. A person chose the goals, ran the game, and accepted or
rejected results; the agents wrote the code.

Treat it accordingly: it is an experiment in AI-built software, not a product. Expect missing
features, incorrect behaviour, unverified claims and stale comments, despite the project's own
accuracy and testing rules. Nothing here has been reviewed by a human to normal engineering
standards, and there is no warranty of any kind. Dreams and its content are the property of
Media Molecule / Sony Interactive Entertainment; this repository contains no game assets.
