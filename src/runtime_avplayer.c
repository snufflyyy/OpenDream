// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* libSceAvPlayer: the PS4 system video player, on the CPU side of the native port.
 *
 * Dreams plays its intro cards (data/videos/Mm-Dreams-logo.mp4, ...) and ~1,070 tutorial videos
 * (H.264 + AAC MP4) through it: sceAvPlayerInitEx with the game's allocators and event callback,
 * AddSource, StreamCount/GetStreamInfo/EnableStream on StateReady, Start, then its own threads
 * ("VideoPlayerUpdate", "VideoPlayerAudio") poll GetVideoDataEx / GetAudioData and CurrentTime.
 *
 * Reference semantics: ref/shadps4 (branch dreams) src/core/libraries/avplayer/{avplayer.cpp,
 * avplayer_state.cpp, avplayer_source.cpp}, including the Dreams fork's change that plays
 * Mm-Dreams-logo.mp4 on the wall clock and drops stale frames (SHADPS4_DREAMS_SKIP_LOGO_VIDEO=1
 * skips it, as in the fork). bbport 0.7's C++ port (port/gpu/platform/avplayer) was the model
 * for running FFmpeg natively. This file is plain C on pthreads:
 *
 *   game thread      Init/AddSource (opens the file and reads its streams at once), Start
 *                    (allocates the output buffers with the game's allocators), Get*Data (copies
 *                    the next due frame from the host queue into the next guest buffer of a
 *                    ring: the frame the game holds stays intact until its next call), Close.
 *   decode thread    one per started player: libdreams-av.so's next_frame() (demux + decode +
 *                    NV12 / s16 conversion) into bounded host queues (VIDEO_QUEUE, AUDIO_QUEUE).
 *   controller       one host thread for all players (attached as a guest thread: it calls the
 *                    game's event callback): StateReady after AddSource (or auto start), StateStop
 *                    once a played file is used up.
 *
 * FFmpeg itself lives in out/libdreams-av.so (./build-deps.sh, src/runtime_avplayer/av_glue.c),
 * loaded with dlopen: without it AddSource fails with NOT_SUPPORTED and the game skips its videos.
 * Guest memory the runtime writes (frames) goes through guest_write(): the GPU side is told
 * (runtime_memory_note_write etc.) as runtime_file.c does for file reads.
 *
 * Env: DREAMS_AV_LIB (library path), DREAMS_AVPLAYER_TRACE=1 (calls and events),
 *      DREAMS_AVPLAYER_SYNC_READY=1 (StateReady from AddSource itself, bbport's order),
 *      DREAMS_AVPLAYER_OFF=1 (fail every AddSource: no videos),
 *      SHADPS4_DREAMS_SKIP_LOGO_VIDEO=1 (the logo ends at once, Dreams fork behaviour). */
#define _GNU_SOURCE
#include "runtime.h"
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define ERR_INVALID_PARAMS ((int32_t)0x806A0001)
#define ERR_OPERATION_FAILED ((int32_t)0x806A0002)
#define ERR_NOT_SUPPORTED ((int32_t)0x806A0004)

enum { EV_STATE_STOP=1, EV_STATE_READY=2, EV_STATE_PLAY=3, EV_STATE_PAUSE=4, EV_STATE_BUFFERING=5, EV_WARNING=0x20 };
/* shadPS4 AvState (avplayer_state.h). */
enum { ST_UNKNOWN, ST_INITIAL, ST_ADDING_SOURCE, ST_READY, ST_PLAY, ST_STOP, ST_EOF, ST_PAUSE, ST_C8, ST_JUMP,
       ST_TRICK, ST_CB, ST_BUFFERING, ST_STARTING, ST_ERROR };

/* ---- ABI (ref avplayer.h; sizes checked) ---- */
typedef struct {
    void *object;
    void *(ABI *allocate)(void *object, uint32_t align, uint32_t size);
    void (ABI *deallocate)(void *object, void *memory);
    void *(ABI *allocate_texture)(void *object, uint32_t align, uint32_t size);
    void (ABI *deallocate_texture)(void *object, void *memory);
} AvMemAllocator;
typedef struct {
    void *object;
    int32_t (ABI *open)(void *object, const char *name);
    int32_t (ABI *close)(void *object);
    int32_t (ABI *read_offset)(void *object, uint8_t *buffer, uint64_t position, uint32_t length);
    uint64_t (ABI *size)(void *object);
} AvFileReplacement;
typedef struct {
    void *object;
    void (ABI *callback)(void *object, int32_t event, int32_t source, void *data);
} AvEventReplacement;
typedef struct {
    AvMemAllocator memory; AvFileReplacement file; AvEventReplacement events;
    uint32_t debug_level, base_priority; int32_t video_buffers; uint8_t auto_start, reserved[3];
    const char *default_language;
} AvInitData;
typedef struct {
    uint64_t this_size;
    AvMemAllocator memory; AvFileReplacement file; AvEventReplacement events;
    const char *default_language;
    uint32_t debug_level, priorities[12];
    int32_t video_buffers; uint8_t auto_start, reserved[3];
} AvInitDataEx;
typedef struct { const char *name; uint32_t length; uint8_t reserved1[4]; uint8_t reserved2[64]; uint32_t source_type; uint8_t reserved3[44]; } AvSourceDetails;
typedef struct { uint16_t channels; uint8_t reserved[2]; uint32_t sample_rate, size; char language[4]; } AvAudio;
typedef struct { uint32_t width, height; float aspect; char language[4]; } AvVideo;
typedef union { uint8_t reserved[16]; AvAudio audio; AvVideo video; } AvStreamDetails;
typedef struct { uint32_t type; uint8_t reserved[4]; AvStreamDetails details; uint64_t duration, start_time; } AvStreamInfo;
typedef struct { void *data; uint8_t reserved[4]; uint64_t timestamp; AvStreamDetails details; } AvFrameInfo;
typedef struct {
    uint32_t width, height; float aspect; uint8_t language[4]; uint32_t framerate;
    uint32_t crop_left, crop_right, crop_top, crop_bottom, pitch;
    uint8_t luma_bits, chroma_bits, full_range, reserved[37];
} AvVideoEx;
typedef struct { uint16_t channels; uint8_t reserved[2]; uint32_t sample_rate, size; uint8_t language[4]; uint8_t reserved1[64]; } AvAudioEx;
typedef union { AvAudioEx audio; AvVideoEx video; uint8_t reserved[80]; } AvStreamDetailsEx;
typedef struct { void *data; uint8_t reserved[4]; uint64_t timestamp; AvStreamDetailsEx details; } AvFrameInfoEx;
_Static_assert(sizeof(AvInitData)==120, "AvPlayerInitData");
_Static_assert(sizeof(AvInitDataEx)==176 && __builtin_offsetof(AvInitDataEx,video_buffers)==164, "AvPlayerInitDataEx");
_Static_assert(sizeof(AvSourceDetails)==128 && __builtin_offsetof(AvSourceDetails,source_type)==80, "AvPlayerSourceDetails");
_Static_assert(sizeof(AvStreamInfo)==40, "AvPlayerStreamInfo");
_Static_assert(sizeof(AvFrameInfo)==40 && __builtin_offsetof(AvFrameInfo,timestamp)==16, "AvPlayerFrameInfo");
_Static_assert(sizeof(AvVideoEx)==80 && sizeof(AvFrameInfoEx)==104, "AvPlayerFrameInfoEx");

/* ---- The decoder library ---- */
static pthread_once_t av_once=PTHREAD_ONCE_INIT;
static RuntimeAv av_api;
static int av_loaded;
static void av_load(void) {
    char path[PATH_MAX];
    const char *env=getenv("DREAMS_AV_LIB");
    if (env && *env) snprintf(path,sizeof(path),"%s",env);
    else {
        ssize_t n=readlink("/proc/self/exe",path,sizeof(path)-1);
        if (n<=0) return;
        path[n]=0;
        char *slash=strrchr(path,'/');
        snprintf(slash ? slash+1 : path,sizeof(path)-(size_t)(slash ? slash+1-path : 0),"libdreams-av.so");
    }
    void *lib=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if (!lib) { printf("Runtime: AvPlayer library %s not loaded (%s): videos skipped. Run ./build-deps.sh\n",path,dlerror()); return; }
#define SYM(field,name) do { *(void **)&av_api.field=dlsym(lib,name); if (!av_api.field) { \
        printf("Runtime: AvPlayer library %s lacks %s; not used\n",path,name); dlclose(lib); return; } } while (0)
    SYM(open,"dreams_av_open"); SYM(close,"dreams_av_close"); SYM(stream_count,"dreams_av_stream_count");
    SYM(stream_info,"dreams_av_stream_info"); SYM(start,"dreams_av_start"); SYM(seek,"dreams_av_seek");
    SYM(next_frame,"dreams_av_next_frame"); SYM(error,"dreams_av_error"); SYM(version,"dreams_av_version");
#undef SYM
    av_loaded=1;
    printf("Runtime: AvPlayer library %s loaded (FFmpeg %s)\n",path,av_api.version());
}
const RuntimeAv *runtime_av_get(void) { pthread_once(&av_once,av_load); return av_loaded ? &av_api : NULL; }

