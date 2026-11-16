// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore: the native runtime's C functions the GPU library calls (defined in port/src/runtime*.c,
// resolved at load time against the dreams-probe executable: -Wl,--allow-shlib-undefined).
// Signatures copied from port/src (bbport 0.7 seam, see notes/bbport-reuse-plan.md §3).
#pragma once
#include <cstdint>

extern "C" {
// runtime_memory/gpu_traps.c, gpu_interface.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
void runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end);
const uint64_t* runtime_memory_generation(void);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap,
                                  RuntimeGpuRange invalidate);
void runtime_memory_set_note_write_hook(RuntimeGpuRange note);
// runtime_kernel.c / runtime_time.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
int32_t* runtime_errno(void);
}
