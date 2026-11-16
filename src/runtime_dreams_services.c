// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* System services Dreams calls at boot that bbport's runtime didn't implement (audit of the
 * loader's return-0 fallback, docs/dreams/STUBS.md, 2026-11-15). Two kinds of entries:
 *
 *  1. Real semantics: functions whose return-0 stub left an out-parameter uninitialised or
 *     faked success where the caller then reads state (mount point strings, ids, sizes, word
 *     filter output, libc heap trace pointers).
 *  2. Deliberate no-ops (`ok`): functions where 0 is the correct offline answer (profilers,
 *     streaming/sharing services, crash handler extras). They are listed explicitly so they stop
 *     showing up as `STUB:` lines: STUBS.md then lists only imports nobody has decided on yet.
 *
 * Semantics follow shadPS4 ref/shadps4/src/core/libraries/<lib>/ (branch `dreams` = upstream +
 * Dreams fork), cited per section. Offline/no-peripheral policy: docs/dreams/ARCHITECTURE.md D5/D6.
 * Every entry is listed in docs/dreams/IMPORTS_LOG.md. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ftw.h>
#include <unistd.h>

#define KERNEL_EINVAL ((int32_t)0x80020016)
#define USER_ID 1 /* runtime_services.c: the one logged-in user */

/* Deliberate "success, nothing to do" (see header, kind 2). */
static ABI int32_t ok(void) { return 0; }

/* ---- libkernel / libSceOpenPsId (ref kernel/kernel.cpp sceKernelGetOpenPsId) ----
 * 16-byte console identity. The Dreams fork uses fixed bytes "shadPS4\0Dreams\0\x01" so saves
 * made under Dreams-ShadPS4 builds see the same console: we use the same value. */
static ABI int32_t open_ps_id(uint8_t *id) {
    static const uint8_t value[16]={'s','h','a','d','P','S','4',0,'D','r','e','a','m','s',0,1};
    if (!id) return KERNEL_EINVAL;
    memcpy(id,value,sizeof(value));
    return 0;
}

/* ---- libSceLibcInternalExt sceLibcHeapGetTraceInfo (ref libc_internal/libc_internal_memory.cpp)
 * libc.prx's malloc init asks for the heap trace info and keeps the two pointers. The return-0
 * stub left them as stack garbage. struct {u64 size; u32 flag, getSegmentInfo; u64 *mask, *table;} */
typedef struct { uint64_t size; uint32_t flag, get_segment_info; uint64_t *atomic_id_mask, *mstate_table; } HeapTraceInfo;
static uint64_t heap_atomic_id_mask, heap_mstate_table[64];
static ABI void libc_heap_trace_info(HeapTraceInfo *info) {
    if (!info) return;
    info->atomic_id_mask=&heap_atomic_id_mask;
    info->mstate_table=heap_mstate_table;
    info->get_segment_info=0;
}

/* ---- libSceUserService sceUserServiceGetUserColor (ref system/userservice.cpp) ----
 * Colour index 0..3 (blue, red, green, pink) of the logged-in user: user 1 = blue. */
#define USER_ERR_INVALID_ARGUMENT ((int32_t)0x80960005)
#define USER_ERR_NOT_LOGGED_IN ((int32_t)0x80960009)
static ABI int32_t user_color(int32_t user, int32_t *color) {
    if (!color) return USER_ERR_INVALID_ARGUMENT;
    if (user!=USER_ID) return USER_ERR_NOT_LOGGED_IN;
    *color=0;
    return 0;
}

/* ---- libSceSystemService (ref system/systemservice.cpp; the fork logs the caller) ----
 * Platform privacy setting: 0 (default, no restriction). */
static ABI int32_t platform_privacy(int32_t *setting) { if (setting) *setting=0; return 0; }

