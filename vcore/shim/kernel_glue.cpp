// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore: the few shadPS4 functions outside the GPU sources that video_core, gnmdriver, videoout
// and equeue call, implemented against the native runtime or as documented no-ops
// (port/docs/dreams/VCORE.md, "Stubs").
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <boost/asio/io_context.hpp>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <thread>
#include "common/rdtsc.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "common/types.h"
#include "core/debug_state.h"
#include "core/file_sys/ifile.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/system/systemservice.h"
#include "runtime_api.h"
#include "video_core/renderdoc.h"

namespace Vcore {
std::atomic<u32> g_sdk_version{0};
std::atomic<bool> g_neo_mode{false};
} // namespace Vcore

// ---- Kernel: clocks, SDK version, errno (one clock shared with the runtime) -------------------
namespace Libraries::Kernel {

u64 PS4_SYSV_ABI sceKernelGetTscFrequency() {
    return runtime_tsc_frequency();
}
u64 PS4_SYSV_ABI sceKernelReadTsc() {
    return Common::FencedRDTSC();
}
u64 PS4_SYSV_ABI sceKernelGetProcessTime() {
    return runtime_process_time_us();
}
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() {
    return runtime_process_time_counter();
}
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() {
    return 1'000'000'000;
}
s32 PS4_SYSV_ABI sceKernelIsNeoMode() {
    return Vcore::g_neo_mode.load(std::memory_order_relaxed) ? 1 : 0; // base PS4 by default
}
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    *ver = static_cast<s32>(Vcore::g_sdk_version.load(std::memory_order_relaxed));
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
s32* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}

// Timer events (sceKernelAddTimerEvent/HRTimer in equeue.cpp) run on boost::asio, serviced by
// one host thread, as in upstream src/core/libraries/kernel/kernel.cpp (KernelServiceThread).
boost::asio::io_context io_context;
namespace {
std::mutex m_asio_req;
std::condition_variable cv_asio_req;
u32 asio_requests = 0;
std::once_flag service_started;
void KernelServiceThread() {
    Common::SetCurrentThreadName("vcore:KernelSvc");
    for (;;) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, [] { return asio_requests != 0; });
            asio_requests = 0;
        }
        io_context.run();
        io_context.restart();
    }
}
} // namespace

void KernelSignalRequest() {
    std::call_once(service_started, [] { std::thread(KernelServiceThread).detach(); });
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

} // namespace Libraries::Kernel

// ---- SystemService: the splash image is the runtime's business ---------------------------------
namespace Libraries::SystemService {
bool IsSplashVisible() {
    // Only used by the ImGui presenter path that vcore replaces (vk_presenter.cpp vcore edit).
    return false;
}
} // namespace Libraries::SystemService

// ---- file_sys/ifile.h helpers used by common/path_util.cpp (game discovery) --------------------
namespace Core::FileSys {
bool IsZArchiveFile(const std::filesystem::path&) {
    return false;
}
std::optional<std::vector<u8>> ReadGameFile(const std::filesystem::path&, std::string_view) {
    return std::nullopt;
}
} // namespace Core::FileSys

