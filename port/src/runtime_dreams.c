// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* PS4 functions Dreams calls that bbport's runtime (written for Bloodborne) didn't implement and
 * whose result matters, so the loader's generic return-0 fallback (probe.c unresolved()) would be
 * wrong: success with uninitialised out-parameters, or a "connected" device that isn't there.
 *
 * Policy (docs/dreams/ARCHITECTURE.md D5/D6): offline, no PS VR / Move / camera / microphone.
 * Semantics follow shadPS4 (ref/shadps4/src/core/libraries/<lib>/...), cited per function.
 * Every function here is listed in docs/dreams/IMPORTS_LOG.md. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdbool.h>
#include <stdio.h>
#include <sys/random.h>
#include <sys/time.h>

/* ---- libSceErrorDialog (ref: ime/error_dialog.cpp, system/commondialog.h) ----
 * Offline, Dreams opens an error dialog for ORBIS_NP_ERROR_SIGNED_OUT (0x80550006) at boot; the
 * Dreams fork dismisses that one silently. We show no UI at all: every dialog is logged and
 * finishes immediately (status FINISHED), which is what the game sees after the user presses OK. */
enum { DIALOG_NONE=0, DIALOG_INITIALIZED=1, DIALOG_RUNNING=2, DIALOG_FINISHED=3 };
#define DIALOG_ERR_NOT_INITIALIZED ((int32_t)0x80B80003)
#define DIALOG_ERR_ARG_NULL ((int32_t)0x80B8000D)
static int error_dialog_status=DIALOG_NONE;
typedef struct { int32_t size, error_code, user_id, reserved; } ErrorDialogParam;
static ABI int32_t error_dialog_initialize(void) { error_dialog_status=DIALOG_INITIALIZED; return 0; }
static ABI int32_t error_dialog_open(const ErrorDialogParam *param) {
    if (!param) return DIALOG_ERR_ARG_NULL;
    printf("Runtime: ErrorDialog code 0x%08x%s: auto-dismissed (no dialog UI)\n",(uint32_t)param->error_code,
           (uint32_t)param->error_code==0x80550006u ? " (NP signed out: expected offline)" : "");
    error_dialog_status=DIALOG_FINISHED;
    return 0;
}
static ABI int32_t error_dialog_status_get(void) { return error_dialog_status; }
static ABI int32_t error_dialog_close(void) { if (error_dialog_status==DIALOG_RUNNING) error_dialog_status=DIALOG_FINISHED; return 0; }
static ABI int32_t error_dialog_terminate(void) { error_dialog_status=DIALOG_NONE; return 0; }

/* ---- libSceSigninDialog (ref: signin_dialog/signindialog.{h,cpp}, signindialog_error.h) ----
 * Offline there is nobody to sign in: no UI, Open logs once and the dialog finishes at once with
 * USER_CANCELED (shadPS4 returns OK; "canceled" is what an offline user choosing Cancel produces and
 * keeps the game from assuming an account exists). Open's parameter (user id, in the real API) is
 * not read. Result: {u32 type (0 OK, 1 USER_CANCELED); s32 reserved[3]}. */