/* ---- libSceAppContent sceAppContentTemporaryDataMount2 (ref app_content/app_content.cpp) ----
 * Writes the mount point "/temp0" (OrbisAppContentMountPoint = char[16]). /temp0 is always mapped
 * to <user>/temp0 by runtime_file.c. Option bit 0 (FORMAT) empties it first. */
#define APP_CONTENT_ERR_PARAMETER ((int32_t)0x80D90002)
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag;
    if (ftw->level>0) remove(path);
    return 0;
}
static ABI int32_t temporary_data_mount2(uint32_t option, char *mount_point) {
    if (!mount_point) return APP_CONTENT_ERR_PARAMETER;
    if (option & 1) {
        char host[4096];
        if (!runtime_file_host_path("/temp0",host,sizeof(host)))
            nftw(host,remove_entry,16,FTW_DEPTH|FTW_PHYS);
    }
    memset(mount_point,0,16);
    strcpy(mount_point,"/temp0");
    printf("Runtime: AppContent temporary data mounted at /temp0%s\n",(option & 1) ? " (formatted)" : "");
    return 0;
}

/* ---- libSceContentSearch (ref content_search/content_search.cpp, Dreams fork version) ----
 * Dreams looks up its own application index and metadata at boot. The fork answers index 0 and
 * metadata handle 1 and otherwise succeeds without data; we do the same. */
static ABI int32_t content_search_my_index(int32_t *index) { if (index) *index=0; return 0; }
static ABI int32_t content_search_open_metadata(const void *content_id, int32_t *handle) {
    (void)content_id; if (handle) *handle=1; return 0;
}

/* ---- libSceContentExport (ref content_export/content_export.cpp) ---- init parameter checks;
 * exporting itself (writing to the PS4 media gallery) isn't supported offline. */
#define CONTENT_EXPORT_ERR_NOINIT ((int32_t)0x809D3004)
#define CONTENT_EXPORT_ERR_MULTIPLEINIT ((int32_t)0x809D3005)
#define CONTENT_EXPORT_ERR_INVALIDPARAM ((int32_t)0x809D3016)
typedef struct { void *malloc_fn, *free_fn, *user; uint64_t bufsize; int64_t reserved0, reserved1; } ContentExportInitParam;
static int content_export_initialized;
static int32_t content_export_init_version(const ContentExportInitParam *p, int version) {
    if (content_export_initialized) return CONTENT_EXPORT_ERR_MULTIPLEINIT;
    if (!p || !p->malloc_fn || !p->free_fn) return CONTENT_EXPORT_ERR_INVALIDPARAM;
    if (version==1 && (p->reserved0 || p->reserved1 || (p->bufsize && p->bufsize<0x100))) return CONTENT_EXPORT_ERR_INVALIDPARAM;
    content_export_initialized=1;
    return 0;
}
static ABI int32_t content_export_init(const ContentExportInitParam *p) { return content_export_init_version(p,0); }
static ABI int32_t content_export_init2(const ContentExportInitParam *p) { return content_export_init_version(p,1); }
static ABI int32_t content_export_term(void) {
    if (!content_export_initialized) return CONTENT_EXPORT_ERR_NOINIT;
    content_export_initialized=0;
    return 0;
}

/* ---- libSceNpUtility word filter (ref np/np_utility/np_utility.cpp) ----
 * Dreams filters user-entered text (creation names, comments) through these. Offline there is no
 * server list: CensorComment accepts, SanitizeComment copies the text unchanged. With the return-0
 * stub SanitizeComment returned success with the output buffer untouched (garbage text).
 * Title contexts: ids 0x01000001.., requests 0x02000001.., at most 32 of each. Requests complete
 * immediately (PollAsync reports FINISHED with the stored result). */