/* ---- Helpers ---- */
static int trace_on(void) {
    static int on=-1;
    if (on<0) { const char *e=getenv("DREAMS_AVPLAYER_TRACE"); on=e && e[0]=='1'; }
    return on;
}
static void trace(const char *format, ...) __attribute__((format(printf,1,2)));
static void trace(const char *format, ...) {
    if (!trace_on()) return;
    char text[320];
    va_list args; va_start(args,format); vsnprintf(text,sizeof(text),format,args); va_end(args);
    printf("AvPlayer: %s\n",text);
}
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec; }
/* The CPU writes guest memory outside guest code: the GPU side hears of it (runtime_file.c). */
static void guest_write(void *dst, const void *src, size_t size) {
    if (!size) return;
    runtime_memory_note_write((uintptr_t)dst,size);
    runtime_memory_prepare_cpu_write((uintptr_t)dst,size);
    memcpy(dst,src,size);
    runtime_memory_note_write((uintptr_t)dst,size);
    runtime_memory_note_cpu_write((uintptr_t)dst,size);
}

/* ---- Player ---- */
#define MAX_STREAMS 16
#define VIDEO_QUEUE 6   /* decoded frames ahead (host memory, ~3 MB each at 1080p) */
#define AUDIO_QUEUE 48  /* ~1 s of 1024-sample AAC frames */
#define AUDIO_BUFFER (8*4*1024) /* shadPS4: 8 channels x 4 bytes x 1024 samples */
typedef struct { uint8_t *data; size_t capacity; RuntimeAvFrame info; } HostFrame;
typedef struct { HostFrame slots[AUDIO_QUEUE]; int head, count, capacity; } FrameQueue;

typedef struct Player {
    struct Player *next;
    int refs;          /* the list's 1 + the controller's while it works on it (under players_lock) */
    int closed;
    AvInitData init;
    int auto_start;
    char default_language[4];
    pthread_mutex_t lock;
    pthread_cond_t room;   /* a queue slot was freed / the decoder must stop */
    int state, previous_state;
    int sync_none;         /* sceAvPlayerSetAvSyncMode(None): frames as soon as decoded */
    /* source */
    void *media;
    char path[512];
    int file_open;         /* the game's open() file callback succeeded: close() owed */
    int realtime;          /* Mm-Dreams-logo.mp4 (Dreams fork): wall clock, stale frames dropped */
    int skip_logo;
    RuntimeAvStream streams[MAX_STREAMS];
    int stream_count, video_stream, audio_stream;
    uint64_t duration_ms;
    /* playback */
    int started, eof, decode_error, thread_running, decoding, stop_decoder; /* running: joinable; decoding: not yet exited */
    pthread_t thread;
    FrameQueue video, audio;
    void *video_buffers[16]; int video_buffer_count, video_next; uint32_t video_buffer_size;
    void *audio_buffers[32]; int audio_buffer_count, audio_next;
    /* clock (ref AvPlayerSource::CurrentTime) */
    uint64_t start_ns, pause_ns, paused_ns, last_audio_ns, base_ms, last_audio_ms, skip_until_ms;
    int paused, have_audio_ms;
    /* controller */
    int ready_pending, stop_event_sent;
} Player;

