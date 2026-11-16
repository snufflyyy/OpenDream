# Dreams versions: applying the fork's shader routes to any game build

Status 2026-11-15. Code: ref/shadps4 branch `docmatch`
(`src/shader_recompiler/dreams_route_match.{h,cpp}`, generated `dreams_route_table.inc`), port
branch `docmatch` (`tools/dreams_shader_routes.py`, `tools/dreams_routes.json`,
`run-dreams.sh` `DREAMS_SIGNATURE_MATCH`).

## 1. The problem

The Dreams-ShadPS4 fork fixes Dreams' GPU work shader by shader. It recognises a shader by the
hash the PS4 shader compiler embedded in the binary (`BinaryInfo.shader_hash`, shadPS4's
`pgm_hash`), captured from **CUSA04301 v2.64**. Every game build embeds new hashes, even for
byte-identical code (the fork's own `docs/GAME_FILES_INSPECTION_20260904.md`: v2.65 has the same
604 shaders, 546 with identical code, and 0 equal hashes). On any other version (the user's
CUSA08010 v2.52, a future v2.65, ...) none of the ~100 hash-gated paths engage:

- all 22 Dreams `DS_ORDERED_COUNT` (DOC) kernels fall back to a plain atomic
  (`Dreams DOC atomic fallback ... reason=kernel shape/feature gate`). DOC must return prefixes in
  wave-creation order; with the atomic they come in GPU arrival order, so sculpt compaction lists
  (traversal, surface extraction, visibility lists) are built in the wrong order and sculpts render
  as scattered blocks / holes (the "Wall of Doubt" and pink/teal blocks, captures
  `work/captures/20261115-191330`);
- non-DOC fixes (GatherVoxels LDS barriers, B1 writers, glass/shadow mip coordinates, ...) are off.

## 2. The fix: identify routes by what the shader *is*

With `SHADPS4_DREAMS_SIGNATURE_MATCH=1` (and a Dreams serial, `Common::IsDreamsSerial`),
`Shader::Info`'s constructor calls `DreamsRoute::Resolve(code, guest_hash)`. When the guest program
implements a fork route, `Info::pgm_hash` becomes that route's **v2.64 hash**, so every existing
hash check (DOC lowering, rasterizer collect/prefix/replay, recompiler fixes, cache suffixes)
applies unchanged. `Info::guest_pgm_hash` keeps the real hash; `Info::dreams_route_alias` marks the
program.

Rules, first match wins:

| # | rule | log `via` | when |
|---|---|---|---|
| 1 | `pgm_hash` is a fork hash | `native` | v2.64 itself (no change) |
| 2 | **code hash** equals one recorded for the route | `code` | code byte-identical to a build already identified (v2.52 today) |
| 3 | **DOC signature** equals exactly one route's, and that signature is unique among routes | `doc-shape` | code changed, ordered-count interface unchanged |

Each guest hash is decided once and logged (VCORE_LOG_FILTER default shows warnings):
```
gpu warning (Render.Recompiler): Dreams route: guest 0x49d0daba -> 0xb535c6c8 Traversal (...) via code (code 0xc671f8f0c5f3d513)
gpu warning (Render.Recompiler): Dreams route: guest 0x... DOC kernel has no route (none; code 0x... doc 000c@226)
```
Then the rasterizer's own logs use the route hash (`Dreams DOC atomic fallback hash=0x252ccde6 ...`
means the *route* refused, not the matching).

### Signatures (identical in Python and C++)

- **Code hash**: XXH64 (seed 0) of the code span = `BinaryInfo.length` bytes from the program
  start (the `s_mov_b32 vcc_hi, #lit` token). This is exactly `ShaderParams::code`; it does not
  contain the embedded hash. Same definition as the fork's "identical declared code spans".
- **DOC signature**: for each `DS_ORDERED_COUNT` in program order `<control>@<counter>`, comma
  separated. `control` = shadPS4's packed control without the GDS binding
  (`((offset1>>4)&3) | ((offset1&3)<<2) | (offset0<<8)`). `counter` = guest GDS dword
  `((base & 0xfffc) + offset0) >> 2` where `base` (bytes) is the first literal with a zero low half
  found by following the last M0 write back through up to 4 scalar definitions. Run-time terms
  (radix pass, row, wave index shifted into M0) are registers and are not part of it. The counters
  are the same numbers the fork hard-codes (`TraversalOrderedCounterIndices` 0x142/0x146/0x143/0x144,
  `SceneCompactOrderedCounterBaseDword` 0x900, `GatherInputOrderedCounterFirstDword` 0x5d, ...).

