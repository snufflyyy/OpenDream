// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* System services audit (src/runtime_dreams_services.c), standalone; tests/run-dreams-tests.sh. */
#include "../src/runtime_dreams_services.c"
#include <assert.h>
#include <sys/stat.h>

static char temp_dir[256];
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,nid)) return (uintptr_t)table[i].function;
    return 0;
}
int runtime_file_host_path(const char *guest, char *out, size_t size) {
    assert(!strcmp(guest,"/temp0")); snprintf(out,size,"%s",temp_dir); return 0;
}
const RuntimeCodecs *runtime_codecs_get(void) { return NULL; }
#define F(type,name) ((type)runtime_dreams_services_resolve(name))

int main(void) {
    uint8_t id[16];
    assert(F(int32_t (ABI *)(uint8_t *),"sceKernelGetOpenPsId")(id)==0 && !memcmp(id,"shadPS4\0Dreams\0\1",16));
    HeapTraceInfo info; memset(&info,0xcc,sizeof(info));
    F(void (ABI *)(HeapTraceInfo *),"sceLibcHeapGetTraceInfo")(&info);
    assert(info.atomic_id_mask && *info.atomic_id_mask==0 && info.mstate_table && info.mstate_table[63]==0);
    int32_t color=-1;
    assert(F(int32_t (ABI *)(int32_t,int32_t *),"sceUserServiceGetUserColor")(1,&color)==0 && color==0);
    assert(F(int32_t (ABI *)(int32_t,int32_t *),"sceUserServiceGetUserColor")(2,&color)==USER_ERR_NOT_LOGGED_IN);
    /* temporary data: mount point string, FORMAT empties the host folder */
    snprintf(temp_dir,sizeof(temp_dir),"out/tests/temp0");
    mkdir(temp_dir,0755); mkdir("out/tests/temp0/sub",0755);
    FILE *f=fopen("out/tests/temp0/sub/file","w"); fputs("x",f); fclose(f);
    char mount[16]; memset(mount,'z',sizeof(mount));
    int32_t (ABI *tmp)(uint32_t,char *)=F(int32_t (ABI *)(uint32_t,char *),"sceAppContentTemporaryDataMount2");
    assert(tmp(0,mount)==0 && !strcmp(mount,"/temp0") && !access("out/tests/temp0/sub/file",F_OK));
    assert(tmp(1,mount)==0 && access("out/tests/temp0/sub",F_OK) && !access(temp_dir,F_OK));
    assert(tmp(0,NULL)==APP_CONTENT_ERR_PARAMETER);
    /* word filter: sanitize copies the text, async requests finish immediately */
    int32_t ctx=F(int32_t (ABI *)(int32_t),"sceNpWordFilterCreateTitleCtxA")(1);
    assert(ctx==0x01000001);
    WordFilterAsyncParam ap={sizeof(ap),0,0,{0}};
    int32_t req=F(int32_t (ABI *)(int32_t,const WordFilterAsyncParam *),"sceNpWordFilterCreateAsyncRequest")(ctx,&ap);
    assert(req==0x02000001);
    int32_t (ABI *poll)(int32_t,int32_t *)=F(int32_t (ABI *)(int32_t,int32_t *),"sceNpWordFilterPollAsync");
    int32_t result=-1;
    assert(poll(req,&result)==1);
    char out[64]; memset(out,'z',sizeof(out));
    int32_t (ABI *sanitize)(int32_t,const char *,char *,void *)=F(int32_t (ABI *)(int32_t,const char *,char *,void *),"sceNpWordFilterSanitizeComment");
    assert(sanitize(req,"my dream",out,NULL)==0 && !strcmp(out,"my dream"));
    assert(poll(req,&result)==0 && result==0);
    assert(sanitize(req,"again",out,NULL)==NPC_ERR_INVALID_TYPE);
    assert(sanitize(0x02000099,"x",out,NULL)==NPC_ERR_INVALID_ID);
    assert(F(int32_t (ABI *)(int32_t),"sceNpWordFilterDeleteRequest")(req)==0);
    assert(F(int32_t (ABI *)(int32_t),"sceNpWordFilterDeleteTitleCtx")(ctx)==0);
    /* Hmd reprojection buffer queries return sizes; VrTracker reports its memory needs */
    assert(F(int32_t (ABI *)(void),"sceHmdReprojectionQueryGarlicBuffSize")()==0x100000);
    assert(F(int32_t (ABI *)(void),"sceHmdReprojectionQueryOnionBuffSize")()==0x810);
    VrTrackerQueryParam vp={.size=sizeof(vp),.move_position=1}; VrTrackerQueryResult vr={0};
    assert(F(int32_t (ABI *)(const VrTrackerQueryParam *,VrTrackerQueryResult *),"sceVrTrackerQueryMemory")(&vp,&vr)==0);
    assert(vr.onion_size==0x800000 && vr.garlic_size==0x3000000 && vr.work_size==0x1000000 && vr.garlic_align==0x10000);
    /* coredump handler registers once */
    int32_t (ABI *core)(void *,uint64_t,void *)=F(int32_t (ABI *)(void *,uint64_t,void *),"sceCoredumpRegisterCoredumpHandler");
    assert(core((void *)main,0x4000,NULL)==0 && core((void *)main,0x4000,NULL)==COREDUMP_ERR_ALREADY_REGISTERED);
    /* At9Enc without the codec library: Dreams' configuration accepted, encode fails cleanly */
    At9EncParam ep={sizeof(ep),2,48000,192,2,4,{0,0,0xffffff01,0xffffff01,4,1,0}};
    int32_t bytes=0, err=1;
    assert(F(int32_t (ABI *)(const At9EncParam *,int32_t *,int32_t *),"sceAt9EncQueryMemSize")(&ep,&bytes,&err)==0 && bytes>0 && err==0);
    ep.bitrate_kbps=96;
    assert(F(int32_t (ABI *)(const At9EncParam *,int32_t *,int32_t *),"sceAt9EncQueryMemSize")(&ep,&bytes,&err)==-1 && bytes==0);
    /* deliberate no-ops resolve */
    assert(runtime_dreams_services_resolve("sceRazorCpuJobManagerJob")==(uintptr_t)ok);
    puts("dreams services tests passed");
    return 0;
}
