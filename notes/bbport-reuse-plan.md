# bbport -> Dreams (CUSA08010 v2.52) native port: reuse plan

Status: research note, 2026-11-15. Read-only analysis of:

- `ref/bbport` (deadinside28/bloodborne_pc, shallow clone). All `path:line` references below are
  relative to `ref/bbport/` unless prefixed.
- `ref/shadps4`: official shadPS4. `origin/main` = `f3687cc4` (2026-11-15). The other agent's
  branches are `dreams` (at upstream main), `dreams-fork-reconstructed` (fork snapshot history,
  **no merge-base with origin/main**) and `dreams-fork-squashed`.
- `ref/dreams-fork` (JesusDaBest/Dreams-ShadPS4: patches + docs; the fork's upstream base is
  shadPS4 `555c458c`, 2026-08-15, 467 commits behind current main; `docs/SOURCE_BUILD.md`).
- A few read-only measurements of our own `game/CUSA08010` files, made by importing bbport's
  `scripts/prepare.py` / `scripts/link_modules.py` parsers from a scratch script (nothing written
  into the repo or the game folder).

## TL;DR / recommendation

**Use bbport's CPU side and leave out its GPU side.** Keep the loader (`src/probe.c`), the offline
SELF->flat-image linker (`scripts/prepare.py`, `scripts/link_modules.py`), and the C HLE runtime
(`src/runtime_*.c`), generalized for Dreams. Replace `gpu/` (libbbgpu) with a new library,
"vcore". It builds **unmodified upstream shadPS4 `src/video_core` + `src/shader_recompiler` +
`core/libraries/{gnmdriver,videoout}` with the Dreams fork patches rebased on top**. A thin adapter
connects it to the runtime, modelled on the shim bbport already wrote for the same purpose
(`gpu/shim/core/*.h`). The upstream code's external surface is small (it is measured in Q4), so I
estimate the adapter at about 1.5–2.5k new lines plus CMake. Porting the fork's ~60 patches into
bbport's 218k-line rewritten renderer would be a hand re-implementation that never tracks upstream.
It would also land in exactly the areas bbport says it never exercised: async compute, GDS/ordered
count, MSAA and readbacks.

## Facts about the Dreams binary that matter here

Measured on `game/CUSA08010` with bbport's parsers:

| Item | Dreams CUSA08010 v2.52 | Bloodborne 1.09 (bbport docs) | Consequence |
|---|---|---|---|
| eboot loaded image | 0xb032348 = 176 MiB (RX 25 MiB, data filesz 11 MiB, memsz 148 MiB incl. bss) | 91 MB | fits the 512 MiB limit (`scripts/prepare.py:42,63,157`, `src/probe.c:606`) |
| eboot relocation types | 8: 32046, 7: 796, 1: 32 | 8, 7, 1, 6 | all supported (`scripts/prepare.py:199-216`) |
| eboot PT_TLS | present, memsz 0 | used | no static TLS in eboot |
| `mov r64, fs:[0]` in eboot/libc (objdump of the RX segments) | **0** | many (rax only) | the FS->GS rewrite (`scripts/link_modules.py:245,300-311`) is a no-op for Dreams; TLS is via `__tls_get_addr` in modules |
| bundled `sce_module/` | `libc.prx`, `libSceFios2.prx`, **`libSceJobManager.prx`** | libc, Fios2 | must add JobManager to `--modules` (`scripts/link_modules.py:242,477`). It has PT_TLS (8 bytes) + 2 DTPMOD64 relocs (supported, `link_modules.py:381-386`) and imports `libSceGnmDriver`, `libSceRazorCpu`, `ulobjmgr` |
| libc `_init_env` is `RET` | yes (`init_env_is_ret: True`) | yes | capability bit 1 is granted (`prepare.py:114-143`, `probe.c:603-610`, `src/runtime.c:72-74,208`). Without it the whole C runtime resolves nothing |
| eboot imported symbols | 812 across 63 libraries | 701 | see library list below |
| GnmDriver imports | 103, including driver-side PM4 builders (`sceGnmDrawIndex*`, `sceGnmSet{Vs,Ps350,Cs,Es,Gs,Hs,Ls}Shader`, `sceGnmDispatch{Direct,Indirect,IndirectOnMec}`, `sceGnmDrawInitDefaultHardwareState350`, `sceGnmInsertWaitFlipDone`, `sceGnmRequestFlipAndSubmitDone`, `sceGnmMapComputeQueueWithPriority`, `sceGnmRegisterGdsResource`, `sceGnmSetGsRingSizes`, `sceGnmDrawIndexMultiInstanced`) | mostly submit/flip | upstream gnmdriver implements all of these. bbport implements most (`gpu/platform/gnm/gnm_commands.cpp`) but stubs `RequestFlipAndSubmitDone`/`SetGsRingSizes` as `Missing`, `RegisterGdsResource` as `Failure` and `DrawIndexMultiInstanced` as `Unreachable` (`gpu/platform/gnm/gnm_driver.cpp:609-619,629-648,821-845`) |

