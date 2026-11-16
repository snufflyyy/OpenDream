// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore shim: replaces shadPS4 src/sdl_window.h. The emulator's window also owns input (pads,
// keyboard/mouse remapping); in the native port input belongs to the runtime
// (port/src/runtime_pad/*, which samples SDL gamepads/keyboard itself), so this window only
// creates the Vulkan surface, pumps events, tracks the size and offers keyboard text entry for
// the IME dialog. Interface follows bbport's gpu/shim/sdl_window.h (GPL-2.0-or-later).
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include "common/types.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 {
    Headless,
    Windows,
    X11,
    Wayland,
    Metal,
};

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();

    s32 GetWidth() const {
        return width.load(std::memory_order_relaxed);
    }
    s32 GetHeight() const {
        return height.load(std::memory_order_relaxed);
    }
    bool IsOpen() const {
        return is_open.load(std::memory_order_relaxed);
    }
    [[nodiscard]] SDL_Window* GetSDLWindow() const {
        return window;
    }
    WindowSystemInfo GetWindowInfo() const {
        return window_info;
    }

    /// Processes pending window events (window thread). Returns false once the user closed it.
    bool PollEvents();

    /// Keyboard text entry for the system IME dialog; typed text shows in the title bar.
    void BeginTextInput(const std::string& initial, const std::string& prompt);
    /// 0 while typing, 1 confirmed (Enter), 2 cancelled (Escape); text is UTF-8.
    int PollTextInput(std::string& text);

private:
    void UpdateTextTitle();

    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::mutex text_mutex;
    bool text_requested{}, text_active{};
    int text_state{};
    std::string text, text_prompt, base_title;
    SDL_Window* window{};
    WindowSystemInfo window_info{};
};

} // namespace Frontend
