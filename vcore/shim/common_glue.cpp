// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore: replacements for shadPS4 src/common/{assert,thread}.cpp and src/core/signals.cpp, whose
// upstream versions reach into the emulator (Core::Emulator, guest pthreads, the process-wide
// signal handlers). The native loader owns the signal handlers: its SIGSEGV handler calls
// bbgpu_handle_fault(), which dispatches to the handlers registered here.
// assert_fail_impl follows bbport's gpu/common/assert.cpp (GPL-2.0-or-later): exit code 23.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <execinfo.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/signals.h"

// ---- common/assert.cpp -----------------------------------------------------------------------

void assert_fail_impl() {
    std::fflush(stdout);
    void* frames[48];
    const int depth = backtrace(frames, 48);
    std::fputs("Assertion call stack:\n", stderr);
    backtrace_symbols_fd(frames, depth, STDERR_FILENO);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::fflush(stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}

// ---- common/thread.cpp (Linux parts, without guest-thread names) ------------------------------

namespace Common {

void SetCurrentThreadRealtime(std::chrono::nanoseconds) {
    // Upstream asks the macOS scheduler for a time constraint; nothing on Linux.
}

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    pthread_t this_thread = pthread_self();
    const auto scheduling_type = SCHED_OTHER;
    const s32 max_prio = sched_get_priority_max(scheduling_type);
    const s32 min_prio = sched_get_priority_min(scheduling_type);
    const u32 level = std::max(static_cast<u32>(new_priority) + 1, 4U);
    struct sched_param params;
    if (max_prio > min_prio) {
        params.sched_priority = min_prio + ((max_prio - min_prio) * level) / 4;
    } else {
        params.sched_priority = min_prio - ((min_prio - max_prio) * level) / 4;
    }
    pthread_setschedparam(this_thread, scheduling_type, &params);
}

void SetCurrentThreadName(const char* name) {
    char truncated[16];
    std::snprintf(truncated, sizeof(truncated), "%s", name);
    pthread_setname_np(pthread_self(), truncated);
}

void SetThreadName(void* thread, const char* name) {
    char truncated[16];
    std::snprintf(truncated, sizeof(truncated), "%s", name);
    pthread_setname_np(*reinterpret_cast<pthread_t*>(thread), truncated);
}

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible) {
    timespec request{
        .tv_sec = static_cast<time_t>(duration.count() / 1'000'000'000),
        .tv_nsec = static_cast<long>(duration.count() % 1'000'000'000),
    };
    timespec remain{};
    int ret;
    while ((ret = nanosleep(&request, &remain)) != 0 && !interruptible) {
        request = remain;
    }
    if (remaining) {
        *remaining = std::chrono::nanoseconds(remain.tv_sec * 1'000'000'000 + remain.tv_nsec);
    }
    return ret == 0;
}

AccurateTimer::AccurateTimer(std::chrono::nanoseconds target_interval)
    : target_interval(target_interval) {}

void AccurateTimer::Start() {
    const auto begin_sleep = std::chrono::high_resolution_clock::now();
    if (total_wait.count() > 0) {
        AccurateSleep(total_wait, nullptr, false);
    }
    start_time = std::chrono::high_resolution_clock::now();
    total_wait -= std::chrono::duration_cast<std::chrono::nanoseconds>(start_time - begin_sleep);
}

void AccurateTimer::End() {
    auto now = std::chrono::high_resolution_clock::now();
    total_wait +=
        target_interval - std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time);
}

std::string GetCurrentThreadName() {
    char name[256];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0) {
        return "<unknown name>";
    }
    return std::string{name};
}

} // namespace Common

// ---- core/signals.cpp: handler registry only ---------------------------------------------------

namespace Core {

SignalDispatch::SignalDispatch() = default;
SignalDispatch::~SignalDispatch() = default;

void SignalDispatch::RemoveHandlers() {}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& entry : access_violation_handlers) {
        if (entry.handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& entry : illegal_instruction_handlers) {
        if (entry.handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core