#define NPC_ERR_INVALID_ARGUMENT ((int32_t)0x80550704)
#define NPC_ERR_TOO_MANY_OBJECTS ((int32_t)0x80550706)
#define NPC_ERR_ABORTED ((int32_t)0x80550707)
#define NPC_ERR_INSUFFICIENT_ARGUMENT ((int32_t)0x8055070C)
#define NPC_ERR_INVALID_ID ((int32_t)0x8055070E)
#define NPC_ERR_INVALID_TYPE ((int32_t)0x80550711)
#define WORD_FILTER_MAX 32
#define WORD_FILTER_COMMENT_MAX 0x400
typedef struct { int32_t id, user; } WordFilterCtx;
typedef struct { int32_t id, ctx, async, used, aborted, done, result; } WordFilterRequest;
static pthread_mutex_t word_filter_lock=PTHREAD_MUTEX_INITIALIZER;
static WordFilterCtx word_ctx[WORD_FILTER_MAX];
static WordFilterRequest word_req[WORD_FILTER_MAX];
static int32_t word_next_ctx=0x01000001, word_next_req=0x02000001;
static WordFilterRequest *word_request(int32_t id) {
    for (int i=0;i<WORD_FILTER_MAX;++i) if (word_req[i].id && word_req[i].id==id) return &word_req[i];
    return NULL;
}
static ABI int32_t word_filter_create_ctx_a(int32_t user) {
    pthread_mutex_lock(&word_filter_lock);
    int32_t r=NPC_ERR_TOO_MANY_OBJECTS;
    for (int i=0;i<WORD_FILTER_MAX;++i) if (!word_ctx[i].id) { word_ctx[i]=(WordFilterCtx){word_next_ctx++,user}; r=word_ctx[i].id; break; }
    pthread_mutex_unlock(&word_filter_lock);
    return r;
}
static ABI int32_t word_filter_delete_ctx(int32_t ctx) {
    pthread_mutex_lock(&word_filter_lock);
    int32_t r=NPC_ERR_INVALID_ID;
    for (int i=0;i<WORD_FILTER_MAX;++i) if (word_ctx[i].id && word_ctx[i].id==ctx) { word_ctx[i].id=0; r=0; }
    if (!r) for (int i=0;i<WORD_FILTER_MAX;++i) if (word_req[i].id && word_req[i].ctx==ctx) word_req[i].id=0;
    pthread_mutex_unlock(&word_filter_lock);
    return r;
}
static int32_t word_filter_create_request(int32_t ctx, int async) {
    pthread_mutex_lock(&word_filter_lock);
    int known=0;
    for (int i=0;i<WORD_FILTER_MAX;++i) if (word_ctx[i].id && word_ctx[i].id==ctx) known=1;
    int32_t r=known ? NPC_ERR_TOO_MANY_OBJECTS : NPC_ERR_INVALID_ID;
    if (known) for (int i=0;i<WORD_FILTER_MAX;++i) if (!word_req[i].id) {
        word_req[i]=(WordFilterRequest){.id=word_next_req++,.ctx=ctx,.async=async};
        r=word_req[i].id; break;
    }
    pthread_mutex_unlock(&word_filter_lock);
    return r;
}
typedef struct { uint64_t size, cpu_affinity_mask; int32_t thread_priority; uint8_t padding[4]; } WordFilterAsyncParam;
static ABI int32_t word_filter_create_async_request(int32_t ctx, const WordFilterAsyncParam *param) {
    if (!param || param->size!=sizeof(*param)) return NPC_ERR_INVALID_ARGUMENT;
    return word_filter_create_request(ctx,1);
}
static ABI int32_t word_filter_create_request_sync(int32_t ctx) { return word_filter_create_request(ctx,0); }
static ABI int32_t word_filter_delete_request(int32_t id) {
    pthread_mutex_lock(&word_filter_lock);
    WordFilterRequest *q=word_request(id);
    if (q) q->id=0;
    pthread_mutex_unlock(&word_filter_lock);
    return q ? 0 : NPC_ERR_INVALID_ID;
}
static ABI int32_t word_filter_abort_request(int32_t id) {
    pthread_mutex_lock(&word_filter_lock);
    WordFilterRequest *q=word_request(id);
    if (q) { q->aborted=1; q->done=1; q->result=NPC_ERR_ABORTED; }
    pthread_mutex_unlock(&word_filter_lock);
    return q ? 0 : NPC_ERR_INVALID_ID;
}
/* Runs a request to completion with result 0. Returns <0 on a request error, else the value the
 * call returns (async requests return 0 and report through PollAsync). */
