# Dreams (CUSA08010 v2.52): what the executable needs from the PS4 OS

Generated 2026-11-15. Machine-readable data: `notes/dreams-imports.json`
(`imports[file][library] -> [{nid, name, implemented_in_bbport, status, bbport_impl, shadps4_hle, type, module, refs}]`,
plus per-file metadata under `files`). Regenerate with:

```
tools/.venv/bin/python -I tools/pkg_extract.py extract game/Dreams.v2.52-CUSA08010.pkg game/CUSA08010 [--digest]
python3 -I tools/ps4_imports.py game/CUSA08010 notes/dreams-imports.json
python3 -I tools/imports_report.py notes/dreams-imports.json game/CUSA08010.extract.json tools/dreams-imports.header.md notes/dreams-imports.md
```

## 1. Extraction

- **Tool**: `tools/pkg_extract.py`, written for this job (Python + `cryptography` in `tools/.venv`). It does
  PKG -> outer PFS (AES-XTS, fake EKPFS via RSA-decrypting the image key) -> `pfs_image.dat` (PFSC,
  64 KiB zlib blocks) -> inner PFS -> files. It walks the real PFS structures (header, inode table,
  superroot, uroot dirents) instead of the heuristics in the old shadPS4 `pkg.cpp`. That code reads file
  size as `int` (breaks on files over 2 GiB; Dreams has a 4.5 GB zip). The fake RSA keys are read at runtime from
  shadPS4 v0.5.0's `keys.h` (sparse clone in `tools/thirdparty/shadps4-v0.5.0`, the last tag with PKG
  install). LibOrbisPkg/PkgTool was not used.
- **Output**: `game/CUSA08010/` holds 1,591 PFS files (13,072,079,754 bytes) in 235 dirs, plus 21
  `sce_sys/` entries from the PKG entry table (license/np* entries decrypted). There is a per-file SHA-256
  manifest in `game/CUSA08010.extract.json`.
- **Verification**:
  - SHA-256 of the whole encrypted 11.5 GiB PFS image matches the PKG header's `pfs_image_digest`, so the
    .pkg file is intact.
  - Outer PFS: mode 0xD (signed and encrypted), 4 inodes; `pfs_image.dat` is contiguous.
  - PFSC: 200,624 blocks. 56,237 zlib blocks each inflate to exactly 0x10000 bytes with a valid stream end
    (adler32 checked). 144,385 blocks are stored raw and 0 are sparse.
  - Inner PFS: 1,829 dinodes = 1,591 files + 235 dirs + superroot/flat_path_table/uroot. All file extents are
    contiguous and don't overlap, and the last one ends exactly at block 200,624. Every file's written size
    equals its inode size. The extractor reported 0 errors.
- **Disk**: /home had 31 GB free before and about 18-20 GB after. The extraction is 13 GB; the rest of the
  drop came from other activity on the disk. There are no duplicate copies (the .pkg is kept).

## 2. Title / content summary

| Field | Value |
|---|---|
| TITLE / TITLE_ID / CONTENT_ID | Dreams(TM) / CUSA08010 / UP9000-CUSA08010_00-DREAMS0000000000 |
| VERSION / APP_VER | 02.52 / 01.00 (base and patch merged into one gd package, the usual backport layout) |
| CATEGORY / APP_TYPE | gd / 1 (full game). PKG content type 0x1A, DRM 0xF, flags 0x0A000000 |
| Required FW (SYSTEM_VER) | 0x08508000 = **8.50** (so it runs on <=10.01 jailbroken consoles; nothing requires 10.x) |
| SDK (eboot + all prx proc/module param) | 0x08508001 (SDK 8.508.001); PUBTOOLINFO sdk_ver=08508000, c_date=2022-11-21 |
| eboot original filename | `C:/dev/release_2212_PS4_Final/dd/bin/ORBIS_Final_Ninja/dd.elf` (module name `dd`) |
| eboot.bin | 38,957,999 bytes, sha256 73a36228...0fa692 |

Top-level layout (`game/CUSA08010/`):

| Path | Size | Notes |
|---|---:|---|
| `data/` | 13 GB | `videos/` 6.4 GB (1,073 .mp4, played through AvPlayer), `dev_levels/` 4.8 GB (dev_levels_00/01.zip, the second is 4.5 GB), `font/` 792 MB, `splats/`, `ui/`, `gobos/`, `meshes/`, `skies/`, `uisounds.zip`, `credits.aac`, ... |
| `localisation/` | 32 MB | en-US, es-MX, fr, pt-BR |
| `eboot.bin` | 38 MB | main executable |
| `sce_sys/` | 14 MB | param.sfo, icons/pics (png+dds), snd0.at9, trophy00.trp, playgo-*, npbind/nptitle, license.*, changeinfo, about/right.sprx (system file, not loaded by the game) |
| `sce_discmap.plt` | 3.2 MB | DiscMap patch table (libSceDiscMap is imported by Fios2) |
| `shaders/dd.shaders` | 2.8 MB | precompiled GCN shader pack |
| `sce_gls/standby_screen/` | 1.8 MB | 6 .bsf live-streaming standby screens |
| `sce_module/` | 1.8 MB | **libc.prx** (1,330,402), **libSceFios2.prx** (457,713), **libSceJobManager.prx** (68,870) |
| `additionaldics/`, `import/templates/`, `unicorn.ini` | tiny | unicorn.ini = CurrentSKU/DefaultLanguage/AgeRestriction |

Extensions: 1,073 mp4, 156 dat, 151 gnf, 97 svg, 31 unimesh, 26 png, 18 json, 11 fnt, 10 zip, 6 bsf.

## 3. Executables