Why not a hash of a "normalised" instruction stream: on v2.52 every identified route has the same
code length as v2.64 and every fork trace PC lands on the same instruction (section 4), so plain
code bytes already identify them; a normalised hash would only add false matches. The DOC
signature is the structural fallback for builds whose code did change.

### Cache

Route-matched programs are compiled every run and never written to the pipeline cache (their
cache keys use the route hash, which would collide with real v2.64 programs). With matching on,
the cache preload also skips stages that could match (any DOC program, and the guest hashes in the
table), so stages compiled earlier without the route are recompiled.

## 3. The route table (v2.52 -> v2.64)

`tools/dreams_routes.json` (source of truth) + the code hashes from the packs ->
`dreams_route_table.inc` (`dreams_shader_routes.py emit`). `key` is the `dd.shaders` entry key,
which is stable across builds (all three keys the fork published map to the same shaders in v2.52).

| fork (v2.64) | route | v2.52 hash | pack key | DOC signature | identified by |
|---|---|---|---|---|---|
| b535c6c8 | Traversal | 49d0daba | 72b79162bade5a7d | 0804@142,1804@146,0c04@143,100c@144 | DOC controls+counters; 20 fork trace PCs on the same instructions |
| 2bfebd3c | QueueProducer | 6e312656 | 7332d0d093b1e4c9 | 000c@142 | counter 0x142 (= 322) |
| 692f0f7f | QueueProducerAlt | a54a6874 | 087cd3031850f1ff | 0c04@143,140c@145 | controls 0x0c04/0x140c, base 0x500 |
| cbac06d2 | CompactClassify | f25b32e9 | 097df0f5cc4e7df1 | 0004@2e0,040c@2e1 | fork's pack key + 2308-byte size; DOC PCs 0x304/0x338; 24x1x1 dispatch as in the fork capture |
| d4532ff4 | CompactScatter | 4b5e25da | 5ff1f30f1383a6d6 | 000c@1e8 | counter base 0x1e8 |
| 81cf58cf | SelectionListCompact | 650ddaf5 | 550dfbc11f7e95af | 000c@a40 | counter 0xa40 |
| 3937a849 | SceneCompact | 9c44f797 | 1a34ba90aa1aaa91 | 0004@900,...,0c0c@903 | controls, base 0x900, 1024 threads, trace PC 0x9c |
| 90272fc4 | IndirectArgs | 61e02868 | 99c1e623dd39f300 | 0004@a7,000c@a6 | row counters 0xa7+, global 0xa6 |
| d8b4ddb5 | VisibilityCandidateCompact | 3ef3cadd | e4e51bfb8332c684 | 000c@155 | counter 0x155+pass |
| 95c28c88 | GatherInputCompact | c97f45af | 20a8554b512230a6 | 000c@5d | counter 0x5d+pass, pass in SRT word 9 |
| 7aa925e9 | VisibilityListCompact | c4150c3c | 422e66e41cfa8b6c | 0004@15f,040c@160 | counters |
| 016b9f6a | VisibilityListCompactAlt | 86a0a18c | efafe0c14c627cda | 0004@161,040c@162 | counters |
| 84aa3dc9 | SculptVolumeWriter | 9a28f02c | 1d06b993301fe08e | 000c@78 | fork's pack key + 1048-byte size; counter 0x78 |
| 4ebeffd2 | B1SeedWriter | 7316c183 | 833ba1c24f3bc611 | 000c@79 | fork's pack key + 364-byte size; counter 0x79 |
| f030fdc4 | SculptSurfaceCompact | 3655c8be | b985d2efcf6a4a85 | 000c@100 (shared) | the 0x100 kernel that gathers from the density volume; 4x4x4; the emitter's exact control assert (binding 5) passes |
| 5ac53394 | SculptSurfaceFinalize | 3f473d70 | 83c2875bbe709d60 | 000c@100 (shared) | 3 buffers before GDS; 4x4x4; control assert (binding 3) passes |
| 252ccde6 | alias-proved compaction | af1f07c9 | c0a99cd3b5f6674b | 000c@400 (shared) | direct, divides by the SRT divisor; route also checks its audited SPIR-V |
| fff20d00 | indirect compaction | e40d1672 | a4bd044cc56ab60b | 000c@400 (shared) | the indirect 0x400 kernel |
| 6f408bbc | SunShadowPartitionByCascade | a777e8df | aed0b33747440486 | 0004@e4,... | base 0xe4 |
| a25580f7 | SunShadowPartitionByCascadeSecondPass | 75ccfb5a | 751d1bba35e8f7c1 | 0004@e8,... | base 0xe8 |
| bce8c0fe | SunShadowSortDrawKeys | efe69ad9 | 219c124a2a5ead2c | 000c@da | base 0xda |
| 4bc902cf | stateful 2-counter compaction | e2a29703 | 7a321923ef895a95 | 0004@200,040c@201 | base 0x200 |
| 7ba4de5d | GatherVoxels | a0a32846 | 246a56e32ad878a1 | - | only v2.52 shader with the fork's 15 trace PCs (0x180 cmp vs 1023, 0x229c GDS add, stores 0x2278/0x22e0/0x3c44 with the documented registers) |
| a3a9e9ef | B1TransformWriter | 1e4b6169 | 0b43adf3d7d23b0f | - | B1 store at 0x2020 (v0), predicate at 0x1f04 |
| 63ddac84 | A3LookupProducer | 28ed40dd | acebdb2fe22583a4 | - | atomics at 0x12c / 0x1ac |
| d049fb84 | ModelFinalize | 6c1424ae | 55f16b0ad6c08b40 | - | V_OR v7,v16 at 0x4d0 |