static int32_t word_filter_run(int32_t id, const char *comment, char *sanitized) {
    pthread_mutex_lock(&word_filter_lock);
    WordFilterRequest *q=word_request(id);
    int32_t r=!q ? NPC_ERR_INVALID_ID : q->aborted ? NPC_ERR_ABORTED : q->used ? NPC_ERR_INVALID_TYPE : 0;
    if (!r) {
        q->used=1; q->done=1; q->result=0;
        if (sanitized) memcpy(sanitized,comment,strlen(comment)+1);
    }
    pthread_mutex_unlock(&word_filter_lock);
    return r;
}
static ABI int32_t word_filter_sanitize(int32_t id, const char *comment, char *sanitized, void *option) {
    if (!comment || !sanitized) return NPC_ERR_INSUFFICIENT_ARGUMENT;
    if (option || strnlen(comment,WORD_FILTER_COMMENT_MAX+1)>WORD_FILTER_COMMENT_MAX) return NPC_ERR_INVALID_ARGUMENT;
    return word_filter_run(id,comment,sanitized);
}
static ABI int32_t word_filter_censor(int32_t id, const char *comment, void *option) {
    if (!comment) return NPC_ERR_INSUFFICIENT_ARGUMENT;
    if (option || strnlen(comment,WORD_FILTER_COMMENT_MAX+1)>WORD_FILTER_COMMENT_MAX) return NPC_ERR_INVALID_ARGUMENT;
    return word_filter_run(id,comment,NULL);
}
static ABI int32_t word_filter_poll(int32_t id, int32_t *result) {
    if (!result) return NPC_ERR_INVALID_ARGUMENT;
    pthread_mutex_lock(&word_filter_lock);
    WordFilterRequest *q=word_request(id);
    int32_t r=!q || !q->async ? NPC_ERR_INVALID_ID : q->done ? 0 : 1; /* 0 FINISHED, 1 RUNNING */
    if (r==0) *result=q->result;
    pthread_mutex_unlock(&word_filter_lock);
    return r;
}
static ABI int32_t word_filter_set_timeout(int32_t id, int32_t retry, uint32_t resolve, uint32_t conn, uint32_t send, uint32_t recv) {
    if (retry<0 || (resolve && resolve<1000000) || (conn && conn<10000000) || (send && send<10000000) ||
        (recv && recv<10000000) || (!retry && !resolve && !conn && !send && !recv))
        return NPC_ERR_INVALID_ARGUMENT;
    pthread_mutex_lock(&word_filter_lock);
    int known=word_request(id)!=NULL;
    for (int i=0;i<WORD_FILTER_MAX;++i) if (word_ctx[i].id && word_ctx[i].id==id) known=1;
    pthread_mutex_unlock(&word_filter_lock);
    return known ? 0 : NPC_ERR_INVALID_ID;
}

/* ---- libSceNpManager reachability callback (ref np/np_manager.cpp) ----
 * One callback may be registered (second: CALLBACK_ALREADY_REGISTERED). Offline the reachability
 * never changes, so it is never called (bbport's NP callbacks aren't either). */
#define NP_ERR_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NP_ERR_CALLBACK_ALREADY_REGISTERED ((int32_t)0x80550008)
static void *np_reachability_cb;
static ABI int32_t np_register_reachability(void *callback, void *user) {
    (void)user;
    if (!callback) return NP_ERR_INVALID_ARGUMENT;
    if (np_reachability_cb) return NP_ERR_CALLBACK_ALREADY_REGISTERED;
    np_reachability_cb=callback;
    return 0;
}
static ABI int32_t np_unregister_reachability(void) { np_reachability_cb=NULL; return 0; }

