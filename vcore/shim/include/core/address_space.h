// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore shim: replaces shadPS4 src/core/address_space.h. The guest address space belongs to the
// native loader/runtime (port/src/runtime_memory/*), not to the GPU library. This class keeps the
// part of upstream's interface that video_core and gnmdriver use and forwards it to the runtime's
// C API (implemented in shim/memory.cpp).
#pragma once

#include <boost/icl/separate_interval_set.hpp>
#include "common/enum.h"
#include "common/types.h"

namespace Core {

enum class MemoryPermission : u32 {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
    ReadWriteExecute = Read | Write | Execute,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

// Same values as upstream (the PS4 user address space layout).
constexpr VAddr SYSTEM_RESERVED_MIN = 0x07FFFC000ULL;
constexpr VAddr SYSTEM_MANAGED_MIN = 0x00000400000ULL;
constexpr VAddr SYSTEM_MANAGED_MAX = 0x07FFFBFFFULL;
constexpr VAddr SYSTEM_RESERVED_MAX = 0xFBFFFFFFFULL;
constexpr VAddr USER_MIN = 0x1000000000ULL;
constexpr VAddr USER_MAX = 0xFBFFFFFFFFULL;

class AddressSpace {
public:
    /// Guest page -> host pointer of its backing (the runtime's memfd view), for hashing guest
    /// memory without tripping write/read traps. Same shape as upstream's
    /// Common::MultiLevelPageTable<Traits>::find(): returns a pointer to the entry, or nullptr.
    /// Read-only, consume immediately: the entry is thread-local and invalidated by the next
    /// lookup; backing contents can change on guest writes/remaps. Split pages use a copy.
    class BackingPageTable {
    public:
        u8* const* find(u64 page) const;
    };

    AddressSpace() = default;

    [[nodiscard]] const BackingPageTable& BackingPages() const noexcept {
        return backing_pages;
    }
    [[nodiscard]] VAddr SystemReservedVirtualBase() const noexcept {
        return SYSTEM_RESERVED_MIN;
    }
    [[nodiscard]] u64 SystemReservedVirtualSize() const noexcept {
        return SYSTEM_RESERVED_MAX - SYSTEM_RESERVED_MIN;
    }
    [[nodiscard]] VAddr UserVirtualBase() const noexcept {
        return USER_MIN;
    }

    /// GPU page tracking (video_core/page_manager.cpp): host page protection of guest memory,
    /// through runtime_memory_gpu_protect (keeps the guest's own protection as the upper bound).
    void Protect(VAddr virtual_addr, u64 size, MemoryPermission perms);

    /// All guest regions (userfaultfd tracking only; vcore keeps signal-based tracking).
    boost::icl::interval_set<VAddr> GetUsableRegions();

private:
    BackingPageTable backing_pages;
};

} // namespace Core
