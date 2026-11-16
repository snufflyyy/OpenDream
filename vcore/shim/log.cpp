// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore: logging backend for shadPS4's LOG_* macros (replaces src/common/logging/log.cpp, which
// needs spdlog and the emulator's log files). Lines go to stdout, interleaved with the native
// runtime's own output, as "gpu <level> (<Class>): <message> [file:line]".
//
// Filter: VCORE_LOG_FILTER, same syntax as shadPS4's log_filter setting, e.g.
//   "*:warning Kernel.Vmm:critical Render.Vulkan:info"
// A class name also matches its sub-classes ("Render:info" covers "Render.Vulkan").
// Default: "*:warning" (the Dreams fork's launcher config uses "*:warning Kernel.Vmm:critical").
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <fmt/format.h>
#include "common/logging/log.h"

namespace Common::Log {

std::array<Level, NUM_LOG_CLASSES> g_class_levels = [] {
    std::array<Level, NUM_LOG_CLASSES> levels{};
    levels.fill(Level::Warning);
    return levels;
}();

namespace {

std::mutex g_log_mutex;

const char* LevelName(Level level) {
    switch (level) {
    case Level::Trace:
        return "trace";
    case Level::Debug:
        return "debug";
    case Level::Info:
        return "info";
    case Level::Warning:
        return "warning";
    case Level::Error:
        return "error";
    case Level::Critical:
        return "critical";
    default:
        return "off";
    }
}

bool ParseLevel(std::string_view text, Level& level) {
    static constexpr std::array<std::pair<std::string_view, Level>, 7> names{{
        {"trace", Level::Trace},
        {"debug", Level::Debug},
        {"info", Level::Info},
        {"warning", Level::Warning},
        {"error", Level::Error},
        {"critical", Level::Critical},
        {"off", Level::Off},
    }};
    for (const auto& [name, value] : names) {
        if (text.size() == name.size() && strncasecmp(text.data(), name.data(), name.size()) == 0) {
            level = value;
            return true;
        }
    }
    return false;
}

struct EnvFilter {
    EnvFilter() {
        const char* filter = std::getenv("VCORE_LOG_FILTER");
        UpdateLogLevels(filter && *filter ? filter : "*:warning");
    }
};

} // namespace

void UpdateLogLevels(std::string_view log_filter) {
    size_t pos = 0;
    while (pos < log_filter.size()) {
        const size_t space = log_filter.find(' ', pos);
        const std::string_view token =
            log_filter.substr(pos, space == std::string_view::npos ? std::string_view::npos
                                                                   : space - pos);
        pos = space == std::string_view::npos ? log_filter.size() : space + 1;
        const size_t colon = token.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const std::string_view name = token.substr(0, colon);
        Level level;
        if (!ParseLevel(token.substr(colon + 1), level)) {
            continue;
        }
        for (int i = 0; i < NUM_LOG_CLASSES; ++i) {
            const std::string_view class_name = NameOf(static_cast<Class>(i));
            const bool match =
                name == "*" || class_name == name ||
                (class_name.size() > name.size() && class_name.starts_with(name) &&
                 class_name[name.size()] == '.');
            if (match) {
                g_class_levels[i] = level;
            }
        }
    }
}

void VLog(Class log_class, Level level, const char* file, int line, const char*,
          fmt::string_view format, fmt::format_args args) {
    std::string message;
    try {
        message = fmt::vformat(format, args);
    } catch (const std::exception& e) {
        message = std::string("<log format error: ") + e.what() + ">";
    }
    const char* base = file ? std::strrchr(file, '/') : nullptr;
    const std::string_view class_name = NameOf(log_class);
    std::scoped_lock lock{g_log_mutex};
    std::fprintf(stdout, "gpu %s (%.*s): %s [%s:%d]\n", LevelName(level),
                 static_cast<int>(class_name.size()), class_name.data(), message.c_str(),
                 base ? base + 1 : (file ? file : "?"), line);
    if (level >= Level::Error) {
        std::fflush(stdout);
    }
}

void Setup(std::string_view) {}
void Switch(std::string_view, bool) {}
void Shutdown() {
    std::fflush(stdout);
}
void Flush() {
    std::fflush(stdout);
}
void Terminate() {
    std::fflush(stdout);
}
void UpdateSinks() {}
void UpdateLogFlushLevel(std::string_view) {}

// Applied after g_class_levels is constructed (same translation unit, declared later).
static EnvFilter g_env_filter;

} // namespace Common::Log