/* ---- libSceCoredump (ref kernel/coredump/coredump.cpp) ---- the handler is stored, never run
 * (we don't produce PS4 core dumps; host crashes are reported by probe.c's fault handler). */
#define COREDUMP_ERR_PARAM ((int32_t)0x81180000)
#define COREDUMP_ERR_NOT_REGISTERED ((int32_t)0x81180001)
#define COREDUMP_ERR_ALREADY_REGISTERED ((int32_t)0x81180002)
static void *coredump_handler;
static ABI int32_t coredump_register(void *handler, uint64_t stack_size, void *common) {
    (void)common;
    if (coredump_handler) return COREDUMP_ERR_ALREADY_REGISTERED;
    if (!handler || stack_size>=0x20000000) return COREDUMP_ERR_PARAM;
    coredump_handler=handler;
    return 0;
}
static ABI int32_t coredump_unregister(void) {
    if (!coredump_handler) return COREDUMP_ERR_NOT_REGISTERED;
    coredump_handler=NULL;
    return 0;
}

/* ---- libSceHmd library state and reprojection buffer queries (ref hmd/hmd.cpp,
 * hmd/hmd_reprojection.cpp). No headset: Initialize succeeds, Open returns a handle (libSceVrTracker
 * internals need one), FieldOfView fails as on real FW >= 4.50 without a headset. The Query*
 * functions return sizes/alignments as their value: the return-0 stub made Dreams allocate
 * zero-sized reprojection buffers. */
#define HMD_ERR_ALREADY_INITIALIZED ((int32_t)0x81110001)
#define HMD_ERR_NOT_INITIALIZED ((int32_t)0x81110002)
#define HMD_ERR_HANDLE_INVALID ((int32_t)0x81110003)
#define HMD_ERR_ALREADY_OPENED ((int32_t)0x81110006)
#define HMD_ERR_PARAMETER_NULL ((int32_t)0x81110008)
#define HMD_ERR_PARAMETER_INVALID ((int32_t)0x81110009)
static int hmd_initialized;
static int32_t hmd_handle;
static ABI int32_t hmd_initialize(const void *param) {
    if (hmd_initialized) return HMD_ERR_ALREADY_INITIALIZED;
    if (!param) return HMD_ERR_PARAMETER_NULL;
    hmd_initialized=1;
    return 0;
}
static ABI int32_t hmd_open(int32_t user, int32_t type, int32_t index, const void *param) {
    if (!hmd_initialized) return HMD_ERR_NOT_INITIALIZED;
    if (type || index || param || user==-1 || user==0xFF) return HMD_ERR_PARAMETER_INVALID;
    if (hmd_handle) return HMD_ERR_ALREADY_OPENED;
    hmd_handle=0xf000000;
    return hmd_handle;
}
static ABI int32_t hmd_close(int32_t handle) {
    if (!hmd_initialized) return HMD_ERR_NOT_INITIALIZED;
    if (!handle || handle!=hmd_handle) return HMD_ERR_HANDLE_INVALID;
    hmd_handle=0;
    return 0;
}
static ABI int32_t hmd_field_of_view(int32_t handle, void *fov) {
    (void)handle;
    return fov ? HMD_ERR_HANDLE_INVALID : HMD_ERR_PARAMETER_NULL;
}
static ABI int32_t hmd_onion_size(void) { return 0x810; }
static ABI int32_t hmd_garlic_size(void) { return 0x100000; }
static ABI int32_t hmd_buffer_align(void) { return 0x100; }

/* ---- libSceVrTracker (ref vr_tracker/vr_tracker.cpp) ---- memory query with the library's
 * real sizes (Dreams allocates them at boot), Init/Term state only: no camera, so nothing is ever
 * tracked and GpuSubmit etc. stay unimplemented. */