"shared" signatures are not unique among routes, so rule 3 never uses them (a future build with
changed code for these four needs its code hash added). Four v2.52 DOC kernels have no fork route
(6324fef1 `0004@210,040c@211`, ce5dd525 and fc2a008a `000c@226`, c7a4a377 `000c@100`); they keep
the atomic path, as they would on v2.64.

## 4. Evidence that v2.52 runs the same kernels as v2.64

- The three pack keys the fork published (`GAME_FILES_INSPECTION_20260904.json`) exist in v2.52
  with the same code sizes (2308, 1048, 364 bytes).
- Every fork trace PC checked lands on the documented instruction in v2.52: b535 (SAVEEXEC 0x29c,
  CNDMASK->v18 0x82c, CMPX v2 0x918, NGE 0xafc/0xb38, SAVEEXEC 0xe0c, ten stores 0x1334-0x1548),
  GatherVoxels (15 PCs incl. the stores with the documented row/value registers), B1 transform
  0x2020, A3 0x12c/0x1ac, ModelFinalize 0x4d0, SceneCompact 0x9c, cbac DOC 0x304/0x338.
- All DOC counters equal the fork's constants; dispatch shapes in the run logs match the fork's
  (4x4x4 for f030/5ac, 1024 threads for SceneCompact, cbac 24x1x1).
- The emitter's exact-control assertions (which include the GDS binding) pass for 4eb, d8, 95c,
  f030 and 5ac.

## 5. Adding a version

```
cd port
python3 -I tools/dreams_shader_routes.py check 2.52=../game/CUSA08010/shaders/dd.shaders \
    2.64=<v2.64 game>/shaders/dd.shaders
```
`check` finds each route in each pack by fork hash, else by pack key, and prints hash, key, code
hash and DOC signature, flagging a differing signature or key. For v2.64 this verifies every
inference above (the fork hash must sit at the listed key). Then:
```
python3 -I tools/dreams_shader_routes.py emit 2.52=... 2.64=... 2.65=... \
    -o ../ref/shadps4/src/shader_recompiler/dreams_route_table.inc
```
records the code hash of every pack (v2.64 entries are skipped: rule 1 covers them). With a v2.64
pack, **unmapped fork routes can be added by key**: look up the fork hash in the 2.64 census
(`census --all 2.64=...`), copy its key into `dreams_routes.json` (no DOC or anchor work needed),
and `emit` with all packs.

`census PACK` lists every DOC kernel (`--all`: every shader) with key, hash, length, code hash,
signature and route.

## 6. Not mapped yet (need a v2.64 pack, or per-shader work)

66 other fork hashes are not in the table. Without a v2.64 `dd.shaders` they need per-shader
identification like section 4. Most are diagnostics in `vk_rasterizer.cpp`. The rendering ones:
- recompiler fixes: glass objects 2b085c6c/15f114ff/619002ef (`scalar_alu.cpp`), detail/region
  5650b8f7, 44c189d5, saved wave masks 29569533/61b54af5, temporal resolve 48a95ab7 and spatial
  reconstruction 80aed032/b1b1758b (LDS barriers), GPU scalar reads bb773aed;
- resource patching: glass mip 345893a7, sun/spot shadow mips e165304b/9f3303a7/cfbf6b00/5e8446a4,
  array LOD 34267ace/3091e6c1, sprite cull fd2a2c3b/e4dcd599/0ffa5e6b;
- sculpt/fleck drawing: VS 3706083c / d25db925, FS ce3b8413 / 9d74c568 / dcc325c2 / 3f6e1a00;
- linear write ranges c4947c72 d9a5eaaf 82719fa8 6a7e1109 e69537e9 4f493234; DMA read sets
  9e9c1c71/dcc325c2/fffa32b6 (these check their compiled SPIR-V themselves).