static pthread_mutex_t players_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t players_cv=PTHREAD_COND_INITIALIZER;
static Player *players;
static int controller_started;
static pthread_t controller_tid;
static atomic_ulong stat_video_frames, stat_audio_frames, stat_players, stat_sources;

static void queue_clear(FrameQueue *q) { q->head=0; q->count=0; }
static void queue_free(FrameQueue *q) { for (int i=0;i<AUDIO_QUEUE;++i) { free(q->slots[i].data); q->slots[i].data=NULL; q->slots[i].capacity=0; } q->head=q->count=0; }
static HostFrame *queue_front(FrameQueue *q) { return q->count ? &q->slots[q->head] : NULL; }
static void queue_pop(FrameQueue *q) { if (q->count) { q->head=(q->head+1)%q->capacity; --q->count; } }

static int set_state(Player *p, int next) {
    /* ref AvPlayerState::IsStateTransitionValid */
    const int c=p->state, over=c==ST_STOP || c==ST_EOF || c==ST_ERROR;
    int ok=1;
    switch (next) {
    case ST_PLAY: ok=!over; break;
    case ST_PAUSE: ok=!over && c!=ST_STARTING; break;
    case ST_JUMP: ok=!over && c!=ST_TRICK && c!=ST_STARTING; break;
    case ST_BUFFERING: ok=!over && c!=ST_PAUSE && c!=ST_STARTING; break;
    default: break;
    }
    if (!ok) { trace("state %d -> %d refused",c,next); return 0; }
    p->previous_state=c; p->state=next;
    return 1;
}

/* Milliseconds of playback; called with p->lock held. */
static uint64_t current_ms(Player *p) {
    if (!p->started) return 0;
    const uint64_t now=p->paused ? p->pause_ns : mono_ns();
    const int active=!p->eof || p->video.count || p->audio.count;
    if (p->realtime || p->audio_stream<0) {
        const int64_t elapsed=(int64_t)(now-p->start_ns-p->paused_ns)/1000000;
        uint64_t t=p->base_ms+(uint64_t)(elapsed>0 ? elapsed : 0);
        return p->duration_ms && t>p->duration_ms ? p->duration_ms : t;
    }
    if (p->eof && !active) return p->duration_ms;
    const uint64_t since=p->have_audio_ms ? p->last_audio_ns : now;
    const int64_t elapsed=(int64_t)(now-since-p->paused_ns)/1000000;
    return (p->have_audio_ms ? p->last_audio_ms : p->base_ms)+(uint64_t)(elapsed>0 ? elapsed : 0);
}
static int source_active(Player *p) { return p->media && (!p->eof || p->video.count || p->audio.count || p->decoding); }

/* ---- Decode thread ---- */
static int32_t reader_read(void *opaque, uint8_t *buffer, uint64_t position, uint32_t length) {
    Player *p=opaque;
    return p->init.file.read_offset(p->init.file.object,buffer,position,length);
}
static void *decode_thread(void *arg) {
    Player *p=arg;
    const RuntimeAv *av=runtime_av_get();
    if (p->init.file.read_offset) runtime_thread_attach_host("AvPlayerDecode"); /* guest read callback */
    pthread_setname_np(pthread_self(),"AvPlayerDecode");
    RuntimeAvFrame f;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        const int stop=p->stop_decoder;
        pthread_mutex_unlock(&p->lock);
        if (stop) break;
        int r;
        if (p->skip_logo) { usleep(100000); r=0; } /* Dreams fork: SHADPS4_DREAMS_SKIP_LOGO_VIDEO */
        else r=av->next_frame(p->media,&f);
        pthread_mutex_lock(&p->lock);
        if (r<=0) {
            if (r<0) { p->decode_error=1; fprintf(stderr,"Runtime: AvPlayer %s: %s\n",p->path,av->error(p->media)); }
            p->eof=1;
            if (p->state==ST_PLAY || p->state==ST_BUFFERING || p->state==ST_PAUSE) set_state(p,ST_EOF);
            pthread_mutex_unlock(&p->lock);
            break;
        }
        FrameQueue *q=f.type==0 ? &p->video : &p->audio;
        if ((f.type==0 && p->video_stream<0) || (f.type==1 && p->audio_stream<0) ||
            f.timestamp_ms+(f.type==0 ? 34 : 22)<=p->skip_until_ms) { /* before a JumpToTime target (decoded from its keyframe) */
            pthread_mutex_unlock(&p->lock); continue;
        }
        while (q->count==q->capacity && !p->stop_decoder) {
            /* A full queue whose oldest frame is already due and has a successor: that frame is
             * stale (the game is not taking frames, or is behind), drop it rather than stall the
             * other stream (Dreams fork: for the logo; here for video always, audio on the wall
             * clock only). */
            HostFrame *front=queue_front(q);
            if (q->count>1 && (f.type==0 || p->realtime) && front->info.timestamp_ms+100<=current_ms(p)) { queue_pop(q); break; }
            struct timespec until; clock_gettime(CLOCK_MONOTONIC,&until);
            until.tv_nsec+=5000000; if (until.tv_nsec>=1000000000) { until.tv_nsec-=1000000000; ++until.tv_sec; }
            pthread_cond_timedwait(&p->room,&p->lock,&until);
        }
        if (p->stop_decoder) { pthread_mutex_unlock(&p->lock); break; }
        HostFrame *slot=&q->slots[(q->head+q->count)%q->capacity];
        if (slot->capacity<f.size) {
            uint8_t *data=realloc(slot->data,f.size);
            if (!data) { p->decode_error=p->eof=1; pthread_mutex_unlock(&p->lock); break; }
            slot->data=data; slot->capacity=f.size;
        }
        memcpy(slot->data,f.data,f.size);
        slot->info=f; slot->info.data=slot->data;
        ++q->count;
        pthread_mutex_unlock(&p->lock);
    }
    pthread_mutex_lock(&p->lock);
    p->decoding=0;
    pthread_mutex_unlock(&p->lock);
    return NULL;
}
static void stop_decoder(Player *p) {
    pthread_mutex_lock(&p->lock);
    const int running=p->thread_running;
    p->stop_decoder=1;
    pthread_cond_broadcast(&p->room);
    pthread_mutex_unlock(&p->lock);
    if (running) pthread_join(p->thread,NULL);
    pthread_mutex_lock(&p->lock);
    p->thread_running=0; p->stop_decoder=0;
    queue_clear(&p->video); queue_clear(&p->audio);
    pthread_mutex_unlock(&p->lock);
}
static int start_decoder(Player *p) {
    p->eof=p->decode_error=0; p->stop_decoder=0;
    if (pthread_create(&p->thread,NULL,decode_thread,p)) return 0;
    p->thread_running=p->decoding=1;
    return 1;
}

