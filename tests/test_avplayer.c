// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* libSceAvPlayer (src/runtime_avplayer.c + out/libdreams-av.so) on a small generated MP4, the way
 * Dreams drives it: InitEx with allocators and an event callback, AddSource, StateReady, stream
 * info, EnableStream, Start, then GetVideoDataEx / GetAudioData polled against CurrentTime until
 * StateStop; also JumpToTime, the game's file callbacks, and auto start without a callback.
 *   avplayer-test <libdreams-av.so> <clip.mp4: 200x120 solid red H.264 30 fps + 48 kHz stereo AAC, 2 s>
 * (tests/run-dreams-tests.sh makes the clip with ffmpeg). */
#include "../src/runtime_avplayer.c"
#include <assert.h>
#include <math.h>

/* ---- Runtime pieces the file uses ---- */
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,nid)) return (uintptr_t)table[i].function;
    return 0;
}
static atomic_ulong notes;
void runtime_memory_note_write(uintptr_t a, uint64_t s) { (void)a; (void)s; atomic_fetch_add(&notes,1); }
void runtime_memory_note_cpu_write(uintptr_t a, uint64_t s) { (void)a; (void)s; }
void runtime_memory_prepare_cpu_write(uintptr_t a, uint64_t s) { (void)a; (void)s; }
void runtime_thread_attach_host(const char *name) { (void)name; }
static const char *clip;
int runtime_file_translate(const char *guest, char *out, size_t size) {
    if (strcmp(guest,"/app0/data/videos/test.mp4") && strcmp(guest,"/app0/data/videos/Mm-Dreams-logo.mp4")) return 2;
    snprintf(out,size,"%s",clip); return 0;
}

/* ---- The "game" ---- */
static atomic_int allocations, frees;
static ABI void *alloc(void *o, uint32_t align, uint32_t size) { (void)o; atomic_fetch_add(&allocations,1); return aligned_alloc(align<16 ? 16 : align,(size+align-1)/align*align); }
static ABI void dealloc(void *o, void *p) { (void)o; atomic_fetch_add(&frees,1); free(p); }
static atomic_int events[8];
static ABI void on_event(void *o, int32_t event, int32_t source, void *data) {
    (void)source; (void)data;
    assert(o==(void *)0x1234);
    if (event>=0 && event<8) atomic_fetch_add(&events[event],1);
}
static FILE *game_file;
static ABI int32_t file_open(void *o, const char *name) { (void)o; assert(strstr(name,"test.mp4")); game_file=fopen(clip,"rb"); return game_file ? 0 : -1; }
static ABI int32_t file_close(void *o) { (void)o; fclose(game_file); game_file=NULL; return 0; }
static ABI int32_t file_read(void *o, uint8_t *b, uint64_t pos, uint32_t len) { (void)o; fseek(game_file,(long)pos,SEEK_SET); return (int32_t)fread(b,1,len,game_file); }
static ABI uint64_t file_size(void *o) { (void)o; fseek(game_file,0,SEEK_END); return (uint64_t)ftell(game_file); }

#define F(type,name) ((type)runtime_avplayer_resolve(name))
static int32_t (ABI *init_ex)(const AvInitDataEx *, Player **);
static int32_t (ABI *t_add_source)(Player *, const char *);
static int32_t (ABI *stream_count)(Player *);
static int32_t (ABI *stream_info)(Player *, uint32_t, AvStreamInfo *);
static int32_t (ABI *enable)(Player *, uint32_t);
static int32_t (ABI *start)(Player *);
static int32_t (ABI *stop)(Player *);
static int32_t (ABI *close_player)(Player *);
static int32_t (ABI *jump)(Player *, uint64_t);
static int (ABI *video_ex)(Player *, AvFrameInfoEx *);
static int (ABI *audio_data)(Player *, AvFrameInfo *);
static int (ABI *is_active)(Player *);
static uint64_t (ABI *current_time)(Player *);