// ---- RenderDoc: no in-process capture in the native port ---------------------------------------
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() {
    return false;
}
// Screenshots use shadPS4's own path (vk_presenter.cpp: game-only readback before scaling,
// saved as PNG under <SHADPS4_USER_DIR>/screenshots). VCORE_SCREENSHOT_EVERY=<seconds> requests
// one every N seconds of presented frames (for headless checks of what is on screen).
namespace {
std::atomic<u32> screenshot_game_only_count{0};
std::atomic<u32> screenshot_with_overlays_count{0};
u32 TimedScreenshot() {
    // VCORE_CAPTURE_MS (test capture mode, shim/capture.cpp: small async PNGs) takes precedence
    // over VCORE_SCREENSHOT_EVERY (full-size shadPS4 screenshots).
    static const double every = [] {
        if (const char* ms = std::getenv("VCORE_CAPTURE_MS"); ms && std::atof(ms) > 0.0) {
            return std::atof(ms) / 1000.0;
        }
        const char* env = std::getenv("VCORE_SCREENSHOT_EVERY");
        return env ? std::atof(env) : 0.0;
    }();
    if (every <= 0.0) {
        return 0;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point next = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                       std::chrono::duration<double>(every));
    const auto now = Clock::now();
    if (now < next) {
        return 0;
    }
    next = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(every));
    return 1;
}
} // namespace
void RequestScreenshot(ScreenshotRequest request) {
    if (request == ScreenshotRequest::GameOnly) {
        screenshot_game_only_count.fetch_add(1, std::memory_order_relaxed);
    } else if (request == ScreenshotRequest::WithOverlays) {
        screenshot_with_overlays_count.fetch_add(1, std::memory_order_relaxed);
    }
}
// Frame pacing statistics (docs/dreams/PERF.md). Presenter::PrepareFrame calls
// ConsumeGameOnlyScreenshotRequests once per game flip, so this sees the rhythm at which the game
// hands out frames. Every VCORE_FRAME_STATS seconds (default 5; 0 = off) it prints
//   "vcore frames: 59.9 fps, frame time p50 16.7 / p99 18.1 / max 21.0 ms, stutters 0 (>1.5x p50)"
// so headless agent runs (no MangoHud in the sandbox) can measure fps and pacing before/after.
static void FrameStatsTick() {
    static const double window_s = [] {
        const char* e = std::getenv("VCORE_FRAME_STATS");
        return e ? std::atof(e) : 5.0;
    }();
    if (window_s <= 0.0) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last{}, window_start{};
    static std::vector<float> ms;
    const auto now = Clock::now();
    if (last.time_since_epoch().count() != 0) {
        ms.push_back(std::chrono::duration<float, std::milli>(now - last).count());
    } else {
        window_start = now;
    }
    last = now;
    const double elapsed = std::chrono::duration<double>(now - window_start).count();
    if (elapsed < window_s || ms.size() < 2) {
        return;
    }
    std::vector<float> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    const float p50 = sorted[sorted.size() / 2];
    const float p99 = sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
    const float worst = sorted.back();
    size_t stutters = 0;
    for (const float v : ms) {
        stutters += v > 1.5f * p50 ? 1 : 0;
    }
    std::printf("vcore frames: %.1f fps, frame time p50 %.1f / p99 %.1f / max %.1f ms, stutters %zu "
                "(>1.5x p50) of %zu\n",
                ms.size() / elapsed, p50, p99, worst, stutters, ms.size());
    std::fflush(stdout);
    ms.clear();
    window_start = now;
}
u32 ConsumeGameOnlyScreenshotRequests() {
    FrameStatsTick();
    return screenshot_game_only_count.exchange(0, std::memory_order_acq_rel) + TimedScreenshot();
}
u32 ConsumeWithOverlaysScreenshotRequests() {
    return screenshot_with_overlays_count.exchange(0, std::memory_order_acq_rel);
}
ScreenshotRequests ConsumeScreenshotRequests() {
    return ScreenshotRequests{
        .game_only_count = ConsumeGameOnlyScreenshotRequests(),
        .with_overlays_count = ConsumeWithOverlaysScreenshotRequests(),
    };
}
} // namespace VideoCore

// ---- Devtools debug state: the emulator's debugger UI does not exist here ----------------------
DebugStateType::DebugStateImpl& DebugState =
    *Common::Singleton<DebugStateType::DebugStateImpl>::Instance();
namespace DebugStateType {
void DebugStateImpl::AddCurrentThreadToGuestList() {}
void DebugStateImpl::RemoveCurrentThreadFromGuestList() {}
void DebugStateImpl::PauseGuestThreads() {}
void DebugStateImpl::ResumeGuestThreads() {}
void DebugStateImpl::RequestFrameDump(s32) {}
void DebugStateImpl::PushQueueDump(QueueDump) {}
std::optional<RegDump*> DebugStateImpl::GetRegDump(uintptr_t, uintptr_t) {
    return std::nullopt;
}
void DebugStateImpl::PushRegsDump(uintptr_t, uintptr_t, const AmdGpu::Regs&) {}
void DebugStateImpl::PushRegsDumpCompute(uintptr_t, uintptr_t, const CsState&) {}
void DebugStateImpl::CollectShader(const std::string&, Shader::SwStage, vk::ShaderModule,
                                   std::span<const u32>, std::span<const u32>,
                                   std::span<const u32>, bool) {}
} // namespace DebugStateType