All four images are fake-signed SELFs: key_type 257, flags 0x22, **0 encrypted and 0 compressed segments**.
auth_id is 0x3100000000000001 for the eboot and 0x...02 for the prx. bbport's `parse_self` rebuilds all of them;
the only missing data is the non-loadable SCE_LIBVERSION phdr. **bbport's own `prepare.py` and
`link_modules.py` run unmodified on Dreams** (with `BB_SKIP_GAME_CHECK=1`):
`prepare` gives 811 imports, 32,874 relocations, 57 required modules and libc `_init_env` == RET.
`link_modules` places libc.prx at 0xb040000 and Fios2 at 0xb160000, with 222 native bindings and 689
imports left to the host runtime. (My table has 812 eboot imports; the extra one is the unreferenced data
symbol `_ZTVSt12bad_weak_ptr`.)

| | eboot.bin | libc.prx | libSceFios2.prx | libSceJobManager.prx |
|---|---|---|---|---|
| ELF type | ET_SCE_DYNEXEC (PIE, base 0), entry 0x80 | ET_SCE_DYNAMIC | ET_SCE_DYNAMIC | ET_SCE_DYNAMIC |
| text (R-X) | 0x0, 26,275,340 B | 1,126,016 B | 378,728 B | 54,176 B |
| relro (R--) | 0x1a00000, 332,928 B | 15,352 B | 1,632 B | 1,312 B |
| data (RW-) | 0x1c00000, filesz 11.4 MB, **memsz 155.4 MB** (about 144 MB bss) | 16,768 / 32,204 | 54,480 / 1,213,740 | 552 / 600 |
| image span | 0xb032348 (176 MiB; bbport probe limit 512 MiB) | 0x11fdcc | 0x18c52c | 0x14258 |
| PT_TLS | present but **size 0** (no static TLS in eboot) | 384/1152, align 16 | 0/64 | 0/8 |
| relocs (type:count) | RELATIVE 32,046; JUMP_SLOT 796; 64 32 | 64 1,129; JUMP_SLOT 107; RELATIVE 668; DTPMOD64 3 | ... DTPMOD64 1 | ... DTPMOD64 2 |
| imports / exports | 812 imports, 60 libraries, 57 DT_NEEDED | 109 imp; exports libc (2,563) + libc_setjmp (2) | 81 imp; exports 168 | 118 imp; exports 11 |

eboot proc param (`ORBI`, 5 entries; the pointer fields are RELATIVE relocations and were resolved):
- process name, main thread name, priority and stack size are not set (system defaults).
- **libc_param** (size 0xA8, 14 entries) sets `_sceLibcMallocReplace`, `_sceLibcNewReplace` and
  `_sceLibcMallocReplaceForTls`. **The game replaces malloc/new/TLS malloc with its own allocator**, so the
  statically linked libc must honour that path, the same way PS4 libc does.
- **kernel_mem_param** sets **flexible_memory_size = 0xD400000 (212 MiB)**.
- fs_param is present but empty, and process_preload_enabled is 0.

DT_TEXTREL is set on eboot, but no relocation targets the text segment. All 30,293 RELATIVE relocations and the
796 GOT slots are in RELRO, and 1,753 are in data. No action needed beyond the usual RELRO handling.

### Unusual / red-flag checks

- **No encryption, no JIT.** No `sceKernelJitCreate*`, `sceKernelMprotect` or `mmap`-with-exec imports. Every
  "jit" string is "jitter" (audio/brush parameters). There is no `sceKernelLoadStartModule` and no `dlsym`
  import, and the strings don't appear in the eboot.
- **Runtime module loading via `sceSysmoduleLoadModule`** (implemented in bbport, `runtime_content.c:module_load`).
  33 call sites were found with `tools/find_import_calls.py`. The IDs, mapped through shadPS4's sysmodule table:
  0xFE JobManager, 0xAD NpTrophy, 0xA4 MsgDialog, 0xAC ErrorDialog, 0xE4 SigninDialog,
  0xB3 NpFriendListDialog, 0xA0 SaveDataDialog, 0xAB WebBrowserDialog, 0xB2 NpProfileDialog,
  0xEB HmdSetupDialog, 0xFD VrServiceDialog, 0xA8 NpCommerce, 0x19 NpUtility, 0x8F Move, 0xBA Random,
  0x9D NpAuth, 0xD1 At9Enc, 0xBC M4aacEnc, 0xA7 Audio3d, 0xD4 Hmd, 0x95 Ime, 0xED VrTracker (later unloaded),
  0xB4 AppContent, 0xA5 AvPlayer, 0xC8 ShareUtility, 0xA6 ContentExport, 0x85 VideoRecording,
  0xC7 ContentSearch, 0x81 GameLiveStreaming, 0x83 PlayGo. All of these are also DT_NEEDED and imported
  statically, so the call only has to report success. **libSceJobManager.prx is game-shipped** but bbport only
  links libc.prx and libSceFios2.prx. JobManager has to be linked as well (`link_modules.py --modules ...`)
  or reimplemented. It needs 59 GnmDriver imports (31 covered), ulobjmgr (2) and RazorCpu (3).
- **One executable** plus three game-shipped prx. The 155 MB bss and 212 MiB flexible memory are large but fit.
- Big systems with no bbport equivalent: **PS VR** (Hmd/VrTracker/dialogs), **Move** and **Camera** (used for
  Move tracking), **Audio3d**, **AT9/AAC encoders** (likely used for capture/sharing), **Ime** (full keyboard
  IME rather than ImeDialog), plus share/streaming. All of these can return "not connected" or "unsupported"
  if the game tolerates it.