static void sleep_ms(int ms) { usleep((useconds_t)ms*1000); }
static int wait_event(int event, int count, int ms) {
    for (int i=0;i<ms && atomic_load(&events[event])<count;++i) sleep_ms(1);
    return atomic_load(&events[event])>=count;
}
static Player *open_player(int callbacks, int file_callbacks) {
    AvInitDataEx init={0};
    init.this_size=sizeof(init);
    init.memory=(AvMemAllocator){NULL,alloc,dealloc,alloc,dealloc};
    if (callbacks) init.events=(AvEventReplacement){(void *)0x1234,on_event};
    if (file_callbacks) init.file=(AvFileReplacement){NULL,file_open,file_close,file_read,file_size};
    init.video_buffers=2;
    init.default_language="eng";
    Player *p=NULL;
    assert(init_ex(&init,&p)==0 && p);
    return p;
}
/* Plays until StateStop; returns video frames taken. Checks frame contents and ordering. */
static int play(Player *p, uint64_t from_ms, int *audio_frames) {
    int video=0, audio=0; uint64_t last_video=0; double energy=0;
    const uint64_t t0=mono_ns();
    while (is_active(p) && mono_ns()-t0<8000000000ull) {
        AvFrameInfoEx v; AvFrameInfo a;
        /* An audio output's pace: 1024-sample frames at 48 kHz, ~100 ms buffered ahead. */
        const double played_ms=(double)(mono_ns()-t0)/1e6+100.0;
        while (audio*1024.0/48.0<played_ms && audio_data(p,&a)) {
            assert(a.details.audio.channels==2 && a.details.audio.sample_rate==48000);
            assert(a.details.audio.size==1024*2*2 && a.data);
            const int16_t *pcm=a.data; double s=0;
            for (int i=0;i<1024*2;++i) s+=(double)pcm[i]*pcm[i];
            energy+=sqrt(s/2048);
            ++audio;
        }
        if (video_ex(p,&v)) {
            assert(v.timestamp+1>=from_ms);
            assert(!video || v.timestamp>last_video);
            assert(v.timestamp<=current_time(p)); /* in sync: not before its time */
            last_video=v.timestamp;
            const AvVideoEx *d=&v.details.video;
            assert(d->width==208 && d->height==128 && d->pitch==256);
            assert(d->crop_right==56 && d->crop_bottom==8 && d->luma_bits==8);
            const uint8_t *y=v.data, *uv=y+256*128;
            /* BT.601 limited red: Y 81, U 90, V 240 (encoder rounding tolerated) */
            assert(abs(y[60*256+100]-81)<8 && abs(uv[30*256+100]-90)<10 && abs(uv[30*256+101]-240)<10);
            assert(!memcmp(y+127*256,y+119*256,200)); /* padding rows repeat the last row */
            ++video;
        }
        sleep_ms(4);
    }
    if (audio_frames) *audio_frames=audio;
    if (audio) printf("  %d video frames (last %llu ms), %d audio frames (mean RMS %.0f)\n",video,(unsigned long long)last_video,audio,energy/audio);
    if (audio) assert(energy/audio>1000);
    return video;
}

