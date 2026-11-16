// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* libkernel event flags (sceKernel{Create,Delete,Set,Clear,Wait,Poll,Cancel}EventFlag).
 * Dreams' bundled libSceJobManager.prx imports them; with the loader's return-0 fallback a Create
 * left the handle uninitialised and every Wait "succeeded" at once.
 *
 * Semantics follow shadPS4 ref/shadps4/src/core/libraries/kernel/threads/event_flag.cpp:
 *  - a flag is a 64-bit pattern; Wait/Poll take a bit pattern and a mode:
 *      0x01 AND (all bits set) / 0x02 OR (any bit set), plus
 *      0x10 CLEAR_ALL (pattern = 0 on success) / 0x20 CLEAR_PAT (pattern &= ~bits on success);
 *  - attr: 0x01 FIFO / 0x02 priority wait queue; 0x10 SINGLE (only one waiter allowed: a second
 *    Wait gets EPERM) / 0x20 MULTI. 0 means SINGLE+FIFO;
 *  - Set ORs bits in and wakes, in queue order, every waiter whose condition now holds, applying
 *    that waiter's clear mode immediately (so a later waiter may no longer match);
 *  - Clear ANDs the pattern with the argument (it keeps the given bits; PS4 semantics);
 *  - Cancel sets the pattern and wakes every waiter with ECANCELED, reporting how many;
 *  - Delete wakes every waiter with EACCES; the handle is invalid afterwards (ESRCH);
 *  - Wait with a timeout pointer: ETIMEDOUT with *timeout = 0 when it expires, otherwise the
 *    remaining microseconds are written back (real kernel behaviour; shadPS4 leaves it unchanged);
 *  - Poll never blocks: EBUSY when the condition doesn't hold.
 *  - the result pattern (when the pointer is given) is the pattern at the moment the condition was
 *    met, before clearing; on timeout/poll failure it is the current pattern.
 * Priority queues are served in FIFO order: host threads have no PS4 priorities (same as the
 * semaphores in runtime_sema.c).
 * Handles are pointers to host objects kept in a registry, so stale/garbage handles give ESRCH
 * instead of a crash. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define E_PERM ERR(1)
#define E_SRCH ERR(3)
#define E_NOMEM ERR(12)
#define E_ACCES ERR(13)
#define E_BUSY ERR(16)
#define E_INVAL ERR(22)
#define E_TIMEDOUT ERR(60)
#define E_NAMETOOLONG ERR(63)
#define E_CANCELED ERR(85)

enum { ATTR_FIFO=0x01, ATTR_PRIO=0x02, ATTR_SINGLE=0x10, ATTR_MULTI=0x20 };
enum { MODE_AND=0x01, MODE_OR=0x02, MODE_CLEAR_ALL=0x10, MODE_CLEAR_PAT=0x20 };

typedef struct Waiter {
    uint64_t bits, result;
    uint32_t mode;
    int32_t status;   /* 1 while waiting; then 0, E_CANCELED or E_ACCES */
    pthread_cond_t wake;
    struct Waiter *next;
} Waiter;
typedef struct EventFlag {
    char name[32];
    uint32_t attr;
    uint64_t bits;
    Waiter *first;
    unsigned waiting;  /* threads inside Wait (still referencing the flag) */
    int deleted;
    struct EventFlag *next;
} EventFlag;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static EventFlag *registry;
static size_t created, deleted_count, sets, waits, timeouts;

