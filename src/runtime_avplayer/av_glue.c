// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Dreams port: FFmpeg side of libSceAvPlayer (src/runtime_avplayer.c). Built by ./build-deps.sh
 * into out/libdreams-av.so against the system FFmpeg (pkg-config libavformat libavcodec
 * libswscale libswresample libavutil) and loaded with dlopen, so dreams-probe itself needs no
 * FFmpeg flags (same scheme as runtime_codecs.c / libdreams-codecs.so).
 *
 * One "media" = one demuxer + up to one video and one audio decoder, driven synchronously by the
 * runtime's decode thread: next_frame() reads packets until a decoded frame is ready and returns it
 * converted to what PS4 AvPlayer hands the game:
 *   video  NV12, rows `pitch` = align64(width) bytes apart, luma plane align16(height) rows (the
 *          last row repeated into the padding), chroma plane right after it (as shadPS4's
 *          videodec/video_utils.cpp CopyNV12Data);
 *   audio  interleaved signed 16-bit PCM, the stream's own channel count and rate (as
 *          avplayer_source.cpp ConvertAudioFrame).
 * Semantics of stream discovery, timestamps and durations follow ref/shadps4 (branch dreams)
 * src/core/libraries/avplayer/avplayer_source.cpp. The ABI between this file and the runtime is
 * RuntimeAv in src/runtime.h. */
#define _GNU_SOURCE
#include "runtime.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))
#define IO_BUFFER 65536

typedef struct {
    AVFormatContext *format;
    AVIOContext *avio;
    RuntimeAvReader reader;
    uint64_t position;
    int stream_map[16]; /* RuntimeAv stream index -> FFmpeg stream index */
    int stream_count;
    int video, audio;   /* FFmpeg indices of the enabled streams, -1 none */
    AVCodecContext *video_codec, *audio_codec;
    AVPacket *packet;
    AVFrame *frame, *converted;
    struct SwsContext *sws;
    SwrContext *swr;
    int swr_format, swr_rate, swr_channels;
    int demux_eof, video_drained, audio_drained;
    int pending_video, pending_audio; /* a decoder may hold more frames: ask it before reading on */
    uint8_t *out;       /* the returned frame's data (valid until the next call) */
    size_t out_capacity;
    char error[160];
} Media;

static void set_error(Media *m, const char *what, int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]="";
    if (code) av_make_error_string(text,sizeof(text),code);
    snprintf(m->error,sizeof(m->error),"%s%s%s",what,code ? ": " : "",text);
}

static int read_packet(void *opaque, uint8_t *buffer, int size) {
    Media *m=opaque;
    if (m->position>=m->reader.size) return AVERROR_EOF;
    uint64_t left=m->reader.size-m->position;
    uint32_t length=(uint32_t)(left<(uint64_t)size ? left : (uint64_t)size);
    int32_t n=m->reader.read(m->reader.opaque,buffer,m->position,length);
    if (n<=0) return AVERROR_EOF;
    m->position+=(uint64_t)n;
    return n;
}
static int64_t seek_packet(void *opaque, int64_t offset, int whence) {
    Media *m=opaque;
    if (whence & AVSEEK_SIZE) return (int64_t)m->reader.size;
    int64_t base=0;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: break;
    case SEEK_CUR: base=(int64_t)m->position; break;
    case SEEK_END: base=(int64_t)m->reader.size; break;
    default: return -1;
    }
    int64_t to=base+offset;
    if (to<0) to=0;
    if ((uint64_t)to>m->reader.size) to=(int64_t)m->reader.size;
    m->position=(uint64_t)to;
    return to;
}

static uint64_t to_ms(int64_t t, AVRational base) {
    if (t==AV_NOPTS_VALUE || t<=0 || base.num<=0 || base.den<=0) return 0;
    int64_t ms=av_rescale_q(t,base,(AVRational){1,1000});
    return ms>0 ? (uint64_t)ms : 0;
}
static uint64_t stream_duration(const AVFormatContext *f, const AVStream *s) {
    uint64_t d=to_ms(s->duration,s->time_base);
    return d ? d : to_ms(f->duration,(AVRational){1,AV_TIME_BASE});
}

