// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Codec library (out/libdreams-codecs.so) through the runtime: fdk-aac via the glue (ADTS and
 * raw access units), the Ajm AAC job path of src/runtime_ajm.c (init, multi-frame decode,
 * sidebands), and the ATRAC9 encoder. Built by tests/run-dreams-tests.sh:
 *   codecs-test <libdreams-codecs.so> <stereo 48 kHz 1 kHz sine, ADTS .aac (ffmpeg)> */
#include "../src/runtime_codecs.c"
#include "../src/runtime_ajm.c"
#include <assert.h>
#include <math.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,nid)) return (uintptr_t)table[i].function;
    return 0;
}
static unsigned char *read_file(const char *path, size_t *size) {
    FILE *f=fopen(path,"rb"); assert(f);
    fseek(f,0,SEEK_END); *size=(size_t)ftell(f); fseek(f,0,SEEK_SET);
    unsigned char *data=malloc(*size); assert(fread(data,1,*size,f)==*size); fclose(f);
    return data;
}
static double rms(const int16_t *pcm, size_t n) { double s=0; for (size_t i=0;i<n;++i) s+=(double)pcm[i]*pcm[i]; return sqrt(s/(double)n); }
static uint32_t adts_length(const unsigned char *p) { return ((uint32_t)(p[3]&3)<<11)|((uint32_t)p[4]<<3)|(p[5]>>5); }

static void glue_adts_and_raw(const RuntimeCodecs *c, const unsigned char *aac, size_t size) {
    static int16_t pcm[2048*8];
    RuntimeAacFrame f;
    void *d=c->aac_open(0,0,0);
    assert(d);
    size_t at=0, frames=0; double energy=0;
    while (at<size) {
        int r=c->aac_decode(d,aac+at,(uint32_t)(size-at),pcm,sizeof(pcm)/2,&f);
        assert(r==0);
        assert(f.bytes_used==adts_length(aac+at));
        assert(f.sample_rate==48000 && f.channels==2 && f.frame_size==1024);
        at+=f.bytes_used; ++frames;
        if (frames>4) energy+=rms(pcm,2048);
    }
    assert(frames>40);
    energy/=(double)(frames-4);
    printf("ADTS: %zu frames, mean RMS %.0f\n",frames,energy);
    assert(energy>1500 && energy<8000); /* ffmpeg's sine source: amplitude 1/8 -> RMS ~2900 */
    assert(c->aac_decode(d,aac,3,pcm,sizeof(pcm)/2,&f)==1); /* truncated header: needs more input */
    c->aac_close(d);
    /* Raw AUs: strip the 7-byte ADTS headers (protection absent) and configure like Ajm RAW. */
    d=c->aac_open(1,3,2);
    assert(d);
    at=0; frames=0;
    while (at<size) {
        uint32_t len=adts_length(aac+at), header=(aac[at+1]&1) ? 7 : 9;
        int r=c->aac_decode(d,aac+at+header,len-header,pcm,sizeof(pcm)/2,&f);
        assert(r==0 && f.bytes_used==len-header && f.channels==2 && f.sample_rate==48000);
        at+=len; ++frames;
    }
    printf("RAW: %zu frames\n",frames);
    c->aac_close(d);
}

/* Ajm: context, AAC instance (S16, interleaved), init, then
 *  ADTS: one multi-frame run job over the first 12 ADTS frames;
 *  RAW: one run job per access unit (raw AUs carry no length: fdk's RAW transport takes one AU
 *       per fill, exactly as shadPS4's ajm_aac.cpp). */