Imports per library (eboot): libc 197, libSceGnmDriver 103, libkernel 80, libSceNet 33, libSceHttp 25,
NpManager 19, AvPlayer 18, **Hmd 17**, VideoOut 17, NpWebApi 15, Rtc 15, GameLiveStreaming 12,
**VrTracker 12**, SaveData 11, Ajm 10, **Audio3d 10**, **Camera 10**, NpUtility 10, Posix 10,
AudioOut 9, **Ime 9**, NpSignaling 9, Pad 9, **Move 8**, NpParty 8, ContentSearch 7, NpTrophy 7,
SaveDataDialog 7, HmdSetupDialog 6, MsgDialog 6, PlayGo 6, VideoRecording 6, ContentExport 5,
NpAuth 5, NpFriendListDialog 5, SystemService 5, UserService 5, VrServiceDialog 5, **At9Enc 4**,
ErrorDialog 4, JobManager 4, **M4aacEnc 4**, NpCommerce 4, NpProfileDialog 4, SigninDialog 4, Ssl 4,
WebBrowserDialog 4, Coredump 4, AppContent 3, **AudioIn 3**, ShareUtility 3, CommonDialog 2,
NetCtl 2, Sysmodule 2, GnmWaitFreeSubmit 1, Random 1, ScreenShot 1,
SystemServicePlatformPrivacy 1, OpenPsId 1, libkernel_cpumode_platform 1. (Exact NIDs and names:
the imports agent's `notes/dreams-imports.md`.)

---

## 1. Loader + offline prep: what is Bloodborne-specific vs generic

### Pipeline

1. `scripts/prepare.py` reads plaintext SELF or ELF (`prepare.py:31-92`) and builds a flat image
   with every PT_LOAD/0x61000010 at its vaddr (image vaddr 0 = offset 0, `prepare.py:153-163`). It
   converts relocations into `(target, kind, value, addend)`: kind 0 = base-relative, 1 = function
   import, 2 = data import (`prepare.py:188-216`). It writes `out/boot.bin` (`BBPROBE2`,
   `prepare.py:239-251`) and `analysis.json`.
2. `scripts/link_modules.py` (successor of `link_libc.py`) appends each bundled PRX after the
   eboot at 64 KiB alignment. It merges import tables and binds imports to module exports by
   (NID, library identity, module identity). `libSceLibcInternal` falls back to libc's same NID
   (`link_modules.py:421-430`). It assigns TLS module ids 2..7 and rewrites
   `mov r64, fs:[0]` -> `gs:` (`link_modules.py:300-311`). Names are canonicalized to
   Bloodborne's letter spelling (`link_modules.py:439`, `scripts/import_scopes.py:12-50`). Output:
   `out/boot-linked.bin` (`BBPROBE5`).
3. `scripts/patches.py` compiles shadPS4/GoldHEN XML (literal writes only) into `BBPATCH2`. Its
   addresses assume eboot base 0x400000 (`patches.py:17`), applied after relocation
   (`probe.c:436-477`).
4. `src/probe.c` maps the image in the "low region" (`probe.c:139-150` -> `runtime_low_map`,
   `src/runtime_memory/low.c:8,79-100`: first free address >= 0x8_0000_0000, below
   0x10_0000_0000). It builds 32-byte trap stubs per import (`probe.c:712-724`) and PROT_NONE data
   trap pages (`probe.c:713-714`). It applies relocations and resolves each import with
   `runtime_resolve()`, else the native module export, else the trap (`probe.c:729-748`). Then it
   applies patches and `bbgpu_patch_image` (`probe.c:754-755`), sets segment protections
   (`probe.c:759-767`), sets TLS/procparam (`probe.c:770-782`), and calls each module's DT_INIT as
   `init(0,NULL,NULL)` in link order (`probe.c:784-790`). Finally it enters the eboot entry with
   `{argc=1, "/app0/eboot.bin"}` on an 8 MiB low stack (`probe.c:806-819`).

### Bloodborne-specific (must change or be removed)

| Where | What | Dreams action |
|---|---|---|
| `scripts/game_check.py:28-35` (`SUPPORTED_TITLES`, `SUPPORTED_VERSION='01.09'`, `SUPPORTED_IMAGE` sha256, `LANCE_60FPS_IMAGE`), `game_check.py:62-90` (`problem()`), called from `prepare.py:164-166` | hard image-hash gate | replace with a CUSA08010 v2.52 image hash (or bypass with `BB_SKIP_GAME_CHECK=1` during bring-up). The DCX/TPF checks (`game_check.py:37-67,70-120`) are FromSoftware formats: drop them |
| `prepare.py:218-235` (`candidates` name hints), `prepare.py:236` (`inspect_libc` required) | BB-chosen hint list; libc evidence mandatory | generic enough; keep. Use upstream shadPS4's NID name database for hints |
| `scripts/link_modules.py:242` `DEFAULT_MODULES = ('libc.prx','libSceFios2.prx')` | module list | add `libSceJobManager.prx` (pass `--modules`) |
| `scripts/import_scopes.py:12-38` | runtime tables keyed on Bloodborne's per-executable library/module letters (`#p#J` = libkernel...) | works for Dreams because canonicalization maps by (name), but libraries absent from BB's table become `NID#libname`. `runtime_thread_resolve` matches **exact** scoped strings (`src/runtime_thread.c:399-426`) while most resolvers match NID only (`src/runtime.h:73-76`). Unify on NID-only matching |
| `scripts/patches.py:17-60` + `patches/Bloodborne.xml` | FPS/resolution/effects presets for BB addresses | drop; keep only the XML->BBPATCH2 compiler for Dreams patches |
| `src/probe.c:96-130` | CPU feature check message names Bloodborne | cosmetic; keep the check (Jaguar ISA: AVX, BMI1, MOVBE, LZCNT) |
| `src/probe.c:699` default title `"Bloodborne"`; `probe.c:793-803` `BB_LOW_INFO_TEST` image offsets | cosmetic / diagnostics | remove |
| `src/probe.c:755` `bbgpu_patch_image` -> `BbGnmHooks::PatchImage`, `BbGameMenu::PatchImage`, `BbFrameRate::PatchImage` (`gpu/shim/bbgpu.cpp:353-357`) | hooks at BB 1.09 addresses. The game menu is on by default (`gpu/shim/bbport_game_menu.cpp:764-797`, prologue-checked). The frame-rate hook is on by default: it does `memcpy(image + 0x4928dfc)` **before** checking anything (`gpu/shim/bbport_frame_rate.cpp:388-410`), so on another image it reads arbitrary bytes | must not run on Dreams. With the GPU replacement these go away entirely |
| `gpu/games/bloodborne*.cpp`, `gpu/games/game_profile.cpp:23-35` (profile by TITLE_ID) | BB hooks (loaders, TLSF heap, allocator at fixed addresses), copy-shader hash `0xfefebf9f` (`gpu/games/bloodborne.cpp:48`) | drop (inactive for an unknown serial anyway) |
| `src/runtime_memory/backing.c:54-59` + `gpu/games/bloodborne/hooks_heap.cpp` | "GraphicsPrivate" heap growth (`BB_HEAP_GROW`) | BB only; leave off |
| `src/runtime_compat.c:3-15` | only BB's three no-op imports. Comment: entries other games needed (event flags, time zones, dialogs, PSN) were **removed** from the public tree ("full project copy of 2026-10-28") | Dreams needs these re-added |
| Runtime coverage in general | ~474 exported names across `src/runtime*.c` (`RuntimeExport` tables), tuned to BB's 701 imports | gap-fill for Dreams' 812 + JobManager/Fios2/libc imports (Q2) |

### Generic (reusable as-is or nearly)

- SELF/ELF parsing, segment merge, relocation handling, NID computation (`prepare.py:21-111`,
  `link_modules.py:248-297`). Supports reloc 1/6/7/8 in the eboot and 1/6/7/8/16 in modules. Not
  supported: R_X86_64_TPOFF64 (18), DTPOFF64 (17), TPOFF32. Dreams doesn't need them (measured
  above).
