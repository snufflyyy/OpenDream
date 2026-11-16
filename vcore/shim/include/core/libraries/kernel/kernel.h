// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore shim: replaces shadPS4 src/core/libraries/kernel/kernel.h (which pulls in the emulator's
// linker). Only what the compiled GPU-side libraries use: the guest errno (served by the native
// runtime's runtime_errno(), see shim/kernel_glue.cpp).
#pragma once

#include "common/types.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Kernel {
s32* PS4_SYSV_ABI __Error();
} // namespace Libraries::Kernel
