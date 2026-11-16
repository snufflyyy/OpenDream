// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Runtime<->vcore ABI contract (AUDIT A3): every function vcore/shim/runtime_api.h declares must
// have exactly the signature the runtime side defines, and libvcore resolves these symbols from
// the dreams-probe executable at load time (-Wl,--allow-shlib-undefined), so a silent drift is a
// crash at run time rather than a link error. This test makes the drift a compile error.
//
// Each prototype below is an independent copy of the runtime's own declaration: src/runtime.h
// where it exists, otherwise the function's definition in the runtime .c file (cited per line --
// those have no header prototype to include). Compiling this file together with runtime_api.h
// makes any disagreement between the two an error; nothing is linked or called.
#include <cstdint>
#include <type_traits>

#include "runtime_api.h"

extern "C" {

// src/runtime.h:39
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);

// src/runtime.h:62
int32_t* runtime_errno(void);

// The runtime's GPU callback type: src/runtime_memory.c:225
// (typedef void (*GpuRange)(uintptr_t address, uint64_t size)).
typedef void (*RuntimeGpuRangeCopy)(uintptr_t address, uint64_t size);

// src/runtime_memory/gpu_traps.c:256
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
// src/runtime_memory/gpu_traps.c:275
void runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
// src/runtime_memory/gpu_traps.c:298
const void* runtime_memory_backing_span(uintptr_t address, uint64_t* size);
// src/runtime_memory/gpu_traps.c:337
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
// src/runtime_memory/gpu_traps.c:351
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
// src/runtime_memory/gpu_traps.c:208
int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end);
// src/runtime_memory/gpu_traps.c:335
const uint64_t* runtime_memory_generation(void);
// src/runtime_memory/gpu_traps.c:367
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
// src/runtime_memory/gpu_traps.c:6
void runtime_memory_set_gpu_hooks(RuntimeGpuRangeCopy map, RuntimeGpuRangeCopy unmap,
                                  RuntimeGpuRangeCopy invalidate);
// src/runtime_memory/gpu_traps.c:20
void runtime_memory_set_note_write_hook(RuntimeGpuRangeCopy note);

// src/runtime_kernel.c:116-118 (the one clock guest and GPU timestamps share)
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);

}  // extern "C"

// Shared types: runtime_api.h's RuntimeGpuRange must be the runtime's callback type
// (src/runtime_memory.c:225). It has no named counterpart in src/runtime.h, so the size/shape is
// pinned here instead.
static_assert(std::is_same_v<RuntimeGpuRange, RuntimeGpuRangeCopy>,
              "runtime_api.h RuntimeGpuRange drifted from src/runtime_memory.c GpuRange");
static_assert(std::is_same_v<RuntimeGpuRange, void (*)(uintptr_t, uint64_t)>);

namespace {

// Taking each address makes the compiler compare runtime_api.h's declaration with the prototype
// above; a drift is a compile error. No function is called and nothing is linked.
struct AbiAddresses {
    decltype(&runtime_memory_is_mapped) memory_is_mapped = &runtime_memory_is_mapped;
    decltype(&runtime_errno) errno_fn = &runtime_errno;
    decltype(&runtime_memory_write_backing) memory_write_backing = &runtime_memory_write_backing;
    decltype(&runtime_memory_read_backing) memory_read_backing = &runtime_memory_read_backing;
    decltype(&runtime_memory_backing_span) memory_backing_span = &runtime_memory_backing_span;
    decltype(&runtime_memory_clamp) memory_clamp = &runtime_memory_clamp;
    decltype(&runtime_memory_region) memory_region = &runtime_memory_region;
    decltype(&runtime_memory_vma_info) memory_vma_info = &runtime_memory_vma_info;
    decltype(&runtime_memory_generation) memory_generation = &runtime_memory_generation;
    decltype(&runtime_memory_gpu_protect) memory_gpu_protect = &runtime_memory_gpu_protect;
    decltype(&runtime_memory_set_gpu_hooks) memory_set_gpu_hooks = &runtime_memory_set_gpu_hooks;
    decltype(&runtime_memory_set_note_write_hook) memory_set_note_write_hook =
        &runtime_memory_set_note_write_hook;
    decltype(&runtime_process_time_us) process_time_us = &runtime_process_time_us;
    decltype(&runtime_process_time_counter) process_time_counter = &runtime_process_time_counter;
    decltype(&runtime_tsc_frequency) tsc_frequency = &runtime_tsc_frequency;
};

}  // namespace

int main() {
    AbiAddresses addresses{};
    (void)addresses;
    return 0;
}
