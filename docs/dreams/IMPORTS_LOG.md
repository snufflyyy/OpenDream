# PS4 imports implemented or changed for Dreams

One row per NID added or changed in the runtime for Dreams. Keep this in sync with the code.
Imports that are only served by the generic return-0 fallback are listed in `STUBS.md` instead.

| NID | Name | Library | Where (port/src) | Semantics / reference | Date |
|---|---|---|---|---|---|
| `rNhWz+lvOMU` | _sceKernelSetThreadDtors | libkernel | `runtime_thread.c` (`set_thread_dtors`, called in `host_start`) | Stores libc's per-thread destructor hook; called on every guest thread exit before TSD cleanup. Ref: shadPS4 `kernel/threads/pthread.cpp` ExitThread | 2026-11-15 |
| (all thread NIDs) | scePthread* / pthread_* | libkernel / libScePosix | `runtime_thread.c` `runtime_thread_resolve` | Now matched by NID only (was `NID#p#J` exact, Bloodborne letter codes) | 2026-11-15 |
| (codec 0/2) | sceAjmInstanceCreate for MP3/AAC | libSceAjm | `runtime_ajm.c` | Instances are created; jobs consume input, output nothing, sidebands report 0 samples, 48 kHz. **Temporary** | 2026-11-15 |
| (any) | unimplemented import | — | `probe.c` `unresolved()` | Log once (`STUB:`) and return 0, as shadPS4 does. `DREAMS_STRICT_IMPORTS=1` = stop | 2026-11-15 |
| (921 NIDs) | names for every Dreams import | — | `import_names_dreams.inc` (generated) | So name-keyed `RuntimeExport` tables bind for Dreams | 2026-11-15 |
| `aNz11fnnzi4` | sceKernelAvailableFlexibleMemorySize | libkernel | `runtime_memory/kernel_api.c` `available_flexible` | 448 MiB budget minus mapped flexible memory (was: out-param uninitialised) | 2026-11-15 |
| `How7B8Oet6k` | scePthreadGetname | libkernel | `runtime_thread.c` `thread_get_name` | Copies creation name (≤32 bytes) | 2026-11-15 |
| `EI-5-jlq2dE` | scePthreadGetthreadid | libkernel | `runtime_thread.c` `thread_get_id` | Host `gettid()` | 2026-11-15 |
| (6 NIDs) | sceErrorDialog{Initialize,Open,UpdateStatus,GetStatus,Close,Terminate} | libSceErrorDialog | `runtime_dreams.c` | No UI: Open logs the code and finishes immediately. Dreams opens 0x80550006 (NP signed out) at boot offline; the fork dismisses it too | 2026-11-15 |
| (10 NIDs) | sceMove{Init,Term,Open,Close,GetDeviceInfo,ReadStateLatest,ReadStateRecent,SetVibration,SetLightSphere,ResetLightSphere} | libSceMove | `runtime_dreams.c` | No controllers: distinct handles from 0x30b0100, reads return NO_CONTROLLER_CONNECTED (1). Ref shadPS4 move.cpp | 2026-11-15 |
| `thDt9upZlp8` | sceHmdGetDeviceInformation | libSceHmd | `runtime_dreams.c` | 32-byte struct zeroed, status NOT_DETECTED (2) | 2026-11-15 |
| `rbknaUjpqWo` | sceNpGetAccountIdA | libSceNpManager | `runtime_dreams.c` | Offline: *id=0, returns SIGNED_OUT 0x80550006 | 2026-11-15 |
| `PI7jIZj4pcE` | sceRandomGetRandomNumber | libSceRandom | `runtime_dreams.c` | getrandom(), ≤64 bytes | 2026-11-15 |
| `-o5uEDpN+oY` / `0NTHN1NKONI` | sceKernelConvertUtcToLocaltime / ConvertLocaltimeToUtc | libkernel | `runtime_dreams.c` | Host zone. LocaltimeToUtc uses UTC = local + minuteswest·60 (shadPS4 has the sign flipped) | 2026-11-15 |
| `nya-R5gDYhM` (+AudioInOpen) | sceAudioInHqOpen / sceAudioInOpen | libSceAudioIn | `runtime_dreams.c` | Refused: NOT_OPENED 0x80260109 (no mic yet; was handle 0) | 2026-11-15 |
| `p6n3Npi3YY4` | sceCameraIsAttached | libSceCamera | `runtime_dreams.c` | 0 = not attached | 2026-11-15 |
| `65VH0Qaaz6s` | sceSaveDataGetMountInfo | libSceSaveData | `runtime_dreams.c` | 32768 blocks (1 GiB), 32000 free | 2026-11-15 |
