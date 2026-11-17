// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Glue compiled into out/libdreams-codecs.so (build-deps.sh), NOT into dreams-probe: a small C
 * API over fdk-aac's public decoder interface (aacdecoder_lib.h) for runtime_ajm.c. The ATRAC9
 * encoder (artswap at9.c) exports its own C API and needs no glue.
 * AAC semantics follow shadPS4 src/core/libraries/ajm/ajm_aac.cpp: RAW mode configures an AAC-LC
 * decoder from the Ajm init parameters (sampling frequency index, channel count; 7 -> channel
 * configuration 11, 8 -> 12), ADTS mode parses headers. Every decode call clears the transport
 * buffer, feeds the whole input and decodes one frame; bytes_used is what that frame consumed.
 * shadPS4 calls the internal CAacDecoder_Init with a hand-built CSAudioSpecificConfig; we build
 * the equivalent 2-byte AudioSpecificConfig and use the public aacDecoder_ConfigRaw. */
#define _GNU_SOURCE
#include "../runtime.h"
#include <aacdecoder_lib.h>

#define EXPORT __attribute__((visibility("default")))

EXPORT void *dreams_aac_open(int raw, unsigned freq_index, unsigned channels) {
    HANDLE_AACDECODER h=aacDecoder_Open(raw ? TT_MP4_RAW : TT_MP4_ADTS,1);
    if (!h) return NULL;
    if (raw) {
        unsigned config=channels==0 ? 2 : channels<=6 ? channels : channels==7 ? 11 : 12;
        /* AudioSpecificConfig: audioObjectType 5 bits (2 = AAC LC), samplingFrequencyIndex 4,
         * channelConfiguration 4, GASpecificConfig: frameLengthFlag 0 (1024), dependsOnCoreCoder 0,
         * extensionFlag 0. */
        unsigned asc=(2u<<11)|((freq_index & 15u)<<7)|((config & 15u)<<3);
        UCHAR bytes[2]={(UCHAR)(asc>>8),(UCHAR)asc};
        UCHAR *buffers[1]={bytes};
        UINT length=2;
        if (aacDecoder_ConfigRaw(h,buffers,&length)!=AAC_DEC_OK) { aacDecoder_Close(h); return NULL; }
    }
    return h;
}
EXPORT void dreams_aac_close(void *decoder) { if (decoder) aacDecoder_Close(decoder); }
static void fill_info(HANDLE_AACDECODER h, RuntimeAacFrame *f) {
    const CStreamInfo *i=aacDecoder_GetStreamInfo(h);
    if (!i) return;
    f->sample_rate=i->sampleRate; f->channels=i->numChannels; f->frame_size=i->frameSize;
    f->samples_per_frame=i->aacSamplesPerFrame; f->bitrate=(int32_t)i->bitRate;
    f->heaac=i->aot==AOT_SBR || i->aot==AOT_PS || i->extAot==AOT_SBR || i->extAot==AOT_PS;
    f->bytes_used=i->numTotalBytes;
}
EXPORT int dreams_aac_info(void *decoder, RuntimeAacFrame *frame) {
    if (!decoder || !frame) return -1;
    fill_info(decoder,frame);
    return 0;
}
EXPORT int dreams_aac_decode(void *decoder, const uint8_t *input, uint32_t size, int16_t *pcm, uint32_t capacity,
                             RuntimeAacFrame *frame) {
    HANDLE_AACDECODER h=decoder;
    frame->bytes_used=0; frame->error=0;
    aacDecoder_SetParam(h,AAC_TPDEC_CLEAR_BUFFER,1);
    UCHAR *buffers[1]={(UCHAR *)input};
    const UINT sizes[1]={size};
    UINT valid=size;
    if (size) aacDecoder_Fill(h,buffers,sizes,&valid);
    AAC_DECODER_ERROR e=aacDecoder_DecodeFrame(h,pcm,(INT)capacity,0);
    if (e==AAC_DEC_NOT_ENOUGH_BITS) return 1;
    if (e!=AAC_DEC_OK) { frame->error=(int32_t)e; return -1; }
    fill_info(h,frame);
    return 0;
}