int main(int argc, char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    assert(argc==3);
    setenv("DREAMS_AV_LIB",argv[1],1);
    clip=argv[2];
    assert(runtime_av_get());
    init_ex=F(int32_t (ABI *)(const AvInitDataEx *, Player **),"sceAvPlayerInitEx");
    t_add_source=F(int32_t (ABI *)(Player *, const char *),"sceAvPlayerAddSource");
    stream_count=F(int32_t (ABI *)(Player *),"sceAvPlayerStreamCount");
    stream_info=F(int32_t (ABI *)(Player *, uint32_t, AvStreamInfo *),"sceAvPlayerGetStreamInfo");
    enable=F(int32_t (ABI *)(Player *, uint32_t),"sceAvPlayerEnableStream");
    start=F(int32_t (ABI *)(Player *),"sceAvPlayerStart");
    stop=F(int32_t (ABI *)(Player *),"sceAvPlayerStop");
    close_player=F(int32_t (ABI *)(Player *),"sceAvPlayerClose");
    jump=F(int32_t (ABI *)(Player *, uint64_t),"sceAvPlayerJumpToTime");
    video_ex=F(int (ABI *)(Player *, AvFrameInfoEx *),"sceAvPlayerGetVideoDataEx");
    audio_data=F(int (ABI *)(Player *, AvFrameInfo *),"sceAvPlayerGetAudioData");
    is_active=F(int (ABI *)(Player *),"sceAvPlayerIsActive");
    current_time=F(uint64_t (ABI *)(Player *),"sceAvPlayerCurrentTime");
    assert(init_ex && t_add_source && stream_count && stream_info && enable && start && stop && close_player && jump &&
           video_ex && audio_data && is_active && current_time);

    /* 1. The game's sequence, with its event callback. */
    Player *p=open_player(1,0);
    assert(t_add_source(p,"/app0/data/videos/missing.mp4")==ERR_OPERATION_FAILED);
    close_player(p);
    p=open_player(1,0);
    assert(t_add_source(p,"/app0/data/videos/test.mp4")==0);
    assert(wait_event(EV_STATE_READY,1,2000));
    assert(stream_count(p)==2);
    int vs=-1, as=-1;
    for (uint32_t i=0;i<2;++i) {
        AvStreamInfo info;
        assert(stream_info(p,i,&info)==0);
        if (info.type==0) {
            vs=(int)i;
            assert(info.details.video.width==208 && info.details.video.height==128);
            assert(fabsf(info.details.video.aspect-200.0f/120.0f)<0.01f);
        } else {
            as=(int)i;
            assert(info.details.audio.channels==2 && info.details.audio.sample_rate==48000 && info.details.audio.size==0);
            assert(!memcmp(info.details.audio.language,"eng",3));
        }
        assert(info.duration>=1900 && info.duration<=2200);
    }
    assert(vs>=0 && as>=0 && stream_info(p,2,&(AvStreamInfo){0})==ERR_OPERATION_FAILED);
    assert(enable(p,(uint32_t)vs)==0 && enable(p,(uint32_t)as)==0);
    assert(!video_ex(p,&(AvFrameInfoEx){0})); /* not started */
    assert(start(p)==0 && atomic_load(&events[EV_STATE_PLAY])==1);
    assert(atomic_load(&allocations)==2+4); /* 2 video (texture) + 4 audio buffers */
    printf("play:\n");
    int audio_frames=0;
    const int frames=play(p,0,&audio_frames);
    assert(frames>=55 && frames<=61 && audio_frames>=90);
    assert(wait_event(EV_STATE_STOP,1,1000));
    assert(atomic_load(&notes)>0);
    /* 2. Start again, then jump to 1 s. */
    assert(start(p)==0);
    sleep_ms(100);
    assert(jump(p,1000)==0);
    printf("jump to 1000 ms:\n");
    const int rest=play(p,1000-40,NULL);
    assert(rest>=20 && rest<=35);
    assert(wait_event(EV_STATE_STOP,2,1000));
    /* 3. Stop mid-way, close: buffers back to the game. */
    assert(start(p)==0);
    sleep_ms(200);
    assert(stop(p)==0 && atomic_load(&events[EV_STATE_STOP])==3 && !is_active(p));
    assert(close_player(p)==0);
    assert(atomic_load(&frees)==atomic_load(&allocations));

    /* 4. The game's file callbacks, auto start (no event callback): plays by itself. */
    p=open_player(0,1);
    assert(t_add_source(p,"/app0/data/videos/test.mp4")==0);
    for (int i=0;i<2000 && !p->started;++i) sleep_ms(1);
    assert(p->started);
    printf("file callbacks + auto start:\n");
    assert(play(p,0,NULL)>=55);
    assert(close_player(p)==0 && !game_file);

    /* 5. The Dreams logo: wall clock, frames due by time even if no audio is taken. */
    memset(events,0,sizeof(events));
    p=open_player(1,0);
    assert(t_add_source(p,"/app0/data/videos/Mm-Dreams-logo.mp4")==0 && p->realtime);
    assert(wait_event(EV_STATE_READY,1,2000));
    enable(p,0); enable(p,1);
    assert(start(p)==0);
    sleep_ms(1000);
    AvFrameInfoEx v; int got=0; uint64_t ts=0;
    while (video_ex(p,&v)) { ++got; ts=v.timestamp; }
    printf("logo after 1 s without taking audio: %d frame(s), latest %llu ms\n",got,(unsigned long long)ts);
    assert(got>=1 && ts>=800 && ts<=1100);
    close_player(p);
    runtime_avplayer_report();
    puts("avplayer-test passed");
    return 0;
}
