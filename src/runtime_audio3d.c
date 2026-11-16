// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* libSceAudio3d: Dreams' main audio output path. Port of shadPS4's software implementation
 * (ref/shadps4/src/core/libraries/audio3d/audio3d.cpp, not the OpenAL variant), routed to our
 * AudioOut (runtime_audio.c, SDL3):
 *  - sceAudio3dInitialize(0) also initialises AudioOut (ALREADY_INIT tolerated);
 *  - sceAudio3dPortOpen(user 0xFF, params, &id): ids 0..3; params are versioned by size_this
 *    (0x10/0x18/0x20/0x28 bytes) exactly as shadPS4 decodes them; granularity is the frame count
 *    of one mix (multiple of 256);
 *  - objects (ObjectReserve/SetAttributes/Unreserve): a PCM attribute queues one granule
 *    (mono, S16 or float, padded/truncated to the granularity) up to queue_depth deep; GAIN
 *    (float) is remembered; other attributes (position, spread, priority, ...) are accepted and
 *    ignored: everything is mixed to stereo without spatialisation (objects mono -> both sides);
 *  - sceAudio3dPortAdvance mixes one granule of every object queue (gain 0 when never set, i.e.
 *    silent, as on shadPS4) plus the bed queue into an S16 stereo frame (mixed queue);
 *  - sceAudio3dPortPush first drains buffers queued through sceAudio3dAudioOutOutput(s) to their
 *    AudioOut ports, then sends mixed frames to a lazily opened AudioOut port (type 126 AUDIO3D,
 *    S16 stereo, 48 kHz) once the mixed queue is full (BLOCKING_SYNC drains to below the depth).
 *    sceAudioOutOutput paces at the buffer period, which is what paces Dreams' audio thread;
 *  - sceAudio3dAudioOutOpen opens a normal AudioOut port tied to an Audio3d port; its
 *    Output(s) calls are queued (queue_depth) and written by the next Push/Flush.
 * Error codes: ref audio3d/audio3d_error.h. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define ERR_INVALID_PORT ((int32_t)0x80EA0002)
#define ERR_INVALID_OBJECT ((int32_t)0x80EA0003)
#define ERR_INVALID_PARAMETER ((int32_t)0x80EA0004)
#define ERR_OUT_OF_MEMORY ((int32_t)0x80EA0005)
#define ERR_OUT_OF_RESOURCES ((int32_t)0x80EA0006)
#define ERR_NOT_READY ((int32_t)0x80EA0007)
#define ERR_NOT_SUPPORTED ((int32_t)0x80EA0008)
#define AUDIO_OUT_ALREADY_INIT ((int32_t)0x8026000E)
#define MAX_PORTS 4
#define MAX_AOUT 8
#define OBJECT_INVALID 0xFFFFFFFFu
enum { BUFFER_NO_ADVANCE=0, BUFFER_ADVANCE_NO_PUSH=1, BUFFER_ADVANCE_AND_PUSH=2 };
enum { FORMAT_S16=0, FORMAT_FLOAT=1 };
enum { ATTR_PCM=1, ATTR_GAIN=5, ATTR_RESET_STATE=7 };
enum { AUDIO_OUT_PORT_AUDIO3D=126, AUDIO_OUT_S16_STEREO=1 };

typedef struct { uint64_t size_this; uint32_t granularity, rate, max_objects, queue_depth, buffer_mode, pad, num_beds; } OpenParameters;
typedef struct { uint32_t format, pad; void *sample_buffer; uint32_t num_samples; } Pcm;
typedef struct { uint32_t attribute_id, pad; void *value; uint64_t value_size; } Attribute;
typedef struct { int32_t handle; const void *data; } OutputParam;
_Static_assert(sizeof(OpenParameters)==0x28 && sizeof(Pcm)==24 && sizeof(Attribute)==24, "Audio3d layouts");