- `sceKernelIsNeoMode` is imported (PS4 Pro path) and missing in bbport. `sceKernelIsProspero`
  (libkernel_cpumode_platform) is imported too.
- GnmDriver: 55/103 covered by bbport's gpu HLE. Most of the missing 48 are resource-registration / Razor /
  debug calls (can return OK), but `sceGnmDrawIndexMultiInstanced`, `sceGnmDrawIndirectCountMulti`,
  `sceGnmDrawOpaqueAuto`, `sceGnmComputeWaitSemaphore`, `sceGnmSetGsRingSizes`,
  `sceGnmRequestFlipAndSubmitDone*` and `sceGnmFlushGarlic` are real rendering paths. All of them have
  shadPS4 HLE entries.
- Network/NP: bbport offline mostly stubs these (52 eboot imports go to generic stubs). Dreams' core
  user-generated-content features (the Dreamiverse) depend on NpWebApi/Http, so offline play limits content to
  local files. bbnet20 (BB_ONLINE) adds shadPS4 NP code for another 59 imports.
- The four unnamed NIDs are named by shadPS4 LIB_FUNCTION:
  `libSceGnmWaitFreeSubmit:R3TYO7Tdv5o` (Func_4774D83BB4DDBF9A), `libSceDiscMap:fJgP+wqifno`
  (Func_7C980FFB0AA27E7A), and `ulobjmgr:BG26hBGiNlw/Smf+fUNblPc` (_sceUlobjmgrRegisterObject/Unregister).

## 4. Coverage summary vs bbport runtime

"Implemented" means one of these:
- bbport's C runtime has the NID (exact NID check or `RuntimeExport` name table in `ref/bbport/src/**`).
- bbport's gpu platform tables have it (GnmDriver/VideoOut/AvPlayer/equeue).
- It is exported by libc.prx / libSceFios2.prx, which bbport links from `sce_module/`.

Online-only (bbnet20) and JobManager-provided functions count as **not** implemented.
"Missing" doesn't break loading: bbport's `probe.c` binds every unresolved import to a trap that fires
only when it is called. So "missing" means "traps if Dreams reaches it". Some of libc.prx's missing
libkernel imports (`pthread_rwlock_*`, `_sceKernelSetThreadDtors`, ...) are probably never reached in
Bloodborne either.

| Image | Imports | Implemented in bbport | Missing | Missing but present in shadPS4 HLE |
|---|---:|---:|---:|---:|
| eboot.bin | 812 (60 libs) | 510 (63%, incl. 197 libc via libc.prx; 52 are generic stubs) | 302 | 269 |
| libc.prx | 109 | 84 | 25 | - |
| libSceFios2.prx | 81 | 79 | 2 | - |
| libSceJobManager.prx (not linked by bbport today) | 118 | 76 | 42 | - |

Of the eboot's 60 libraries, 29 have **zero** bbport coverage (167 functions, including the 4 JobManager imports). libkernel is
72/80, and libc is 197/197 through the linked libc.prx.

## Coverage tables (generated by tools/imports_report.py)

