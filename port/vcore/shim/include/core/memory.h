// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore shim: replaces shadPS4 src/core/memory.h. In the native port the guest's memory is mapped
// and owned by the C runtime (port/src/runtime_memory/*). This MemoryManager keeps exactly the
// methods video_core, the shader recompiler, gnmdriver and videoout call, implemented over the
// runtime's C API (shim/memory.cpp). Pattern taken from bbport's gpu/shim/core/memory.h.
#pragma once

#include "common/singleton.h"
#include "common/types.h"
#include "core/address_space.h"

namespace Vulkan {
class Rasterizer;
}

namespace Core {

constexpr u64 DEFAULT_MAPPING_BASE = 0x200000000;
constexpr u64 GUEST_PAGE_BITS = 14;
constexpr u64 GUEST_PAGE_SIZE = 1ULL << GUEST_PAGE_BITS;

class MemoryManager {
public:
    MemoryManager() = default;
    ~MemoryManager() = default;

    /// Also installs the runtime's GPU hooks: map/unmap/invalidate of guest ranges reach the
    /// rasterizer (existing mappings are replayed by the runtime).
    void SetRasterizer(Vulkan::Rasterizer* rasterizer_);
    Vulkan::Rasterizer* GetRasterizer() const {
        return rasterizer;
    }

    AddressSpace& GetAddressSpace() {
        return impl;
    }
    VAddr SystemReservedVirtualBase() noexcept {
        return impl.SystemReservedVirtualBase();
    }

    bool IsValidGpuMapping(VAddr virtual_addr, u64 size) {
        // The PS4's GPU can only handle 40 bit addresses.
        const VAddr max_gpu_address{0x10000000000};
        return virtual_addr + size < max_gpu_address;
    }

    /// Upstream: the range lies inside the VMA map without gaps (any VMA kind). Here: the runtime
    /// has the whole range mapped (size 0: the address itself).
    bool IsValidMapping(VAddr virtual_addr, u64 size = 0);

    u64 ClampRangeSize(VAddr virtual_addr, u64 size);
    void CopySparseMemory(VAddr source, u8* dest, u64 size);
    bool TryWriteBacking(void* address, const void* data, u64 size);
    bool TryReadBacking(VAddr address, void* dest, u64 size);
    bool TryStoreBacking(VAddr address, const void* data, u32 size);
    void InvalidateMemory(VAddr address, u64 size);

private:
    AddressSpace impl;
    Vulkan::Rasterizer* rasterizer{};
};

using Memory = Common::Singleton<MemoryManager>;

} // namespace Core