#define VRT_ERR_NOT_INIT ((int32_t)0x81260801)
#define VRT_ERR_ALREADY_INITIALIZED ((int32_t)0x81260802)
#define VRT_ERR_ARGUMENT_INVALID ((int32_t)0x81260806)
typedef struct { uint32_t size, profile, reserved[6], hmd_position, pad_position, move_position, gun_position, reserved2[4]; } VrTrackerQueryParam;
typedef struct { uint32_t size, onion_size, onion_align, garlic_size, garlic_align, work_size, work_align, reserved[9]; } VrTrackerQueryResult;
_Static_assert(sizeof(VrTrackerQueryParam)==64 && sizeof(VrTrackerQueryResult)==64, "VrTracker query layouts");
static int vr_tracker_initialized;
static ABI int32_t vr_tracker_query_memory(const VrTrackerQueryParam *p, VrTrackerQueryResult *r) {
    if (!p || !r || p->size!=sizeof(*p) || (p->profile!=0 && p->profile!=100) || p->pad_position>1 ||
        p->hmd_position>0 || p->move_position>1 || p->gun_position>1)
        return VRT_ERR_ARGUMENT_INVALID;
    r->onion_size=p->move_position==1 ? 0x800000 : 0x400000;
    r->onion_align=r->garlic_align=r->work_align=0x10000;
    r->garlic_size=0x3000000;
    r->work_size=0x1000000;
    return 0;
}
static ABI int32_t vr_tracker_init(const void *param) {
    if (vr_tracker_initialized) return VRT_ERR_ALREADY_INITIALIZED;
    if (!param) return VRT_ERR_ARGUMENT_INVALID;
    vr_tracker_initialized=1;
    return 0;
}
static ABI int32_t vr_tracker_term(void) {
    if (!vr_tracker_initialized) return VRT_ERR_NOT_INIT;
    vr_tracker_initialized=0;
    return 0;
}

/* libSceIme (keyboard and sceImeOpen text entry) moved to runtime_ime.c. */

/* ---- libSceAt9Enc (ref audio/at9enc.cpp, Dreams fork; artswap ATRAC9 encoder) ----
 * Dreams creates an ATRAC9 encoder at boot (sound recording). The encoder lives in the optional
 * codec library (runtime_codecs.c, out/libdreams-codecs.so from build-deps.sh). Without it, only
 * the exact configuration Dreams uses is accepted and Encode fails cleanly (no PCM consumed). */