- Multi-module static link with export binding and per-module TLS (`link_modules.py:352-419`),
  module DT_INIT calls (`probe.c:784-790`), procparam handoff (`probe.c:782`,
  `src/runtime.c:30-47`), and the main/thread TLS layout: FreeBSD variant II TCB, GS base via
  `arch_prctl(ARCH_SET_GS)` (`src/runtime_thread.c:62-89`).
- Memory map (all generic, PS4-like): low region [0x8_0000_0000, 0x10_0000_0000) for the image,
  stacks, thread records and trampolines (`src/runtime_memory/low.c:6-8`). Guest user range
  [0x10_0000_0000, 0xfc_0000_0000) (`src/runtime_memory.c:55-56`). 16 KiB pages
  (`runtime_memory.c:53`). Direct memory 5056 MiB (`BB_DMEM_MB`, `runtime_memory.c:23-34`).
  Flexible 448 MiB (`runtime_memory.c:52`). +512 MiB port-private (`runtime_memory.c:35-47`). Host
  glibc heap is kept below 1 TiB via `-no-pie` + `mallopt` (`probe.c:510-513`): the guest packs
  pointers in 40 bits. Not Dreams-specific, but keep it.
- Watchdog: `alarm(--timeout)` defaults to **10 s** (`probe.c:541,593-595`). Pass `--timeout 0`
  for real runs.

### What an arbitrary PS4 eboot (Dreams) additionally needs

1. A Dreams image check plus `libSceJobManager.prx` in the link list.
2. **Runtime libraries Dreams imports that bbport lacks or stubs to "stop":** Camera, Move, Hmd,
   HmdSetupDialog, VrTracker, VrServiceDialog, Audio3d, AudioIn (mic), At9Enc, M4aacEnc, Ime (not
   just ImeDialog), ContentSearch, ContentExport, ShareUtility, NpParty, NpFriendListDialog,
   SigninDialog, ErrorDialog, Coredump, Random, OpenPsId, GnmWaitFreeSubmit, RazorCpu (from
   JobManager), ulobjmgr, libkernel_cpumode_platform, SystemServicePlatformPrivacy. For a
   first-frame milestone, most should report "device not connected / not signed in / not
   supported" with correct error codes. The shadPS4 fork has working HLE for several of them in C++
   (camera, mic encoder, at9enc, content search: `dreams-fork/patches/dreams-microphone-encoder-20261031.patch`,
   CMake hunk of `dreams-focused-20260817-current.patch`). Use those as references.
