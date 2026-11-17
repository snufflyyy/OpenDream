// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore: Core::MemoryManager / Core::AddressSpace (shim/include/core/{memory,address_space}.h)
// over the native runtime's guest-memory C API. Modelled on bbport's gpu/shim/bbgpu.cpp
// (GPL-2.0-or-later); semantics follow upstream shadPS4 src/core/memory.cpp where they differ.
#include <algorithm>
#include <cstring>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/memory.h"
#include "runtime_api.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace Core {

namespace {

// The mapped region of the last lookup, per thread, valid for the runtime's mapping-table
// generation it was read at (bbport: the GPU thread checks thousands of ranges per frame).
struct RegionCache {
    u64 generation = 1; // odd: invalid
    uintptr_t start = 0, end = 0;
};
thread_local RegionCache region_cache;

bool CachedMapped(VAddr address, u64 size) {
    static const uint64_t* const generation_ptr = runtime_memory_generation();
    const u64 generation = __atomic_load_n(generation_ptr, __ATOMIC_ACQUIRE);
    if (generation & 1) {
        return runtime_memory_is_mapped(address, size) != 0;
    }
    auto& cache = region_cache;
    if (cache.generation == generation && address >= cache.start && address + size <= cache.end) {
        return true;
    }
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped) {
        return false;
    }
    cache = {generation, start, end};
    if (address + size <= end) {
        return true;
    }
    // Spans several adjacent mappings.
    return runtime_memory_clamp(address, size) == size;
}

Vulkan::Rasterizer* g_rasterizer = nullptr;

} // namespace

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    g_rasterizer = rasterizer_;
    // Upstream MemoryManager calls RegisterMemory (page tracking pages) + MapMemory (GPU-visible
    // range set) on every guest mapping, UnmapMemory on unmapping. The runtime replays the
    // mappings that already exist when the hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            if (!g_rasterizer) {
                return;
            }
            g_rasterizer->RegisterMemory(address, size);
            if (address + size < (u64{1} << 40)) {
                g_rasterizer->MapMemory(address, size);
            }
        },
        [](uintptr_t address, uint64_t size) {
            if (g_rasterizer) {
                g_rasterizer->UnmapMemory(address, size);
            }
        },
        [](uintptr_t address, uint64_t size) {
            // Host-side writes the GPU cannot trap (file reads into guest memory): same as the
            // emulator's file write path (Rasterizer::InvalidateMemory).
            if (g_rasterizer) {
                g_rasterizer->InvalidateMemory(address, size);
            }
        });
}

void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) {
        rasterizer->InvalidateMemory(address, size);
    }
}

bool MemoryManager::IsValidMapping(VAddr virtual_addr, u64 size) {
    // Upstream checks the range against its VMA map (which spans the whole reserved space). The
    // native guest space is sparse, so "valid" here means really mapped: callers then dereference
    // the address directly.
    return CachedMapped(virtual_addr, std::max<u64>(size, 1));
}

u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    // Upstream: only ranges of 1 GiB or more are clamped (buffers are filled through
    // CopySparseMemory, which reads gaps as zeros).
    static constexpr u64 MinSizeToClamp = u64{1} << 30;
    if (size < MinSizeToClamp) {
        return size;
    }
    const u64 clamped = runtime_memory_clamp(virtual_addr, size);
    ASSERT_MSG(clamped != 0, "Attempted to access invalid address {:#x}", virtual_addr);
    return clamped;
}

void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // Upstream reads the backing (physical) view page by page, zeros for unbacked pages: the copy
    // never faults on GPU-protected pages. runtime_memory_read_backing does exactly that.
    runtime_memory_read_backing(source, dest, size);
}

bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    const VAddr virtual_addr = reinterpret_cast<VAddr>(address);
    ASSERT_MSG(IsValidMapping(virtual_addr, size), "Attempted to access invalid address {:#x}",
               virtual_addr);
    return runtime_memory_write_backing(virtual_addr, data, size) != 0;
}

bool MemoryManager::TryReadBacking(VAddr address, void* dest, u64 size) {
    if (!dest || !size || !CachedMapped(address, size)) {
        return false;
    }
    int prot = 0, type = 0;
    uintptr_t end = 0;
    if (!runtime_memory_vma_info(address, &prot, &type, &end) || !(prot & 1)) {
        return false; // upstream: needs CpuRead
    }
    runtime_memory_read_backing(address, dest, size);
    return true;
}

bool MemoryManager::TryStoreBacking(VAddr address, const void* data, u32 size) {
    const u32 alignment = size <= 8 ? size : 4;
    if (!data || size < 4 || size > 4096 || size % 4 || address % alignment != 0) {
        return false;
    }
    int prot = 0, type = 0;
    uintptr_t end = 0;
    if (!runtime_memory_vma_info(address, &prot, &type, &end) || !(prot & 2) ||
        address + size > end) {
        return false; // upstream: needs CpuWrite, one mapping
    }
    return runtime_memory_write_backing(address, data, size) != 0;
}

// ---- AddressSpace ----------------------------------------------------------------------------

void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}

boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(SYSTEM_MANAGED_MIN, USER_MAX);
    return set;
}

} // namespace Core