static void media_close(void *handle);
EXPORT void *dreams_av_open(const char *host_path, const RuntimeAvReader *reader) {
    Media *m=calloc(1,sizeof(*m));
    if (!m) return NULL;
    m->video=m->audio=-1;
    m->format=avformat_alloc_context();
    if (!m->format) { free(m); return NULL; }
    int r;
    if (reader) {
        m->reader=*reader;
        uint8_t *buffer=av_malloc(IO_BUFFER);
        m->avio=avio_alloc_context(buffer,IO_BUFFER,0,m,read_packet,NULL,seek_packet);
        m->format->pb=m->avio;
        m->format->flags|=AVFMT_FLAG_CUSTOM_IO;
        r=avformat_open_input(&m->format,NULL,NULL,NULL);
    } else {
        r=avformat_open_input(&m->format,host_path,NULL,NULL);
    }
    if (r<0) { /* avformat_open_input frees the context on failure */
        fprintf(stderr,"Runtime: AvPlayer: cannot open %s (%s)\n",host_path ? host_path : "(game file callbacks)",av_err2str(r));
        m->format=NULL;
        media_close(m);
        return NULL;
    }
    if ((r=avformat_find_stream_info(m->format,NULL))<0) {
        fprintf(stderr,"Runtime: AvPlayer: no stream info (%s)\n",av_err2str(r));
        media_close(m);
        return NULL;
    }
    for (unsigned i=0;i<m->format->nb_streams && m->stream_count<16;++i) {
        const AVCodecParameters *p=m->format->streams[i]->codecpar;
        if (p->codec_type!=AVMEDIA_TYPE_VIDEO && p->codec_type!=AVMEDIA_TYPE_AUDIO) continue;
        if (!avcodec_find_decoder(p->codec_id)) continue;
        m->stream_map[m->stream_count++]=(int)i;
    }
    m->packet=av_packet_alloc();
    m->frame=av_frame_alloc();
    m->converted=av_frame_alloc();
    return m;
}

static void media_close(void *handle) {
    Media *m=handle;
    if (!m) return;
    avcodec_free_context(&m->video_codec);
    avcodec_free_context(&m->audio_codec);
    av_packet_free(&m->packet);
    av_frame_free(&m->frame);
    av_frame_free(&m->converted);
    sws_freeContext(m->sws);
    swr_free(&m->swr);
    if (m->format) avformat_close_input(&m->format);
    if (m->avio) { av_freep(&m->avio->buffer); avio_context_free(&m->avio); }
    free(m->out);
    free(m);
}
EXPORT void dreams_av_close(void *handle) { media_close(handle); }

EXPORT int dreams_av_stream_count(void *handle) { return handle ? ((Media *)handle)->stream_count : -1; }

EXPORT int dreams_av_stream_info(void *handle, int index, RuntimeAvStream *out) {
    Media *m=handle;
    if (!m || index<0 || index>=m->stream_count || !out) return -1;
    memset(out,0,sizeof(*out));
    const AVStream *s=m->format->streams[m->stream_map[index]];
    const AVCodecParameters *p=s->codecpar;
    out->ffmpeg_index=m->stream_map[index];
    out->duration_ms=stream_duration(m->format,s);
    const AVDictionaryEntry *lang=av_dict_get(s->metadata,"language",NULL,0);
    if (lang) memcpy(out->language,lang->value,strnlen(lang->value,3));
    if (p->codec_type==AVMEDIA_TYPE_VIDEO) {
        out->type=0;
        out->width=(uint32_t)p->width; out->height=(uint32_t)p->height;
        out->aspect=p->height ? (float)p->width/(float)p->height : 0.0f;
    } else {
        out->type=1;
        out->channels=(uint32_t)p->ch_layout.nb_channels; out->sample_rate=(uint32_t)p->sample_rate;
    }
    return 0;
}

static AVCodecContext *open_decoder(Media *m, int ffmpeg_index) {
    const AVStream *s=m->format->streams[ffmpeg_index];
    const AVCodec *codec=avcodec_find_decoder(s->codecpar->codec_id);
    AVCodecContext *c=codec ? avcodec_alloc_context3(codec) : NULL;
    if (!c) { set_error(m,"no decoder",0); return NULL; }
    int r=avcodec_parameters_to_context(c,s->codecpar);
    c->thread_count=0; /* FFmpeg picks (frame threads for H.264) */
    c->pkt_timebase=s->time_base;
    if (r<0 || (r=avcodec_open2(c,codec,NULL))<0) { set_error(m,"decoder open",r); avcodec_free_context(&c); }
    return c;
}

static void rewind_media(Media *m) {
    av_seek_frame(m->format,-1,0,AVSEEK_FLAG_BACKWARD);
    if (m->video_codec) avcodec_flush_buffers(m->video_codec);
    if (m->audio_codec) avcodec_flush_buffers(m->audio_codec);
    m->demux_eof=0; m->pending_video=m->pending_audio=0;
    m->video_drained=m->video<0; m->audio_drained=m->audio<0;
}

