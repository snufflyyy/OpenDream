# Imports served by the return-0 fallback (17, from cpu-audit-2.log, in first-call order)

**Run type: `--cpu-only`** (2026-11-15, `work/cpu-audit-2.log`, GPU library = vcore build, no GPU
work executes). All 17 remaining entries are GnmDriver/VideoOut: the GPU library's job. A
`--cpu-only` run stops early (the main loop waits for GPU fences), so imports first called later
in boot (with the GPU on) don't appear here; regenerate from a GPU run to see those:
`python3 -I tools/stub_report.py <log> --md port/docs/dreams/STUBS.md`.

Audit of 2026-11-15 (CPU side): every non-GPU entry of the previous list (cpu-only run: 54
entries, GPU run `gpu-run1.log`: 67) now has an explicit implementation; see `IMPORTS_LOG.md`.
Entries where returning 0 was already right are explicit no-ops (`ok` in
`src/runtime_dreams_services.c`) so they no longer show up here. Non-GPU imports that nothing
resolves at load (`BB_LIST_UNRESOLVED=1`): 258 -> 154; the rest are mostly features Dreams only
reaches online, with PS VR/Move/camera, or in dialogs (Np*, Hmd/VrTracker, Camera, *Dialog, Http)
and are listed per library by `python3 -I tools/unresolved_report.py <log>`.

| # | NID | name | first caller | arg0 |
|---|---|---|---|---|
| 1 | `yvZ73uQUqrk` | sceGnmSubmitDone | eboot+0xce4f0b | 0x443b7c |
| 2 | `ln33zjBrfjk` | sceGnmGetTheTessellationFactorRingBufferBaseAddress | eboot+0xcbd054 | 0x316ec2f0 |
| 3 | `ZFqKFl23aMc` | sceGnmRegisterOwner | eboot+0x144ed10 | 0x808e3a2c8 |
| 4 | `b0xyllnVY-I` | sceGnmAddEqEvent | eboot+0xcbd218 | 0x40000 |
| 5 | `A+uGq+3KFtQ` | sceGnmMapComputeQueueWithPriority | eboot+0x2a1b8 | 0x2 |
| 6 | `5udAm+6boVg` | sceGnmCreateWorkloadStream | eboot+0xcbd416 | 0x80179eb2c |
| 7 | `Up36PTk687E` | sceVideoOutOpen | eboot+0xcbdcaf | 0xff |
| 8 | `kGVLc3htQE8` | sceVideoOutGetDeviceCapabilityInfo_ | eboot+0xcbe1db | 0x0 |
| 9 | `nvEwfYAImTs` | sceGnmRegisterResource | eboot+0x1457bd6 | 0x80bef81fc |
| 10 | `fhKwCVVj9nk` | sceGnmUnregisterOwnerAndResources | eboot+0x1434978 | 0xffffffff |
| 11 | `i6-sR91Wt-4` | sceVideoOutSetBufferAttribute | eboot+0x1452c4c | 0x80bef82a0 |
| 12 | `w3BY+tAEiQY` | sceVideoOutRegisterBuffers | eboot+0x1452c83 | 0x0 |
| 13 | `pjkDsgxli6c` | sceVideoOutModeSetAny_ | eboot+0x1452c98 | 0x80bef8270 |
| 14 | `N1bEoJ4SRw4` | sceVideoOutConfigureOutputMode_ | eboot+0x1452cfe | 0x0 |
| 15 | `nF6bFRUBRAU` | sceGnmDispatchInitDefaultHardwareState | eboot+0x2a6f9 | 0x103c24cc00 |
| 16 | `ffrNQOshows` | sceGnmComputeWaitOnAddress | eboot+0x2c0ee | 0x103c24cc2c |
| 17 | `byXlqupd8cE` | sceGnmDingDongForWorkload | eboot+0x33c | 0x0 |