/* ---- Events ---- */
static void emit(Player *p, int event, void *data) {
    trace("%p event %d to the game (state %d)",(void *)p,event,p->state);
    if (p->init.events.callback) p->init.events.callback(p->init.events.object,event,0,data);
}
static int32_t player_start(Player *p);
static int32_t enable_stream(Player *p, uint32_t index);
/* ref AvPlayerState::AutoPlayEventCallback: first stream of each kind, or the default language's. */
static void auto_start(Player *p) {
    int video=-1, audio=-1;
    for (int i=0;i<p->stream_count;++i) {
        const RuntimeAvStream *s=&p->streams[i];
        int *slot=s->type==0 ? &video : &audio;
        if (*slot<0 || (p->default_language[0] && !strncmp(p->default_language,s->language,4))) *slot=i;
    }
    if (video>=0) enable_stream(p,(uint32_t)video);
    if (audio>=0) enable_stream(p,(uint32_t)audio);
    player_start(p);
}
static void announce_ready(Player *p) {
    if (p->auto_start) auto_start(p);
    else emit(p,EV_STATE_READY,NULL);
}

static void *controller_thread(void *arg) {
    (void)arg;
    runtime_thread_attach_host("AvPlayerControl");
    pthread_setname_np(pthread_self(),"AvPlayerCtl");
    for (;;) {
        Player *work[16]; int n=0;
        pthread_mutex_lock(&players_lock);
        for (Player *p=players;p && n<16;p=p->next) if (!p->closed) { ++p->refs; work[n++]=p; }
        pthread_mutex_unlock(&players_lock);
        for (int i=0;i<n;++i) {
            Player *p=work[i];
            int ready=0, stop=0;
            pthread_mutex_lock(&p->lock);
            if (p->ready_pending) { p->ready_pending=0; if (p->state==ST_ADDING_SOURCE) { set_state(p,ST_READY); ready=1; } }
            /* ref AvPlayerState::UpdateEndOfFileState */
            if (p->state==ST_EOF && !source_active(p) && !p->stop_event_sent) { p->stop_event_sent=1; stop=1; }
            pthread_mutex_unlock(&p->lock);
            if (ready) announce_ready(p);
            if (stop) emit(p,EV_STATE_STOP,NULL);
        }
        pthread_mutex_lock(&players_lock);
        for (int i=0;i<n;++i) --work[i]->refs;
        pthread_cond_broadcast(&players_cv);
        pthread_mutex_unlock(&players_lock);
        usleep(5000);
    }
    return NULL;
}

/* ---- API ---- */
static int has_allocators(const AvMemAllocator *m) { return m->allocate && m->deallocate && m->allocate_texture && m->deallocate_texture; }
static Player *player_new(const AvInitData *init) {
    Player *p=calloc(1,sizeof(*p));
    if (!p) return NULL;
    p->init=*init;
    p->auto_start=!init->events.callback || init->auto_start;
    if (init->default_language) memcpy(p->default_language,init->default_language,strnlen(init->default_language,4));
    pthread_mutex_init(&p->lock,NULL);
    pthread_condattr_t ca; pthread_condattr_init(&ca); pthread_condattr_setclock(&ca,CLOCK_MONOTONIC);
    pthread_cond_init(&p->room,&ca); pthread_condattr_destroy(&ca);
    p->state=p->previous_state=ST_INITIAL;
    p->video_stream=p->audio_stream=-1;
    p->video.capacity=VIDEO_QUEUE; p->audio.capacity=AUDIO_QUEUE;
    p->refs=1;
    pthread_mutex_lock(&players_lock);
    p->next=players; players=p;
    if (!controller_started) {
        pthread_t t;
        if (!pthread_create(&t,NULL,controller_thread,NULL)) { pthread_detach(t); controller_tid=t; controller_started=1; }
    }
    pthread_mutex_unlock(&players_lock);
    atomic_fetch_add(&stat_players,1);
    return p;
}
static ABI Player *av_init(AvInitData *data) {
    if (!data || !has_allocators(&data->memory)) return NULL;
    Player *p=player_new(data);
    trace("Init -> %p (auto start %d, callback %d, %d buffers)",(void *)p,data->auto_start,data->events.callback!=NULL,data->video_buffers);
    return p;
}
static ABI int32_t av_init_ex(const AvInitDataEx *data, Player **out) {
    if (!data || !out || !has_allocators(&data->memory)) return ERR_INVALID_PARAMS;
    AvInitData init={0};
    init.memory=data->memory; init.file=data->file; init.events=data->events;
    init.default_language=data->default_language; init.video_buffers=data->video_buffers; init.auto_start=data->auto_start;
    *out=player_new(&init);
    trace("InitEx -> %p (auto start %d, callback %d, file callbacks %d, %d buffers, language %.3s)",(void *)*out,
          data->auto_start,data->events.callback!=NULL,data->file.open!=NULL,data->video_buffers,
          data->default_language ? data->default_language : "-");
    return *out ? 0 : ERR_OPERATION_FAILED;
}
static ABI int32_t av_post_init(Player *p, const void *data) { return p && data ? 0 : ERR_INVALID_PARAMS; }