Status legend (from dreams-imports.json `notes.status_legend`):
- `bbport:runtime_nid`: exact NID check in ref/bbport/src/*.c
- `bbport:runtime_name`: name in a RuntimeExport table (bbport binds NID->name via import_names*.inc)
- `bbport:gpu_hle`: NID table in ref/bbport/gpu/platform (GnmDriver/VideoOut/AvPlayer/equeue)
- `bbport:bbnet_online_only`: only in ref/bbport/gpu/bbnet20 (shadPS4 NP/net code, BB_ONLINE=1); counted as NOT implemented
- `+online`: also overridden by bbnet20 when BB_ONLINE=1
- `:stub`: bbport maps it to a generic stub (ok_void/net_unreachable/...)
- `game_module:*`: exported by libc.prx/libSceFios2.prx from sce_module/, which bbport links statically
- `game_module_unlinked:*`: exported by another shipped module (libSceJobManager.prx); bbport does not link it yet - counted as NOT implemented
- `missing`: no implementation found in bbport
- *shadPS4 HLE* = NID has a `LIB_FUNCTION` in ref/shadps4 (many of those are stubs too).

### eboot.bin (ET_SCE_DYNEXEC, module `dd`)

| Library | Funcs | bbport impl | of which stubs | online-only (bbnet20) | shadPS4 HLE | Missing in bbport |
|---|---:|---:|---:|---:|---:|---|
| libSceGnmDriver | 103 | 55 | 0 | 0 | 103 | sceGnmComputeWaitSemaphore, sceGnmDebugHardwareStatus, sceGnmDestroyWorkloadStream, sceGnmDisableMipStatsReport, sceGnmDrawIndexMultiInstanced, sceGnmDrawIndirectCountMulti, sceGnmDrawOpaqueAuto, sceGnmDriverCaptureInProgress, sceGnmDriverTraceInProgress, sceGnmDriverTriggerCapture, sceGnmFindResourcesPublic, sceGnmFlushGarlic, ... (+36) |
| libSceHmd | 17 | 0 | 0 | 0 | 17 | sceHmdClose, sceHmdGet2DEyeOffset, sceHmdGetDeviceInformation, sceHmdGetFieldOfView, sceHmdInitialize315, sceHmdOpen, sceHmdReprojectionFinalize, sceHmdReprojectionInitialize, sceHmdReprojectionQueryGarlicBuffAlign, sceHmdReprojectionQueryGarlicBuffSize, sceHmdReprojectionQueryOnionBuffAlign, sceHmdReprojectionQueryOnionBuffSize, ... (+5) |
| libSceGameLiveStreaming | 12 | 0 | 0 | 0 | 12 | sceGameLiveStreamingEnableLiveStreaming, sceGameLiveStreamingGetCurrentBroadcastScreenLayout, sceGameLiveStreamingGetSocialFeedbackMessages, sceGameLiveStreamingGetSocialFeedbackMessagesCount, sceGameLiveStreamingInitialize, sceGameLiveStreamingPermitServerSideRecording, sceGameLiveStreamingPostSocialMessage, sceGameLiveStreamingSetCameraFrameSetting, sceGameLiveStreamingSetLinkCommentPreset, sceGameLiveStreamingSetMetadata, sceGameLiveStreamingSetStandbyScreenResource, sceGameLiveStreamingTerminate |
| libSceVrTracker | 12 | 0 | 0 | 0 | 12 | sceVrTrackerCpuProcess, sceVrTrackerGetResult, sceVrTrackerGetTime, sceVrTrackerGpuSubmit, sceVrTrackerGpuWait, sceVrTrackerInit, sceVrTrackerQueryMemory, sceVrTrackerRecalibrate, sceVrTrackerRegisterDevice, sceVrTrackerTerm, sceVrTrackerUnregisterDevice, sceVrTrackerUpdateMotionSensorData |
| libSceAudio3d | 10 | 0 | 0 | 0 | 10 | sceAudio3dAudioOutClose, sceAudio3dAudioOutOpen, sceAudio3dAudioOutOutputs, sceAudio3dInitialize, sceAudio3dObjectReserve, sceAudio3dObjectSetAttributes, sceAudio3dObjectUnreserve, sceAudio3dPortAdvance, sceAudio3dPortOpen, sceAudio3dPortPush |
| libSceCamera | 10 | 0 | 0 | 0 | 10 | sceCameraClose, sceCameraGetCalibrationData, sceCameraGetFrameData, sceCameraIsAttached, sceCameraOpen, sceCameraSetAttribute, sceCameraSetConfig, sceCameraSetVideoSync, sceCameraStart, sceCameraStop |
| libSceNpManager | 19 | 9 | 6 | 10 | 19 | sceNpCheckNpAvailabilityA, sceNpCreateRequest, sceNpGetAccountCountryA, sceNpGetAccountDateOfBirthA, sceNpGetAccountIdA, sceNpGetAccountLanguageA, sceNpGetParentalControlInfoA, sceNpHasSignedUp, sceNpRegisterNpReachabilityStateCallback, sceNpUnregisterNpReachabilityStateCallback |
| libSceNpUtility | 10 | 0 | 0 | 10 | 10 | sceNpBandwidthTestGetStatus, sceNpBandwidthTestInitStart, sceNpBandwidthTestShutdown, sceNpWordFilterCreateAsyncRequest, sceNpWordFilterCreateTitleCtxA, sceNpWordFilterDeleteRequest, sceNpWordFilterDeleteTitleCtx, sceNpWordFilterPollAsync, sceNpWordFilterSanitizeComment, sceNpWordFilterSetTimeout |
| libSceIme | 9 | 0 | 0 | 0 | 9 | sceImeClose, sceImeGetPanelPositionAndForm, sceImeKeyboardGetInfo, sceImeKeyboardOpen, sceImeOpen, sceImeParamInit, sceImeSetCaret, sceImeSetText, sceImeUpdate |
| libSceNpWebApi | 15 | 6 | 6 | 9 | 15 | sceNpWebApiCreateContextA, sceNpWebApiCreateExtdPushEventFilter, sceNpWebApiCreateHandle, sceNpWebApiDeleteExtdPushEventFilter, sceNpWebApiDeleteHandle, sceNpWebApiGetMemoryPoolStats, sceNpWebApiRegisterExtdPushEventCallbackA, sceNpWebApiSendRequest2, sceNpWebApiUnregisterExtdPushEventCallback |
| libSceMove | 8 | 0 | 0 | 0 | 8 | sceMoveClose, sceMoveGetDeviceInfo, sceMoveInit, sceMoveOpen, sceMoveReadStateRecent, sceMoveResetLightSphere, sceMoveSetLightSphere, sceMoveSetVibration |
| libSceNpParty | 8 | 0 | 0 | 8 | 8 | sceNpPartyCheckCallback, sceNpPartyCreateA, sceNpPartyInitialize, sceNpPartyLeave, sceNpPartyRegisterHandlerA, sceNpPartySendBinaryMessage, sceNpPartyShowInvitationListA, sceNpPartyTerminate |
| libSceRtc | 15 | 7 | 0 | 0 | 15 | sceRtcGetDaysInMonth, sceRtcTickAddDays, sceRtcTickAddHours, sceRtcTickAddMicroseconds, sceRtcTickAddMinutes, sceRtcTickAddMonths, sceRtcTickAddSeconds, sceRtcTickAddYears |
| libkernel | 80 | 72 | 0 | 0 | 80 | sceKernelAvailableDirectMemorySize, sceKernelAvailableFlexibleMemorySize, sceKernelIsNeoMode, sceKernelMemoryPoolGetBlockStats, sceKernelSleep, sceKernelSync, scePthreadGetname, scePthreadGetthreadid |
| libSceContentSearch | 7 | 0 | 0 | 0 | 7 | sceContentSearchCloseMetadata, sceContentSearchGetMetadataFieldInfo, sceContentSearchGetMetadataValue, sceContentSearchGetMyApplicationIndex, sceContentSearchInit, sceContentSearchOpenMetadataByContentId, sceContentSearchSearchContent |
| libSceHmdSetupDialog | 6 | 0 | 0 | 0 | 6 | sceHmdSetupDialogClose, sceHmdSetupDialogGetResult, sceHmdSetupDialogInitialize, sceHmdSetupDialogOpen, sceHmdSetupDialogTerminate, sceHmdSetupDialogUpdateStatus |
| libSceHttp | 25 | 19 | 17 | 6 | 25 | sceHttpCreateConnection, sceHttpGetAllResponseHeaders, sceHttpGetMemoryPoolStats, sceHttpParseResponseHeader, sceHttpUriParse, sceHttpsSetSslCallback |
| libSceVideoRecording | 6 | 0 | 0 | 0 | 1 | sceVideoRecordingClose, sceVideoRecordingGetStatus, sceVideoRecordingOpen2, sceVideoRecordingSetInfo, sceVideoRecordingStart, sceVideoRecordingStop |
| libSceContentExport | 5 | 0 | 0 | 0 | 1 | sceContentExportFinish, sceContentExportFromData, sceContentExportGetProgress, sceContentExportInit2, sceContentExportStart |
| libSceNpFriendListDialog | 5 | 0 | 0 | 0 | 0 | sceNpFriendListDialogGetResultA, sceNpFriendListDialogInitialize, sceNpFriendListDialogOpenA, sceNpFriendListDialogTerminate, sceNpFriendListDialogUpdateStatus |
| libSceSaveData | 11 | 6 | 0 | 0 | 11 | sceSaveDataCheckBackupData, sceSaveDataClearProgress, sceSaveDataGetMountInfo, sceSaveDataGetProgress, sceSaveDataRestoreBackupData |
| libSceVideoOut | 17 | 12 | 0 | 0 | 12 | sceVideoOutCursorDisable, sceVideoOutCursorEnable, sceVideoOutCursorSetHotSpot, sceVideoOutCursorSetImageAddress, sceVideoOutCursorSetPosition |
| libSceVrServiceDialog | 5 | 0 | 0 | 0 | 5 | sceVrServiceDialogClose, sceVrServiceDialogInitialize, sceVrServiceDialogOpen, sceVrServiceDialogTerminate, sceVrServiceDialogUpdateStatus |
| libSceAt9Enc | 4 | 0 | 0 | 0 | 4 | sceAt9EncClearContext, sceAt9EncCreateEncoder, sceAt9EncEncode, sceAt9EncQueryMemSize |
| libSceCoredump | 4 | 0 | 0 | 0 | 1 | sceCoredumpAttachMemoryRegion, sceCoredumpRegisterCoredumpHandler, sceCoredumpSetUserDataType, sceCoredumpWriteUserData |
| libSceErrorDialog | 4 | 0 | 0 | 0 | 4 | sceErrorDialogInitialize, sceErrorDialogOpen, sceErrorDialogTerminate, sceErrorDialogUpdateStatus |
| libSceJobManager (via sce_module/libSceJobManager.prx) | 4 | 0 | 0 | 0 | 0 | _ZN3sce3Job10JobManager10initializeEPvmjjjmPKNS1_12ConfigParamsE, _ZN3sce3Job10JobManager27calculateRequiredMemorySizeEPKNS1_21MemorySizeQueryParamsE, _ZN3sce3Job10JobManager27getSequenceFactoryInterfaceENS1_8PriorityE, _ZN3sce3Job10JobManagerC1Ev |
| libSceM4aacEnc | 4 | 0 | 0 | 0 | 0 | sceM4aacEncCreateEncoder, sceM4aacEncDeleteEncoder, sceM4aacEncEncode, sceM4aacEncFlush |
| libSceNet | 33 | 29 | 15 | 4 | 33 | sceNetEtherNtostr, sceNetGetMacAddress, sceNetGetMemoryPoolStats, sceNetResolverGetError |
| libSceNpSignaling | 9 | 5 | 5 | 4 | 9 | sceNpSignalingActivateConnectionA, sceNpSignalingCreateContextA, sceNpSignalingGetConnectionInfoA, sceNpSignalingGetLocalNetInfo |
| libScePlayGo | 6 | 2 | 0 | 0 | 6 | scePlayGoGetLanguageMask, scePlayGoGetProgress, scePlayGoGetToDoList, scePlayGoSetLanguageMask |
| libSceSigninDialog | 4 | 0 | 0 | 0 | 4 | sceSigninDialogInitialize, sceSigninDialogOpen, sceSigninDialogTerminate, sceSigninDialogUpdateStatus |
| libSceWebBrowserDialog | 4 | 0 | 0 | 0 | 4 | sceWebBrowserDialogInitialize, sceWebBrowserDialogOpen, sceWebBrowserDialogTerminate, sceWebBrowserDialogUpdateStatus |
| libSceAudioOut | 9 | 6 | 0 | 0 | 9 | sceAudioOutMasteringInit, sceAudioOutMasteringSetParam, sceAudioOutSetMixLevelPadSpk |
| libSceNpTrophy | 7 | 4 | 0 | 3 | 7 | sceNpTrophyCaptureScreenshot, sceNpTrophyDestroyContext, sceNpTrophyDestroyHandle |
| libSceSaveDataDialog | 7 | 4 | 0 | 0 | 7 | sceSaveDataDialogClose, sceSaveDataDialogGetResult, sceSaveDataDialogProgressBarSetValue |
| libSceShareUtility | 3 | 0 | 0 | 0 | 0 | sceShareUtilityAdvanceInitialize, sceShareUtilityAdvanceSetUploadContentData, sceShareUtilityAdvanceWakeUpShareMenuDefault |
| libSceMsgDialog | 6 | 4 | 0 | 0 | 6 | sceMsgDialogClose, sceMsgDialogGetStatus |
| libSceNpAuth | 5 | 3 | 1 | 2 | 5 | sceNpAuthAbortRequest, sceNpAuthGetAuthorizationCodeA |
| libScePad | 9 | 7 | 0 | 0 | 9 | scePadSetAngularVelocityBiasCorrectionState, scePadSetLightBar |
| libSceSsl | 4 | 2 | 2 | 2 | 4 | sceSslGetMemoryPoolStats, sceSslGetSerialNumber |
| libSceAjm | 10 | 9 | 0 | 0 | 10 | sceAjmStrError |
| libSceAppContent | 3 | 2 | 0 | 0 | 3 | sceAppContentTemporaryDataMount2 |
| libSceAudioIn | 3 | 2 | 0 | 0 | 3 | sceAudioInHqOpen |
| libSceCommonDialog | 2 | 1 | 0 | 0 | 2 | sceCommonDialogIsUsed |
| libSceGnmWaitFreeSubmit | 1 | 0 | 0 | 0 | 1 | Func_4774D83BB4DDBF9A |
| libSceNpProfileDialog | 4 | 3 | 0 | 1 | 4 | sceNpProfileDialogOpenA |
| libSceOpenPsId | 1 | 0 | 0 | 0 | 1 | sceKernelGetOpenPsId |
| libSceRandom | 1 | 0 | 0 | 0 | 1 | sceRandomGetRandomNumber |
| libSceScreenShot | 1 | 0 | 0 | 0 | 1 | sceScreenShotSetParam |
| libSceSystemService | 5 | 4 | 0 | 0 | 5 | sceSystemServiceGetHdrToneMapLuminance |
| libSceSystemServicePlatformPrivacy | 1 | 0 | 0 | 0 | 1 | sceSystemServiceGetPlatformPrivacySetting |
| libSceUserService | 5 | 4 | 0 | 0 | 5 | sceUserServiceGetUserColor |
| libkernel_cpumode_platform | 1 | 0 | 0 | 0 | 1 | sceKernelIsProspero |
| libSceAvPlayer | 18 | 18 | 0 | 0 | 18 | - |
| libSceNetCtl | 2 | 2 | 0 | 0 | 2 | - |
| libSceNpCommerce | 4 | 4 | 0 | 0 | 4 | - |
| libScePosix | 10 | 10 | 0 | 0 | 10 | - |
| libSceSysmodule | 2 | 2 | 0 | 0 | 2 | - |
| libc (via sce_module/libc.prx) | 197 | 197 | 0 | 0 | 53 | - |
| **total** | **812** | **510** | 52 | 59 | 635 | |

### sce_module/libSceFios2.prx (ET_SCE_DYNAMIC, module `libSceFios2`)

| Library | Funcs | bbport impl | of which stubs | online-only (bbnet20) | shadPS4 HLE | Missing in bbport |
|---|---:|---:|---:|---:|---:|---|
| libSceDiscMap | 2 | 1 | 0 | 0 | 2 | Func_7C980FFB0AA27E7A |
| libSceSysmodule | 2 | 1 | 0 | 0 | 2 | sceSysmoduleLoadModuleInternalWithArg |
| libSceLibcInternal (via sce_module/libc.prx) | 16 | 16 | 0 | 0 | 3 | - |
| libkernel | 61 | 61 | 0 | 0 | 61 | - |
| **total** | **81** | **79** | 0 | 0 | 68 | |

### sce_module/libSceJobManager.prx (ET_SCE_DYNAMIC, module `libSceJobManager`)

| Library | Funcs | bbport impl | of which stubs | online-only (bbnet20) | shadPS4 HLE | Missing in bbport |
|---|---:|---:|---:|---:|---:|---|
| libSceGnmDriver | 59 | 31 | 0 | 0 | 59 | sceGnmComputeWaitSemaphore, sceGnmDebugHardwareStatus, sceGnmDestroyWorkloadStream, sceGnmDisableMipStatsReport, sceGnmDriverCaptureInProgress, sceGnmDriverTraceInProgress, sceGnmDriverTriggerCapture, sceGnmFlushGarlic, sceGnmGetEqTimeStamp, sceGnmGetGpuBlockStatus, sceGnmGetGpuInfoStatus, sceGnmGetLastWaitedAddress, ... (+16) |
| libkernel | 41 | 32 | 0 | 0 | 41 | sceKernelCancelEventFlag, sceKernelClearEventFlag, sceKernelCreateEventFlag, sceKernelDeleteEventFlag, sceKernelIsNeoMode, sceKernelPollEventFlag, sceKernelSetEventFlag, sceKernelWaitEventFlag, scePthreadGetthreadid |
| libSceRazorCpu | 3 | 0 | 0 | 0 | 3 | sceRazorCpuJobManagerDispatch, sceRazorCpuJobManagerJob, sceRazorCpuJobManagerSequence |
| ulobjmgr | 2 | 0 | 0 | 0 | 2 | _sceUlobjmgrRegisterObject, _sceUlobjmgrUnregisterObject |
| libSceLibcInternal (via sce_module/libc.prx) | 13 | 13 | 0 | 0 | 8 | - |
| **total** | **118** | **76** | 0 | 0 | 113 | |

### sce_module/libc.prx (ET_SCE_DYNAMIC, module `libc`)

| Library | Funcs | bbport impl | of which stubs | online-only (bbnet20) | shadPS4 HLE | Missing in bbport |
|---|---:|---:|---:|---:|---:|---|
| libkernel | 96 | 73 | 0 | 0 | 84 | __elf_phdr_match_addr, _is_signal_return, _nanosleep, _sceKernelRtldThreadAtexitDecrement, _sceKernelRtldThreadAtexitIncrement, _sceKernelSetThreadAtexitCount, _sceKernelSetThreadAtexitReport, _sceKernelSetThreadDtors, pthread_rwlock_rdlock, pthread_rwlock_unlock, pthread_rwlock_wrlock, sceKernelChmod, ... (+11) |
| libSceLibcInternalExt | 1 | 0 | 0 | 0 | 1 | sceLibcHeapGetTraceInfo |
| libSceSysmodule | 1 | 0 | 0 | 0 | 1 | sceSysmoduleGetModuleInfoForUnwind |
| libSceFios2 (via sce_module/libSceFios2.prx) | 11 | 11 | 0 | 0 | 0 | - |
| **total** | **109** | **84** | 0 | 0 | 86 | |

### Libraries the eboot imports that bbport has nothing for

- **VR / PSVR**: libSceHmd (17), libSceHmdSetupDialog (6), libSceVrTracker (12), libSceVrServiceDialog (5)
- **Move / Camera**: libSceMove (8), libSceCamera (10)
- **Audio encode / 3D**: libSceAudio3d (10), libSceAt9Enc (4), libSceM4aacEnc (4)
- **Share / capture / streaming**: libSceGameLiveStreaming (12), libSceVideoRecording (6), libSceScreenShot (1), libSceShareUtility (3), libSceContentExport (5), libSceContentSearch (7)
- **System dialogs / IME**: libSceIme (9), libSceErrorDialog (4), libSceSigninDialog (4), libSceNpFriendListDialog (5), libSceWebBrowserDialog (4)
- **NP / online**: libSceNpParty (8), libSceNpUtility (10), libSceOpenPsId (1), libSceSystemServicePlatformPrivacy (1)
- **Debug / misc**: libSceCoredump (4), libSceRandom (1), libkernel_cpumode_platform (1), libSceGnmWaitFreeSubmit (1)
- **Other**: libSceJobManager (4)

### Partially covered eboot libraries: full missing lists

- **libSceAjm** (1/10 missing): `sceAjmStrError`
- **libSceAppContent** (1/3 missing): `sceAppContentTemporaryDataMount2`
- **libSceAudioIn** (1/3 missing): `sceAudioInHqOpen`
- **libSceAudioOut** (3/9 missing): `sceAudioOutMasteringInit`, `sceAudioOutMasteringSetParam`, `sceAudioOutSetMixLevelPadSpk`
- **libSceCommonDialog** (1/2 missing): `sceCommonDialogIsUsed`
- **libSceGnmDriver** (48/103 missing): `sceGnmComputeWaitSemaphore`, `sceGnmDebugHardwareStatus`, `sceGnmDestroyWorkloadStream`, `sceGnmDisableMipStatsReport`, `sceGnmDrawIndexMultiInstanced`, `sceGnmDrawIndirectCountMulti`, `sceGnmDrawOpaqueAuto`, `sceGnmDriverCaptureInProgress`, `sceGnmDriverTraceInProgress`, `sceGnmDriverTriggerCapture`, `sceGnmFindResourcesPublic`, `sceGnmFlushGarlic`, `sceGnmGetEqTimeStamp`, `sceGnmGetGpuBlockStatus`, `sceGnmGetGpuInfoStatus`, `sceGnmGetLastWaitedAddress`, `sceGnmGetNumTcaUnits`, `sceGnmGetOwnerName`, `sceGnmGetPhysicalCounterFromVirtualized`, `sceGnmGetProtectionFaultTimeStamp`, `sceGnmGetResourceBaseAddressAndSizeInBytes`, `sceGnmGetResourceName`, `sceGnmGetResourceShaderGuid`, `sceGnmGetResourceType`, `sceGnmGetResourceUserData`, `sceGnmGetShaderProgramBaseAddress`, `sceGnmGetShaderStatus`, `sceGnmInsertSetColorMarker`, `sceGnmIsUserPaEnabled`, `sceGnmLogicalCuIndexToPhysicalCuIndex`, `sceGnmLogicalTcaUnitToPhysical`, `sceGnmQueryResourceRegistrationUserMemoryRequirements`, `sceGnmRegisterGdsResource`, `sceGnmRegisterOwner`, `sceGnmRegisterResource`, `sceGnmRequestFlipAndSubmitDone`, `sceGnmRequestFlipAndSubmitDoneForWorkload`, `sceGnmRequestMipStatsReportAndReset`, `sceGnmSetGsRingSizes`, `sceGnmSetResourceRegistrationUserMemory`, `sceGnmSetResourceUserData`, `sceGnmSetWaveLimitMultipliers`, `sceGnmSetupMipStatsReport`, `sceGnmUnregisterAllResourcesForOwner`, `sceGnmUnregisterOwnerAndResources`, `sceGnmUnregisterResource`, `sceRazorCaptureImmediate`, `sceRazorIsLoaded`
- **libSceHttp** (6/25 missing): `sceHttpCreateConnection`, `sceHttpGetAllResponseHeaders`, `sceHttpGetMemoryPoolStats`, `sceHttpParseResponseHeader`, `sceHttpUriParse`, `sceHttpsSetSslCallback`
- **libSceMsgDialog** (2/6 missing): `sceMsgDialogClose`, `sceMsgDialogGetStatus`
- **libSceNet** (4/33 missing): `sceNetEtherNtostr`, `sceNetGetMacAddress`, `sceNetGetMemoryPoolStats`, `sceNetResolverGetError`
- **libSceNpAuth** (2/5 missing): `sceNpAuthAbortRequest`, `sceNpAuthGetAuthorizationCodeA`
- **libSceNpManager** (10/19 missing): `sceNpCheckNpAvailabilityA`, `sceNpCreateRequest`, `sceNpGetAccountCountryA`, `sceNpGetAccountDateOfBirthA`, `sceNpGetAccountIdA`, `sceNpGetAccountLanguageA`, `sceNpGetParentalControlInfoA`, `sceNpHasSignedUp`, `sceNpRegisterNpReachabilityStateCallback`, `sceNpUnregisterNpReachabilityStateCallback`
- **libSceNpProfileDialog** (1/4 missing): `sceNpProfileDialogOpenA`
- **libSceNpSignaling** (4/9 missing): `sceNpSignalingActivateConnectionA`, `sceNpSignalingCreateContextA`, `sceNpSignalingGetConnectionInfoA`, `sceNpSignalingGetLocalNetInfo`
- **libSceNpTrophy** (3/7 missing): `sceNpTrophyCaptureScreenshot`, `sceNpTrophyDestroyContext`, `sceNpTrophyDestroyHandle`
- **libSceNpWebApi** (9/15 missing): `sceNpWebApiCreateContextA`, `sceNpWebApiCreateExtdPushEventFilter`, `sceNpWebApiCreateHandle`, `sceNpWebApiDeleteExtdPushEventFilter`, `sceNpWebApiDeleteHandle`, `sceNpWebApiGetMemoryPoolStats`, `sceNpWebApiRegisterExtdPushEventCallbackA`, `sceNpWebApiSendRequest2`, `sceNpWebApiUnregisterExtdPushEventCallback`
- **libScePad** (2/9 missing): `scePadSetAngularVelocityBiasCorrectionState`, `scePadSetLightBar`
- **libScePlayGo** (4/6 missing): `scePlayGoGetLanguageMask`, `scePlayGoGetProgress`, `scePlayGoGetToDoList`, `scePlayGoSetLanguageMask`
- **libSceRtc** (8/15 missing): `sceRtcGetDaysInMonth`, `sceRtcTickAddDays`, `sceRtcTickAddHours`, `sceRtcTickAddMicroseconds`, `sceRtcTickAddMinutes`, `sceRtcTickAddMonths`, `sceRtcTickAddSeconds`, `sceRtcTickAddYears`
- **libSceSaveData** (5/11 missing): `sceSaveDataCheckBackupData`, `sceSaveDataClearProgress`, `sceSaveDataGetMountInfo`, `sceSaveDataGetProgress`, `sceSaveDataRestoreBackupData`
- **libSceSaveDataDialog** (3/7 missing): `sceSaveDataDialogClose`, `sceSaveDataDialogGetResult`, `sceSaveDataDialogProgressBarSetValue`
- **libSceSsl** (2/4 missing): `sceSslGetMemoryPoolStats`, `sceSslGetSerialNumber`
- **libSceSystemService** (1/5 missing): `sceSystemServiceGetHdrToneMapLuminance`
- **libSceUserService** (1/5 missing): `sceUserServiceGetUserColor`
- **libSceVideoOut** (5/17 missing): `sceVideoOutCursorDisable`, `sceVideoOutCursorEnable`, `sceVideoOutCursorSetHotSpot`, `sceVideoOutCursorSetImageAddress`, `sceVideoOutCursorSetPosition`
- **libkernel** (8/80 missing): `sceKernelAvailableDirectMemorySize`, `sceKernelAvailableFlexibleMemorySize`, `sceKernelIsNeoMode`, `sceKernelMemoryPoolGetBlockStats`, `sceKernelSleep`, `sceKernelSync`, `scePthreadGetname`, `scePthreadGetthreadid`

### eboot imports bbport only stubs (generic ok/fail handlers)

`sceHttpAddRequestHeader`, `sceHttpCreateRequestWithURL`, `sceHttpCreateTemplate`, `sceHttpDeleteConnection`, `sceHttpDeleteRequest`, `sceHttpDeleteTemplate`, `sceHttpDestroyEpoll`, `sceHttpGetResponseContentLength`, `sceHttpGetStatusCode`, `sceHttpInit`, `sceHttpReadData`, `sceHttpSendRequest`, `sceHttpSetEpoll`, `sceHttpSetNonblock`, `sceHttpTerm`, `sceHttpUnsetEpoll`, `sceHttpsDisableOption`, `sceNetAccept`, `sceNetBind`, `sceNetConnect`, `sceNetEpollControl`, `sceNetEpollWait`, `sceNetGetsockopt`, `sceNetListen`, `sceNetRecv`, `sceNetRecvfrom`, `sceNetResolverStartNtoa`, `sceNetSend`, `sceNetSendto`, `sceNetSetsockopt`, `sceNetSocket`, `sceNetSocketClose`, `sceNpAuthDeleteRequest`, `sceNpAbortRequest`, `sceNpCheckCallback`, `sceNpDeleteRequest`, `sceNpGetNpId`, `sceNpGetOnlineId`, `sceNpSetContentRestriction`, `sceNpSignalingDeactivateConnection`, `sceNpSignalingDeleteContext`, `sceNpSignalingGetConnectionStatus`, `sceNpSignalingInitialize`, `sceNpSignalingTerminate`, `sceNpWebApiCreateRequest`, `sceNpWebApiDeleteContext`, `sceNpWebApiDeleteRequest`, `sceNpWebApiInitialize`, `sceNpWebApiReadData`, `sceNpWebApiTerminate`, `sceSslInit`, `sceSslTerm`

### Implemented by name in bbport but NID missing from import_names*.inc

bbport's `runtime_lookup` maps NID->name through `import_names*.inc`; these need a table entry (trivial) before the existing implementation binds:

`close`, `ftruncate`, `lseek`, `open`, `pread`, `read`, `sceKernelBatchMap2`, `sceKernelGetProcessTimeCounterFrequency`, `sceKernelGetProcessTimeCounter`, `sceKernelMapNamedDirectMemory`, `scePadRead`, `sceRtcGetTime_t`, `write`