static EventFlag *find(EventFlag *handle) {
    for (EventFlag *f=registry;f;f=f->next) if (f==handle) return f;
    return NULL;
}
static int matches(uint64_t bits, uint64_t want, uint32_t mode) {
    return (mode & MODE_OR) ? (bits & want)!=0 : (bits & want)==want;
}
static void apply_clear(EventFlag *f, uint64_t want, uint32_t mode) {
    if (mode & MODE_CLEAR_ALL) f->bits=0;
    else if (mode & MODE_CLEAR_PAT) f->bits&=~want;
}
static int valid_mode(uint32_t mode) {
    uint32_t wait=mode & 0xf, clear=mode & 0xf0;
    return (mode & ~UINT32_C(0xff))==0 && (wait==MODE_AND || wait==MODE_OR) &&
           (clear==0 || clear==MODE_CLEAR_ALL || clear==MODE_CLEAR_PAT);
}
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(RUNTIME_COND_CLOCK,&t);
    return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
}
static void unlink_waiter(EventFlag *f, Waiter *w) {
    for (Waiter **p=&f->first;*p;p=&(*p)->next) if (*p==w) { *p=w->next; return; }
}

static ABI int32_t ef_create(EventFlag **out, const char *name, uint32_t attr, uint64_t init, const void *opt) {
    if (!out || !name) return E_INVAL;
    if (opt || (attr & ~UINT32_C(0x33)) || (attr & 0x3)==0x3 || (attr & 0x30)==0x30) return E_INVAL;
    if (strnlen(name,32)>=32) return E_NAMETOOLONG;
    EventFlag *f=calloc(1,sizeof(*f));
    if (!f) return E_NOMEM;
    strcpy(f->name,name);
    f->attr=attr; f->bits=init;
    pthread_mutex_lock(&lock);
    f->next=registry; registry=f; ++created;
    pthread_mutex_unlock(&lock);
    *out=f;
    return 0;
}
static ABI int32_t ef_delete(EventFlag *handle) {
    pthread_mutex_lock(&lock);
    EventFlag **p=&registry;
    while (*p && *p!=handle) p=&(*p)->next;
    if (!*p) { pthread_mutex_unlock(&lock); return E_SRCH; }
    EventFlag *f=*p;
    *p=f->next; f->deleted=1; ++deleted_count;
    for (Waiter *w=f->first;w;w=w->next) { w->status=E_ACCES; w->result=f->bits; pthread_cond_signal(&w->wake); }
    f->first=NULL;
    if (!f->waiting) free(f);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t ef_set(EventFlag *handle, uint64_t bits) {
    pthread_mutex_lock(&lock);
    EventFlag *f=find(handle);
    if (!f) { pthread_mutex_unlock(&lock); return E_SRCH; }
    f->bits|=bits; ++sets;
    for (Waiter **p=&f->first;*p;) {
        Waiter *w=*p;
        if (!matches(f->bits,w->bits,w->mode)) { p=&w->next; continue; }
        w->result=f->bits;
        apply_clear(f,w->bits,w->mode);
        w->status=0;
        *p=w->next;
        pthread_cond_signal(&w->wake);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t ef_clear(EventFlag *handle, uint64_t bits) {
    pthread_mutex_lock(&lock);
    EventFlag *f=find(handle);
    if (f) f->bits&=bits;
    pthread_mutex_unlock(&lock);
    return f ? 0 : E_SRCH;
}
static ABI int32_t ef_cancel(EventFlag *handle, uint64_t pattern, int32_t *waiters) {
    pthread_mutex_lock(&lock);
    EventFlag *f=find(handle);
    if (!f) { pthread_mutex_unlock(&lock); return E_SRCH; }
    int32_t n=0;
    f->bits=pattern;
    for (Waiter *w=f->first;w;w=w->next) { w->status=E_CANCELED; w->result=pattern; pthread_cond_signal(&w->wake); ++n; }
    f->first=NULL;
    if (waiters) *waiters=n;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* block: 0 = poll. timeout: NULL = infinite. */
static int32_t wait_flag(EventFlag *handle, uint64_t bits, uint32_t mode, uint64_t *result, uint32_t *timeout, int block) {
    if (!bits || !valid_mode(mode)) {
        pthread_mutex_lock(&lock);
        int known=find(handle)!=NULL;
        pthread_mutex_unlock(&lock);
        return known ? E_INVAL : E_SRCH;
    }
    pthread_mutex_lock(&lock);
    EventFlag *f=find(handle);
    if (!f) { pthread_mutex_unlock(&lock); return E_SRCH; }
    ++waits;
    if (matches(f->bits,bits,mode)) {
        if (result) *result=f->bits;
        apply_clear(f,bits,mode);
        pthread_mutex_unlock(&lock);
        return 0;
    }
    /* SINGLE flags: only one thread may wait at a time (also checked for polls, as shadPS4 does). */
    if (!(f->attr & ATTR_MULTI) && f->first) { pthread_mutex_unlock(&lock); return E_PERM; }
    if (!block || (timeout && !*timeout)) {
        if (result) *result=f->bits;
        if (block) { *timeout=0; ++timeouts; }
        pthread_mutex_unlock(&lock);
        return block ? E_TIMEDOUT : E_BUSY;
    }
    Waiter w={.bits=bits,.mode=mode,.status=1};
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr,RUNTIME_COND_CLOCK);
    pthread_cond_init(&w.wake,&attr);
    pthread_condattr_destroy(&attr);
    Waiter **tail=&f->first;
    while (*tail) tail=&(*tail)->next;
    *tail=&w;
    ++f->waiting;
    const uint64_t start=runtime_wait_clock();
    uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000u : 0;
    struct timespec end={(time_t)(deadline/1000000000u),(long)(deadline%1000000000u)};
    int32_t status=0;
    while (w.status==1) {
        int e=timeout ? pthread_cond_timedwait(&w.wake,&lock,&end) : pthread_cond_wait(&w.wake,&lock);
        if (e==ETIMEDOUT && w.status==1) {
            unlink_waiter(f,&w);
            w.status=E_TIMEDOUT; w.result=f->bits; ++timeouts;
        }
    }
    status=w.status;
    runtime_wait_note(0,runtime_wait_clock()-start);
    if (result) *result=w.result;
    if (timeout) {
        uint64_t now=now_ns();
        *timeout=status==E_TIMEDOUT || now>=deadline ? 0 : (uint32_t)((deadline-now)/1000u);
    }
    pthread_cond_destroy(&w.wake);
    if (!--f->waiting && f->deleted) free(f);
    pthread_mutex_unlock(&lock);
    return status;
}
static ABI int32_t ef_wait(EventFlag *handle, uint64_t bits, uint32_t mode, uint64_t *result, uint32_t *timeout) {
    return wait_flag(handle,bits,mode,result,timeout,1);
}
static ABI int32_t ef_poll(EventFlag *handle, uint64_t bits, uint32_t mode, uint64_t *result) {
    return wait_flag(handle,bits,mode,result,NULL,0);
}
/* sceKernelOpenEventFlag / CloseEventFlag: named cross-process flags. shadPS4 stubs them (OK);
 * Dreams doesn't import them. Not exported here. */

uintptr_t runtime_eventflag_resolve(const char *name) {
    if (runtime_nid_is(name,"BpFoboUJoZU")) return (uintptr_t)ef_create;
    if (runtime_nid_is(name,"8mql9OcQnd4")) return (uintptr_t)ef_delete;
    if (runtime_nid_is(name,"IOnSvHzqu6A")) return (uintptr_t)ef_set;
    if (runtime_nid_is(name,"7uhBFWRAS60")) return (uintptr_t)ef_clear;
    if (runtime_nid_is(name,"PZku4ZrXJqg")) return (uintptr_t)ef_cancel;
    if (runtime_nid_is(name,"JTvBflhYazQ")) return (uintptr_t)ef_wait;
    if (runtime_nid_is(name,"9lvj5DjHZiA")) return (uintptr_t)ef_poll;
    return 0;
}
void runtime_eventflag_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: event flags created=%zu, deleted=%zu, sets=%zu, waits=%zu, timeouts=%zu\n",
           created,deleted_count,sets,waits,timeouts);
    pthread_mutex_unlock(&lock);
}