static void free_buffers(Player *p) {
    for (int i=0;i<p->video_buffer_count;++i) if (p->video_buffers[i]) p->init.memory.deallocate_texture(p->init.memory.object,p->video_buffers[i]);
    for (int i=0;i<p->audio_buffer_count;++i) if (p->audio_buffers[i]) p->init.memory.deallocate(p->init.memory.object,p->audio_buffers[i]);
    p->video_buffer_count=p->audio_buffer_count=0; p->video_next=p->audio_next=0;
}

static int32_t add_source(Player *p, const char *guest_path, size_t length) {
    if (!length || length>=sizeof(p->path)) return ERR_INVALID_PARAMS;
    char path[512]; memcpy(path,guest_path,length); path[length]=0;
    const char *dot=strrchr(path,'.');
    if (dot && !strcasecmp(dot,".m3u8")) { printf("Runtime: AvPlayer: HTTP live streaming not supported (%s)\n",path); return ERR_NOT_SUPPORTED; }
    const RuntimeAv *av=runtime_av_get();
    const char *off=getenv("DREAMS_AVPLAYER_OFF");
    if (!av || (off && off[0]=='1')) { printf("Runtime: AvPlayer: %s not played (%s)\n",path,av ? "DREAMS_AVPLAYER_OFF" : "no decoder library"); return ERR_NOT_SUPPORTED; }
    pthread_mutex_lock(&p->lock);
    const int busy=p->media!=NULL;
    pthread_mutex_unlock(&p->lock);
    if (busy) { trace("AddSource: one source per player"); return ERR_OPERATION_FAILED; }
    void *media=NULL; int file_open=0;
    if (p->init.file.open && p->init.file.read_offset && p->init.file.size) {
        if (p->init.file.open(p->init.file.object,path)<0) { printf("Runtime: AvPlayer: the game's file callback can't open %s\n",path); return ERR_OPERATION_FAILED; }
        file_open=1;
        RuntimeAvReader reader={.opaque=p,.size=p->init.file.size(p->init.file.object),.read=reader_read};
        media=av->open(path,&reader);
    } else {
        char host[1024];
        if (runtime_file_translate(path,host,sizeof(host))) { printf("Runtime: AvPlayer: no host file for %s\n",path); return ERR_OPERATION_FAILED; }
        media=av->open(host,NULL);
    }
    if (!media) {
        if (file_open && p->init.file.close) p->init.file.close(p->init.file.object);
        pthread_mutex_lock(&p->lock); set_state(p,ST_ERROR); pthread_mutex_unlock(&p->lock);
        return ERR_OPERATION_FAILED;
    }
    pthread_mutex_lock(&p->lock);
    p->media=media; p->file_open=file_open;
    snprintf(p->path,sizeof(p->path),"%s",path);
    const size_t n=strlen(path), logo=strlen("/Mm-Dreams-logo.mp4");
    p->realtime=n>=logo && !strcmp(path+n-logo,"/Mm-Dreams-logo.mp4"); /* ref avplayer_source.cpp Init */
    const char *skip=getenv("SHADPS4_DREAMS_SKIP_LOGO_VIDEO");
    p->skip_logo=p->realtime && skip && skip[0] && skip[0]!='0';
    p->stream_count=av->stream_count(media);
    if (p->stream_count>MAX_STREAMS) p->stream_count=MAX_STREAMS;
    p->duration_ms=0;
    for (int i=0;i<p->stream_count;++i) {
        av->stream_info(media,i,&p->streams[i]);
        if (p->streams[i].duration_ms>p->duration_ms) p->duration_ms=p->streams[i].duration_ms;
    }
    const int found=p->stream_count>0;
    set_state(p,found ? ST_ADDING_SOURCE : ST_ERROR);
    const char *sync=getenv("DREAMS_AVPLAYER_SYNC_READY");
    const int sync_ready=found && sync && sync[0]=='1';
    if (found && !sync_ready) p->ready_pending=1;
    if (sync_ready) set_state(p,ST_READY);
    pthread_mutex_unlock(&p->lock);
    atomic_fetch_add(&stat_sources,1);
    printf("Runtime: AvPlayer: %s: %d stream(s), %llu ms%s\n",path,p->stream_count,(unsigned long long)p->duration_ms,
           p->realtime ? (p->skip_logo ? " (Dreams logo: skipped)" : " (Dreams logo: wall clock)") : "");
    if (!found) return ERR_NOT_SUPPORTED;
    if (sync_ready) announce_ready(p);
    return 0;
}
static ABI int32_t av_add_source(Player *p, const char *path) {
    if (!p || !path) return ERR_INVALID_PARAMS;
    const int32_t r=add_source(p,path,strlen(path));
    trace("AddSource %p %s -> %#x",(void *)p,path,(unsigned)r);
    return r;
}
static ABI int32_t av_add_source_ex(Player *p, uint32_t uri_type, const AvSourceDetails *details) {
    if (!p || uri_type!=0 || !details || !details->name) return ERR_INVALID_PARAMS;
    const int32_t r=add_source(p,details->name,strnlen(details->name,details->length));
    trace("AddSourceEx %p %.*s (type %u) -> %#x",(void *)p,(int)details->length,details->name,details->source_type,(unsigned)r);
    return r;
}
static ABI int32_t av_stream_count(Player *p) {
    if (!p) return ERR_INVALID_PARAMS;
    pthread_mutex_lock(&p->lock);
    const int32_t n=p->media ? p->stream_count : ERR_OPERATION_FAILED;
    pthread_mutex_unlock(&p->lock);
    trace("StreamCount %p -> %d",(void *)p,n);
    return n;
}
static ABI int32_t av_stream_info(Player *p, uint32_t index, AvStreamInfo *info) {
    if (!p || !info) return ERR_INVALID_PARAMS;
    pthread_mutex_lock(&p->lock);
    if (!p->media || index>=(uint32_t)p->stream_count) { pthread_mutex_unlock(&p->lock); return ERR_OPERATION_FAILED; }
    const RuntimeAvStream *s=&p->streams[index];
    memset(info,0,sizeof(*info));
    info->duration=s->duration_ms;
    if (s->type==0) {
        info->type=0;
        info->details.video.width=(s->width+15u)&~15u; info->details.video.height=(s->height+15u)&~15u;
        info->details.video.aspect=s->aspect;
        memcpy(info->details.video.language,s->language,4);
    } else {
        info->type=1;
        info->details.audio.channels=(uint16_t)s->channels; info->details.audio.sample_rate=s->sample_rate;
        memcpy(info->details.audio.language,s->language,4);
    }
    pthread_mutex_unlock(&p->lock);
    trace("GetStreamInfo %p %u -> type %u, %llu ms, language '%.4s'",(void *)p,index,info->type,(unsigned long long)info->duration,s->language);
    return 0;
}
static int32_t enable_stream(Player *p, uint32_t index) {
    pthread_mutex_lock(&p->lock);
    int32_t r=ERR_OPERATION_FAILED;
    if (p->media && index<(uint32_t)p->stream_count) {
        if (p->streams[index].type==0) p->video_stream=(int)index; else p->audio_stream=(int)index;
        r=0;
    }
    pthread_mutex_unlock(&p->lock);
    trace("EnableStream %p %u -> %#x",(void *)p,index,(unsigned)r);
    return r;
}
static ABI int32_t av_enable_stream(Player *p, uint32_t index) { return p ? enable_stream(p,index) : ERR_INVALID_PARAMS; }
static ABI int32_t av_disable_stream(Player *p, uint32_t index) {
    if (!p) return ERR_INVALID_PARAMS;
    pthread_mutex_lock(&p->lock);
    if (!p->started) { /* takes effect at the next Start (shadPS4: stub) */
        if (p->video_stream==(int)index) p->video_stream=-1;
        if (p->audio_stream==(int)index) p->audio_stream=-1;
    }
    pthread_mutex_unlock(&p->lock);
    trace("DisableStream %p %u",(void *)p,index);
    return 0;
}