3. libkernel extras the public runtime lacks: event flags (`sceKernelCreateEventFlag`...),
   `sceKernelInstallExceptionHandler`/`RaiseException` (not found anywhere in `src/`),
   `sceKernelLoadStartModule`/`Dlsym` (not implemented: only BB's static link path), CPU-mode and
   NEO queries, plus whatever `notes/dreams-imports.md` lists.
4. Possibly Dreams-specific eboot patches (offline gates, server checks). The fork's byte patches
   target **CUSA04301 1.00 executable / 2.64 content**, are env-gated and serial-checked
   (`ref/shadps4` branch `dreams-fork-reconstructed:src/core/module.cpp:351-1195`, 22
   `"CUSA04301"` checks; `src/core/dreams_scene_readiness.h:14`). They **do not apply** to
   CUSA08010 v2.52. Re-derive per binary if needed (the fork's own docs warn against transplanting
   offsets: `dreams-fork/docs/SOURCE_BUILD.md`, "Dreams 02.65 is not validated").

## 2. How the game calls into the runtime

- **Import resolution = static GOT/PLT binding at load time, no lazy PLT and no syscall
  emulation.** Every relocation that names an import is written once with the final address.
  The resolution order is `runtime_resolve(name, is_data)`, then the bundled module's native
  export, then a trap (`probe.c:729-748`). `runtime_resolve` (`src/runtime.c:207-271`) checks a
  few libc contracts by NID (`_init_env`, `atexit`, `__cxa_atexit`, `exit`, guards,
  `__stack_chk_*`, `memcpy/memset/memmove` wrappers that notify the GPU of CPU writes,
  `__tls_get_addr`, `sceKernelGetProcParam`, ...). It then chains the per-subsystem resolvers
  (mutex, thread, content, time, sema, rwlock, memory, kernel, services, ajm, audio, pad, rtc,
  savedata, file, compat) and finally `bbgpu_resolve()` (GPU-library `LIB_FUNCTION` table,
  `gpu/shim/bbgpu.cpp:329-338`). Most tables are `{"symbolName", fn}` arrays. A NID is mapped to a
  name through `src/import_names.inc` (750 BB NIDs) + `import_names_extra.inc` (`runtime.c:272-295`).
  **Unknown NIDs therefore can't be resolved even if implemented**, so the name table must be
  extended (use upstream shadPS4's NID list).
- **Native PS4 code is reused where possible:** the game's own `libc.prx` (C/C++ runtime, malloc,
  stdio over libkernel imports) and `libSceFios2.prx` run natively. The C runtime "overrides"
  only selected functions (host contracts first, `link_modules.py:220-230`). libc's
  `libSceLibcInternal` imports bind to libc's own exports.
- **Unimplemented imports:** a function import gets a stub `mov rsi,rdi; mov edi,index; movabs
  rax,unresolved; jmp rax` (`probe.c:716-723`). The first call prints `STOP: first unsupported
  PS4 import`, the caller offset and the first argument, and then `_exit(20)` (`probe.c:167-182`).
  Data imports point into PROT_NONE pages, so the first access faults (`probe.c:713-714,730`).
  `BB_LIST_UNRESOLVED=1` lists all unresolved imports before entry (`probe.c:725-752`).
  `--strict-imports` disables the runtime entirely. **There are no silent success stubs**
  (`src/runtime.c:3`). The GnmDriver table in the GPU lib has its own stub kinds (`Silent`,
  `Missing` = return 0 + warn once, `Failure`, `Unreachable`; `gpu/platform/gnm/gnm_driver.cpp:609-619`).
- **libkernel is HLE'd entirely in C**: no libkernel.prx is loaded.
  - `runtime_kernel.c`: clocks, TSC, sleep, errno mapping to FreeBSD (`:33-...`), `scePthreadOnce`
    and keys, signal bookkeeping, sysctl subset (`:324-346`).
  - `runtime_thread.c`: ScePthread on host pthreads, with low-memory thread records because BB
    packs handles into 40 bits (`:90-109`).
  - Mutex, rwlock and sema each have their own file; memory is `runtime_memory/kernel_api.c:408-419`.
  - Files (`runtime_file.c`) map `/app0`, `/savedata`... to host paths.
  - Event queues (`sceKernelCreateEqueue/WaitEqueue/AddUserEvent/Timer`) live in the GPU library,
    `gpu/platform/kernel/event_queue.cpp:563-579`, registered by `bbgpu_register_kernel()` even in
    `--cpu-only` (`probe.c:583`). Keep that in mind when replacing `gpu/`.
- **Threading/TLS:** each guest thread gets a TCB (`tcb[0]=self, [1]=dtv, [2]=thread`) and the
  static TLS block below it. GS base = TCB; glibc keeps FS (`runtime_thread.c:3-6,73-89`). Module
  TLS is lazily allocated per thread in `__tls_get_addr` for module ids 2..7 (`runtime.c:25-70`).
  Guest stacks come from the low region (`runtime_thread.c:295-304`; main 8 MiB, `probe.c:816-819`).
  Priorities and affinity are recorded, not enforced.
- **Exceptions/signals:** C++ exceptions unwind natively inside the game's libc (its own
  unwinder). One SIGSEGV/SIGBUS/SIGILL handler (`probe.c:220-315,585-587`):
  1. calls `bbgpu_handle_fault()` first (GPU write-trap pages);
  2. then a per-thread `sigsetjmp` recovery for speculative reads (`runtime_fault_recover`);
  3. otherwise prints a guest/host backtrace and `_exit(128+sig)`.

  SIGUSR2 dumps all threads; SIGALRM is the watchdog. Guest `signal`/`sigprocmask` are
  bookkeeping only. `sceKernelInstallExceptionHandler` is **missing**.
- **Memory (`src/runtime_memory*.c`, 1.8k lines):**
  - **Backing:** one sealed `memfd` backs all direct memory [0, pool) and flexible memory after it
    (`runtime_memory/backing.c:24-48`). Every guest mapping is `MAP_SHARED|MAP_FIXED` of that fd at
    the PS4 address, so aliasing of physical ranges works, and a second "backing view" mapping lets
    the GPU side write around protections (`runtime_memory_write_backing/read_backing`,
    `runtime_memory/gpu_traps.c:256-297`).
  - **VMA table** with PS4 semantics (reserve, map direct/flexible, mprotect, VirtualQuery,
    BatchMap, DirectMemoryQuery) in `runtime_memory/kernel_api.c` and `vma.c`.
  - **Write/read traps:** reference-counted per-page reasons (`runtime_memory_trap`,
    `gpu_traps.c:91-180`). This lets several GPU subsystems protect the same page;
    `runtime_memory_gpu_protect` is what the GPU lib's `AddressSpace::Protect` calls
    (`gpu/shim/bbgpu.cpp:192-203`).
  - **Kernel-write path:** before `read()` into guest memory, the file layer calls
    `runtime_memory_note_write` / `touch_for_write` (`src/runtime_file.c:562-591`) so a protected
    page doesn't make the syscall return EFAULT. **This is essential for any GPU backend that uses
    mprotect tracking.**
  - **GPU-visible memory:** the bbport GPU lib exports chunks as dma-buf (AMD) or imports the memfd
    as host memory (NVIDIA) (`runtime_memory/gpu_interface.c:75-104`,
    `gpu/shim/bbport_guest_memory*.cpp`). Upstream shadPS4 doesn't need this: it copies, or imports
    host pointers when `IsDirectMemoryAccessEnabled`.

## 3. Runtime <-> GPU boundary (the seam)

The whole C interface is `gpu/bbgpu.h` (109 lines). The part that matters:

| Call | Direction | Purpose |
|---|---|---|
| `bbgpu_register_kernel()` (`bbgpu.h:20`, `bbgpu.cpp:323-327`) | runtime -> GPU | registers equeue + AvPlayer `LIB_FUNCTION`s |
| `bbgpu_init(&BbGpuConfig{title, serial, user_dir, sdk_version, psf_attributes, w, h})` (`bbgpu.h:11-22`, `bbgpu.cpp:271-321`) | runtime -> GPU | starts the SDL window thread, selects the game profile, registers GnmDriver + VideoOut, which create the presenter and the command processor (`gnm_driver.cpp:56-57`: globals `presenter`, `liverpool`) |
| `bbgpu_resolve("NID#L#M")` (`bbgpu.cpp:329-338`) | runtime -> GPU | last resolver: NID-only match against the `LIB_FUNCTION` table filled through a fake `Core::Loader::SymbolsResolver` (`gpu/shim/core/libraries/libs.h`) |
| `bbgpu_handle_fault(ucontext, addr)` (`bbgpu.cpp:340-351`) | loader signal handler -> GPU | dispatches to `Core::Signals` access-violation handlers (header-only registry, `gpu/shim/core/signals.h`) |
| `bbgpu_patch_image` | loader -> GPU | BB hooks (drop) |
| text input, mouse, overlay, pad filter, trophies, camera (`bbgpu.h:29-101`) | runtime <-> GPU | window/SDL services used by `runtime_pad`, ImeDialog, trophies. Some needed (IME text, pad from SDL), the rest BB extras |

