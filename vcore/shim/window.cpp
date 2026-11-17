// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore: the game window (shim/include/sdl_window.h). Reduced from bbport's gpu/shim/window.cpp
// (GPL-2.0-or-later): no overlay menu or cursor art. Gamepads and the keyboard are sampled by the
// runtime (port/src/runtime_pad); their SDL events are pumped here along with the window's.
// Mouse (Dreams port, docs/dreams/INPUT.md): a click in the window takes the mouse (relative mode,
// pointer hidden and locked; that click is not a game button), F1 lets it go and takes it back,
// losing focus lets it go. The runtime reads the motion and wheel through bbgpu_mouse_take
// (bbgpu.cpp) and turns them into the imp's motion (runtime_pad/sensors.c).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // DualShock 4 / DualSense full reports, gamepad kept while unfocused (runtime_pad reads them).
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1", SDL_HINT_DEFAULT);
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1", SDL_HINT_DEFAULT);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN,
                           fullscreen && fullscreen[0] == '1');
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection =
            SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(
            SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection =
            SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface =
            SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    std::printf("vcore: window %dx%d on %s\n", w, h, driver);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::EndTextInput() {
    std::scoped_lock lock{text_mutex};
    text_requested = false;
    text_end_requested = true;
}

void WindowSDL::TakeMouse(double& dx, double& dy, int& up, int& down) {
    std::scoped_lock lock{mouse_mutex};
    dx = mouse_dx;
    dy = mouse_dy;
    up = wheel_up;
    down = wheel_down;
    mouse_dx = mouse_dy = 0;
    wheel_up = wheel_down = 0;
}

// Window thread only. Motion from before is dropped, so taking the mouse does not move the imp.
void WindowSDL::CaptureMouse(bool capture) {
    if (capture == mouse_captured.load(std::memory_order_relaxed)) {
        return;
    }
    if (!SDL_SetWindowRelativeMouseMode(window, capture) && capture) {
        std::printf("Mouse: relative mouse mode failed: %s\n", SDL_GetError());
        return;
    }
    {
        std::scoped_lock lock{mouse_mutex};
        mouse_dx = mouse_dy = 0;
        wheel_up = wheel_down = 0;
    }
    mouse_captured.store(capture, std::memory_order_relaxed);
    if (capture) {
        capture_settle_ms = SDL_GetTicks() + 100; // the pointer's jump into relative mode
    }
    static bool told;
    if (capture && !told) {
        told = true;
        std::printf("Mouse: taken by the game window (F1 releases it, a click or F1 takes it again)\n");
    }
}

void WindowSDL::UpdateTextTitle() {
    const std::string title =
        text_active ? base_title + " - " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                    : base_title;
    SDL_SetWindowTitle(window, title.c_str());
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input is toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
        if (text_end_requested) {
            text_end_requested = false;
            if (text_active) {
                text_active = false;
                text_state = 2;
                SDL_StopTextInput(window);
                UpdateTextTitle();
            }
        }
    }
    if (capture_requested.exchange(false, std::memory_order_relaxed)) {
        mouse_user_released = false;
        CaptureMouse(true);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_MOUSE_MOTION && mouse_captured.load(std::memory_order_relaxed) &&
            SDL_GetTicks() >= capture_settle_ms) {
            std::scoped_lock lock{mouse_mutex};
            mouse_dx += event.motion.xrel;
            mouse_dy += event.motion.yrel;
        }
        if (text_active &&
            (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
                    --cut;
                }
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER ||
                       event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // A click takes the mouse (it is not passed on as a game button: the runtime reads the
        // buttons only while the mouse is held).
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !mouse_captured.load(std::memory_order_relaxed)) {
            if (capture_enabled.load(std::memory_order_relaxed)) {
                mouse_user_released = false;
                CaptureMouse(true);
            }
            continue;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F1) {
            mouse_user_released = mouse_captured.load(std::memory_order_relaxed);
            CaptureMouse(!mouse_user_released && capture_enabled.load(std::memory_order_relaxed));
            continue;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL && mouse_captured.load(std::memory_order_relaxed)) {
            const float notches = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            if (notches != 0) {
                std::scoped_lock lock{mouse_mutex};
                (notches > 0 ? wheel_up : wheel_down) += 1;
            }
        }
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            CaptureMouse(false);
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (is_open) {
                std::printf("Window: closed (%s): the game ends\n",
                            event.type == SDL_EVENT_QUIT ? "quit" : "close button");
            }
            is_open = false;
            break;
        default:
            break;
        }
    }
    return is_open;
}

} // namespace Frontend