typedef struct { void *samples; uint32_t frames, channels, format; } Granule;
typedef struct { Granule *items; uint32_t head, count, capacity; } Queue;
typedef struct { uint32_t id; int unreserved; float gain; Queue pcm; } Object;
typedef struct { int32_t handle; uint32_t bytes, samples; int is_float; Queue pending; } AudioOutLink;
typedef struct {
    int used;
    OpenParameters p;
    int32_t out_handle;          /* lazily opened AUDIO3D port for the mix */
    Object *objects; uint32_t object_count, next_object_id;
    Queue bed, mixed;
    AudioOutLink links[MAX_AOUT]; int link_count;
} Port;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized;
static Port ports[MAX_PORTS];
static size_t frames_mixed, frames_pushed;

int32_t runtime_audio_out_init(void);
int32_t runtime_audio_out_open(int32_t user, int32_t type, int32_t index, uint32_t length, uint32_t freq, uint32_t param);
int32_t runtime_audio_out_output(int32_t handle, const void *data);
int32_t runtime_audio_out_close(int32_t handle);

static int queue_push(Queue *q, Granule g, uint32_t capacity) {
    if (!q->items) { q->items=calloc(capacity ? capacity : 1,sizeof(Granule)); q->capacity=capacity ? capacity : 1; }
    if (!q->items || q->count>=q->capacity) return 0;
    q->items[(q->head+q->count)%q->capacity]=g; ++q->count; return 1;
}
static int queue_pop(Queue *q, Granule *g) {
    if (!q->count) return 0;
    *g=q->items[q->head]; q->head=(q->head+1)%q->capacity; --q->count; return 1;
}
static void queue_free(Queue *q) {
    Granule g;
    while (queue_pop(q,&g)) free(g.samples);
    free(q->items); memset(q,0,sizeof(*q));
}
static Port *port_get(uint32_t id) { return initialized && id<MAX_PORTS && ports[id].used ? &ports[id] : NULL; }
static Object *object_get(Port *p, uint32_t id) {
    for (uint32_t i=0;i<p->object_count;++i) if (p->objects[i].id==id) return &p->objects[i];
    return NULL;
}
static void object_remove(Port *p, Object *o) {
    queue_free(&o->pcm);
    *o=p->objects[--p->object_count];
}