EXPORT int dreams_av_start(void *handle, int video_stream, int audio_stream) {
    Media *m=handle;
    if (!m) return -1;
    int v=video_stream>=0 && video_stream<m->stream_count ? m->stream_map[video_stream] : -1;
    int a=audio_stream>=0 && audio_stream<m->stream_count ? m->stream_map[audio_stream] : -1;
    if (v>=0 && m->format->streams[v]->codecpar->codec_type!=AVMEDIA_TYPE_VIDEO) v=-1;
    if (a>=0 && m->format->streams[a]->codecpar->codec_type!=AVMEDIA_TYPE_AUDIO) a=-1;
    if (v<0 && a<0) { set_error(m,"no stream enabled",0); return -1; }
    if (v!=m->video) { avcodec_free_context(&m->video_codec); m->video=v; if (v>=0 && !(m->video_codec=open_decoder(m,v))) return -1; }
    if (a!=m->audio) { avcodec_free_context(&m->audio_codec); m->audio=a; if (a>=0 && !(m->audio_codec=open_decoder(m,a))) return -1; }
    rewind_media(m);
    return 0;
}

EXPORT int dreams_av_seek(void *handle, uint64_t ms) {
    Media *m=handle;
    if (!m) return -1;
    int64_t ts=av_rescale((int64_t)ms,AV_TIME_BASE,1000);
    int r=avformat_seek_file(m->format,-1,INT64_MIN,ts,ts,0);
    if (r<0) r=av_seek_frame(m->format,-1,ts,AVSEEK_FLAG_BACKWARD);
    if (m->video_codec) avcodec_flush_buffers(m->video_codec);
    if (m->audio_codec) avcodec_flush_buffers(m->audio_codec);
    m->demux_eof=0; m->pending_video=m->pending_audio=0;
    m->video_drained=m->video<0; m->audio_drained=m->audio<0;
    return r<0 ? -1 : 0;
}

static uint64_t frame_ms(const AVFrame *f, AVRational base) {
    int64_t t=f->best_effort_timestamp;
    if (t==AV_NOPTS_VALUE) t=f->pts;
    if (t==AV_NOPTS_VALUE) t=f->pkt_dts;
    return to_ms(t,base);
}

static int reserve(Media *m, size_t size) {
    if (size<=m->out_capacity) return 0;
    uint8_t *p=realloc(m->out,size);
    if (!p) return -1;
    m->out=p; m->out_capacity=size;
    return 0;
}

static int emit_video(Media *m, AVFrame *f, RuntimeAvFrame *out) {
    const AVFrame *src=f;
    if (f->format!=AV_PIX_FMT_NV12) {
        m->sws=sws_getCachedContext(m->sws,f->width,f->height,(enum AVPixelFormat)f->format,f->width,f->height,
                                    AV_PIX_FMT_NV12,SWS_BILINEAR,NULL,NULL,NULL);
        av_frame_unref(m->converted);
        m->converted->format=AV_PIX_FMT_NV12; m->converted->width=f->width; m->converted->height=f->height;
        if (!m->sws || av_frame_get_buffer(m->converted,0)<0 ||
            sws_scale(m->sws,(const uint8_t *const *)f->data,f->linesize,0,f->height,m->converted->data,m->converted->linesize)<0) {
            set_error(m,"NV12 conversion",0);
            return -1;
        }
        src=m->converted;
    }
    const uint32_t width=(uint32_t)src->width, rows=(uint32_t)src->height;
    const uint32_t pitch=(width+63u)&~63u, height=(rows+15u)&~15u;
    const size_t size=(size_t)pitch*height*3/2;
    if (reserve(m,size)) { set_error(m,"out of memory",0); return -1; }
    memset(m->out,0,size);
    uint8_t *luma=m->out, *chroma=m->out+(size_t)pitch*height;
    for (uint32_t y=0;y<height;++y) {
        const uint32_t from=y<rows ? y : rows-1;
        memcpy(luma+(size_t)y*pitch,src->data[0]+(ptrdiff_t)from*src->linesize[0],width);
    }
    for (uint32_t y=0;y<height/2;++y) {
        const uint32_t from=y<rows/2 ? y : rows/2-1;
        memcpy(chroma+(size_t)y*pitch,src->data[1]+(ptrdiff_t)from*src->linesize[1],width);
    }
    memset(out,0,sizeof(*out));
    out->type=0;
    out->timestamp_ms=frame_ms(f,m->format->streams[m->video]->time_base);
    out->data=m->out; out->size=size;
    out->width=(width+15u)&~15u; out->height=height; out->pitch=pitch; out->frame_width=width; out->frame_height=rows;
    out->crop_left=(uint32_t)f->crop_left; out->crop_right=(uint32_t)f->crop_right+(pitch-width);
    out->crop_top=(uint32_t)f->crop_top; out->crop_bottom=(uint32_t)f->crop_bottom+(height-rows);
    out->sample_aspect=(float)av_q2d(f->sample_aspect_ratio);
    return 1;
}

