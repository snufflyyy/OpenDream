// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Optional codec library: out/libdreams-codecs.so, built by ./build-deps.sh from
 * ref/shadps4/externals (fdk-aac, artswap ATRAC9 encoder) + src/runtime_codecs/codecs_glue.c.
 * Loaded with dlopen on first use so the main build (build-dreams.sh) needs no extra libraries
 * or flags: without the library, Ajm AAC stays silent and At9Enc can't encode (both logged).
 * Search order: $DREAMS_CODECS_LIB, then libdreams-codecs.so next to the executable. */
#define _GNU_SOURCE
#include "runtime.h"
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static pthread_once_t once=PTHREAD_ONCE_INIT;
static RuntimeCodecs codecs;
static int loaded;

static void load(void) {
    char path[PATH_MAX];
    const char *env=getenv("DREAMS_CODECS_LIB");
    if (env && *env) snprintf(path,sizeof(path),"%s",env);
    else {
        ssize_t n=readlink("/proc/self/exe",path,sizeof(path)-1);
        if (n<=0) { puts("Runtime: codec library not found (no /proc/self/exe)"); return; }
        path[n]=0;
        char *slash=strrchr(path,'/');
        snprintf(slash ? slash+1 : path,sizeof(path)-(size_t)(slash ? slash+1-path : 0),"libdreams-codecs.so");
    }
    void *lib=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if (!lib) {
        printf("Runtime: codec library %s not loaded (%s): AAC silent, no ATRAC9 encoder. Run ./build-deps.sh\n",path,dlerror());
        return;
    }
#define SYM(field,name) do { *(void **)&codecs.field=dlsym(lib,name); if (!codecs.field) { \
        printf("Runtime: codec library %s lacks %s; not used\n",path,name); dlclose(lib); return; } } while (0)
    SYM(aac_open,"dreams_aac_open"); SYM(aac_close,"dreams_aac_close");
    SYM(aac_decode,"dreams_aac_decode"); SYM(aac_info,"dreams_aac_info");
    SYM(at9_stream_work_size,"at9_stream_work_size"); SYM(at9_context_size,"at9_context_size");
    SYM(at9_context_create,"at9_context_create"); SYM(at9_context_reset,"at9_context_reset");
    SYM(at9_context_encode,"at9_context_encode");
#undef SYM
    loaded=1;
    printf("Runtime: codec library %s loaded (fdk-aac decoder, ATRAC9 encoder)\n",path);
}
const RuntimeCodecs *runtime_codecs_get(void) {
    pthread_once(&once,load);
    return loaded ? &codecs : NULL;
}