The reverse direction (GPU lib -> runtime) is plain `extern "C"` (`bbgpu.cpp:44-64`):

- `runtime_memory_is_mapped/region/clamp/generation` back `MemoryManager::ClampRangeSize` and
  `CopySparseMemory` (`bbgpu.cpp:112-184`).
- `runtime_memory_write_backing/read_backing` back `MemoryManager::TryWriteBacking/ReadBacking`.
- `runtime_memory_gpu_protect` backs `AddressSpace::Protect`.
- `runtime_memory_set_gpu_hooks(map, unmap, invalidate)`: the runtime calls the rasterizer's
  `MapMemory/UnmapMemory/InvalidateMemory` on guest map changes and replays existing mappings
  (`bbgpu.cpp:88-108`, `runtime_memory/gpu_traps.c:6-20`).
- `runtime_memory_set_note_write_hook` (file reads), `runtime_memory_set_cpu_write_hook` (libc
  memcpy/memset wrappers).
- Clocks: `runtime_process_time_*`, `runtime_tsc_frequency`; `runtime_errno` serves
  `Libraries::Kernel::__Error` (`bbgpu.cpp:211-228,363-367`).

**How libGnm submissions reach the CP.** The game builds PM4 itself: libGnm is statically linked.
For Dreams it also calls driver-side builders such as `sceGnmDrawIndex` and `sceGnmSetVsShader`
that write PM4 into the game's buffer. Those builders and `sceGnmSubmit[AndFlip]CommandBuffers` /
`sceGnmDingDong` (compute rings) / `sceGnmSubmitDone` are imports into the GPU lib's GnmDriver
(`gpu/platform/gnm/gnm_driver.cpp:484-560`). That code calls
`liverpool->Submit*` / `SubmitDone` with BB-tuned pacing locks (`BB_SUBMIT_LOCK`,
`gnm_driver.cpp:1-14`).

**Flip / videoout.** `gpu/platform/videoout/videoout.cpp:428-457`
(Open/RegisterBuffers/SubmitFlip/AddFlipEvent/AddVblankEvent/...) feeds the presenter. Flip events
and vblank go to the guest through the equeue.

**Events / labels / EOP.** The command processor raises `BbIrq` lines (`gpu/platform/interrupts.h:13-34`:
GfxEop, GfxFlip, GpuIdle, ComputeN RelMem); GnmDriver turns them into equeue events
(`sceGnmAddEqEvent`). Labels (EOP/EOS/RELEASE_MEM) are written into guest memory by the GPU
(buffer marker or a portable Vulkan write) in stream order (`docs/EMULATION_REMOVAL_PLAN.ru.md`,
steps 3 and 2026-10-09 addendum).

This is the same shape as upstream: `Platform::IrqC`, `Liverpool::SubmitGfx/SubmitAsc`,
`VideoOutDriver`, `EqueueInternal`. bbport renamed the namespaces (`BbCp`, `BbIrq`, `BbKernel`).
**The seam to plug upstream in is: `runtime_resolve -> bbgpu_resolve` (LIB_FUNCTION table), the
`runtime_memory_*` C functions, the fault-dispatch call, and the shared equeue.**

## 4. Upstream video_core instead of bbport's gpu/: feasibility

### What bbport's gpu/ is today

`gpu/VENDOR.txt` says it started from shadPS4 `b4e7ae77` (2026-09-24) and was "rewritten part by
part"; `gpu/shadps4` was deleted 2026-10-13 (`docs/SHADPS4_EXIT_PLAN.ru.md` header). The tree is
about 218k lines:

- `render` 27k, `shader` 25k, `upscale` 15k, `shim` 14k, `platform` 9k, `buffers` 6k,
  `textures` 5k, `cp` 5k, `gcn` 6k, `layer` 4k, `games` 5k.
- Own CP decoder (`gpu/cp`), stage A/B pipelined rasterizer, own memory model (sparse arena /
  dma-buf / host import), FSR/DLSS/frame generation, BB profile hooks.
- Only the shader translator still mirrors upstream layout: 105 of 110 files share names with
  upstream `src/shader_recompiler`; byte-identical SPIR-V on BB's corpus at the vendored commit.

bbport's own docs list what it never exercised (`docs/SHADPS4_EXIT_PLAN.ru.md` §3.1, §5, §6a;
`EMULATION_REMOVAL_PLAN.ru.md` step 5): async compute (ASC) runs as coroutines on the graphics
thread and drains the A/B pipe on every packet. GDS is only partial. MSAA/FMASK decompress is
skipped. Tessellation/GS are unverified. Memory correctness without game hooks relies on write
traps. The runtime "implements exactly what Bloodborne calls". Dreams leans on exactly these:
compute-heavy SDF sculpt pipeline, `DS_ORDERED_COUNT` + GDS, Precise readbacks
(`dreams-fork/docs/SOURCE_BUILD.md`, `ref/shadps4/DREAMS_DEVELOPMENT.md`).

### What upstream video_core needs from the rest of shadPS4 (measured on origin/main)

Includes from `src/video_core` + `src/shader_recompiler` into non-video code:

