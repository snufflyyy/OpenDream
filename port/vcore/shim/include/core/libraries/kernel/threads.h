// SPDX-FileCopyrightText: Copyright 2026 bbport contributors, OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore shim: replaces shadPS4 src/core/libraries/kernel/threads.h (the emulator's guest pthread
// implementation; no compiled shadPS4 file needs it). Copied from bbport 0.7
// gpu/shim/core/libraries/kernel/threads.h for the bbport AvPlayer that vcore compiles in place
// (port/gpu/platform/avplayer): host threads that may call guest code (AvPlayer allocator
// callbacks) get a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <functional>
#include <stop_token>
#include <thread>
#include "common/types.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() {
        Stop();
    }
    void Run(std::function<void(std::stop_token)>&& func) {
        thread = std::jthread([func = std::move(func)](std::stop_token stop) {
            runtime_thread_attach_host("vcore:hle");
            func(stop);
        });
    }
    // A thread may stop its own Thread object (AvPlayer does); it detaches instead of joining.
    void Join() {
        if (!thread.joinable()) {
            return;
        }
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();
        } else {
            thread.join();
        }
    }
    bool Joinable() const {
        return thread.joinable();
    }
    void Stop() {
        if (thread.joinable()) {
            thread.request_stop();
            Join();
        }
    }

private:
    std::jthread thread;
};
} // namespace Libraries::Kernel
