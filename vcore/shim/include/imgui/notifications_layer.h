// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore shim: replaces shadPS4 src/imgui/notifications_layer.h (no ImGui overlay in vcore):
// notifications are printed to stdout instead.
#pragma once

#include <cstdio>
#include <string>
#include <vector>
#include "common/types.h"

namespace shadNotifications {
enum class position {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};
inline void QueueNotification(std::string message, float, position, std::vector<u8> = {}) {
    std::printf("GPU notification: %s\n", message.c_str());
}
} // namespace shadNotifications