| Dependency | Use | Adapter |
|---|---|---|
| `core/memory.h` (5 files) | `MemoryManager` methods used: `TryWriteBacking` (5), `CopySparseMemory` (3), `ClampRangeSize` (3), `IsValidMapping` (2), `GetAddressSpace().Protect` (3), `SetRasterizer`, `IsValidGpuMapping` | **bbport already wrote this shim** (`gpu/shim/core/memory.h`, `address_space.h`, impls in `gpu/shim/bbgpu.cpp:87-209`). Add `IsValidMapping`. ~150 lines |
| `core/signals.h` (page_manager, flatten_extended_userdata_pass) | access-violation handler registry | reuse `gpu/shim/core/signals.h` (52 lines) |
| `core/emulator_settings.h` (19 files) | 31 getters (`GetReadbacksMode`, `IsPipelineCacheEnabled`, `IsUserfaultfdTracking`, `GetVblankFrequency`, ...) | env-backed class like `gpu/shim/core/emulator_settings.h` (already has ~27 of the 31). ~120 lines |
| `core/debug_state.h` (6 files) | `DebugState.*`: 16 members (reg dumps, frame markers) | no-op stub. ~80 lines |
| `core/platform.h` | `Platform::IrqC` (header-only, self-contained) | use upstream file as-is |
| `core/libraries/kernel/{process,time}.h` | `sceKernelIsNeoMode`, `ReadTsc`, `GetTscFrequency` | ~30 lines -> runtime clocks |
| `core/libraries/videoout/{buffer,driver}.h`, `gnmdriver/gnmdriver.h`, `system/systemservice.h` | types, `VideoOutDriver`, `IsSplashVisible` | compile upstream `videoout/driver.cpp` + `video_out.cpp` (~930 lines) and `gnmdriver.cpp` (3.4k) with the adapter |
| `core/devtools/*`, `imgui/*` (presenter, swapchain, instance, scheduler) | ImGui overlay layers (friends, shadnet notifications, invitation prompt) | compile upstream `src/imgui/renderer/*` core, stub the online layers (~60 lines) or stub ImGui::Core entirely |
| `sdl_window.h` | `Frontend::WindowSDL` (surface info, size) | small window class; bbport has one (`gpu/shim/window.cpp`, `sdl_window.h`). Upstream's `sdl_window.cpp` pulls input/pad/keyboard: replace |
| `common/*` | mostly self-contained. Only `assert.cpp`, `logging/log.cpp`, `path_util.cpp`, `thread.cpp`, `memory_patcher.cpp` reach into `core/` | compile `common/` minus `memory_patcher`/discord. Patch or stub 4 files (bbport did the same: `gpu/common`) |

`Libraries::*` symbols referenced from `video_core`: `GnmDriver::sceGnmGetGpuCoreClockFrequency`,
`Kernel::sceKernelGetTscFrequency/ReadTsc/IsNeoMode`, `SystemService::IsSplashVisible`, and
VideoOut enums. The Dreams fork (branch `dreams-fork-reconstructed`) uses **the same surface**
plus `ElfInfo::GameSerial()`/`GetSplashData()`. Its runtime gates are `GameSerial()=="CUSA04301"`
(`liverpool.cpp:44,52`; `emit_spirv_atomic.cpp`; `export.cpp`) and 135 literal `pgm_hash == 0x...`
comparisons. **These must be extended to CUSA08010, and the shader hashes re-verified on v2.52.**

Upstream platform glue that the adapter must reconcile with the C runtime:

- **Equeue.** Upstream `gnmdriver`/`videoout` call `Libraries::Kernel::EqueueInternal`
  (`AddEvent/TriggerEvent/...`, upstream `src/core/libraries/kernel/equeue.h:153-207`). The
  runtime's equeue is bbport's `BbKernel::EventQueue` (`gpu/platform/kernel/event_queue.h:22-80`,
  different API). Keep one implementation shared by guest `sceKernelWaitEqueue` and the GPU side.
  Either port bbport's into vcore and write an `EqueueInternal` facade (~200 lines), or adopt
  upstream `equeue.cpp` (911 lines; pulls `file_sys`/`net` for I/O events, which need stubbing).
- **Memory registration.** Upstream `MemoryManager` calls `rasterizer->RegisterMemory`/`MapMemory`/
  `UnmapMemory` (upstream `src/core/memory.cpp:136-145,460-477,595,784`). Wire bbport's
  `runtime_memory_set_gpu_hooks` to these. Also wire `runtime_memory_set_note_write_hook` to
  `rasterizer->InvalidateMemory` so file reads into tracked pages keep working (see Q2, kernel-write
  path).
- **Fault handling.** Upstream `page_manager.cpp` protects through `AddressSpace::Protect` and
  handles faults through `Core::Signals`, or userfaultfd when `IsUserfaultfdTracking()`. Keep it on
  the signal path. The loader's `fault()` already calls `bbgpu_handle_fault` first, so this works
  unchanged.
- **Symbol registration.** Upstream libraries register with
  `LIB_FUNCTION(nid, lib, ver, mod, fn)`. bbport's `gpu/shim/core/libraries/libs.h` provides the
  compatible macro into a NID table queried by `bbgpu_resolve`.

**Adapter estimate:**

| Part | Lines |
|---|---|
| Shim headers + impls | ~600 |
| Equeue facade | ~200 |
| Window + IME text input | ~400 |
| C bridge (`bbgpu.h` subset: init/resolve/handle_fault/register_kernel/text input) | ~300 |
| Settings, debug and imgui stubs | ~250 |
| CMake target listing upstream sources + shadowing include dir | ~300 |
| **Total** | **~1.5–2.5k** |

Upstream sources compile unmodified because the adapter headers sit earlier on the include path
(the same trick as `gpu/shim/core/`).

### Alternative: keep bbport gpu/ and port the fork's patches

**Patch size.** The fork carries ~60 patches. The cumulative ones are 350–750 KB. The recent
targeted ones (2026-10-15 .. 11-13) total several hundred KB.

**Where they land.** They touch `shader_recompiler` (ir/passes, backend/spirv, translate),
`video_core/texture_cache` (mip handling, readback, eviction, copy/resolve), `buffer_cache`,
`renderer_vulkan` (`vk_rasterizer.cpp` DOC replay, pipeline serialization, host passes:
`dreams_ordered_prefix_pass`), `amdgpu/liverpool` and `page_manager`.