With a v2.64 pack each is one `key` line in `dreams_routes.json` (section 5).

## 7. Switches

| switch | default | effect |
|---|---|---|
| `SHADPS4_DREAMS_SIGNATURE_MATCH=1` | off in shadPS4 (emulator behaviour unchanged) | rules 2 and 3 |
| `DREAMS_SIGNATURE_MATCH` (run-dreams.sh) | **1** | exports the above; `=0` for A/B runs |

## 8. Result on v2.52 (2026-11-15, agent runs, same scripted input)

| | matching off (`run-20261115-215739`) | matching on (`run-20261115-215358`) |
|---|---|---|
| `Dreams route:` lines | 0 | 26 (all `via code`) |
| DOC atomic fallbacks | 22 | 1: `252ccde6 reason=252 unaudited translated binary` |
| assertions / STOP | none | none |
| homespace scene behind the main menu | white blocky patches | smooth (no patches) |

Compare sheet: `work/captures/20261115-215358/sheets/compare-20261115-215739.png` (left on,
right off). Inside the homespace (full-width captures, ○ from the main menu; on
`run-20261115-220022`, off `run-20261115-220312`, sheet
`work/captures/20261115-220022/sheets/compare-20261115-220312.png`): with matching the clouds are
Dreams' painterly brush-stroke flecks and the sculpted puppet is solid
(`work/captures/20261115-220022/f00023_t00075.7.png`); without, the clouds are flat white
rectangular blocks and slabs (`work/captures/20261115-220312/f00022_t00078.5.png`). The one remaining fallback is the route's own guard: the 252 replay only runs when
the translated SPIR-V equals the fork's audited binary, and the rebased recompiler (upstream
shadPS4 + fork) emits different SPIR-V. That affects v2.64 the same way (rebase issue, not a
version issue); the shader keeps the atomic path as before.


## 9. Fail-safe: a route that doesn't fit is refused, not fatal (branch docmatch2)

The fork's route lowering asserts on the exact IR shape it was written for (phi layouts, DOC
controls with GDS binding, resource counts, ...). An assertion used to stop the game (exit 23).
Now (`vk_pipeline_cache.cpp` `CompileModule`) a routed program (`Info::dreams_route_alias`) is
translated and emitted inside a `Common::RecoverableAssertScope` (`common/assert.h`): a failed
`ASSERT`/`UNREACHABLE` on that thread throws `Common::RecoverableAssertion` instead. The route is
then refused for that guest hash (`DreamsRoute::Refuse`, logged
`Dreams route: guest X -> Y REFUSED, using the generic path: <assertion>`), `Info` is reset to
the guest hash, and the program is compiled on the generic path (atomic DOC fallback). Nothing has
been created on the device when translation or emission throws. Later `Resolve()` calls for that
guest hash return the guest hash. Limits: a refusal in a *later permutation* of a program whose
first permutation used the route leaves that one permutation generic under the route's host
path (logged as an error, not seen so far); the refusal is per process (next run tries again).

Test/diagnostic switches (pass `VCORE_KEEP_SHADPS4_ENV=1` through run-dreams.sh):
- `SHADPS4_DREAMS_ROUTE_FORCE_REFUSE=<route hex>`: fail that route at the end of `EmitSPIRV`,
  after every route emitter ran. Verified with 84aa3dc9: refused, generic DOC fallback, game kept
  running (`work/logs/run-20261115-221903.log`).
- `SHADPS4_DREAMS_ROUTE_IR_DUMP=<route hex>`: IR + ASL dumps of that routed program at
  pre-res-discover / pre-res-patch / pre-lower-phi / final, as
  `work/vcore-user/shader/dumps/cs_<route>.pid<pid>.g<guest>.<stage>.irprogram.txt`.

### The intermittent 84aa stop (run-20261115-220815: "Dreams 84aa checkpoint value 7 is not a phi")

Not a different shader variant or cache state. 84aa's lane-active value at the DOC cut is
`Phi [#true], [#true]` (pre-lower-phi dump); upstream's post-structurizer SSA rebuild
(`LowerPhisToRegsPass` + `SsaRewritePass`) folds it to the immediate `#true`, every time. The
fork's emitter (written before that rebuild existed) called `Arg(2).Inst()` on it: the union's
bits were read as an `IR::Inst*`, so the "is a phi" check depended on leftover memory. When it
happened to pass, the post-DOC merge called `SetDefinition` through that garbage pointer (memory
corruption). Fix (`emit_spirv_atomic.cpp` `EmitDreamsSculptCheckpoint`): `TryInst()`, constant
state values need no restore (identical in Collect, Replay and native), assert only on real
shapes (constant-false lane-active, or a non-phi instruction).
