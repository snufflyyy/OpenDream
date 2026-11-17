// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore shim: replaces shadPS4 src/imgui/imgui_texture.h. vcore has no Dear ImGui: the presenter
// blits the game frame straight to the swapchain (`// vcore:` edit in vk_presenter.cpp).
#pragma once

#include <filesystem>
#include <vector>
#include "common/types.h"

using ImTextureID = u64;
using ImGuiID = unsigned int;

namespace ImGui {

class RefCountedTexture {
public:
    struct Image {
        ImTextureID im_id;
        u32 width;
        u32 height;
    };
    static RefCountedTexture DecodePngTexture(std::vector<u8>) {
        return {};
    }
    static RefCountedTexture DecodePngFile(std::filesystem::path) {
        return {};
    }
    [[nodiscard]] Image GetTexture() const {
        return {};
    }
    explicit(false) operator bool() const {
        return false;
    }
};

} // namespace ImGui
