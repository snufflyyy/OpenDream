// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore: the bbport 0.7 runtime<->GPU C API (port/gpu/bbgpu.h) on top of shadPS4's GPU code.
// Structure follows bbport's gpu/shim/bbgpu.cpp (GPL-2.0-or-later); the initialisation order
// follows shadPS4's Emulator::Run (src/emulator.cpp): settings -> ElfInfo -> window ->
// HLE libraries (GnmDriver creates Liverpool + Presenter, VideoOut creates its driver).
//
// The functions of bbgpu.h that only serve bbport's Bloodborne features (camera hooks, trophies UI,
// mouse look, shader recipe tools, overlay menu, ...) are stubs here: see VCORE.md "Stubs".
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "common/dreams_title.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/libs.h"
#include "core/libraries/videoout/video_out.h"
#include "core/signals.h"
#include "gpu/bbgpu.h"
#include "sdl_window.h"

// shadPS4 globals the GPU sources expect (src/emulator.cpp / main.cpp define them upstream).
Frontend::WindowSDL* g_window = nullptr;

namespace Vcore {
extern std::atomic<u32> g_sdk_version;
} // namespace Vcore

// ---- NID table filled by LIB_FUNCTION (shim/include/core/libraries/libs.h) ---------------------
namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::mutex g_symbols_mutex;
std::vector<Symbol> g_symbols;
} // namespace

void LinkSymbolImpl(Core::Loader::SymbolsResolver*, char const* nid, char const* lib, u16,
                    char const* mod, u64 symbol, Core::Loader::SymbolType) {
    std::scoped_lock lock{g_symbols_mutex};
    g_symbols.push_back({nid, lib, mod, symbol});
}

void Core::Loader::SymbolsResolver::AddSymbol(const char* nid, const char* library,
                                              const char* module, SymbolType type, u64 address) {
    LinkSymbolImpl(this, nid, library, 1, module, address, type);
}

// bbport's AvPlayer (port/gpu/platform/avplayer, compiled in place).
namespace BbAvPlayer {
void RegisterLib(Core::Loader::SymbolsResolver* sym);
}

// ElfInfo's fields are private to its friend Core::Emulator (the emulator fills them while
// loading the game). vcore defines that class only to fill the same fields from BbGpuConfig.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const BbGpuConfig& config, const std::string& app_ver,
                            u32 firmware_ver, const std::filesystem::path& game_folder) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.app_ver = app_ver;
        info.firmware_ver = firmware_ver & 0xFFF00000;
        info.raw_firmware_ver = firmware_ver;
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
        info.game_folder = game_folder;
    }
};
} // namespace Core

namespace {

// Minimal param.sfo reader (same format the loader's sfo_value() reads): string or u32 by key.
struct SfoValue {
    std::string text;
    u32 number = 0;
    bool found = false;
};
SfoValue ReadSfo(const std::filesystem::path& path, const char* key) {
    SfoValue out;
    std::ifstream file{path, std::ios::binary};
    std::vector<u8> data{std::istreambuf_iterator<char>(file), {}};
    auto rd32 = [&](size_t at) -> u32 {
        return at + 4 <= data.size() ? u32(data[at]) | u32(data[at + 1]) << 8 |
                                           u32(data[at + 2]) << 16 | u32(data[at + 3]) << 24
                                     : 0;
    };
    if (data.size() < 20 || std::memcmp(data.data(), "\0PSF", 4) != 0) {
        return out;
    }
    const u32 key_table = rd32(8), data_table = rd32(12), count = rd32(16);
    for (u32 i = 0; i < count; ++i) {
        const size_t entry = 20 + size_t(i) * 16;
        if (entry + 16 > data.size()) {
            break;
        }
        const u32 key_offset = u32(data[entry]) | u32(data[entry + 1]) << 8;
        const u32 fmt = u32(data[entry + 2]) | u32(data[entry + 3]) << 8;
        const u32 length = rd32(entry + 4), offset = rd32(entry + 12);
        const size_t key_at = key_table + key_offset;
        if (key_at >= data.size() ||
            std::strncmp(reinterpret_cast<const char*>(&data[key_at]), key,
                         data.size() - key_at) != 0) {
            continue;
        }
        const size_t value_at = data_table + offset;
        if (value_at + length > data.size()) {
            return out;
        }
        out.found = true;
        if (fmt == 0x0404) {
            out.number = rd32(value_at);
        } else {
            out.text.assign(reinterpret_cast<const char*>(&data[value_at]), length);
            out.text.resize(std::strlen(out.text.c_str()));
        }
        return out;
    }
    return out;
}

// SDL video must be driven from one thread: the window lives on its own host thread.
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready = false;

} // namespace