static int emit_audio(Media *m, AVFrame *f, RuntimeAvFrame *out) {
    const int channels=f->ch_layout.nb_channels;
    if (channels<=0 || f->nb_samples<=0) return 0;
    const size_t size=(size_t)channels*(size_t)f->nb_samples*2;
    if (reserve(m,size)) { set_error(m,"out of memory",0); return -1; }
    if (f->format==AV_SAMPLE_FMT_S16) {
        memcpy(m->out,f->data[0],size);
    } else {
        if (!m->swr || m->swr_format!=f->format || m->swr_rate!=f->sample_rate || m->swr_channels!=channels) {
            swr_free(&m->swr);
            if (swr_alloc_set_opts2(&m->swr,&f->ch_layout,AV_SAMPLE_FMT_S16,f->sample_rate,&f->ch_layout,
                                    (enum AVSampleFormat)f->format,f->sample_rate,0,NULL)<0 || swr_init(m->swr)<0) {
                swr_free(&m->swr);
                set_error(m,"PCM conversion",0);
                return -1;
            }
            m->swr_format=f->format; m->swr_rate=f->sample_rate; m->swr_channels=channels;
        }
        uint8_t *dst[1]={m->out};
        int n=swr_convert(m->swr,dst,f->nb_samples,(const uint8_t **)f->extended_data,f->nb_samples);
        if (n<0) { set_error(m,"PCM conversion",n); return -1; }
    }
    memset(out,0,sizeof(*out));
    out->type=1;
    out->timestamp_ms=frame_ms(f,m->format->streams[m->audio]->time_base);
    out->data=m->out; out->size=size;
    out->channels=(uint32_t)channels; out->sample_rate=(uint32_t)f->sample_rate; out->samples=(uint32_t)f->nb_samples;
    return 1;
}

/* A frame the decoder has ready: 1 (out filled), 0 none now, -1 error. `drained` set at its EOF. */
static int receive(Media *m, AVCodecContext *c, int video, int *drained, RuntimeAvFrame *out) {
    for (;;) {
        int r=avcodec_receive_frame(c,m->frame);
        if (r==AVERROR(EAGAIN)) return 0;
        if (r==AVERROR_EOF) { *drained=1; return 0; }
        if (r<0) { set_error(m,"decoding",r); return -1; }
        r=video ? emit_video(m,m->frame,out) : emit_audio(m,m->frame,out);
        av_frame_unref(m->frame);
        if (r) return r;
    }
}

EXPORT int dreams_av_next_frame(void *handle, RuntimeAvFrame *out) {
    Media *m=handle;
    if (!m || (m->video<0 && m->audio<0)) return -1;
    for (;;) {
        int r;
        if (m->pending_video && m->video_codec) {
            if ((r=receive(m,m->video_codec,1,&m->video_drained,out))) return r;
            m->pending_video=0;
        }
        if (m->pending_audio && m->audio_codec) {
            if ((r=receive(m,m->audio_codec,0,&m->audio_drained,out))) return r;
            m->pending_audio=0;
        }
        if (m->demux_eof) {
            if ((m->video<0 || m->video_drained) && (m->audio<0 || m->audio_drained)) return 0;
            /* Both decoders were flushed at EOF: what they still hold comes out, then EOF. */
            m->pending_video=m->video>=0 && !m->video_drained;
            m->pending_audio=m->audio>=0 && !m->audio_drained;
            if (m->pending_video && (r=receive(m,m->video_codec,1,&m->video_drained,out))) return r;
            if (m->pending_audio && (r=receive(m,m->audio_codec,0,&m->audio_drained,out))) return r;
            if ((m->video<0 || m->video_drained) && (m->audio<0 || m->audio_drained)) return 0;
            continue;
        }
        r=av_read_frame(m->format,m->packet);
        if (r==AVERROR_EOF || (r<0 && avio_feof(m->format->pb))) {
            m->demux_eof=1;
            if (m->video_codec) avcodec_send_packet(m->video_codec,NULL);
            if (m->audio_codec) avcodec_send_packet(m->audio_codec,NULL);
            continue;
        }
        if (r<0) { set_error(m,"demuxing",r); return -1; }
        AVCodecContext *c=m->packet->stream_index==m->video ? m->video_codec
                         : m->packet->stream_index==m->audio ? m->audio_codec : NULL;
        if (c) {
            r=avcodec_send_packet(c,m->packet);
            if (r<0 && r!=AVERROR(EAGAIN) && r!=AVERROR_INVALIDDATA) { av_packet_unref(m->packet); set_error(m,"decoding",r); return -1; }
            if (c==m->video_codec) m->pending_video=1; else m->pending_audio=1;
        }
        av_packet_unref(m->packet);
    }
}

EXPORT const char *dreams_av_error(void *handle) { return handle ? ((Media *)handle)->error : "no media"; }
EXPORT const char *dreams_av_version(void) { return av_version_info(); }
