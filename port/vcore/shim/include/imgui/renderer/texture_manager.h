// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore shim: replaces shadPS4 src/imgui/renderer/texture_manager.h (no Dear ImGui in vcore).
#pragma once

#include "imgui/imgui_texture.h"

namespace ImGui::Core::TextureManager {
inline void Submit() {}
} // namespace ImGui::Core::TextureManager
