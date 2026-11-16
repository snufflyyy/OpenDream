// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BB_RUNTIME_H
#define BB_RUNTIME_H
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#define ABI __attribute__((sysv_abi))
/* The clock of the runtime's condition variable deadlines: monotonic on Linux; winpthreads'
 * conditions take only CLOCK_REALTIME deadlines. */
#ifdef _WIN32
#define RUNTIME_COND_CLOCK CLOCK_REALTIME
#else
#define RUNTIME_COND_CLOCK CLOCK_MONOTONIC
#endif
/* A local time's offset east of UTC, seconds (Windows' struct tm has no tm_gmtoff). */
#ifdef _WIN32
#define RUNTIME_TM_GMTOFF(t) bb_gmtoff(&(t))
#else
#define RUNTIME_TM_GMTOFF(t) ((t).tm_gmtoff)
#endif
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
/* Reserves the guest's address range (Windows; probe.c, first thing). */
void runtime_memory_reserve(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
/* The CPU is about to write the range outside guest code (a file read): tells the GPU side. */
void runtime_memory_note_write(uintptr_t address, uint64_t size);
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size);
void runtime_memory_prepare_cpu_write(uintptr_t address, uint64_t size);
/* bbport (frame stats): a guest thread was blocked `ns` in the runtime (0 cond, 1 mutex, 2 sema, 3 sleep). */
void runtime_wait_note(int kind, uint64_t ns);
void runtime_wait_report(double frames);
void runtime_guest_call_sites(uint64_t out[3]);
/* bbport: times the game's heap asked for more memory (posix_mmap): a sign it leaks. */
uint64_t runtime_heap_growths(void);
static inline uint64_t runtime_wait_clock(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec; }
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
/* An import names `nid`: the NID alone decides (the hash of the symbol's name), whatever library
 * letters the game's import carries (pthread_mutex_lock is libScePosix's in one game, libkernel's
 * in another). */
static inline int runtime_nid_is(const char *name, const char *nid) {
    const size_t length = strcspn(name, "#");
    return length == strlen(nid) && !strncmp(name, nid, length);
}
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
/* Online play (runtime_net.c, BB_ONLINE=1): the online module answers the network and PSN imports. */
void runtime_net_configure(const char *app0, const char *serial, const char *title, const char *version);
int runtime_net_enabled(void);
int runtime_net_symbol(const char *symbol);
uintptr_t runtime_net_resolve(const char *name);
void runtime_net_image_ready(uintptr_t base, uint64_t size);
void runtime_net_image_mapped(uint8_t *image, uint64_t size);
void runtime_net_start(void);
__attribute__((noreturn)) void runtime_thread_exit(void *value);
/* Guest threads for host code (the online module's NP threads); 0 or a positive errno value. */
int32_t runtime_thread_spawn(void **thread, void *(ABI *entry)(void *), void *argument, uint64_t stack, const char *name);
int32_t runtime_thread_join_spawned(void *thread, void **result);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
/* bbport: the loader changed the protection of part of the low region (mmap PROT_ bits). */
void runtime_low_protect(uintptr_t start, size_t size, int prot);
/* bbport: the low region's mapping holding address: 1 with its PS4 protection bits and end. */
int runtime_memory_low_info(uintptr_t address, int *prot, uintptr_t *end);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
/* A guest path ("/app0/...") as the host file it maps to: 0, or an errno. */
int runtime_file_host_path(const char *guest,char *out,size_t size);
/* runtime_remote.c: the game's files fetched from a phone over adb on first open (BB_APP0_ADB). */
void runtime_remote_configure(const char *app0);
int runtime_remote_fetch(const char *host);
int runtime_remote_size(const char *host,int64_t *size);
/* The character save the game uses: its slot (userdataNNNN, 0..9) and host folder; -1 before any. */
int runtime_savedata_active_slot(char *dir, size_t size);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
void runtime_thread_attach_host(const char *name);
/* The game's libc.prx where it was linked (image + module base), 0 before or without it. */
extern uintptr_t runtime_libc_base;
/* Calls that need no work here (runtime_compat.c). */
uintptr_t runtime_compat_resolve(const char *name);
/* Dreams port: PS4 functions Dreams needs beyond bbport's runtime (runtime_dreams.c). */
uintptr_t runtime_dreams_resolve(const char *name);
/* Dreams port: system services audit (runtime_dreams_services.c), event flags
 * (runtime_eventflag.c), Audio3d (runtime_audio3d.c). */
uintptr_t runtime_dreams_services_resolve(const char *name);
uintptr_t runtime_eventflag_resolve(const char *name);
void runtime_eventflag_report(void);
uintptr_t runtime_audio3d_resolve(const char *name);
void runtime_audio3d_report(void);
/* AudioOut entry points for other runtime modules (runtime_audio.c). */
int32_t runtime_audio_out_init(void);
int32_t runtime_audio_out_open(int32_t user, int32_t type, int32_t index, uint32_t length, uint32_t freq, uint32_t param);
int32_t runtime_audio_out_output(int32_t handle, const void *data);
int32_t runtime_audio_out_close(int32_t handle);
/* Dreams port: optional codec library out/libdreams-codecs.so (build-deps.sh), loaded with dlopen
 * by runtime_codecs.c: fdk-aac (AAC decoding for Ajm) and the artswap ATRAC9 encoder (At9Enc).
 * The glue is src/runtime_codecs/codecs_glue.c; this struct is the ABI between them. */
