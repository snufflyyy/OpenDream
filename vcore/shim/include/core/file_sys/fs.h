// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore shim: replaces shadPS4 src/core/file_sys/fs.h for core/libraries/kernel/equeue.cpp only.
// The native runtime owns guest files (port/src/runtime_file.c); here the handle table only hands
// out kqueue/equeue handle numbers. They start at VCORE_EQUEUE_HANDLE_BASE so they never collide
// with the runtime's file descriptors. Read/Write/Vnode kevents on runtime files therefore report
// EBADF (no game known to need them yet; see port/docs/dreams/VCORE.md).
#pragma once

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include "common/types.h"

namespace Core::FileSys {

constexpr int VCORE_EQUEUE_HANDLE_BASE = 0x40000;

enum class FileType {
    Regular, // standard file
    Directory,
    Device,
    Socket,
    Equeue
};

struct IFileStub {
    u64 Size() const {
        return 0;
    }
    u64 Tell() const {
        return 0;
    }
};

struct File {
    std::atomic_bool is_opened{};
    std::atomic<FileType> type{FileType::Regular};
    std::unique_ptr<IFileStub> handle;
};

class HandleTable {
public:
    int CreateHandle() {
        std::scoped_lock lock{mutex};
        const int handle = next++;
        files[handle] = std::make_unique<File>();
        files[handle]->is_opened = true;
        return handle;
    }
    void DeleteHandle(int d) {
        std::scoped_lock lock{mutex};
        files.erase(d);
    }
    File* GetFile(int d) {
        std::scoped_lock lock{mutex};
        const auto it = files.find(d);
        return it == files.end() ? nullptr : it->second.get();
    }

private:
    std::mutex mutex;
    int next = VCORE_EQUEUE_HANDLE_BASE;
    std::map<int, std::unique_ptr<File>> files;
};

} // namespace Core::FileSys
