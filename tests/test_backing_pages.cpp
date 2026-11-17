// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Test the production adapter at the runtime C API boundary, without Vulkan or game data.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "core/address_space.h"
#include "runtime_api.h"

static constexpr u64 Page=16384, Base=0x1000000000;
static std::array<u8,Page> bytes{};
static bool mapped=true;
static u64 contiguous=Page;
static unsigned copies=0;
extern "C" const void *runtime_memory_backing_span(uintptr_t address,uint64_t *size) {
    assert(address==Base && *size==Page);
    *size=contiguous;
    return mapped ? bytes.data() : nullptr;
}
extern "C" int runtime_memory_is_mapped(uintptr_t address,uint64_t size) {
    assert(address==Base && size==1);
    return mapped;
}
extern "C" void runtime_memory_read_backing(uintptr_t address,void *out,uint64_t size) {
    assert(address==Base && size==Page);
    ++copies;
    std::memcpy(out,bytes.data(),Page);
}
int main(int argc,char **argv) {
    const bool copy_mode=argc>1 && std::strcmp(argv[1],"copy")==0;
    if (copy_mode) assert(setenv("VCORE_BACKING_COPY","1",1)==0);
    else assert(unsetenv("VCORE_BACKING_COPY")==0);
    for (size_t i=0;i<Page;++i) bytes[i]=u8(i*13);
    Core::AddressSpace::BackingPageTable table;
    auto entry=table.find(Base/ Page);
    assert(entry && *entry && std::memcmp(*entry,bytes.data(),Page)==0);
    assert(copies==(copy_mode ? 1U : 0U));
    if (!copy_mode) assert(*entry==bytes.data());
    // A truncated span must never be exposed as a full page: preserve the copying path.
    contiguous=Page/2;
    entry=table.find(Base/Page);
    assert(entry && *entry && std::memcmp(*entry,bytes.data(),Page)==0);
    assert(copies==(copy_mode ? 2U : 1U));
    mapped=false;
    assert(!table.find(Base/Page));
    assert(copies==(copy_mode ? 2U : 1U));
    std::puts(copy_mode ? "PASS backing pages: copy-mode control" :
                         "PASS backing pages: zero-copy, partial-page fallback and unmapped page");
}