#define SIGNIN_ERR_NOT_INITIALIZED ((int32_t)0x81350001)
#define SIGNIN_ERR_ALREADY_INITIALIZED ((int32_t)0x81350002)
#define SIGNIN_ERR_PARAM_INVALID ((int32_t)0x81350003)
#define SIGNIN_ERR_INVALID_STATE ((int32_t)0x81350005)
typedef struct { uint32_t type; int32_t reserved[3]; } SigninDialogResult;
_Static_assert(sizeof(SigninDialogResult)==16, "OrbisSigninDialogResult layout");
static int signin_status=DIALOG_NONE;
static ABI int32_t signin_initialize(void) {
    if (signin_status!=DIALOG_NONE) return SIGNIN_ERR_ALREADY_INITIALIZED;
    signin_status=DIALOG_INITIALIZED;
    return 0;
}
static ABI int32_t signin_open(const void *param) {
    (void)param;
    if (signin_status==DIALOG_NONE) return SIGNIN_ERR_NOT_INITIALIZED;
    if (signin_status!=DIALOG_INITIALIZED) return SIGNIN_ERR_INVALID_STATE;
    static int logged;
    if (!__atomic_exchange_n(&logged,1,__ATOMIC_RELAXED))
        printf("Runtime: SigninDialog open: offline, finished immediately as USER_CANCELED (no dialog UI)\n");
    signin_status=DIALOG_FINISHED;
    return 0;
}
static ABI int32_t signin_status_get(void) { return signin_status; }
static ABI int32_t signin_get_result(SigninDialogResult *result) {
    if (signin_status==DIALOG_NONE) return SIGNIN_ERR_NOT_INITIALIZED;
    if (!result) return SIGNIN_ERR_PARAM_INVALID;
    if (signin_status!=DIALOG_FINISHED) return SIGNIN_ERR_INVALID_STATE;
    memset(result,0,sizeof(*result));
    result->type=1; /* USER_CANCELED */
    return 0;
}
static ABI int32_t signin_close(void) {
    if (signin_status==DIALOG_NONE) return SIGNIN_ERR_NOT_INITIALIZED;
    if (signin_status!=DIALOG_INITIALIZED) signin_status=DIALOG_INITIALIZED;
    return 0;
}
static ABI int32_t signin_terminate(void) {
    if (signin_status==DIALOG_NONE) return SIGNIN_ERR_NOT_INITIALIZED;
    signin_status=DIALOG_NONE;
    return 0;
}

/* ---- libkernel sceKernelSync (ref: kernel/file_system.cpp: flushes the emulator's open-file table,
 * returns void). Our guest files are plain host fds without userspace buffering, so there is nothing
 * to flush; a host sync() would stall the whole machine for seconds. Deliberate no-op. */
static ABI void kernel_sync(void) {}

/* ---- libSceMove (ref: move/move.cpp) ----
 * No Move controllers. Open still returns a distinct handle (libSceVrTracker internals rely on
 * it); reads report NO_CONTROLLER_CONNECTED (=1, a positive status, not an error code). */
#define MOVE_ERR_NOT_INIT ((int32_t)0x80EE0001)
#define MOVE_ERR_ALREADY_INIT ((int32_t)0x80EE0002)
#define MOVE_ERR_INVALID_ARG ((int32_t)0x80EE0003)
#define MOVE_NO_CONTROLLER 1
static int move_initialized;
static ABI int32_t move_init(void) { if (move_initialized) return MOVE_ERR_ALREADY_INIT; move_initialized=1; return 0; }
static ABI int32_t move_term(void) { if (!move_initialized) return MOVE_ERR_NOT_INIT; move_initialized=0; return 0; }
static ABI int32_t move_open(int32_t user, int32_t type, int32_t index) {
    (void)user; (void)type; (void)index;
    if (!move_initialized) return MOVE_ERR_NOT_INIT;
    static int32_t handle=0x30b0000;
    return __atomic_add_fetch(&handle,0x100,__ATOMIC_RELAXED);
}
static ABI int32_t move_close(int32_t handle) { (void)handle; return move_initialized ? 0 : MOVE_ERR_NOT_INIT; }
static ABI int32_t move_device_info(int32_t handle, void *info) {
    (void)handle;
    if (!move_initialized) return MOVE_ERR_NOT_INIT;
    return info ? MOVE_NO_CONTROLLER : MOVE_ERR_INVALID_ARG;
}
static ABI int32_t move_read_latest(int32_t handle, void *data) {
    (void)handle;
    if (!move_initialized) return MOVE_ERR_NOT_INIT;
    return data ? MOVE_NO_CONTROLLER : MOVE_ERR_INVALID_ARG;
}
static ABI int32_t move_read_recent(int32_t handle, int64_t timestamp, void *data, int32_t *count) {
    (void)handle;
    if (!move_initialized) return MOVE_ERR_NOT_INIT;
    if (timestamp<0 || !data || !count) return MOVE_ERR_INVALID_ARG;
    *count=0;
    return MOVE_NO_CONTROLLER;
}
static ABI int32_t move_ok(void) { return move_initialized ? 0 : MOVE_ERR_NOT_INIT; } /* vibration, light sphere */

