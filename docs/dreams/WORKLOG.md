# Work log (newest last; append, don't rewrite)

## 2026-11-15

- Researched options. PS4Recomp (wie-project) is macOS Apple-Silicon only, so it can't target this
  machine. Picked the bbport model instead (decision D1).
- Found the Dreams shadPS4 fork (JesusDaBest/Dreams-ShadPS4). Per its notes it reaches offline
  menus, DreamShaping and edit mode. It's a repo of per-build `.patch` files + binaries targeting
  CUSA04301 2.64. Upstream shadPS4 issues #2812/#3141 show stock shadPS4 fails on Dreams' shaders
  and compute pipelines.
- Extracted the user's fpkg → `game/CUSA08010` with our own extractor (`tools/pkg_extract.py`),
  fully verified (see RUNNING.md). Deleted the .pkg at the user's request after verification.
- Import analysis (`notes/dreams-imports.md`): 812 eboot imports. bbport covers 510; 269 of the
  302 missing have shadPS4 HLE implementations to borrow.
- bbport reuse audit (`notes/bbport-reuse-plan.md`): keep CPU side, replace GPU side (D2).
- Milestone 1: built bbport on Fedora (four environment fixes, see BUILD.md). The Vulkan smoke test
  passes on RX 7800 XT RADV.
- Milestone 2: prepare.py + link_modules.py (with libSceJobManager.prx) run unmodified on Dreams
  with `BB_SKIP_GAME_CHECK=1`.
- Created `port/` (git clone of ref/bbport, branch `dreams`) with `build-dreams.sh` and
  `run-dreams.sh`.
- First `--cpu-only` run: image maps, relocations apply, libc.prx init starts, then it stops at
  `_sceKernelSetThreadDtors` (rNhWz+lvOMU) called from libc.prx+0x790. 416 imports were
  unresolved at load.
- Found two systemic reasons implemented functions didn't bind:
  1. NID→name tables only listed Bloodborne's imports → added generated
     `src/import_names_dreams.inc` (`tools/gen_import_names.py`, 921 names);
  2. `runtime_thread_resolve` matched whole `NID#lib#mod` strings with Bloodborne's letter codes
     → now matches on the NID only.
- Implemented `_sceKernelSetThreadDtors`; the registered hook runs at guest thread exit, before
  TSD destructors (shadPS4 order).
- Learned that shadPS4 binds every unimplemented import to a log-and-return-0 stub
  (`core/aerolib/stubs.cpp`), and Dreams is in-game that way on the fork. Gave our loader the same
  fallback (`STUB:` lines, log once); `DREAMS_STRICT_IMPORTS=1` restores stop-at-first.
- Dreams `main()` runs natively. Next stop was Ajm AAC → added a silent MP3/AAC decoder for now.
- `--cpu-only` then idles in the main loop: GPU fences never signal.
- Ran with bbport's GPU library (`BB_GAME_PROFILE=none`). The Vulkan presenter comes up, the
  game submits real GPU work, and the first compute shader fails on `DS_ORDERED_COUNT`, the opcode
  the Dreams fork implements. This confirms the next step is milestone 6/7 (vcore from
  `ref/shadps4` branch `dreams`).
- Tools added: `tools/unresolved_report.py` (group `BB_LIST_UNRESOLVED` output by library with
  shadPS4 refs), `tools/stub_report.py` (→ `docs/dreams/STUBS.md`).
- Patch-stack agent finished: `ref/shadps4` branch `dreams` = upstream `f3687cc4` (2026-11-15) + the
  Dreams fork's 46 incremental patches, ported per subsystem (10 commits; full report
  `notes/shadps4-dreams-rebase.md`). `5d41108e` makes the fork's ~160 `CUSA04301` gates also accept
  CUSA08010 / `SHADPS4_DREAMS_TITLE_IDS`. The `DS_ORDERED_COUNT` translation itself is generic
  (`frontend/translate/data_share.cpp`), but ~60 per-shader paths are matched by shader hash
  (v2.64 hashes) and the eboot patches check exact bytes, so on 2.52 those fall back or skip.
- Reference run of the rebased **emulator** (`tools/run-shadps4-reference.sh`, fork launcher's
  config + env, state in `work/shadps4-ref/user/`): Dreams runs 150 s with no crash, signs in
  offline, and the GPU thread is busy in Dreams DOC / texture-alias / inline-table paths. Logged
  "DOC atomic fallback ... feature gate" for some hashes, and both eboot patches "did not match
  this executable" (expected on 2.52). What is on screen isn't verified yet (needs the user's eyes).
  Gotcha: seeding a save under the old `user/savedata/` layout makes current shadPS4 open a
  blocking "Save Migration" SDL dialog. The current layout is `user/home/1000/savedata/<serial>/`.
- Lantern Engine review (decision D8 in ARCHITECTURE.md): it vendors near-upstream shadPS4
  video_core + shader_recompiler with a shim, which is the template for vcore. Its renderer lacks
  the Dreams fork's work and is 337 commits behind, so it's not the base. Dispatched an agent to
  build vcore (port/vcore/, see VCORE.md when it lands).
- The user ran `tools/run-shadps4-reference.sh` and confirmed Dreams boots to the
  controller-introduction menus in the rebased emulator.
- Replaced semantically wrong return-0 stubs with real behaviour in new `src/runtime_dreams.c`
  (error dialog, Move, Hmd info, Np account, random, time zones, AudioIn, camera, savedata mount
  info) plus flexible-memory size and thread name/id. All verified to bind in a `--cpu-only` run
  (`work/cpu-only-run6.log`). Full list: IMPORTS_LOG.md.