typedef struct { uint32_t size; int32_t channels, sample_rate, bitrate_kbps; uint32_t frame_mode, pcm_mode, options[7]; } At9EncParam;
_Static_assert(sizeof(At9EncParam)==52, "At9Enc EncodeParam layout");
static int at9enc_supported(const At9EncParam *p) {
    static const uint32_t observed[7]={0,0,0xffffff01,0xffffff01,4,1,0};
    return p && p->size==sizeof(*p) && (p->channels==1 || p->channels==2) && p->sample_rate==48000 &&
           p->bitrate_kbps==96*p->channels && p->frame_mode==2 && p->pcm_mode==4 && !memcmp(p->options,observed,sizeof(observed));
}
static int32_t at9enc_result(int32_t result, int32_t *error) { if (error) *error=result; return result; }
static ABI int32_t at9enc_query_mem_size(const At9EncParam *p, int32_t *bytes, int32_t *error) {
    if (bytes) *bytes=0;
    const RuntimeCodecs *c=runtime_codecs_get();
    if (!at9enc_supported(p) || !bytes) return at9enc_result(-1,error);
    *bytes=c ? (int32_t)c->at9_context_size() : 0x1000; /* without the encoder: a token work area */
    return at9enc_result(0,error);
}
static ABI int32_t at9enc_create(void *work, const At9EncParam *p, int32_t bytes, int32_t *error) {
    if (!at9enc_supported(p) || bytes<0) return at9enc_result(-1,error);
    const RuntimeCodecs *c=runtime_codecs_get();
    int32_t r=c ? c->at9_context_create(work,(size_t)bytes,p->channels,p->bitrate_kbps) : 0;
    printf("Runtime: ATRAC9 encoder %s (%d ch, %d kb/s)\n",!c ? "accepted WITHOUT codec library (encode fails)" : r ? "creation failed" : "ready",
           p->channels,p->bitrate_kbps);
    return at9enc_result(r,error);
}
static ABI int32_t at9enc_clear(void *work, int32_t *error) {
    const RuntimeCodecs *c=runtime_codecs_get();
    return at9enc_result(c ? c->at9_context_reset(work) : 0,error);
}
static ABI int32_t at9enc_encode(void *work, const int16_t *input, uint8_t *output, int32_t input_bytes, int32_t capacity,
                                 int32_t *output_bytes, int32_t *consumed, int32_t *error) {
    if (output_bytes) *output_bytes=0;
    if (consumed) *consumed=0;
    const RuntimeCodecs *c=runtime_codecs_get();
    if (!c) return at9enc_result(-1,error);
    /* Scratch tables are per thread and reusable (shadPS4: thread_local vector). */
    static __thread void *scratch;
    static __thread size_t scratch_size;
    if (!scratch) {
        scratch_size=c->at9_stream_work_size();
        scratch=malloc(scratch_size);
        if (!scratch) return at9enc_result(-2,error);
    }
    int out=0, used=0;
    int32_t r=c->at9_context_encode(work,scratch,scratch_size,input,input_bytes,output,capacity,&out,&used);
    if (output_bytes) *output_bytes=out;
    if (consumed) *consumed=used;
    return at9enc_result(r,error);
}

/* ---- libkernel misc ---- */
static ABI int32_t kernel_sleep(uint32_t seconds) { /* ref kernel/time.cpp sceKernelSleep */
    struct timespec t={(time_t)seconds,0};
    while (nanosleep(&t,&t)) {}
    return 0;
}