extern "C" int bbgpu_init(const BbGpuConfig* config) {
    const std::string serial = config->serial ? config->serial : "UNKNOWN";
    // Game folder (param.sfo: APP_VER, SYSTEM_VER): run-dreams.sh exports VCORE_GAME_DIR.
    std::filesystem::path game_folder;
    std::string app_ver;
    u32 firmware_ver = 0;
    if (const char* dir = std::getenv("VCORE_GAME_DIR"); dir && *dir) {
        game_folder = dir;
        const auto sfo = game_folder / "sce_sys" / "param.sfo";
        app_ver = ReadSfo(sfo, "APP_VER").text;
        firmware_ver = ReadSfo(sfo, "SYSTEM_VER").number;
        const auto version = ReadSfo(sfo, "VERSION").text;
        std::printf("vcore: game %s version %s (APP_VER %s, SYSTEM_VER %#x) from %s\n",
                    serial.c_str(), version.c_str(), app_ver.c_str(), firmware_ver, dir);
    }
    Vcore::g_sdk_version = config->sdk_version;

    // Settings: shadPS4's own config files under SHADPS4_USER_DIR (path_util.cpp vcore edit):
    // config.json (global, created with defaults) + custom_configs/<serial>.json (the Dreams fork's
    // per-game config, readbacks_mode=2 Precise etc.; run-dreams.sh installs it).
    EmulatorSettings.Load();
    const bool game_config = EmulatorSettings.Load(serial);
    std::printf("vcore: shadPS4 user dir %s; per-game config custom_configs/%s.json %s\n",
                Common::FS::GetUserPath(Common::FS::PathType::UserDir).string().c_str(),
                serial.c_str(), game_config ? "loaded" : "NOT FOUND (defaults)");
    std::printf("vcore: readbacks_mode=%d copy_gpu_buffers=%d direct_memory_access=%d "
                "patch_shaders=%d pipeline_cache=%d vblank=%u present_mode=%s\n",
                static_cast<int>(EmulatorSettings.GetReadbacksMode()),
                EmulatorSettings.IsCopyGpuBuffers(), EmulatorSettings.IsDirectMemoryAccessEnabled(),
                EmulatorSettings.IsPatchShaders(), EmulatorSettings.IsPipelineCacheEnabled(),
                EmulatorSettings.GetVblankFrequency(), EmulatorSettings.GetPresentMode().c_str());

    Core::Emulator::FillElfInfo(*config, app_ver, firmware_ver, game_folder);
    std::printf("vcore: GameSerial()=%s (Dreams gates %s)\n",
                std::string(Common::ElfInfo::Instance().GameSerial()).c_str(),
                Common::IsDreamsSerial(Common::ElfInfo::Instance().GameSerial()) ? "ON" : "off");

    const std::string title = config->title ? config->title : "Dreams";
    const s32 width = config->width, height = config->height;
    std::thread([title, width, height] {
        Common::SetCurrentThreadName("vcore:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        std::fflush(stdout);
        std::_Exit(0);
    }).detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }

    // HLE libraries the GPU side provides (shadPS4 Libraries::InitHLELibs order).
    Libraries::GnmDriver::RegisterLib(nullptr); // creates Liverpool + Vulkan Presenter
    Libraries::VideoOut::RegisterLib(nullptr);
    return 0;
}

extern "C" void bbgpu_register_kernel(void) {
    // Kernel event queues: shared by the guest (sceKernelWaitEqueue, ...) and the GPU side
    // (GnmDriver EOP events, VideoOut flip/vblank events).
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    // AvPlayer (Dreams' intro/tutorial videos): bbport's native implementation (FFmpeg; reads
    // files through runtime_file_translate, calls the guest's allocators on runtime-attached host
    // threads). Upstream shadPS4's AvPlayer assumes the emulator's file system and guest threads.
#ifdef VCORE_BBPORT_AVPLAYER
    BbAvPlayer::RegisterLib(&resolver);
#endif
}

extern "C" uintptr_t bbgpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    std::scoped_lock lock{g_symbols_mutex};
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" unsigned bbgpu_symbol_count(void) {
    std::scoped_lock lock{g_symbols_mutex};
    return unsigned(g_symbols.size());
}