/* Playback off (decoder stopped, queues and clock reset); buffers kept. */
static void halt(Player *p) {
    stop_decoder(p);
    pthread_mutex_lock(&p->lock);
    p->started=0; p->paused=0; p->eof=0;
    pthread_mutex_unlock(&p->lock);
}
static int32_t player_start(Player *p) {
    pthread_mutex_lock(&p->lock);
    const int s=p->state;
    const int startable=p->media && s!=ST_UNKNOWN && s!=ST_INITIAL && s!=ST_ADDING_SOURCE && s!=ST_ERROR &&
                        (p->video_stream>=0 || p->audio_stream>=0);
    const int video=p->video_stream, audio=p->audio_stream;
    pthread_mutex_unlock(&p->lock);
    if (!startable) { trace("Start %p refused (state %d)",(void *)p,s); return ERR_OPERATION_FAILED; }
    halt(p); /* a playback under way (or ended) starts over */
    const RuntimeAv *av=runtime_av_get();
    if (av->start(p->media,video,audio)) {
        fprintf(stderr,"Runtime: AvPlayer %s: %s\n",p->path,av->error(p->media));
        pthread_mutex_lock(&p->lock); set_state(p,ST_ERROR); pthread_mutex_unlock(&p->lock);
        return ERR_OPERATION_FAILED;
    }
    /* Output buffers from the game's allocators, on this (game) thread, as shadPS4 Start does. */
    free_buffers(p);
    int count=p->init.video_buffers<2 ? 2 : p->init.video_buffers>16 ? 16 : p->init.video_buffers;
    if (video>=0) {
        const uint32_t pitch=(p->streams[video].width+63u)&~63u, height=(p->streams[video].height+15u)&~15u;
        p->video_buffer_size=pitch*height*3/2;
        for (int i=0;i<count;++i) p->video_buffers[p->video_buffer_count++]=p->init.memory.allocate_texture(p->init.memory.object,0x100,p->video_buffer_size);
    }
    if (audio>=0) for (int i=0;i<count*2;++i) p->audio_buffers[p->audio_buffer_count++]=p->init.memory.allocate(p->init.memory.object,0x10,AUDIO_BUFFER);
    for (int i=0;i<p->video_buffer_count;++i) if (!p->video_buffers[i]) { free_buffers(p); fprintf(stderr,"Runtime: AvPlayer: the game's allocator gave no frame buffer\n"); return ERR_OPERATION_FAILED; }
    for (int i=0;i<p->audio_buffer_count;++i) if (!p->audio_buffers[i]) { free_buffers(p); fprintf(stderr,"Runtime: AvPlayer: the game's allocator gave no audio buffer\n"); return ERR_OPERATION_FAILED; }
    pthread_mutex_lock(&p->lock);
    p->stop_event_sent=0;
    set_state(p,ST_STARTING);
    p->start_ns=mono_ns(); p->paused_ns=0; p->paused=0; p->base_ms=0; p->have_audio_ms=0; p->skip_until_ms=0;
    p->started=1;
    const int ok=start_decoder(p);
    if (ok) set_state(p,ST_PLAY); else { set_state(p,ST_ERROR); p->started=0; }
    pthread_mutex_unlock(&p->lock);
    trace("Start %p (%s, video %d, audio %d, %d buffers of %u bytes) -> %s",(void *)p,p->path,video,audio,p->video_buffer_count,p->video_buffer_size,ok ? "ok" : "failed");
    if (!ok) return ERR_OPERATION_FAILED;
    emit(p,EV_STATE_PLAY,NULL);
    return 0;
}
static ABI int32_t av_start(Player *p) { return p ? player_start(p) : ERR_INVALID_PARAMS; }
static int32_t player_stop(Player *p) {
    pthread_mutex_lock(&p->lock);
    const int refused=!p->media || p->state==ST_STOP;
    pthread_mutex_unlock(&p->lock);
    if (refused) return ERR_OPERATION_FAILED;
    halt(p);
    pthread_mutex_lock(&p->lock);
    set_state(p,ST_STOP);
    p->stop_event_sent=1;
    pthread_mutex_unlock(&p->lock);
    emit(p,EV_STATE_STOP,NULL);
    return 0;
}
static ABI int32_t av_stop(Player *p) {
    const int32_t r=p ? player_stop(p) : ERR_INVALID_PARAMS;
    trace("Stop %p -> %#x",(void *)p,(unsigned)r);
    return r;
}
static ABI int32_t av_pause(Player *p) {
    if (!p) return ERR_INVALID_PARAMS;
    pthread_mutex_lock(&p->lock);
    const int s=p->state;
    if (s==ST_EOF) { pthread_mutex_unlock(&p->lock); return 0; }
    if (!p->media || s==ST_PAUSE || s==ST_READY || s==ST_INITIAL || s==ST_UNKNOWN || s==ST_ADDING_SOURCE || !set_state(p,ST_PAUSE)) {
        pthread_mutex_unlock(&p->lock); return ERR_OPERATION_FAILED;
    }
    p->paused=1; p->pause_ns=mono_ns();
    pthread_mutex_unlock(&p->lock);
    emit(p,EV_STATE_PAUSE,NULL);
    return 0;
}
static ABI int32_t av_resume(Player *p) {
    if (!p) return ERR_INVALID_PARAMS;
    pthread_mutex_lock(&p->lock);
    if (!p->media || p->state!=ST_PAUSE) { pthread_mutex_unlock(&p->lock); return ERR_OPERATION_FAILED; }
    p->paused_ns+=mono_ns()-p->pause_ns; p->paused=0;
    const int back=p->previous_state;
    set_state(p,back);
    pthread_mutex_unlock(&p->lock);
    const int event=back==ST_PLAY ? EV_STATE_PLAY : back==ST_BUFFERING ? EV_STATE_BUFFERING : back==ST_STOP ? EV_STATE_STOP : 0;
    if (event) emit(p,event,NULL);
    return 0;
}
/* shadPS4 stubs this; here it seeks for real: decoder restarted at `ms`, clock set there. */
static ABI int32_t av_jump(Player *p, uint64_t ms) {
    if (!p) return ERR_INVALID_PARAMS;
    pthread_mutex_lock(&p->lock);
    const int playing=p->started && p->media && p->state!=ST_STOP && p->state!=ST_ERROR;
    pthread_mutex_unlock(&p->lock);
    trace("JumpToTime %p %llu ms%s",(void *)p,(unsigned long long)ms,playing ? "" : " (not playing: ignored)");
    if (!playing) return 0;
    stop_decoder(p);
    runtime_av_get()->seek(p->media,ms);
    pthread_mutex_lock(&p->lock);
    p->start_ns=mono_ns(); p->paused_ns=0; p->base_ms=ms; p->have_audio_ms=0; p->skip_until_ms=ms;
    if (p->paused) p->pause_ns=p->start_ns;
    if (p->state==ST_EOF) { p->state=ST_PLAY; p->stop_event_sent=0; }
    start_decoder(p);
    pthread_mutex_unlock(&p->lock);
    return 0;
}

