// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// CPU-only backing-view contracts. Include the runtime to exercise the real mappings without
// linking a GPU, loader or game. Unused entry points are discarded by --gc-sections.
#include "../src/runtime_memory.c"
#include <assert.h>

static int unused_import(uint64_t phys, uint64_t size, void *pointer) {
    (void)phys; (void)size; (void)pointer;
    abort();
}

int main(void) {
    assert(pool()==0);
    void *guest=NULL;
    assert(place(&guest,2*PAGE,0x33,0,PAGE,KIND_DIRECT,0,0)==0);
    memset(guest,0x5a,2*PAGE);
    uint64_t size=PAGE;
    const unsigned char *p=runtime_memory_backing_span((uintptr_t)guest,&size);
    assert(p && p!=(unsigned char *)guest && size==PAGE);
    assert(p[0]==0x5a && p[PAGE-1]==0x5a);
    puts("PASS memory backing: complete direct page is an alias, not a copy");

    void *alias=NULL;
    assert(place(&alias,2*PAGE,0x33,0,PAGE,KIND_DIRECT,0,0)==0);
    ((unsigned char *)alias)[17]=0x71;
    assert(p[17]==0x71);
    // The borrowed view must not fault while the GPU owns the guest page.
    runtime_memory_trap((uintptr_t)guest,PAGE,0x10,1);
    size=PAGE;
    assert(runtime_memory_backing_span((uintptr_t)guest,&size)==p && p[17]==0x71);
    unsigned char copy[PAGE];
    runtime_memory_read_backing((uintptr_t)guest,copy,PAGE);
    assert(memcmp(copy,p,PAGE)==0);
    runtime_memory_trap((uintptr_t)guest,PAGE,0x10,0);
    puts("PASS memory backing: aliases and GPU read traps preserve hash bytes");

    size=UINT64_MAX;
    assert(runtime_memory_backing_span((uintptr_t)guest+PAGE+7,&size)==p+PAGE+7);
    assert(size==PAGE-7);
    // Even adjacent guest VMAs need not have adjacent physical backing.
    void *next=(unsigned char *)guest+PAGE;
    assert(place(&next,PAGE,0x33,MAP_FIXED_FLAG,PAGE,KIND_DIRECT,0,4*PAGE)==0);
    memset(next,0x39,PAGE);
    size=2*PAGE;
    assert(runtime_memory_backing_span((uintptr_t)guest,&size)==p && size==PAGE);
    runtime_memory_read_backing((uintptr_t)guest,copy,PAGE);
    assert(memcmp(copy,p,PAGE)==0);
    size=PAGE;
    const unsigned char *q=runtime_memory_backing_span((uintptr_t)next,&size);
    assert(q && q!=p+PAGE && q[0]==0x39);
    assert(direct_unmap(next,PAGE)==0);
    size=PAGE;
    assert(!runtime_memory_backing_span((uintptr_t)next,&size) && size==0);
    puts("PASS memory backing: split, remapped and unmapped VMAs are bounded");

    void *reserved=NULL, *flex=NULL;
    assert(place(&reserved,PAGE,0,0,PAGE,KIND_RESERVED,0,0)==0);
    size=PAGE;
    assert(!runtime_memory_backing_span((uintptr_t)reserved,&size) && size==0);
    size=PAGE;
    assert(!runtime_memory_backing_span(UINTPTR_MAX,&size) && size==0);
    assert(!runtime_memory_backing_span((uintptr_t)guest,NULL));
    size=0;
    assert(!runtime_memory_backing_span((uintptr_t)guest,&size) && size==0);
    assert(place(&flex,PAGE,0x33,0,PAGE,KIND_FLEXIBLE,0,POOL_SIZE)==0);
    memset(flex,0x62,PAGE);
    size=PAGE;
    q=runtime_memory_backing_span((uintptr_t)flex,&size);
    assert(q && size==PAGE && q[PAGE-1]==0x62);
    // Private direct backing cannot be lent; flexible memory still has its shared view.
    private_import=unused_import;
    size=PAGE;
    assert(!runtime_memory_backing_span((uintptr_t)guest,&size) && size==0);
    size=PAGE;
    assert(runtime_memory_backing_span((uintptr_t)flex,&size)==q && size==PAGE);
    private_import=NULL;
    puts("PASS memory backing: flexible memory, reservations, invalid input and private fallback");
    return 0;
}
