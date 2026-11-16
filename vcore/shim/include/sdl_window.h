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
    /// The game closed its text box first: stop typing into it (window thread does it).
    void EndTextInput();

    /// Mouse (bbgpu_mouse_*, runtime_pad): a click in the window takes the mouse (relative mode),
    /// F1 or losing focus lets it go. Motion and wheel notches since the last call, reset.
    void TakeMouse(double& dx, double& dy, int& wheel_up, int& wheel_down);
    bool MouseCaptured() const {
        return mouse_captured.load(std::memory_order_relaxed);
    }
    /// Asks the window thread to take the mouse (as a click does).
    void RequestMouseCapture() {
        capture_requested.store(true, std::memory_order_relaxed);
    }
    /// false: clicks never take the mouse (bbport.ini mouse_look=0).
    void EnableMouseCapture(bool enabled) {
        capture_enabled.store(enabled, std::memory_order_relaxed);
    }

private:
    void UpdateTextTitle();
    void CaptureMouse(bool capture);

    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::mutex text_mutex;
    bool text_requested{}, text_active{}, text_end_requested{};
    int text_state{};
    std::string text, text_prompt, base_title;
    SDL_Window* window{};
    WindowSystemInfo window_info{};
    std::mutex mouse_mutex;
    double mouse_dx{}, mouse_dy{};
    int wheel_up{}, wheel_down{};
    std::atomic<bool> mouse_captured{false}, capture_requested{false}, capture_enabled{true};
    bool mouse_user_released{};
    u64 capture_settle_ms{};
};

} // namespace Frontend