/* ---- libSceHmd (ref: hmd/hmd.cpp) ----
 * sceHmdGetDeviceInformation: 32-byte struct {u32 status; s32 user_id; u8 r0[4]; {u32 w,h;
 * u16 lat90,lat120} device_info; u8 hmu_mount; u8 r1[7]}. Status 2 = NOT_DETECTED (no PS VR). */
typedef struct { uint32_t status; int32_t user_id; uint8_t reserve0[4]; uint32_t width, height;
                 uint16_t latency_90hz, latency_120hz; uint8_t hmu_mount, reserve1[7]; } HmdDeviceInformation;
_Static_assert(sizeof(HmdDeviceInformation)==32, "OrbisHmdDeviceInformation layout");
static ABI int32_t hmd_device_information(HmdDeviceInformation *info) {
    if (!info) return (int32_t)0x81110008; /* ORBIS_HMD_ERROR_PARAMETER_NULL (hmd/hmd_error.h) */
    memset(info,0,sizeof(*info));
    info->status=2;
    info->user_id=1;
    return 0;
}

/* ---- libSceNpManager (ref: np/np_manager.cpp) ---- offline: signed out. */
#define NP_ERR_SIGNED_OUT ((int32_t)0x80550006)
#define NP_ERR_INVALID_ARGUMENT ((int32_t)0x80550003)
static ABI int32_t np_account_id(int32_t user, uint64_t *account) {
    if (!account || user==-1) return NP_ERR_INVALID_ARGUMENT;
    *account=0;
    return NP_ERR_SIGNED_OUT;
}
static ABI int32_t np_has_signed_up(int32_t user, bool *signed_up) {
    /* ref: np_manager.cpp sceNpHasSignedUp (fw < 9.00: user id -1 is invalid). Offline policy: the
     * one local user has never signed up to PSN. (The Dreams fork reports true for its offline
     * user; Dreams reads this only to decide whether to offer sign-up.) */
    if (!signed_up) return NP_ERR_INVALID_ARGUMENT;
    *signed_up=false;
    if (user==-1) return NP_ERR_INVALID_ARGUMENT;
    if (user!=1) return (int32_t)0x80550007; /* ORBIS_NP_ERROR_USER_NOT_FOUND */
    return 0;
}

/* ---- libSceRandom (ref: random/random.cpp): at most 64 bytes, from the host CSPRNG. */
static ABI int32_t random_number(uint8_t *buffer, uint64_t size) {
    if (size>64 || (size && !buffer)) return (int32_t)0x817C0016; /* SCE_RANDOM_ERROR_INVALID */
    for (uint64_t done=0; done<size;) {
        ssize_t n=getrandom(buffer+done,size-done,0);
        if (n>0) done+=(uint64_t)n;
    }
    return 0;
}

/* ---- libkernel time zone conversion (ref: kernel/time.cpp). libc.prx's localtime/mktime use
 * these; with return-0 stubs and no out-values, local time was garbage. */
typedef struct { int64_t t; uint32_t west_sec, dst_sec; } KernelTimesec;
typedef struct { int32_t minuteswest, dsttime; } KernelTimezone;
static ABI int32_t utc_to_local(int64_t time, int64_t *local, KernelTimesec *st, uint64_t *dst_sec) {
    time_t t=(time_t)time;
    struct tm tm;
    localtime_r(&t,&tm);
    int32_t dst=tm.tm_isdst>0 ? 3600 : 0;
    if (local) *local=time+RUNTIME_TM_GMTOFF(tm);
    if (st) { st->t=time; st->west_sec=(uint32_t)((int32_t)RUNTIME_TM_GMTOFF(tm)-dst); st->dst_sec=(uint32_t)dst; }
    if (dst_sec) *dst_sec=(uint64_t)dst;
    return 0;
}
static ABI int32_t local_to_utc(int64_t time, int64_t unused, int64_t *seconds, KernelTimezone *tz, int32_t *dst_seconds) {
    (void)unused;
    if (!tz) return (int32_t)0x80020016; /* EINVAL */
    /* Same convention as runtime_kernel.c sceKernelGettimezone: minutes west of UTC including DST,
     * dsttime 0. UTC = local + minuteswest*60. (shadPS4's version subtracts, i.e. has the sign
     * flipped for zones west of UTC; we don't copy that.) */
    time_t now=(time_t)time;
    struct tm tm;
    localtime_r(&now,&tm);
    tz->minuteswest=(int32_t)(-RUNTIME_TM_GMTOFF(tm)/60);
    tz->dsttime=0;
    if (seconds) *seconds=time+(int64_t)tz->minuteswest*60;
    if (dst_seconds) *dst_seconds=tm.tm_isdst>0 ? 3600 : 0;
    return 0;
}

