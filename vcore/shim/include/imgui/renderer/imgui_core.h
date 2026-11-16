// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore shim: replaces shadPS4 src/imgui/renderer/imgui_core.h. vcore has no Dear ImGui overlay:
// these are no-ops, and the presenter draws the game frame itself (`// vcore:` edit in
// vk_presenter.cpp). Signatures follow upstream so the callers compile unchanged.
#pragma once

#include "common/types.h"
#include "imgui/imgui_texture.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;
namespace Frontend {
class WindowSDL;
}
namespace Vulkan {
class Instance;
}

namespace ImGui {
inline void* GetCurrentContext() {
    return nullptr;
}
} // namespace ImGui

namespace ImGui::Core {
inline void Initialize(const Vulkan::Instance&, const Frontend::WindowSDL&, u32, vk::Format,
                       const vk::AllocationCallbacks* = nullptr) {}
inline void OnResize() {}
inline void OnSurfaceFormatChange(vk::Format) {}
inline void Shutdown(const vk::Device&) {}
inline bool ProcessEvent(SDL_Event*) {
    return false;
}
inline ImGuiID NewFrame(bool = false) {
    return 0;
}
inline void Render(const vk::CommandBuffer&, const vk::ImageView&, const vk::Extent2D&) {}
inline bool MustKeepDrawing() {
    return false;
}
} // namespace ImGui::Core