static ABI int32_t av_close(Player *p) {
    if (!p) return ERR_INVALID_PARAMS;
    trace("Close %p",(void *)p);
    halt(p);
    pthread_mutex_lock(&players_lock);
    p->closed=1;
    for (Player **at=&players;*at;at=&(*at)->next) if (*at==p) { *at=p->next; break; }
    /* The controller may be working on this player: wait, unless this is its own event callback. */
    if (!(controller_started && pthread_equal(pthread_self(),controller_tid)))
        while (p->refs>1) pthread_cond_wait(&players_cv,&players_lock);
    pthread_mutex_unlock(&players_lock);
    free_buffers(p);
    if (p->media) runtime_av_get()->close(p->media);
    if (p->file_open && p->init.file.close) p->init.file.close(p->init.file.object);
    queue_free(&p->video); queue_free(&p->audio);
    /* The record itself is kept (a few KB per player): the controller or a late game call may still
     * hold the pointer; freeing it is not worth that race. */
    p->media=NULL;
    return 0;
}

static ABI int av_get_video_ex(Player *p, AvFrameInfoEx *info) {
    if (!p || !info) return 0;
    pthread_mutex_lock(&p->lock);
    HostFrame *f=p->started && !p->paused && p->state!=ST_STOP && p->state!=ST_ERROR ? queue_front(&p->video) : NULL;
    if (!f || (!p->sync_none && f->info.timestamp_ms>current_ms(p)) || !p->video_buffer_count) { pthread_mutex_unlock(&p->lock); return 0; }
    void *dst=p->video_buffers[p->video_next];
    if (f->info.size>p->video_buffer_size) { /* resolution change mid-file: not supported */
        queue_pop(&p->video); pthread_cond_broadcast(&p->room); pthread_mutex_unlock(&p->lock);
        fprintf(stderr,"Runtime: AvPlayer: frame of %llu bytes > buffer %u: dropped\n",(unsigned long long)f->info.size,p->video_buffer_size);
        return 0;
    }
    p->video_next=(p->video_next+1)%p->video_buffer_count;
    guest_write(dst,f->info.data,f->info.size);
    memset(info,0,sizeof(*info));
    info->data=dst;
    info->timestamp=f->info.timestamp_ms;
    AvVideoEx *v=&info->details.video;
    v->width=f->info.width; v->height=f->info.height; v->aspect=f->info.sample_aspect;
    v->crop_left=f->info.crop_left; v->crop_right=f->info.crop_right; v->crop_top=f->info.crop_top; v->crop_bottom=f->info.crop_bottom;
    v->pitch=f->info.pitch; v->luma_bits=8; v->chroma_bits=8;
    queue_pop(&p->video);
    pthread_cond_broadcast(&p->room);
    pthread_mutex_unlock(&p->lock);
    const unsigned long n=atomic_fetch_add(&stat_video_frames,1)+1;
    if (trace_on() && (n<=3 || n%300==0)) trace("video frame %lu: %llu ms, %ux%u pitch %u -> %p",n,(unsigned long long)info->timestamp,v->width,v->height,v->pitch,dst);
    return 1;
}
static ABI int av_get_video(Player *p, AvFrameInfo *info) {
    AvFrameInfoEx ex;
    if (!info || !av_get_video_ex(p,&ex)) return 0;
    memset(info,0,sizeof(*info));
    info->data=ex.data; info->timestamp=ex.timestamp;
    info->details.video.width=ex.details.video.width; info->details.video.height=ex.details.video.height;
    info->details.video.aspect=ex.details.video.aspect;
    return 1;
}
static ABI int av_get_audio(Player *p, AvFrameInfo *info) {
    if (!p || !info) return 0;
    pthread_mutex_lock(&p->lock);
    HostFrame *f=p->started && !p->paused && p->state!=ST_STOP && p->state!=ST_ERROR ? queue_front(&p->audio) : NULL;
    if (!f || !p->audio_buffer_count || (p->realtime && f->info.timestamp_ms>current_ms(p))) { pthread_mutex_unlock(&p->lock); return 0; }
    void *dst=p->audio_buffers[p->audio_next];
    p->audio_next=(p->audio_next+1)%p->audio_buffer_count;
    const size_t size=f->info.size<AUDIO_BUFFER ? f->info.size : AUDIO_BUFFER;
    guest_write(dst,f->info.data,size);
    p->last_audio_ms=f->info.timestamp_ms; p->last_audio_ns=mono_ns(); p->have_audio_ms=1;
    if (!p->realtime) p->paused_ns=0; /* ref: m_pause_duration reset with each audio frame */
    memset(info,0,sizeof(*info));
    info->data=dst; info->timestamp=f->info.timestamp_ms;
    info->details.audio.channels=(uint16_t)f->info.channels; info->details.audio.sample_rate=f->info.sample_rate;
    info->details.audio.size=(uint32_t)size;
    queue_pop(&p->audio);
    pthread_cond_broadcast(&p->room);
    pthread_mutex_unlock(&p->lock);
    atomic_fetch_add(&stat_audio_frames,1);
    return 1;
}
static ABI int av_is_active(Player *p) {
    if (!p) return 0;
    pthread_mutex_lock(&p->lock);
    const int active=p->media && p->state!=ST_STOP && p->state!=ST_ERROR && (!p->started || source_active(p));
    pthread_mutex_unlock(&p->lock);
    static Player *last_player; static int last=-1;
    if (p!=last_player || active!=last) { last_player=p; last=active; trace("IsActive %p -> %d",(void *)p,active); }
    return active;
}
static ABI uint64_t av_current_time(Player *p) {
    if (!p) return (uint64_t)(int64_t)ERR_INVALID_PARAMS;
    pthread_mutex_lock(&p->lock);
    const uint64_t t=p->media ? current_ms(p) : 0;
    pthread_mutex_unlock(&p->lock);
    return t;
}
static ABI int32_t av_set_sync(Player *p, int32_t mode) {
    if (!p) return ERR_INVALID_PARAMS;
    pthread_mutex_lock(&p->lock); p->sync_none=mode==1; pthread_mutex_unlock(&p->lock);
    return 0;
}
static ABI int32_t av_set_looping(Player *p, int looping) {
    if (!p) return ERR_INVALID_PARAMS;
    if (looping) printf("Runtime: AvPlayer: looping not supported (plays once)\n");
    return 0;
}
static ABI int32_t av_ok_handle(Player *p) { return p ? 0 : ERR_INVALID_PARAMS; }
static ABI int32_t av_ok(void) { return 0; }