/* ---- libSceAudioIn: no microphone offline (ref: audio/audioin.cpp opens an SDL device; we
 * report the port can't be opened, so the game doesn't read from handle 0). */
static ABI int32_t audio_in_open(int32_t user, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param) {
    (void)user; (void)index; (void)len; (void)param;
    static int warned;
    if (!__atomic_exchange_n(&warned,1,__ATOMIC_RELAXED))
        printf("Runtime: AudioIn open (type %u, %u Hz) refused: no microphone support yet\n",type,freq);
    return (int32_t)0x80260109; /* ORBIS_AUDIO_IN_ERROR_NOT_OPENED */
}

/* ---- libSceCamera: not attached (ref: camera/camera.cpp, 0 = disconnected). */
static ABI int32_t camera_attached(int32_t index) { return index ? (int32_t)0x802E0003 : 0; }

/* ---- libSceSaveData sceSaveDataGetMountInfo (ref: save_data/savedata.cpp): {u64 blocks,
 * freeBlocks; u8 reserved[32]}. Blocks are 32 KiB. We report a generous 1 GiB quota, mostly
 * free; host disk space is the real limit. */
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SaveDataMountInfo;
static ABI int32_t savedata_mount_info(const void *mount_point, SaveDataMountInfo *info) {
    if (!mount_point || !info) return (int32_t)0x809F0000; /* ORBIS_SAVE_DATA_ERROR_PARAMETER */
    memset(info,0,sizeof(*info));
    info->blocks=32768;
    info->free_blocks=32000;
    return 0;
}

static const RuntimeExport exports[]={
    {"sceErrorDialogInitialize",error_dialog_initialize}, {"sceErrorDialogOpen",error_dialog_open},
    {"sceErrorDialogUpdateStatus",error_dialog_status_get}, {"sceErrorDialogGetStatus",error_dialog_status_get},
    {"sceErrorDialogClose",error_dialog_close}, {"sceErrorDialogTerminate",error_dialog_terminate},
    {"sceSigninDialogInitialize",signin_initialize}, {"sceSigninDialogOpen",signin_open},
    {"sceSigninDialogUpdateStatus",signin_status_get}, {"sceSigninDialogGetStatus",signin_status_get},
    {"sceSigninDialogGetResult",signin_get_result}, {"sceSigninDialogClose",signin_close},
    {"sceSigninDialogTerminate",signin_terminate}, {"sceKernelSync",kernel_sync},
    {"sceNpHasSignedUp",np_has_signed_up},
    {"sceMoveInit",move_init}, {"sceMoveTerm",move_term}, {"sceMoveOpen",move_open}, {"sceMoveClose",move_close},
    {"sceMoveGetDeviceInfo",move_device_info}, {"sceMoveReadStateLatest",move_read_latest},
    {"sceMoveReadStateRecent",move_read_recent}, {"sceMoveSetVibration",move_ok},
    {"sceMoveSetLightSphere",move_ok}, {"sceMoveResetLightSphere",move_ok},
    {"sceHmdGetDeviceInformation",hmd_device_information},
    {"sceNpGetAccountIdA",np_account_id},
    {"sceRandomGetRandomNumber",random_number},
    {"sceKernelConvertUtcToLocaltime",utc_to_local}, {"sceKernelConvertLocaltimeToUtc",local_to_utc},
    {"sceAudioInHqOpen",audio_in_open}, {"sceAudioInOpen",audio_in_open},
    {"sceCameraIsAttached",camera_attached},
    {"sceSaveDataGetMountInfo",savedata_mount_info},
};
uintptr_t runtime_dreams_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