typedef struct {
    int32_t sample_rate, channels, frame_size, samples_per_frame, bitrate, heaac;
    uint32_t bytes_used; /* input bytes the frame consumed */
    int32_t error;       /* fdk AAC_DECODER_ERROR on failure */
} RuntimeAacFrame;
typedef struct {
    /* AAC: raw=1 -> raw access units configured from (freq_index, channels) like shadPS4's RAW
     * mode; raw=0 -> ADTS. decode: 0 ok, 1 needs more input, -1 error (frame->error). */
    void *(*aac_open)(int raw, unsigned freq_index, unsigned channels);
    void (*aac_close)(void *decoder);
    int (*aac_decode)(void *decoder, const uint8_t *input, uint32_t size, int16_t *pcm, uint32_t pcm_capacity, RuntimeAacFrame *frame);
    int (*aac_info)(void *decoder, RuntimeAacFrame *frame);
    /* ATRAC9 encoder (ref/shadps4/externals/artswap_at9/at9_stream.h). */
    size_t (*at9_stream_work_size)(void);
    size_t (*at9_context_size)(void);
    int (*at9_context_create)(void *context, size_t bytes, int channels, int bitrate);
    int (*at9_context_reset)(void *context);
    int (*at9_context_encode)(void *context, void *scratch, size_t scratch_bytes, const int16_t *input, int input_bytes,
                              unsigned char *output, int output_capacity, int *output_bytes, int *consumed_bytes);
} RuntimeCodecs;
/* The loaded codec library, or NULL (then AAC is silent and At9Enc can't encode). */
const RuntimeCodecs *runtime_codecs_get(void);

/* Dreams port: libSceAvPlayer (src/runtime_avplayer.c) decodes with FFmpeg through the optional
 * library out/libdreams-av.so (build-deps.sh, glue src/runtime_avplayer/av_glue.c), loaded with
 * dlopen like the codec library. These structs are the ABI between the two. */
typedef struct {
    void *opaque;
    uint64_t size;
    /* The game's read_offset callback (or a test's): bytes read, <=0 at the end / on error. */
    int32_t (*read)(void *opaque, uint8_t *buffer, uint64_t position, uint32_t length);
} RuntimeAvReader;
typedef struct {
    int32_t type;           /* 0 video, 1 audio */
    int32_t ffmpeg_index;
    uint32_t width, height; /* video: the coded size (not aligned) */
    float aspect;           /* video: width / height */
    uint32_t channels, sample_rate;
    char language[4];       /* ISO 639-2, up to 3 letters, zero padded */
    uint64_t duration_ms;
} RuntimeAvStream;
typedef struct {
    int32_t type;           /* 0 video (NV12), 1 audio (interleaved s16) */
    uint64_t timestamp_ms;
    const uint8_t *data;    /* owned by the media, valid until its next call */
    uint64_t size;
    /* video: width = align16(frame_width), height = align16(frame_height) = the plane's rows,
     * pitch = align64(frame_width); crops as shadPS4's AvPlayerSource::PrepareVideoFrame */
    uint32_t width, height, pitch, frame_width, frame_height;
    uint32_t crop_left, crop_right, crop_top, crop_bottom;
    float sample_aspect;
    uint32_t channels, sample_rate, samples; /* audio */
} RuntimeAvFrame;
typedef struct {
    /* reader NULL: open host_path; else read through the reader (host_path only for messages). */
    void *(*open)(const char *host_path, const RuntimeAvReader *reader);
    void (*close)(void *media);
    int (*stream_count)(void *media);
    int (*stream_info)(void *media, int index, RuntimeAvStream *out);
    /* Opens the decoders of the chosen streams (-1: none) and rewinds; 0 ok, -1 error. */
    int (*start)(void *media, int video_stream, int audio_stream);
    int (*seek)(void *media, uint64_t ms);
    /* The next decoded frame of either enabled stream: 1 frame, 0 end of file, -1 error. */
    int (*next_frame)(void *media, RuntimeAvFrame *out);
    const char *(*error)(void *media);
    const char *(*version)(void);
} RuntimeAv;
/* The loaded AvPlayer decoder library, or NULL (then AddSource fails: no video, game goes on). */
const RuntimeAv *runtime_av_get(void);
/* runtime_avplayer.c: a video plays now (any player) / stop it (pad: Enter, Escape, Options). */
int runtime_avplayer_active(void);
int runtime_avplayer_skip(void);
uintptr_t runtime_avplayer_resolve(const char *name);
void runtime_avplayer_report(void);
/* runtime_ime.c: libSceIme (sceImeOpen text entry through the game window, USB keyboard events). */
uintptr_t runtime_ime_resolve(const char *name);
#endif