extern "C" int bbgpu_handle_fault(void* ucontext, void* address) {
    // video_core/page_manager.cpp registers its write/read-protection handler here; the loader's
    // SIGSEGV handler asks us first.
    return Core::Signals::Instance()->DispatchAccessViolation(ucontext, address) ? 1 : 0;
}

// ---- IME text entry through the game window ----------------------------------------------------
extern "C" int bbgpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) {
        return 0;
    }
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

// runtime_ime.c: the game closed its IME first (weak there).
extern "C" void bbgpu_text_input_end(void) {
    if (g_window) {
        g_window->EndTextInput();
    }
}

extern "C" int bbgpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) {
        return 2;
    }
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}

extern "C" float bbgpu_frame_aspect(void) {
    return 16.0f / 9.0f;
}

extern "C" void bbgpu_overlay_banner(const char* utf8) {
    if (utf8) {
        std::printf("GPU banner: %s\n", utf8);
    }
}

// ---- bbport (Bloodborne) product features: stubs ----------------------------------------------
extern "C" {
void bbgpu_dump_guest_writes(void*) {}
int bbgpu_overlay_captures_input(void) {
    return 0;
}
// Mouse (window.cpp): the motion and wheel the window collected while it held the mouse.
void bbgpu_mouse_take(double* dx, double* dy, int* wheel_up, int* wheel_down) {
    double x = 0, y = 0;
    int up = 0, down = 0;
    if (g_window) {
        g_window->TakeMouse(x, y, up, down);
    }
    *dx = x;
    *dy = y;
    *wheel_up = up;
    *wheel_down = down;
}
int bbgpu_mouse_captured(void) {
    return g_window && g_window->MouseCaptured() ? 1 : 0;
}
void bbgpu_mouse_capture(void) {
    if (g_window) {
        g_window->RequestMouseCapture();
    }
}
void bbgpu_mouse_look_enable(int enabled) {
    if (g_window) {
        g_window->EnableMouseCapture(enabled != 0);
    }
}
void bbgpu_add_event_filter(BbGpuEventFilter) {}
int bbgpu_world_active(void) {
    return 0;
}
unsigned long long bbgpu_world_us(void) {
    return 0;
}
#ifndef VCORE_BBPORT_AVPLAYER // otherwise defined by port/gpu/platform/avplayer/avplayer.cpp
int bbgpu_video_active(void) {
    return 0;
}
int bbgpu_video_skip(void) {
    return 0;
}
#endif
void bbgpu_add_pointer_query(BbGpuPointerQuery) {}
int bbgpu_pointer_wanted(void) {
    return 0;
}
void bbgpu_add_pad_filter(BbGpuPadFilter) {}
void bbgpu_filter_pad(uint32_t*, const uint8_t*) {}
void bbgpu_trophy_setup(const char*, const char*) {}
int bbgpu_trophy_unlock(int) {
    return -1;
}
int bbgpu_camera_look(float, float) {
    return 0;
}
int bbgpu_rebuild_shader_caches(void) {
    return 0;
}
int bbgpu_rebuild_shader_cache(const char*) {
    std::fprintf(stderr, "vcore: --rebuild-shader-cache is a bbport tool (not in vcore)\n");
    return 1;
}
int bbgpu_predict_shaders_test(const char*, const char*, const char*, const char*, int) {
    return 1;
}
int bbgpu_merge_shader_metas(const char*, const char*, const char*, const char*, const char*,
                             const char*, int) {
    return 1;
}
int bbgpu_meta_reconstruct_test(const char*, const char*, const char*) {
    return 1;
}
int bbgpu_merge_shader_packs(const char*, const char*, const char*, const char*, const char*,
                             const char*, int) {
    return 1;
}
int bbgpu_export_shader_recipes(const char*, const char*, const char*) {
    return 1;
}
int bbgpu_import_shader_recipes(const char*, const char*, const char*, unsigned long long,
                                unsigned long long) {
    return 1;
}
int bbgpu_free_camera(void) {
    return 0;
}
uint64_t bbgpu_camera_state(void) {
    return 0;
}
void bbgpu_patch_image(unsigned char*, uint64_t) {}
// Not in bbgpu.h but called by port/src/runtime_memory/kernel_api.c (BB_ZERO_SCAN diagnostics).
int bbgpu_gpu_touched(uint64_t) {
    return 0;
}
void bbgpu_straddle_report(void) {}
} // extern "C"