typedef struct { SidebandResult result; SidebandStream stream; SidebandFormat format; uint32_t mframe[2]; } Side;
static uint32_t run(uint32_t ctx, uint32_t inst, const void *in, size_t in_size, void *out, size_t out_size, Side *side) {
    unsigned char batch[256];
    memset(side,0xcc,sizeof(*side));
    uint64_t run_flags=(UINT64_C(1)<<12)|(UINT64_C(1)<<47)|(UINT64_C(1)<<46);  /* multiple frames, stream, format */
    unsigned char *p=job_run(batch,inst,run_flags,(void *)in,in_size,out,out_size,side,sizeof(*side),NULL);
    uint32_t batch_id=0;
    assert(ajm_batch_start(ctx,batch,(uint32_t)(p-batch),0,NULL,&batch_id)==0);
    assert(ajm_batch_wait(ctx,batch_id,0,NULL)==0);
    return (uint32_t)side->result.result;
}
static void ajm_aac_job(const unsigned char *aac, size_t size, uint32_t config) {
    assert(size>16*1024/8); /* at least the 12 frames used below */
    uint32_t ctx=0, inst=0;
    assert(ajm_initialize(0,&ctx)==0);
    assert(ajm_module_register(ctx,2,0)==0);
    uint64_t flags=1|((uint64_t)2<<3)|((uint64_t)FORMAT_S16<<7);
    assert(ajm_instance_create(ctx,2,flags,&inst)==0);
    unsigned char batch[256];
    uint32_t init[2]={config,3}; /* 1 ADTS / 2 RAW, 48000 */
    SidebandResult control_result;
    unsigned char *p=job_control(batch,inst,(UINT64_C(1)<<14)|(UINT64_C(1)<<13),init,8,&control_result,sizeof(control_result),NULL);
    uint32_t batch_id=0;
    assert(ajm_batch_start(ctx,batch,(uint32_t)(p-batch),0,NULL,&batch_id)==0 && ajm_batch_wait(ctx,batch_id,0,NULL)==0);
    assert(control_result.result==0);
    static int16_t out[12*1024*2];
    Side side;
    size_t at=0, used=0; int32_t written=0; uint32_t frames=0;
    if (config==1) {
        for (int i=0;i<12;++i) at+=adts_length(aac+at);
        assert(run(ctx,inst,aac,at,out,sizeof(out),&side)==RESULT_PARTIAL_INPUT); /* ran out of input */
        used=(size_t)side.stream.input_consumed; written=side.stream.output_written; frames=side.mframe[0];
    } else for (int i=0;i<12;++i) {
        uint32_t len=adts_length(aac+at), header=(aac[at+1]&1) ? 7 : 9;
        uint32_t r=run(ctx,inst,aac+at+header,len-header,(char *)out+written,sizeof(out)-(size_t)written,&side);
        assert(r==RESULT_PARTIAL_INPUT || r==0);
        assert(side.stream.input_consumed==(int32_t)(len-header));
        at+=len; used+=len-header; written+=side.stream.output_written; frames+=side.mframe[0];
    }
    printf("Ajm AAC %s: input %zu bytes, output %d bytes, frames %u, total samples %llu, %u ch %u Hz\n",config==1 ? "ADTS" : "RAW",
           used,written,frames,(unsigned long long)side.stream.total_decoded_samples,side.format.channels,side.format.sample_rate);
    /* 12 frames decoded, the first 2 dropped (decoder delay, no EnableNondelayOutput) -> 10 x 1024 x 2 ch. */
    assert(config!=1 || used==at);
    assert(frames==12);
    assert(written==10*1024*2*2);
    assert(side.stream.total_decoded_samples==10*1024);
    assert(side.format.channels==2 && side.format.sample_rate==48000 && side.format.channel_mask==3);
    assert(rms(out+4*2048,2048)>1500);
    assert(ajm_instance_destroy(ctx,inst)==0);
    assert(ajm_finalize(ctx)==0);
}

static void at9_encoder(const RuntimeCodecs *c) {
    size_t bytes=c->at9_context_size(), scratch_size=c->at9_stream_work_size();
    void *ctx=calloc(1,bytes), *scratch=malloc(scratch_size);
    assert(ctx && scratch && c->at9_context_create(ctx,bytes,2,192)==0);
    static int16_t pcm[48000*2]; static unsigned char out[65536];
    for (int i=0;i<48000;++i) pcm[2*i]=pcm[2*i+1]=(int16_t)(8000*sin(2*M_PI*1000*i/48000.0));
    int produced=0, consumed=0, total_out=0, total_in=0;
    for (int round=0;round<20;++round) {
        int r=c->at9_context_encode(ctx,scratch,scratch_size,pcm+total_in/2,(int)sizeof(pcm)-total_in,out,sizeof(out),&produced,&consumed);
        assert(r==0);
        total_out+=produced; total_in+=consumed;
        if (!consumed) break;
    }
    printf("ATRAC9: %d PCM bytes -> %d bytes\n",total_in,total_out);
    assert(total_in>0 && total_out>0);
    free(ctx); free(scratch);
}

int main(int argc, char **argv) {
    assert(argc==3);
    setenv("DREAMS_CODECS_LIB",argv[1],1);
    const RuntimeCodecs *c=runtime_codecs_get();
    assert(c);
    size_t size; unsigned char *aac=read_file(argv[2],&size);
    glue_adts_and_raw(c,aac,size);
    ajm_aac_job(aac,size,1);
    ajm_aac_job(aac,size,2);
    at9_encoder(c);
    free(aac);
    puts("codec tests passed");
    return 0;
}
