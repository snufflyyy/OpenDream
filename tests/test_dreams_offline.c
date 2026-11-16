// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Offline NP / sign-in dialog / kernel sync semantics (src/runtime_dreams.c), standalone. */
#include "../src/runtime_dreams.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,nid)) return (uintptr_t)table[i].function;
    return 0;
}
#define F(type,name) ((type)runtime_dreams_resolve(name))
typedef struct { uint32_t type; int32_t reserved[3]; } Result;

int main(void) {
    /* sceNpHasSignedUp: out-param is a bool, offline user has not signed up */
    int32_t (ABI *has)(int32_t,bool *)=F(int32_t (ABI *)(int32_t,bool *),"sceNpHasSignedUp");
    bool su=true;
    assert(has(1,&su)==0 && su==false);
    su=true; assert(has(-1,&su)==(int32_t)0x80550003 && su==false);
    su=true; assert(has(7,&su)==(int32_t)0x80550007 && su==false);
    assert(has(1,NULL)==(int32_t)0x80550003);
    /* sign-in dialog: init -> open -> FINISHED + USER_CANCELED -> close -> terminate */
    int32_t (ABI *init)(void)=F(int32_t (ABI *)(void),"sceSigninDialogInitialize");
    int32_t (ABI *open)(const void *)=F(int32_t (ABI *)(const void *),"sceSigninDialogOpen");
    int32_t (ABI *upd)(void)=F(int32_t (ABI *)(void),"sceSigninDialogUpdateStatus");
    int32_t (ABI *res)(Result *)=F(int32_t (ABI *)(Result *),"sceSigninDialogGetResult");
    int32_t (ABI *close_)(void)=F(int32_t (ABI *)(void),"sceSigninDialogClose");
    int32_t (ABI *term)(void)=F(int32_t (ABI *)(void),"sceSigninDialogTerminate");
    Result r; memset(&r,0xcc,sizeof(r));
    assert(upd()==0 && open(NULL)==(int32_t)0x81350001 && res(&r)==(int32_t)0x81350001);
    assert(init()==0 && upd()==1 && init()==(int32_t)0x81350002);
    assert(res(&r)==(int32_t)0x81350005);
    assert(open(NULL)==0 && upd()==3 && open(NULL)==(int32_t)0x81350005);
    assert(res(NULL)==(int32_t)0x81350003);
    assert(res(&r)==0 && r.type==1 && !r.reserved[0] && !r.reserved[1] && !r.reserved[2]);
    assert(close_()==0 && upd()==1 && open(NULL)==0);
    assert(term()==0 && upd()==0 && term()==(int32_t)0x81350001);
    /* sceKernelSync resolves and returns */
    F(void (ABI *)(void),"sceKernelSync")();
    puts("dreams offline tests passed");
    return 0;
}