static ABI int32_t a3d_initialize(int64_t reserved) {
    if (reserved) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    int32_t r=initialized ? ERR_NOT_READY : 0;
    initialized=1;
    pthread_mutex_unlock(&lock);
    if (r) return r;
    int32_t init=runtime_audio_out_init();
    if (init<0 && init!=AUDIO_OUT_ALREADY_INIT) return init;
    puts("Runtime: Audio3d initialized (software stereo mix -> AudioOut)");
    return 0;
}
static ABI int32_t a3d_terminate(void) {
    pthread_mutex_lock(&lock);
    for (int i=0;i<MAX_PORTS;++i) if (ports[i].used) {
        Port *p=&ports[i];
        for (uint32_t o=0;o<p->object_count;++o) queue_free(&p->objects[o].pcm);
        free(p->objects); queue_free(&p->bed); queue_free(&p->mixed);
        for (int l=0;l<p->link_count;++l) { queue_free(&p->links[l].pending); runtime_audio_out_close(p->links[l].handle); }
        if (p->out_handle>=0) runtime_audio_out_close(p->out_handle);
        memset(p,0,sizeof(*p));
    }
    initialized=0;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t a3d_port_open(int32_t user, const OpenParameters *in, uint32_t *id) {
    if (user!=0xFF || !in || !id) { if (id) *id=OBJECT_INVALID; return ERR_INVALID_PARAMETER; }
    *id=OBJECT_INVALID;
    if (!initialized) return ERR_NOT_READY;
    OpenParameters e={0x28,in->granularity,in->rate,512,2,BUFFER_NO_ADVANCE,0,2};
    switch (in->size_this & ~UINT64_C(7)) {
    case 0x10: break;
    case 0x18: e.max_objects=in->max_objects; e.queue_depth=in->queue_depth; e.buffer_mode=BUFFER_ADVANCE_NO_PUSH; break;
    case 0x20: e.max_objects=in->max_objects; e.queue_depth=in->queue_depth; e.buffer_mode=in->buffer_mode; break;
    case 0x28: e.max_objects=in->max_objects; e.queue_depth=in->queue_depth; e.buffer_mode=in->buffer_mode; e.num_beds=in->num_beds; break;
    default: return ERR_INVALID_PARAMETER;
    }
    if (e.rate!=0 || e.granularity<0x100 || (e.granularity & 0xff) || !e.max_objects || !e.queue_depth ||
        (e.granularity==0x100 && e.queue_depth>0x40) || (e.granularity==0x200 && e.queue_depth>0x1f) ||
        (e.granularity==0x300 && e.queue_depth>0x14) || (e.queue_depth>0xf && e.granularity>0x3ff) ||
        e.buffer_mode>2 || (e.num_beds & ~UINT32_C(1))!=2)
        return ERR_INVALID_PARAMETER;
    if (e.max_objects>512) e.max_objects=512;
    pthread_mutex_lock(&lock);
    int slot=-1;
    for (int i=0;i<MAX_PORTS;++i) if (!ports[i].used) { slot=i; break; }
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_OUT_OF_RESOURCES; }
    Port *p=&ports[slot];
    memset(p,0,sizeof(*p));
    p->objects=calloc(e.max_objects,sizeof(Object));
    if (!p->objects) { pthread_mutex_unlock(&lock); return ERR_OUT_OF_MEMORY; }
    p->used=1; p->p=e; p->out_handle=-1;
    *id=(uint32_t)slot;
    pthread_mutex_unlock(&lock);
    printf("Runtime: Audio3d port %d opened (granularity %u, max objects %u, queue depth %u, buffer mode %u, beds %u)\n",
           slot,e.granularity,e.max_objects,e.queue_depth,e.buffer_mode,e.num_beds);
    return 0;
}
static ABI int32_t a3d_port_close(uint32_t id) {
    pthread_mutex_lock(&lock);
    Port *p=port_get(id);
    if (!p) { pthread_mutex_unlock(&lock); return ERR_INVALID_PORT; }
    for (uint32_t o=0;o<p->object_count;++o) queue_free(&p->objects[o].pcm);
    free(p->objects); queue_free(&p->bed); queue_free(&p->mixed);
    int32_t handles[MAX_AOUT+1]; int n=0;
    for (int l=0;l<p->link_count;++l) { queue_free(&p->links[l].pending); handles[n++]=p->links[l].handle; }
    if (p->out_handle>=0) handles[n++]=p->out_handle;
    memset(p,0,sizeof(*p));
    pthread_mutex_unlock(&lock);
    for (int i=0;i<n;++i) runtime_audio_out_close(handles[i]);
    return 0;
}
static ABI int32_t a3d_object_reserve(uint32_t port, uint32_t *object) {
    if (!object) return ERR_INVALID_PARAMETER;
    *object=OBJECT_INVALID;
    pthread_mutex_lock(&lock);
    Port *p=port_get(port);
    int32_t r=0;
    if (!p) r=ERR_INVALID_PORT;
    else if (p->object_count>=p->p.max_objects) r=ERR_OUT_OF_RESOURCES;
    else {
        do ++p->next_object_id; while (!p->next_object_id || p->next_object_id==OBJECT_INVALID || object_get(p,p->next_object_id));
        Object *o=&p->objects[p->object_count++];
        memset(o,0,sizeof(*o));
        o->id=p->next_object_id;
        *object=o->id;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t a3d_object_unreserve(uint32_t port, uint32_t object) {
    pthread_mutex_lock(&lock);
    Port *p=port_get(port);
    Object *o=p ? object_get(p,object) : NULL;
    int32_t r=!p ? ERR_INVALID_PORT : !o || o->unreserved ? ERR_INVALID_OBJECT : 0;
    if (!r) { if (o->pcm.count) o->unreserved=1; else object_remove(p,o); }
    pthread_mutex_unlock(&lock);
    return r;
}
/* Copies one granule (padded with silence / truncated to the granularity). */
static int32_t enqueue(Queue *q, const Pcm *pcm, uint32_t channels, uint32_t granularity, uint32_t depth) {
    if (!pcm->sample_buffer || !pcm->num_samples) return ERR_INVALID_PARAMETER;
    uint32_t bps=pcm->format==FORMAT_S16 ? 2 : 4;
    if ((uintptr_t)pcm->sample_buffer & (bps-1)) return ERR_INVALID_PARAMETER;
    if (q->count>=depth) return ERR_NOT_READY;
    void *copy=calloc(granularity*channels,bps);
    if (!copy) return ERR_OUT_OF_MEMORY;
    uint32_t frames=pcm->num_samples<granularity ? pcm->num_samples : granularity;
    memcpy(copy,pcm->sample_buffer,(size_t)frames*channels*bps);
    if (!queue_push(q,(Granule){copy,granularity,channels,pcm->format},depth)) { free(copy); return ERR_NOT_READY; }
    return 0;
}
static ABI int32_t a3d_object_set_attributes(uint32_t port, uint32_t object, uint64_t count, const Attribute *attributes) {
    pthread_mutex_lock(&lock);
    Port *p=port_get(port);
    int32_t r=0;
    if (!p) r=ERR_INVALID_PORT;
    else if (!count || !attributes) r=ERR_INVALID_PARAMETER;
    if (r) { pthread_mutex_unlock(&lock); return r; }
    Object *o=object_get(p,object);
    if (!o) { pthread_mutex_unlock(&lock); return 0; } /* raced with Unreserve: no-op (shadPS4) */
    if (o->unreserved) { pthread_mutex_unlock(&lock); return ERR_INVALID_OBJECT; }
    for (uint64_t i=0;i<count;++i) if (attributes[i].attribute_id==ATTR_RESET_STATE) {
        queue_free(&o->pcm); o->gain=0; break;
    }
    for (uint64_t i=0;i<count && !r;++i) {
        const Attribute *a=&attributes[i];
        if (a->attribute_id==ATTR_PCM && a->value && a->value_size>=sizeof(Pcm))
            r=enqueue(&o->pcm,(const Pcm *)a->value,1,p->p.granularity,p->p.queue_depth);
        else if (a->attribute_id==ATTR_GAIN && a->value && a->value_size>=sizeof(float))
            memcpy(&o->gain,a->value,sizeof(float));
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t a3d_object_set_attribute(uint32_t port, uint32_t object, uint32_t id, const void *value, uint64_t size) {
    if (!size && id!=ATTR_RESET_STATE) {
        pthread_mutex_lock(&lock);
        int known=port_get(port)!=NULL;
        pthread_mutex_unlock(&lock);
        return known ? ERR_INVALID_PARAMETER : ERR_INVALID_PORT;
    }
    Attribute a={id,0,(void *)value,size};
    return a3d_object_set_attributes(port,object,1,&a);
}
static ABI int32_t a3d_bed_write2(uint32_t port, uint32_t channels, uint32_t format, void *buffer, uint32_t samples,
                                  uint32_t route, int restricted) {
    (void)route; (void)restricted;
    if (format>FORMAT_FLOAT || !(channels==2 || channels==6 || channels==8)) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    Port *p=port_get(port);
    int32_t r=!p ? ERR_INVALID_PORT : 0;
    if (!r) { Pcm pcm={format,0,buffer,samples}; r=enqueue(&p->bed,&pcm,channels,p->p.granularity,p->p.queue_depth); }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t a3d_bed_write(uint32_t port, uint32_t channels, uint32_t format, void *buffer, uint32_t samples) {
    return a3d_bed_write2(port,channels,format,buffer,samples,0,0);
}
static float sample_at(const Granule *g, uint32_t frame, uint32_t channel) {
    size_t i=(size_t)frame*g->channels+channel;
    if (g->format==FORMAT_S16) return ((const int16_t *)g->samples)[i]/32768.0f;
    return ((const float *)g->samples)[i];
}
static void mix_in(Queue *q, float gain, float *mix, uint32_t granularity) {
    Granule g;
    if (!queue_pop(q,&g)) return;
    if (gain!=0.0f) for (uint32_t f=0;f<granularity && f<g.frames;++f) {
        float l, r;
        if (g.channels==1) l=r=sample_at(&g,f,0);
        else {
            /* PS4 order L R C LFE SL SR BL BR; LFE dropped (shadPS4 DOWNMIX_LFE = 0). */
            l=sample_at(&g,f,0); r=sample_at(&g,f,1);
            if (g.channels>=3) { float c=0.7071f*sample_at(&g,f,2); l+=c; r+=c; }
            if (g.channels>=6) { l+=0.7071f*sample_at(&g,f,4); r+=0.7071f*sample_at(&g,f,5); }
            if (g.channels>=8) { l+=0.7071f*sample_at(&g,f,6); r+=0.7071f*sample_at(&g,f,7); }
        }
        mix[f*2]+=l*gain; mix[f*2+1]+=r*gain;
    }
    free(g.samples);
}
static int32_t mix_granule(Port *p) {
    uint32_t n=p->p.granularity;
    float *mix=calloc((size_t)n*2,sizeof(float));
    int16_t *out=malloc((size_t)n*2*sizeof(int16_t));
    if (!mix || !out) { free(mix); free(out); return ERR_OUT_OF_MEMORY; }
    mix_in(&p->bed,1.0f,mix,n);
    for (uint32_t i=0;i<p->object_count;++i) mix_in(&p->objects[i].pcm,p->objects[i].gain,mix,n);
    for (uint32_t i=0;i<n*2;++i) {
        float v=mix[i]<-1.0f ? -1.0f : mix[i]>1.0f ? 1.0f : mix[i];
        out[i]=(int16_t)(v*32767.0f);
    }
    free(mix);
    if (!queue_push(&p->mixed,(Granule){out,n,2,FORMAT_S16},p->p.queue_depth)) { free(out); return ERR_NOT_READY; }
    for (uint32_t i=0;i<p->object_count;)
        if (p->objects[i].unreserved && !p->objects[i].pcm.count) object_remove(p,&p->objects[i]); else ++i;
    ++frames_mixed;
    return 0;
}
static ABI int32_t a3d_port_advance(uint32_t port) {
    pthread_mutex_lock(&lock);
    Port *p=port_get(port);
    int32_t r=!p ? ERR_INVALID_PORT : p->p.buffer_mode==BUFFER_NO_ADVANCE ? ERR_NOT_SUPPORTED :
              p->mixed.count>=p->p.queue_depth ? ERR_NOT_READY : mix_granule(p);
    pthread_mutex_unlock(&lock);
    return r;
}
/* Writes every buffer queued by sceAudio3dAudioOutOutput(s). Called without the lock held. */
static int32_t drain_links(uint32_t port) {
    for (;;) {
        pthread_mutex_lock(&lock);
        Port *p=port_get(port);
        Granule g={0}; int32_t handle=-1;
        if (p) for (int l=0;l<p->link_count;++l) if (queue_pop(&p->links[l].pending,&g)) { handle=p->links[l].handle; break; }
        pthread_mutex_unlock(&lock);
        if (handle<0) return 0;
        int32_t r=runtime_audio_out_output(handle,g.samples);
        free(g.samples);
        if (r<0) return r;
    }
}
static int32_t ensure_out_port(Port *p) {
    if (p->out_handle>=0) return 0;
    int32_t h=runtime_audio_out_open(0xFF,AUDIO_OUT_PORT_AUDIO3D,0,p->p.granularity,48000,AUDIO_OUT_S16_STEREO);
    if (h<0) return h;
    p->out_handle=h;
    return 0;
}
/* Pops one mixed frame and outputs it (blocks one buffer period in AudioOut). 1 = sent. */
static int32_t push_one(uint32_t port) {
    pthread_mutex_lock(&lock);
    Port *p=port_get(port);
    Granule g; int32_t handle=p ? p->out_handle : -1;
    int have=p && queue_pop(&p->mixed,&g);
    pthread_mutex_unlock(&lock);
    if (!have) return 0;
    int32_t r=runtime_audio_out_output(handle,g.samples);
    free(g.samples);
    if (r<0) return r;
    ++frames_pushed;
    return 1;
}
static ABI int32_t a3d_port_push(uint32_t port, uint32_t blocking) {
    pthread_mutex_lock(&lock);
    Port *p=port_get(port);
    int32_t r=!p ? ERR_INVALID_PORT : p->p.buffer_mode!=BUFFER_ADVANCE_AND_PUSH ? ERR_NOT_SUPPORTED : 0;
    pthread_mutex_unlock(&lock);
    if (r) return r;
    if ((r=drain_links(port))<0) return r;
    pthread_mutex_lock(&lock);
    p=port_get(port);
    r=p ? ensure_out_port(p) : ERR_INVALID_PORT;
    int full=p && p->mixed.count>=p->p.queue_depth;
    pthread_mutex_unlock(&lock);
    if (r<0 || !full) return r<0 ? r : 0;
    if ((r=push_one(port))<=0) return r;
    if (blocking==0) return 0; /* ASYNC */
    for (;;) {
        pthread_mutex_lock(&lock);
        p=port_get(port);
        full=p && p->mixed.count>=p->p.queue_depth;
        pthread_mutex_unlock(&lock);
        if (!full) return 0;
        if ((r=push_one(port))<=0) return r;
    }
}
static ABI int32_t a3d_port_flush(uint32_t port) {
    pthread_mutex_lock(&lock);
    Port *p=port_get(port);
    if (!p) { pthread_mutex_unlock(&lock); return ERR_INVALID_PORT; }
    int links=p->link_count;
    int32_t r=0;
    if (!links) {
        int pending=p->bed.count>0;
        for (uint32_t i=0;i<p->object_count && !pending;++i) pending=p->objects[i].pcm.count>0;
        if (!p->mixed.count && pending) { r=mix_granule(p); if (r==ERR_NOT_READY) r=0; }
        if (!r && p->mixed.count) r=ensure_out_port(p);
    }
    pthread_mutex_unlock(&lock);
    if (r<0) return r;
    if (links) return drain_links(port);
    while ((r=push_one(port))>0) {}
    return r;
}
static ABI int32_t a3d_port_queue_level(uint32_t port, uint32_t *level, uint32_t *available) {
    pthread_mutex_lock(&lock);
    Port *p=port_get(port);
    int32_t r=!p ? ERR_INVALID_PORT : !level && !available ? ERR_INVALID_PARAMETER : 0;
    if (!r) {
        if (level) *level=p->mixed.count;
        if (available) *available=p->mixed.count<p->p.queue_depth ? p->p.queue_depth-p->mixed.count : 0;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t a3d_audio_out_open(uint32_t port, int32_t user, int32_t type, int32_t index, uint32_t len,
                                      uint32_t freq, uint32_t param) {
    pthread_mutex_lock(&lock);
    Port *p=port_get(port);
    int32_t r=!p ? ERR_INVALID_PORT : len!=p->p.granularity ? ERR_INVALID_PARAMETER : p->link_count>=MAX_AOUT ? ERR_OUT_OF_RESOURCES : 0;
    pthread_mutex_unlock(&lock);
    if (r) return r;
    int32_t handle=runtime_audio_out_open(user,type,index,len,freq,param);
    if (handle<0) return handle;
    static const uint8_t channels[8]={1,2,8,1,2,8,8,8};
    uint32_t format=param & 0xff, ch=channels[format & 7], bps=(format>=3 && format!=6) ? 4 : 2;
    pthread_mutex_lock(&lock);
    p=port_get(port);
    if (!p) { pthread_mutex_unlock(&lock); runtime_audio_out_close(handle); return ERR_INVALID_PORT; }
    AudioOutLink *l=&p->links[p->link_count++];
    memset(l,0,sizeof(*l));
    l->handle=handle; l->bytes=len*ch*bps; l->samples=len*ch; l->is_float=bps==4;
    pthread_mutex_unlock(&lock);
    printf("Runtime: Audio3d AudioOut port %#x opened on Audio3d port %u (type %d, %u ch)\n",(unsigned)handle,port,type,ch);
    return handle;
}
static ABI int32_t a3d_audio_out_close(int32_t handle) {
    pthread_mutex_lock(&lock);
    int found=0;
    for (int i=0;i<MAX_PORTS && !found;++i) if (ports[i].used)
        for (int l=0;l<ports[i].link_count;++l) if (ports[i].links[l].handle==handle) {
            queue_free(&ports[i].links[l].pending);
            ports[i].links[l]=ports[i].links[--ports[i].link_count];
            found=1; break;
        }
    pthread_mutex_unlock(&lock);
    return found ? runtime_audio_out_close(handle) : ERR_INVALID_PORT;
}
static ABI int32_t a3d_audio_out_output(int32_t handle, const void *data) {
    if (!data) return ERR_INVALID_PARAMETER;
    if (handle<0 || (handle & 0xffff)>25) return ERR_INVALID_PORT;
    pthread_mutex_lock(&lock);
    for (int i=0;i<MAX_PORTS;++i) if (ports[i].used)
        for (int l=0;l<ports[i].link_count;++l) {
            AudioOutLink *a=&ports[i].links[l];
            if (a->handle!=handle) continue;
            int32_t r;
            if ((uintptr_t)data & (a->is_float ? 3u : 1u)) r=ERR_INVALID_PARAMETER;
            else if (a->pending.count>=ports[i].p.queue_depth) r=ERR_NOT_READY;
            else {
                void *copy=malloc(a->bytes);
                if (!copy) r=ERR_OUT_OF_MEMORY;
                else {
                    memcpy(copy,data,a->bytes);
                    r=queue_push(&a->pending,(Granule){copy,0,0,0},ports[i].p.queue_depth) ? (int32_t)a->samples : (free(copy),ERR_NOT_READY);
                }
            }
            pthread_mutex_unlock(&lock);
            return r;
        }
    pthread_mutex_unlock(&lock);
    return ERR_INVALID_PORT;
}
static ABI int32_t a3d_audio_out_outputs(const OutputParam *params, uint32_t count) {
    if (!params || !count) return ERR_INVALID_PARAMETER;
    for (uint32_t i=0;i<count;++i) { int32_t r=a3d_audio_out_output(params[i].handle,params[i].data); if (r<0) return r; }
    return 0;
}
static ABI int32_t a3d_default_parameters(OpenParameters *p) {
    /* shadPS4: writes the 0x20-byte version (num_beds untouched); NULL is accepted. */
    if (p) { OpenParameters d={0x20,256,0,512,2,BUFFER_ADVANCE_AND_PUSH,0,0}; memcpy(p,&d,0x20); }
    return 0;
}

static const RuntimeExport exports[]={
    {"sceAudio3dInitialize",a3d_initialize}, {"sceAudio3dTerminate",a3d_terminate},
    {"sceAudio3dPortOpen",a3d_port_open}, {"sceAudio3dPortClose",a3d_port_close},
    {"sceAudio3dObjectReserve",a3d_object_reserve}, {"sceAudio3dObjectUnreserve",a3d_object_unreserve},
    {"sceAudio3dObjectSetAttributes",a3d_object_set_attributes}, {"sceAudio3dObjectSetAttribute",a3d_object_set_attribute},
    {"sceAudio3dBedWrite",a3d_bed_write}, {"sceAudio3dBedWrite2",a3d_bed_write2},
    {"sceAudio3dPortAdvance",a3d_port_advance}, {"sceAudio3dPortPush",a3d_port_push},
    {"sceAudio3dPortFlush",a3d_port_flush}, {"sceAudio3dPortGetQueueLevel",a3d_port_queue_level},
    {"sceAudio3dAudioOutOpen",a3d_audio_out_open}, {"sceAudio3dAudioOutClose",a3d_audio_out_close},
    {"sceAudio3dAudioOutOutput",a3d_audio_out_output}, {"sceAudio3dAudioOutOutputs",a3d_audio_out_outputs},
    {"sceAudio3dGetDefaultOpenParameters",a3d_default_parameters},
};
uintptr_t runtime_audio3d_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_audio3d_report(void) { printf("Runtime: Audio3d frames mixed=%zu, pushed=%zu\n",frames_mixed,frames_pushed); }