int runtime_avplayer_active(void) {
    int active=0;
    pthread_mutex_lock(&players_lock);
    for (Player *p=players;p && !active;p=p->next) {
        pthread_mutex_lock(&p->lock);
        active=p->started && p->state==ST_PLAY;
        pthread_mutex_unlock(&p->lock);
    }
    pthread_mutex_unlock(&players_lock);
    return active;
}
/* Ends the playing video(s) as if the file ended: the game gets StateStop from the controller and
 * moves on (its own Stop/Close then free the buffers on its thread). */
int runtime_avplayer_skip(void) {
    int skipped=0;
    pthread_mutex_lock(&players_lock);
    for (Player *p=players;p;p=p->next) {
        pthread_mutex_lock(&p->lock);
        if (p->started && p->state==ST_PLAY) {
            p->stop_decoder=1; pthread_cond_broadcast(&p->room);
            queue_clear(&p->video); queue_clear(&p->audio);
            set_state(p,ST_EOF);
            skipped=1;
            printf("Runtime: AvPlayer: %s skipped by the player\n",p->path);
        }
        pthread_mutex_unlock(&p->lock);
    }
    pthread_mutex_unlock(&players_lock);
    return skipped;
}

static const RuntimeExport exports[]={
    {"sceAvPlayerInit",av_init}, {"sceAvPlayerInitEx",av_init_ex}, {"sceAvPlayerPostInit",av_post_init},
    {"sceAvPlayerAddSource",av_add_source}, {"sceAvPlayerAddSourceEx",av_add_source_ex},
    {"sceAvPlayerStreamCount",av_stream_count}, {"sceAvPlayerGetStreamInfo",av_stream_info},
    {"sceAvPlayerEnableStream",av_enable_stream}, {"sceAvPlayerDisableStream",av_disable_stream},
    {"sceAvPlayerStart",av_start}, {"sceAvPlayerStop",av_stop}, {"sceAvPlayerPause",av_pause},
    {"sceAvPlayerResume",av_resume}, {"sceAvPlayerJumpToTime",av_jump}, {"sceAvPlayerClose",av_close},
    {"sceAvPlayerGetVideoDataEx",av_get_video_ex}, {"sceAvPlayerGetVideoData",av_get_video},
    {"sceAvPlayerGetAudioData",av_get_audio}, {"sceAvPlayerIsActive",av_is_active},
    {"sceAvPlayerCurrentTime",av_current_time}, {"sceAvPlayerSetAvSyncMode",av_set_sync},
    {"sceAvPlayerSetLooping",av_set_looping}, {"sceAvPlayerSetTrickSpeed",av_ok_handle},
    {"sceAvPlayerChangeStream",av_ok}, {"sceAvPlayerSetLogCallback",av_ok}, {"sceAvPlayerVprintf",av_ok},
};
uintptr_t runtime_avplayer_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_avplayer_report(void) {
    printf("Runtime: AvPlayer players=%lu sources=%lu video frames=%lu audio frames=%lu\n",
           atomic_load(&stat_players),atomic_load(&stat_sources),atomic_load(&stat_video_frames),atomic_load(&stat_audio_frames));
}