static const RuntimeExport exports[]={
    {"sceKernelGetOpenPsId",open_ps_id}, {"sceKernelGetOpenPsIdForSystem",open_ps_id},
    {"sceLibcHeapGetTraceInfo",libc_heap_trace_info},
    {"sceUserServiceGetUserColor",user_color},
    {"sceSystemServiceGetPlatformPrivacySetting",platform_privacy},
    {"sceAppContentTemporaryDataMount2",temporary_data_mount2},
    {"sceContentSearchInit",ok}, {"sceContentSearchGetMyApplicationIndex",content_search_my_index},
    {"sceContentSearchOpenMetadataByContentId",content_search_open_metadata}, {"sceContentSearchCloseMetadata",ok},
    {"sceContentSearchSearchContent",ok}, {"sceContentSearchGetMetadataValue",ok}, {"sceContentSearchGetMetadataFieldInfo",ok},
    {"sceContentExportInit",content_export_init}, {"sceContentExportInit2",content_export_init2}, {"sceContentExportTerm",content_export_term},
    {"sceNpWordFilterCreateTitleCtxA",word_filter_create_ctx_a}, {"sceNpWordFilterDeleteTitleCtx",word_filter_delete_ctx},
    {"sceNpWordFilterCreateAsyncRequest",word_filter_create_async_request}, {"sceNpWordFilterCreateRequest",word_filter_create_request_sync},
    {"sceNpWordFilterDeleteRequest",word_filter_delete_request}, {"sceNpWordFilterAbortRequest",word_filter_abort_request},
    {"sceNpWordFilterSanitizeComment",word_filter_sanitize}, {"sceNpWordFilterCensorComment",word_filter_censor},
    {"sceNpWordFilterPollAsync",word_filter_poll}, {"sceNpWordFilterSetTimeout",word_filter_set_timeout},
    {"sceNpRegisterNpReachabilityStateCallback",np_register_reachability},
    {"sceNpUnregisterNpReachabilityStateCallback",np_unregister_reachability},
    {"sceCoredumpRegisterCoredumpHandler",coredump_register}, {"sceCoredumpUnregisterCoredumpHandler",coredump_unregister},
    {"sceCoredumpAttachMemoryRegion",ok}, {"sceCoredumpSetUserDataType",ok}, {"sceCoredumpWriteUserData",ok},
    {"sceHmdInitialize315",hmd_initialize}, {"sceHmdInitialize",hmd_initialize}, {"sceHmdOpen",hmd_open}, {"sceHmdClose",hmd_close},
    {"sceHmdGetFieldOfView",hmd_field_of_view},
    {"sceHmdReprojectionQueryOnionBuffSize",hmd_onion_size}, {"sceHmdReprojectionQueryOnionBuffAlign",hmd_buffer_align},
    {"sceHmdReprojectionQueryGarlicBuffSize",hmd_garlic_size}, {"sceHmdReprojectionQueryGarlicBuffAlign",hmd_buffer_align},
    {"sceHmdReprojectionInitialize",ok}, {"sceHmdReprojectionFinalize",ok},
    {"sceVrTrackerQueryMemory",vr_tracker_query_memory}, {"sceVrTrackerInit",vr_tracker_init}, {"sceVrTrackerTerm",vr_tracker_term},
    {"sceAt9EncQueryMemSize",at9enc_query_mem_size}, {"sceAt9EncCreateEncoder",at9enc_create},
    {"sceAt9EncClearContext",at9enc_clear}, {"sceAt9EncEncode",at9enc_encode},
    {"sceKernelSleep",kernel_sleep},
    /* Deliberate no-ops (0 is the right answer offline / without a profiler). Refs: razor_cpu/,
     * ulobjmgr/, kernel/memory.cpp + process.cpp (sanitizers off, not a PS4 Pro/PS5), sharing and
     * streaming services (game_live_streaming/, video_recording/: shadPS4 stubs them to OK; no
     * ShareUtility in shadPS4), system/commondialog.cpp (no common dialog in use), pad/pad.cpp. */
    {"sceRazorCpuJobManagerSequence",ok}, {"sceRazorCpuJobManagerJob",ok}, {"sceRazorCpuJobManagerDispatch",ok},
    {"_sceUlobjmgrRegisterObject",ok}, {"_sceUlobjmgrUnregisterObject",ok},
    {"_sceKernelSetThreadAtexitCount",ok}, {"_sceKernelSetThreadAtexitReport",ok},
    {"sceKernelGetSanitizerNewReplaceExternal",ok}, {"sceKernelGetSanitizerMallocReplaceExternal",ok},
    {"sceKernelIsAddressSanitizerEnabled",ok}, {"sceKernelIsNeoMode",ok}, {"sceKernelIsProspero",ok},
    {"sceCommonDialogIsUsed",ok},
    {"sceShareUtilityAdvanceInitialize",ok}, {"sceShareUtilityAdvanceSetUploadContentData",ok},
    {"sceVideoRecordingSetInfo",ok}, {"sceVideoRecordingClose",ok},
    {"sceGameLiveStreamingInitialize",ok}, {"sceGameLiveStreamingTerminate",ok},
    {"sceGameLiveStreamingPermitServerSideRecording",ok}, {"sceGameLiveStreamingSetStandbyScreenResource",ok},
    {"sceGameLiveStreamingSetCameraFrameSetting",ok}, {"sceGameLiveStreamingSetLinkCommentPreset",ok},
    {"sceGameLiveStreamingSetMetadata",ok}, {"sceGameLiveStreamingEnableLiveStreaming",ok},
    {"scePadSetAngularVelocityBiasCorrectionState",ok}, {"scePadSetLightBar",ok},
    {"sceNpTrophyDestroyHandle",ok}, {"sceNpTrophyDestroyContext",ok},
};
uintptr_t runtime_dreams_services_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
