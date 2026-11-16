// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore shim: replaces shadPS4 src/core/net/guest_net.h for equeue.cpp. Sockets belong to the
// native runtime (port/src/runtime_net.c; Dreams is offline). Socket kevents report EBADF.
#pragma once

#include <span>
#include "common/types.h"

namespace Core::Net {

namespace Host {
enum class Error : s32 {
    Ok = 0,
    BadF = 9,
};
enum : u32 {
    EvIn = 1u << 0,
    EvOut = 1u << 1,
};
} // namespace Host

struct NetResult {
    s64 value;
    Host::Error error;
};

struct SelectEntry {
    s32 id;
    u32 interest;
    u32 ready;
};

inline NetResult SocketBytesReadable(s32) {
    return {0, Host::Error::BadF};
}
inline NetResult SocketSelect(std::span<SelectEntry>, s64) {
    return {0, Host::Error::BadF};
}

} // namespace Core::Net
