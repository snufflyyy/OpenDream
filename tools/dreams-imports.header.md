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
