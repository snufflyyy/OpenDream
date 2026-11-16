// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Event flags (src/runtime_eventflag.c), built standalone by tests/run-dreams-tests.sh. */
#include "../src/runtime_eventflag.c"
#include <assert.h>
#include <unistd.h>

void runtime_wait_note(int kind, uint64_t ns) { (void)kind; (void)ns; }

typedef int32_t (ABI *Create)(EventFlag **, const char *, uint32_t, uint64_t, const void *);
typedef int32_t (ABI *Handle)(EventFlag *);
typedef int32_t (ABI *Bits)(EventFlag *, uint64_t);
typedef int32_t (ABI *Cancel)(EventFlag *, uint64_t, int32_t *);
typedef int32_t (ABI *Wait)(EventFlag *, uint64_t, uint32_t, uint64_t *, uint32_t *);
typedef int32_t (ABI *Poll)(EventFlag *, uint64_t, uint32_t, uint64_t *);
static Create create; static Handle destroy; static Bits set, clear; static Cancel cancel; static Wait wait_ef; static Poll poll_ef;
#define U(x) ((int32_t)(uint32_t)(x))

static void resolve(void) {
    create=(Create)runtime_eventflag_resolve("BpFoboUJoZU#p#J");
    destroy=(Handle)runtime_eventflag_resolve("8mql9OcQnd4#I#J");
    set=(Bits)runtime_eventflag_resolve("IOnSvHzqu6A");
    clear=(Bits)runtime_eventflag_resolve("7uhBFWRAS60#p#J");
    cancel=(Cancel)runtime_eventflag_resolve("PZku4ZrXJqg#p#J");
    wait_ef=(Wait)runtime_eventflag_resolve("JTvBflhYazQ#p#J");
    poll_ef=(Poll)runtime_eventflag_resolve("9lvj5DjHZiA#p#J");
    assert(create && destroy && set && clear && cancel && wait_ef && poll_ef);
}
static void basics(void) {
    EventFlag *f=NULL; uint64_t r=0;
    assert(U(create(NULL,"x",0,0,NULL))==U(0x80020016));
    assert(U(create(&f,"0123456789012345678901234567890123",0,0,NULL))==U(0x8002003f));
    assert(U(create(&f,"x",0x33,0,NULL))==U(0x80020016));
    assert(create(&f,"basic",0x21,0x5,NULL)==0 && f);
    assert(poll_ef(f,0x1,MODE_AND,&r)==0 && r==0x5);
    assert(poll_ef(f,0x7,MODE_AND,&r)==U(E_BUSY) && r==0x5);
    assert(poll_ef(f,0x6,MODE_OR|MODE_CLEAR_PAT,&r)==0 && r==0x5);
    assert(poll_ef(f,0x4,MODE_OR,&r)==U(E_BUSY) && r==0x1);         /* 0x4 cleared, 0x1 kept */
    assert(poll_ef(f,0,MODE_AND,&r)==U(E_INVAL));
    assert(poll_ef(f,1,0x3,&r)==U(E_INVAL));
    assert(set(f,0xf0)==0);
    assert(clear(f,0x30)==0);                                         /* keeps only 0x30 */
    assert(poll_ef(f,0xff,MODE_OR|MODE_CLEAR_ALL,&r)==0 && r==0x30);
    assert(poll_ef(f,0xff,MODE_OR,&r)==U(E_BUSY) && r==0);
    uint32_t timeout=0;
    assert(wait_ef(f,1,MODE_AND,&r,&timeout)==U(E_TIMEDOUT) && timeout==0);
    timeout=20000;
    uint64_t t0=now_ns();
    assert(wait_ef(f,1,MODE_AND,&r,&timeout)==U(E_TIMEDOUT) && timeout==0);
    assert(now_ns()-t0>=19000000u);
    assert(destroy(f)==0);
    assert(destroy(f)==U(E_SRCH));
    assert(set(f,1)==U(E_SRCH));
    assert(poll_ef(f,1,MODE_AND,&r)==U(E_SRCH));
}
typedef struct { EventFlag *f; uint64_t bits; uint32_t mode; int32_t status; uint64_t result; uint32_t timeout; int use_timeout; } Job;
static void *waiter(void *p) {
    Job *j=p;
    j->status=wait_ef(j->f,j->bits,j->mode,&j->result,j->use_timeout ? &j->timeout : NULL);
    return NULL;
}
static void wait_for_waiters(EventFlag *f, unsigned n) {
    for (int i=0;i<2000;++i) {
        pthread_mutex_lock(&lock);
        unsigned k=0; for (Waiter *w=f->first;w;w=w->next) ++k;
        pthread_mutex_unlock(&lock);
        if (k==n) return;
        usleep(1000);
    }
    assert(!"waiters did not block");
}
static void wakeups(void) {
    EventFlag *f;
    assert(create(&f,"multi",ATTR_MULTI|ATTR_FIFO,0,NULL)==0);
    Job a={f,0x3,MODE_AND|MODE_CLEAR_PAT,99,0,0,0}, b={f,0x1,MODE_OR,99,0,0,0}, c={f,0x8,MODE_OR,99,0,5000000,1};
    pthread_t ta,tb,tc;
    pthread_create(&ta,NULL,waiter,&a); wait_for_waiters(f,1);
    pthread_create(&tb,NULL,waiter,&b); wait_for_waiters(f,2);
    pthread_create(&tc,NULL,waiter,&c); wait_for_waiters(f,3);
    assert(set(f,0x1)==0);            /* b (OR 0x1) wakes; a needs 0x3 */
    pthread_join(tb,NULL);
    assert(b.status==0 && b.result==0x1);
    wait_for_waiters(f,2);
    assert(set(f,0x2)==0);            /* a wakes, clears 0x3 */
    pthread_join(ta,NULL);
    assert(a.status==0 && a.result==0x3);
    uint64_t r=0;
    assert(poll_ef(f,0x3,MODE_OR,&r)==U(E_BUSY) && r==0);
    assert(set(f,0x8)==0);
    pthread_join(tc,NULL);
    assert(c.status==0 && c.result==0x8 && c.timeout>0 && c.timeout<=5000000);
    /* cancel: every waiter gets ECANCELED and the new pattern */
    Job d={f,0x100,MODE_AND,99,0,0,0}, e={f,0x200,MODE_AND,99,0,0,0};
    pthread_t td,te;
    pthread_create(&td,NULL,waiter,&d); pthread_create(&te,NULL,waiter,&e); wait_for_waiters(f,2);
    int32_t n=-1;
    assert(cancel(f,0x42,&n)==0 && n==2);
    pthread_join(td,NULL); pthread_join(te,NULL);
    assert(d.status==U(E_CANCELED) && d.result==0x42 && e.status==U(E_CANCELED));
    assert(poll_ef(f,0x42,MODE_AND,&r)==0);
    /* delete: waiters get EACCES */
    Job g={f,0x1000,MODE_AND,99,0,0,0};
    pthread_t tg;
    pthread_create(&tg,NULL,waiter,&g); wait_for_waiters(f,1);
    assert(destroy(f)==0);
    pthread_join(tg,NULL);
    assert(g.status==U(E_ACCES));
}
static void single(void) {
    EventFlag *f;
    assert(create(&f,"single",0,0,NULL)==0);   /* attr 0 = SINGLE */
    Job a={f,1,MODE_AND,99,0,0,0};
    pthread_t ta;
    pthread_create(&ta,NULL,waiter,&a); wait_for_waiters(f,1);
    uint32_t timeout=1000; uint64_t r;
    assert(wait_ef(f,2,MODE_AND,&r,&timeout)==U(E_PERM));
    assert(set(f,1)==0);
    pthread_join(ta,NULL);
    assert(a.status==0);
    assert(destroy(f)==0);
}
int main(void) {
    resolve();
    basics();
    wakeups();
    single();
    runtime_eventflag_report();
    puts("eventflag tests passed");
    return 0;
}
