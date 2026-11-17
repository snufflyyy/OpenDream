// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Audio3d (src/runtime_audio3d.c) against a fake AudioOut, built by tests/run-dreams-tests.sh. */
#include "../src/runtime_audio3d.c"
#include <assert.h>
#include <math.h>

/* Test doubles: the runtime's name lookup (tables keyed by symbol name; here the "NID" is the
 * name itself) and AudioOut (records what Audio3d sends). */
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,nid)) return (uintptr_t)table[i].function;
    return 0;
}
static int out_opened, out_outputs, out_closed; static int16_t last_frame[512*2]; static uint32_t last_len;
int32_t runtime_audio_out_init(void) { return 0; }
int32_t runtime_audio_out_open(int32_t user, int32_t type, int32_t index, uint32_t length, uint32_t freq, uint32_t param) {
    (void)user; (void)index; assert(freq==48000); last_len=length; ++out_opened;
    if (type==126) assert((param & 0xff)==1);
    return 0x20000000 | (type<<16) | out_opened;
}
int32_t runtime_audio_out_output(int32_t handle, const void *data) {
    (void)handle; ++out_outputs; memcpy(last_frame,data,last_len*4); return (int32_t)last_len*2;
}
int32_t runtime_audio_out_close(int32_t handle) { (void)handle; ++out_closed; return 0; }

#define GET(t,n) ((t)runtime_audio3d_resolve(n))
int main(void) {
    int32_t (ABI *init)(int64_t)=GET(int32_t (ABI *)(int64_t),"sceAudio3dInitialize");
    int32_t (ABI *open)(int32_t,const OpenParameters *,uint32_t *)=GET(int32_t (ABI *)(int32_t,const OpenParameters *,uint32_t *),"sceAudio3dPortOpen");
    int32_t (ABI *reserve)(uint32_t,uint32_t *)=GET(int32_t (ABI *)(uint32_t,uint32_t *),"sceAudio3dObjectReserve");
    int32_t (ABI *unreserve)(uint32_t,uint32_t)=GET(int32_t (ABI *)(uint32_t,uint32_t),"sceAudio3dObjectUnreserve");
    int32_t (ABI *attrs)(uint32_t,uint32_t,uint64_t,const Attribute *)=GET(int32_t (ABI *)(uint32_t,uint32_t,uint64_t,const Attribute *),"sceAudio3dObjectSetAttributes");
    int32_t (ABI *advance)(uint32_t)=GET(int32_t (ABI *)(uint32_t),"sceAudio3dPortAdvance");
    int32_t (ABI *push)(uint32_t,uint32_t)=GET(int32_t (ABI *)(uint32_t,uint32_t),"sceAudio3dPortPush");
    int32_t (ABI *level)(uint32_t,uint32_t *,uint32_t *)=GET(int32_t (ABI *)(uint32_t,uint32_t *,uint32_t *),"sceAudio3dPortGetQueueLevel");
    int32_t (ABI *aopen)(uint32_t,int32_t,int32_t,int32_t,uint32_t,uint32_t,uint32_t)=GET(int32_t (ABI *)(uint32_t,int32_t,int32_t,int32_t,uint32_t,uint32_t,uint32_t),"sceAudio3dAudioOutOpen");
    int32_t (ABI *aouts)(const OutputParam *,uint32_t)=GET(int32_t (ABI *)(const OutputParam *,uint32_t),"sceAudio3dAudioOutOutputs");
    int32_t (ABI *close)(uint32_t)=GET(int32_t (ABI *)(uint32_t),"sceAudio3dPortClose");
    assert(init && open && reserve && unreserve && attrs && advance && push && level && aopen && aouts && close);
    uint32_t id=7;
    OpenParameters p={0x28,256,0,64,2,BUFFER_ADVANCE_AND_PUSH,0,2};
    assert(open(0xff,&p,&id)==ERR_NOT_READY && id==OBJECT_INVALID);
    assert(init(0)==0 && init(0)==ERR_NOT_READY);
    assert(open(1,&p,&id)==ERR_INVALID_PARAMETER);
    OpenParameters bad=p; bad.granularity=300;
    assert(open(0xff,&bad,&id)==ERR_INVALID_PARAMETER);
    assert(open(0xff,&p,&id)==0 && id==0);
    uint32_t obj=0, obj2=0;
    assert(reserve(id,&obj)==0 && obj==1 && reserve(id,&obj2)==0 && obj2==2);
    assert(reserve(9,&obj2)==ERR_INVALID_PORT && obj2==OBJECT_INVALID);
    /* object 1: 0.5 gain, constant 0.25 mono float; object 2: no gain -> silent */
    static float pcm1[256], pcm2[256];
    for (int i=0;i<256;++i) { pcm1[i]=0.25f; pcm2[i]=1.0f; }
    float gain=0.5f;
    Pcm a={FORMAT_FLOAT,0,pcm1,256}, b={FORMAT_FLOAT,0,pcm2,256};
    Attribute set1[2]={{ATTR_GAIN,0,&gain,4},{ATTR_PCM,0,&a,sizeof(a)}}, set2[1]={{ATTR_PCM,0,&b,sizeof(b)}};
    assert(attrs(id,1,2,set1)==0 && attrs(id,2,1,set2)==0);
    assert(advance(id)==0);
    uint32_t lv=9, av=9;
    assert(level(id,&lv,&av)==0 && lv==1 && av==1);
    assert(push(id,1)==0 && out_outputs==0);       /* queue below depth: nothing sent yet */
    assert(attrs(id,1,1,&set1[1])==0);
    assert(advance(id)==0 && advance(id)==ERR_NOT_READY);
    assert(push(id,1)==0 && out_opened==1 && out_outputs==1);
    assert(abs(last_frame[0]-(int16_t)(0.125f*32767.0f))<=1 && last_frame[0]==last_frame[1]);
    assert(level(id,&lv,&av)==0 && lv==1);
    /* unreserve with queued PCM: kept until mixed */
    assert(attrs(id,1,1,&set1[1])==0 && unreserve(id,1)==0 && unreserve(id,1)==ERR_INVALID_OBJECT);
    assert(push(id,0)==0);
    /* AudioOut ports opened through Audio3d are queued and drained by Push */
    int32_t h=aopen(id,0xff,0,0,256,48000,1);
    assert(h>0 && aopen(id,0xff,0,0,512,48000,1)==ERR_INVALID_PARAMETER);
    static int16_t stereo[512];
    OutputParam op={h,stereo};
    assert(aouts(&op,1)==0 && aouts(&op,1)==0 && aouts(&op,1)==ERR_NOT_READY);
    int before=out_outputs;
    assert(push(id,1)==0 && out_outputs==before+2);
    assert(close(id)==0 && close(id)==ERR_INVALID_PORT && out_closed==2);
    runtime_audio3d_report();
    puts("audio3d tests passed");
    return 0;
}
