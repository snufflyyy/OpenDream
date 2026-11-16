// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Runtime-backed page lookup used by shadPS4 texture_cache.cpp::HashBackingMemory.
#include <cstdlib>
#include <string_view>
#include "core/memory.h"
#include "runtime_api.h"

namespace Core {

u8* const* AddressSpace::BackingPageTable::find(u64 page) const {
    // The consumer hashes each page immediately and never writes through this upstream-shaped
    // pointer. Lend complete pages from the permanent backing view instead of copying 16 KiB
    // per lookup. Never use the guest view: GPU read protection can fault/deadlock there.
    static const bool force_copy = [] {
        const char* value = std::getenv("VCORE_BACKING_COPY");
        return value && std::string_view{value} == "1";
    }();
    thread_local u8 copy[GUEST_PAGE_SIZE];
    thread_local u8* entry = nullptr;
    const VAddr address = page << GUEST_PAGE_BITS;
    if (!force_copy) {
        u64 span = GUEST_PAGE_SIZE;
        const void* backing = runtime_memory_backing_span(address, &span);
        if (backing && span == GUEST_PAGE_SIZE) {
            entry = static_cast<u8*>(const_cast<void*>(backing));
            return &entry;
        }
    }
    // A guest page can straddle VMAs with unrelated physical storage (or holes). Preserve
    // the original copy/zero-fill behavior for that case and for private direct allocations.
    if (!runtime_memory_is_mapped(address, 1)) {
        return nullptr;
    }
    runtime_memory_read_backing(address, copy, GUEST_PAGE_SIZE);
    entry = copy;
    return &entry;
}

} // namespace Core
