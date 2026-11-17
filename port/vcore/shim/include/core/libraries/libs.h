// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// vcore shim: replaces shadPS4 src/core/libraries/libs.h. LIB_FUNCTION/LIB_OBJ fill vcore's NID
// table (shim/symbols.cpp), which the native loader queries through bbgpu_resolve(). The game
// calls these functions directly on its own threads (no HOST_CALL TLS switching is needed: guest
// and host share the SysV ABI and glibc's TLS on Linux).
#pragma once

#include "common/types.h"

namespace Core::Loader {
enum class SymbolType {
    Unknown,
    Function,
    Object,
    Tls,
    NoType,
};
// The emulator's resolver is replaced by vcore's NID table (shim/bbgpu.cpp). AddSymbol keeps the
// call shape bbport's own libraries use (vcore compiles bbport's AvPlayer in place).
class SymbolsResolver {
public:
    void AddSymbol(const char* nid, const char* library, const char* module, SymbolType type,
                   u64 address);
};
} // namespace Core::Loader

void LinkSymbolImpl(Core::Loader::SymbolsResolver* sym, char const* nid, char const* lib,
                    u16 libversion, char const* mod, u64 symbol, Core::Loader::SymbolType sym_type);

#define LIB_FUNCTION(nid, lib, libversion, mod, function)                                          \
    LinkSymbolImpl(sym, nid, lib, libversion, mod, reinterpret_cast<u64>(function),                \
                   Core::Loader::SymbolType::Function)

#define LIB_OBJ(nid, lib, libversion, mod, obj)                                                    \
    LinkSymbolImpl(sym, nid, lib, libversion, mod, reinterpret_cast<u64>(obj),                     \
                   Core::Loader::SymbolType::Object)

namespace Libraries {
void InitHLELibs(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries
