// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Test-run frame capture for the native port (docs/dreams/CAPTURE.md).
//
// Why: agents working on the port can't watch the game window. With VCORE_CAPTURE_MS=<ms> the
// presenter reads back the game image every <ms> (kernel_glue.cpp TimedScreenshot ->
// vk_presenter.cpp SavePendingScreenshot -> VideoCore::CaptureSink, a `// vcore:` hook with a
// weak default in ref/shadps4 branch `vcore`). This sink takes ownership of the RGBA8 pixels,
// and a background thread:
//   - box-downscales to VCORE_CAPTURE_WIDTH (default 480) wide,
//   - compares with the last *saved* frame (mean absolute difference of luma, 0..255) and saves a
//     PNG only if it changed by more than VCORE_CAPTURE_DIFF (default 1.0) or VCORE_CAPTURE_KEEP_S
//     seconds passed since the last save (default 10), so static menus don't produce piles of
//     identical images,
//   - appends every capture (saved or not) to index.csv: seq,t_ms,saved,diff,file,
//   - prints "CAPTURE #<seq> t=<s> diff=<d> <file>" to stdout for saved frames, so frames can be
//     matched to the surrounding lines in the run log.
// The presenter never waits: if the worker is behind (more than 4 frames queued) the frame is
// dropped and counted as such in index.csv (saved = -1).
//
// Output dir: VCORE_CAPTURE_DIR (run-dreams.sh sets work/captures/<run>), else
// "captures/<timestamp>" in the current directory. Review with tools/capture_sheet.py.

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <stb_image_write.h>

#include "common/types.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Job {
    std::vector<u8> rgba;
    u32 width;
    u32 height;
    u32 seq;
    double t_ms;
};

struct Capture {
    bool enabled = false;
    u32 width_out = 480;
    double diff_threshold = 1.0;
    double keep_s = 10.0;
    std::filesystem::path dir;
    FILE* index = nullptr;
    Clock::time_point start = Clock::now();

    std::mutex lock;
    std::condition_variable cv;
    std::deque<Job> queue;
    std::thread worker;
    u32 seq = 0;

    std::vector<u8> last_luma;
    double last_saved_ms = -1e30;
    u32 saved = 0;

    Capture() {
        const char* ms = std::getenv("VCORE_CAPTURE_MS");
        if (!ms || std::atof(ms) <= 0.0) {
            return;
        }
        if (const char* w = std::getenv("VCORE_CAPTURE_WIDTH"); w && std::atoi(w) >= 64) {
            width_out = static_cast<u32>(std::atoi(w));
        }
        if (const char* d = std::getenv("VCORE_CAPTURE_DIFF")) {
            diff_threshold = std::atof(d);
        }
        if (const char* k = std::getenv("VCORE_CAPTURE_KEEP_S")) {
            keep_s = std::atof(k);
        }
        if (const char* d = std::getenv("VCORE_CAPTURE_DIR"); d && *d) {
            dir = d;
        } else {
            char stamp[32];
            const std::time_t now = std::time(nullptr);
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
            dir = std::filesystem::path("captures") / stamp;
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        index = std::fopen((dir / "index.csv").string().c_str(), "w");
        if (!index) {
            std::fprintf(stderr, "vcore capture: cannot write %s; capture off\n", dir.string().c_str());
            return;
        }
        std::fprintf(index, "seq,t_ms,saved,diff,file\n");
        std::fflush(index);
        enabled = true;
        worker = std::thread([this] { Run(); });
        worker.detach(); // process exit ends it; index.csv is flushed per line
        std::printf("vcore capture: every %s ms, %u px wide, diff > %.2f or every %.0f s -> %s\n", ms,
                    width_out, diff_threshold, keep_s, dir.string().c_str());
        std::fflush(stdout);
    }

    bool Take(std::vector<u8>& rgba, u32 width, u32 height) {
        if (!enabled) {
            return false;
        }
        const double t_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        std::unique_lock guard{lock};
        const u32 n = ++seq;
        if (queue.size() >= 4) {
            std::fprintf(index, "%u,%.0f,-1,,(dropped: writer behind)\n", n, t_ms);
            std::fflush(index);
            return true;
        }
        queue.push_back(Job{std::move(rgba), width, height, n, t_ms});
        guard.unlock();
        cv.notify_one();
        return true;
    }

    void Run() {
        for (;;) {
            Job job;
            {
                std::unique_lock guard{lock};
                cv.wait(guard, [this] { return !queue.empty(); });
                job = std::move(queue.front());
                queue.pop_front();
            }
            Process(job);
        }
    }

    void Process(const Job& job) {
        // Box downscale by an integer factor that keeps the width >= width_out (1920 -> 480: 4x4).
        const u32 factor = std::max<u32>(1, job.width / width_out);
        const u32 w = job.width / factor;
        const u32 h = job.height / factor;
        std::vector<u8> small(static_cast<size_t>(w) * h * 3);
        std::vector<u8> luma(static_cast<size_t>(w) * h);
        const u32 area = factor * factor;
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                u32 sum[3] = {0, 0, 0};
                for (u32 dy = 0; dy < factor; ++dy) {
                    const u8* p = &job.rgba[(static_cast<size_t>(y * factor + dy) * job.width +
                                             x * factor) *
                                            4];
                    for (u32 dx = 0; dx < factor; ++dx, p += 4) {
                        sum[0] += p[0];
                        sum[1] += p[1];
                        sum[2] += p[2];
                    }
                }
                u8* o = &small[(static_cast<size_t>(y) * w + x) * 3];
                o[0] = static_cast<u8>(sum[0] / area);
                o[1] = static_cast<u8>(sum[1] / area);
                o[2] = static_cast<u8>(sum[2] / area);
                luma[static_cast<size_t>(y) * w + x] =
                    static_cast<u8>((o[0] * 77u + o[1] * 150u + o[2] * 29u) >> 8);
            }
        }
        double diff = 255.0;
        if (last_luma.size() == luma.size()) {
            u64 total = 0;
            for (size_t i = 0; i < luma.size(); ++i) {
                total += static_cast<u64>(std::abs(int(luma[i]) - int(last_luma[i])));
            }
            diff = static_cast<double>(total) / static_cast<double>(luma.size());
        }
        const bool save =
            diff > diff_threshold || (job.t_ms - last_saved_ms) >= keep_s * 1000.0;
        std::string name;
        if (save) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "f%05u_t%07.1f.png", job.seq, job.t_ms / 1000.0);
            name = buf;
            stbi_write_png((dir / name).string().c_str(), static_cast<int>(w), static_cast<int>(h), 3,
                           small.data(), static_cast<int>(w * 3));
            last_luma = std::move(luma);
            last_saved_ms = job.t_ms;
            ++saved;
            std::printf("CAPTURE #%u t=%.1fs diff=%.1f %s\n", job.seq, job.t_ms / 1000.0, diff,
                        name.c_str());
            std::fflush(stdout);
        }
        std::fprintf(index, "%u,%.0f,%d,%.2f,%s\n", job.seq, job.t_ms, save ? 1 : 0, diff,
                     name.c_str());
        std::fflush(index);
    }
};

Capture& Instance() {
    static Capture capture;
    return capture;
}
// Construct at library load so t_ms counts from process start (aligns with the whole run log).
[[maybe_unused]] const bool g_capture_constructed = (Instance(), true);

} // namespace

namespace VideoCore {
// Strong definition of the weak hook in vk_presenter.cpp (ref/shadps4 branch `vcore`).
bool CaptureSink(std::vector<u8>& rgba, u32 width, u32 height) {
    return Instance().Take(rgba, width, height);
}
} // namespace VideoCore