**Fit with bbport.** Shader patches would port semi-mechanically into `gpu/shader`. Everything
else targets classes bbport replaced: `Rasterizer` split into `render/raster_*.cpp` with stage
A/B, `BbCp`, the layer memory model and texture spares. Each fix would have to be re-derived by
hand. There would be no `git apply`, no upstream tracking, and no fork validation data that
transfers.

**What bbport would bring.** Better performance and upscalers, which are irrelevant before first
frame.

### Recommendation

**Go hybrid: bbport CPU side + upstream video_core (+ fork patches) behind a bbport-style shim.**
Reasons:

1. The fork's correctness work is the scarcest asset and only applies to upstream's structure.
2. Dreams needs the subsystems bbport admits it never exercised (ASC, GDS/ordered count, MSAA,
   readbacks).
3. The seam is narrow and already proven by bbport's own shim.
4. Upstream keeps moving (e.g. `S_BFM_B64`, #5724 on 2026-11-15) and we can follow it.

Accepted costs: upstream's slower memory path (copies, or host-pointer import) and no bbport
upscalers. Revisit bbport's guest-memory model (`gpu/shim/bbport_guest_memory*.cpp`) only after
first frame, if performance demands it.

Keep bbport `gpu/` in the tree as a reference, not as a build dependency.

## 5. Build

- `build.sh` only re-execs into `nix-shell` when `pkg-config --exists vulkan sdl3`, `cmake` or
  `ninja` is missing (`build.sh` lines 14-18). With system packages it builds without nix:
  - `cmake -S gpu -B out/gpu -G Ninja` (needs `third_party` submodules: fsr-vulkan, imgui,
    LibAtrac9; `.gitmodules`);
  - then `gcc -std=c11 -no-pie src/probe.c src/runtime*.c ... -lbbgpu`.
- `-DBB_NET=OFF` drops the online module (protobuf/json/openssl/miniupnpc).
- The C side must stay `-no-pie` (host heap below 1 TiB, `probe.c:510-513`).

Fedora 44 equivalents of `shell.nix` (checked with `dnf repoquery`; all available, RPM Fusion
enabled):

| nix | Fedora |
|---|---|
| gcc gnumake cmake ninja pkg-config python3 binutils | `gcc gcc-c++ make cmake ninja-build pkgconf-pkg-config python3 binutils git` |
| vulkan-headers vulkan-loader | `vulkan-headers vulkan-loader-devel` (+ `vulkan-validation-layers` for debugging) |
| sdl3 (+libdecor) | `SDL3-devel libdecor-devel` |
| ffmpeg-headless | `ffmpeg-devel` (RPM Fusion; needed for AvPlayer H.264/AAC) or `ffmpeg-free-devel` (patent-limited) |
| boost fmt magic-enum robin-map xxhash | `boost-devel fmt-devel magic_enum-devel robin-map-devel xxhash-devel` |
| vulkan-memory-allocator | `VulkanMemoryAllocator-devel` |
| glslang spirv-headers | `glslang` (provides `glslang`/`glslangValidator`, required by `gpu/CMakeLists.txt:92`), `spirv-headers-devel` |
| spirv-cross | not used by `gpu/CMakeLists.txt`; skip |
| xbyak zydis miniz | `xbyak-devel zydis-devel miniz-devel` (miniz is FetchContent'ed if absent, `gpu/CMakeLists.txt:81-90`) |
| libx11 libxcb libxau xorgproto wayland alsa-lib | `libX11-devel libxcb-devel libXau-devel xorg-x11-proto-devel wayland-devel alsa-lib-devel` |
| protobuf nlohmann_json openssl zlib miniupnpc (online only) | `protobuf-devel json-devel openssl-devel zlib-ng-compat-devel miniupnpc-devel` |

For the hybrid vcore library, upstream's CMake finds most deps or falls back to its submodules
(`ref/shadps4/.gitmodules`: sirit, vma, glslang, xbyak, zydis, fmt, magic_enum, robin-map, half,
xxhash...). Upstream wants fmt >= 12, VulkanHeaders >= 1.4.330 and Zydis >= 5 (upstream
`CMakeLists.txt:237-265`). If Fedora's versions are older, use the bundled externals for those. In
practice: build vcore with upstream's `externals/` subset (sirit, vma, glslang, xbyak, zydis,
half, magic_enum, robin-map, xxhash, fmt), and use system SDL3/Vulkan loader/ffmpeg/boost.

## Component list

| Component | Verdict | Notes |
|---|---|---|
| `scripts/prepare.py` (SELF parse, flat image, relocs) | **reuse as-is** | swap the game check |
| `scripts/link_modules.py` | **reuse, small adapt** | add JobManager; make import spelling NID-only |
| `scripts/import_scopes.py` | adapt or drop | once resolvers are NID-only, it's cosmetic |
| `scripts/game_check.py` | **replace** | CUSA08010 v2.52 image hash |
| `scripts/patches.py` (XML->BBPATCH2) | adapt | keep the compiler, drop BB presets |
| `scripts/link_libc.py`, `content_profile.py`, `mods.py`, `online.py`, `bbsc_gameserver` | drop | superseded or BB-specific |
| `src/probe.c` loader | **reuse, small adapt** | remove BB bits (title, low-info test, BB patch_image), default `--timeout 0`, keep traps/fault chain |
| `src/runtime.c`, `runtime_thread.c`, `runtime_mutex.c`, `runtime_rwlock.c`, `runtime_sema.c`, `runtime_kernel.c`, `runtime_time.c`, `runtime_rtc.c` | **reuse + extend** | add event flags, exception handler, module APIs, missing libkernel/posix NIDs; NID-only thread table |
| `src/runtime_memory*.c` (memfd pool, VMA, traps, backing view) | **reuse** | drop BB heap-grow; keep trap reasons + backing writes (upstream page_manager needs them) |
| `src/runtime_file.c`, `runtime_savedata.c`, `runtime_content.c`, `runtime_audio.c`, `runtime_ajm.c` (ATRAC9 only), `runtime_pad*` | **reuse + adapt** | Dreams: AAC/MP3 in Ajm likely needed, At9Enc/M4aacEnc, AudioIn, Audio3d; pad motion matters (Dreams default control is motion) |
| `src/runtime_services.c`, `runtime_net.c`, `runtime_trophy.c`, `runtime_compat.c` | adapt | offline/not-signed-in contracts for Np*, Http, dialogs |
| `src/runtime_remote.c`, `runtime_rewatch.c` | drop | phone adb fetch / RE tooling |
| new: Camera, Move, Hmd/VrTracker, Ime, ContentSearch/Export, JobManager-side (RazorCpu, ulobjmgr), Random | **write** | "not connected" first; reference the fork's C++ HLE |
| `gpu/` (cp, render, shader, buffers, layer, textures, memory, gcn, upscale, vulkan, games, dlss_bridge, bbnet20) | **replace** by vcore (upstream video_core + shader_recompiler + fork patches) | keep as reference only |
| `gpu/shim/core/*.h`, `gpu/shim/core/libraries/libs.h`, `gpu/shim/bbgpu.cpp` memory section | **adapt into vcore adapter** | the template for the seam |
| `gpu/platform/kernel/event_queue.*` | adapt (move into vcore or runtime) | single equeue + `EqueueInternal` facade |
| `gpu/platform/gnm`, `videoout` | replace with upstream gnmdriver/videoout | upstream covers Dreams' 103 Gnm imports |
| `gpu/platform/avplayer` | replace with upstream `libraries/avplayer` or adapt | Dreams imports AvPlayer 18 |
| `gpu/shim/window.cpp`, `sdl_window.h` | adapt | minimal window + text input + pad events |
| `gpu/shim/bbport_*` (overlay, game menu, frame rate, trophies UI, cursor, button prompts...) | drop | BB product features |
| `launcher/`, `packaging/`, `run.py/run.sh`, `tools/` | drop / rewrite minimal `run.sh` | |
| `third_party/LibAtrac9` | reuse | |

## Milestones toward "Dreams eboot boots to first frame natively"

1. **Toolchain sanity (no nix).** Install the Fedora packages. Build bbport unmodified
   (`build.sh`, `-DBB_NET=OFF`). Run `out/bb-probe --vulkan-only`.
2. **Offline prep for Dreams.**
   - Add the CUSA08010 image hash.
   - Run `link_modules.py --modules libc.prx libSceFios2.prx libSceJobManager.prx`.
   - Produce `boot-linked.bin`.
   - Run `bb-probe ... --cpu-only --timeout 0` with `BB_LIST_UNRESOLVED=1`, and diff against
     `notes/dreams-imports.md`.
3. **Runtime generalization.**
   - Make resolvers NID-only and import upstream's NID->name list.
   - Add missing libkernel (event flags, exception handler, module/cpumode queries) and
     libc-adjacent NIDs.
4. **CPU-only boot.** libc/Fios2/JobManager `DT_INIT`s return 0, then entry runs. Iterate on
   "STOP: first unsupported PS4 import" until the game reaches its first `sceVideoOutOpen` /
   `sceGnmSubmit*`. Peripherals (Camera, Move, Hmd, VrTracker, AudioIn, Np*, Ime, dialogs) report
   not-present / offline.
5. **Null GPU backend.** A ~300-line stub `bbgpu`:
   - accepts submissions;
   - writes EOP labels / signals equeue at once;
   - fires flip + vblank events at 60 Hz.

   This proves the equeue and VideoOut contracts and lets the game reach its main loop without
   Vulkan. (Upstream `IsNullGPU` can play this role once vcore exists.)
6. **vcore library on stock upstream.**
   - Build upstream `video_core` + `shader_recompiler` + `gnmdriver` + `videoout` (+ platform.h,
     the common subset) with the adapter.
   - Wire `runtime_memory_*` hooks, `bbgpu_handle_fault`, `bbgpu_resolve`, and the shared equeue.
   - Use Precise readbacks (the fork's required setting).
   - Target: the Sony/MM splash flips on screen.
7. **Apply Dreams fork patches rebased on latest upstream** (the other agent's `dreams` branch).
   - Extend `GameSerial()=="CUSA04301"` gates to CUSA08010.
   - Log `pgm_hash` of Dreams' compute shaders to confirm the 135 hash-guarded paths still match
     on v2.52. Disable mismatching guards rather than force them.
8. **First frame of the main menu / homespace.** Then audio (AudioOut + Ajm AAC/AT9), pad with
   motion, savedata (tutorial-skip save: `notes/tutorial-skip.md`), and AvPlayer intro (or skip).
9. **After first frame (optional).** Evaluate bbport's guest-memory model (dma-buf/host import)
   and the stage A/B ideas for performance. Re-derive any Dreams eboot patches for v2.52.

## Risks / open questions

- **Shader hashes and serial gates.** The fork targets CUSA04301 2.64 content. Our v2.52 shaders
  may differ, which would silently disable fixes. This needs a hash census early (milestone 7).
- **Async compute and GDS ordered-count correctness** is still unresolved even in the fork (Tren
  slowdowns, black patches; `dreams-fork/README.md`, `docs/STATUS.md`). First frame is plausible.
  "Playable" depends on the fork's progress.
- **40-bit pointer packing.** bbport keeps all host-allocated guest-visible objects below 1 TiB. Any
  vcore allocation handed to the guest must also be low. Use `runtime_low_map`.
- **Kernel-write EFAULT.** Every runtime path that lets the kernel write into guest memory must
  pre-touch or unprotect tracked pages. bbport does this for files (`src/runtime_file.c:562-591`).
  Audit sockets, AvPlayer and the AudioIn copy paths too.
- **Equeue ownership.** It currently lives in the GPU lib (`probe.c:583`). Moving it must keep
  `--cpu-only` working.
- **The `dreams-fork-reconstructed` branch has no merge-base with upstream.** Fork patch
  provenance relies on `dreams-fork/patches` + `docs/SOURCE_BUILD.md` (base `555c458c`, then
  `b84847d1` "Checkpoint Dreams full sculpt coverage" + ordered patch list).
- **Licensing.** All three trees are GPL-2.0-or-later, so they can be combined.
