// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Dreams frame rate: patches to the loaded eboot image, applied by bbgpu_patch_image() before the
// game runs (the image is still writable then; src/probe.c protects it afterwards).
// Background, evidence and measurements: docs/dreams/FPS.md and notes/high-fps-research.md.
//
// Dreams picks its own presentation rate every frame:
//   - site 0 (v2.52 eboot +0xe34d73, v2.64 +0xc2075e): `movzx eax, byte [flag30]` selects the
//     sceVideoOutSetFlipRate argument: 1 (every second vblank, 30 fps) or 0 (every vblank);
//   - site 1 (v2.52 +0xe72d81, v2.64 +0xc502d1): `cmp byte [flag30], 0` then `sete al` indexes
//     the table {1/30, 1/60}: the smallest frame time of the game's render clock (the clock
//     itself is measured wall time). Faster frames than this floor make render-side motion fast.
// The Dreams fork's SHADPS4_DREAMS_FORCE_60HZ_RENDERING (ref/shadps4 src/core/module.cpp,
// ApplyDreamsEbootPatches) patches exactly these two sites (CUSA04301 v2.64 offsets, exact
// bytes). Here the same two patches are found by signature (opcodes fixed, rip displacements
// free, both sites must name the same flag and the table must hold 1/30 and 1/60), so they work
// on v2.52 and v2.64 alike. Pair or nothing, as in the fork: a half-applied pair would mix the
// 30 and 60 Hz paths. Gameplay (the game's 30 Hz update worker) is not touched.
//
// Switches (environment, read once at load):
//   DREAMS_60HZ=1      the fork's pair: flip rate 0, render-clock floor 1/60 (60 fps).
//   DREAMS_FPS=<n>     n > 60 (up to 240): the pair plus a render-clock floor of 1/n. Needs a
//                      display that flips faster than 60 Hz: run-dreams.sh then also sets the
//                      vblank frequency to n. n = 60 is DREAMS_60HZ=1.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

namespace {

using u8 = std::uint8_t;
using u64 = std::uint64_t;

/// A byte pattern: -1 matches any byte.
using Pattern = std::vector<int>;

/// The eboot's code: Dreams' text segment starts at image offset 0; libc.prx is linked at
/// 0xb040000 (scripts/link_modules.py), so the eboot's code and read-only data end before that.
constexpr u64 EbootLimit = 0xb040000;

std::vector<u64> FindAll(const u8* image, u64 size, const Pattern& pattern) {
    std::vector<u64> hits;
    const u64 n = pattern.size();
    if (size < n) {
        return hits;
    }
    const u8 first = static_cast<u8>(pattern[0]);
    for (u64 at = 0; at + n <= size; ++at) {
        const void* p = std::memchr(image + at, first, size - n + 1 - at);
        if (!p) {
            break;
        }
        at = static_cast<const u8*>(p) - image;
        bool match = true;
        for (u64 i = 1; i < n && match; ++i) {
            match = pattern[i] < 0 || image[at + i] == static_cast<u8>(pattern[i]);
        }
        if (match) {
            hits.push_back(at);
        }
    }
    return hits;
}

/// Target of a rip-relative operand: disp32 at `disp_at`, instruction ends at `insn_end`.
u64 RipTarget(const u8* image, u64 disp_at, u64 insn_end) {
    std::int32_t disp;
    std::memcpy(&disp, image + disp_at, 4);
    return insn_end + static_cast<std::int64_t>(disp);
}

float ReadFloat(const u8* image, u64 at) {
    float v;
    std::memcpy(&v, image + at, 4);
    return v;
}

struct Sites {
    u64 rate = 0;   ///< site 0: movzx eax, byte [flag30] (7 bytes)
    u64 floor = 0;  ///< site 1: cmp byte [flag30], 0 (7 bytes)
    u64 table = 0;  ///< the {1/30, 1/60} floor table site 1's lea names
    u64 flag = 0;
};

std::optional<Sites> FindSites(const u8* image, u64 size) {
    const u64 limit = size < EbootLimit ? size : EbootLimit;
    // Site 0: movzx eax, byte [rip+d]; jmp +9; xor eax, eax; mov byte [rip+d], 0
    const Pattern rate = {0x0f, 0xb6, 0x05, -1, -1, -1, -1, 0xeb, 0x09, 0x31, 0xc0,
                          0xc6, 0x05, -1,   -1, -1, -1, 0x00};
    // Site 1: cmp byte [rip+d], 0; lea rcx, [rip+d]; mov dword [rip+d], imm32
    const Pattern floor = {0x80, 0x3d, -1, -1, -1, -1, 0x00, 0x48, 0x8d, 0x0d,
                           -1,   -1,   -1, -1, 0xc7, 0x05};
    std::vector<Sites> found;
    for (const u64 r : FindAll(image, limit, rate)) {
        const u64 flag = RipTarget(image, r + 3, r + 7);
        if (RipTarget(image, r + 13, r + 18) != flag) {
            continue; // both instructions must name the same flag
        }
        for (const u64 f : FindAll(image, limit, floor)) {
            if (RipTarget(image, f + 2, f + 7) != flag) {
                continue;
            }
            const u64 table = RipTarget(image, f + 10, f + 14);
            if (table + 8 > size || std::fabs(ReadFloat(image, table) - 1.0f / 30.0f) > 1e-6f ||
                std::fabs(ReadFloat(image, table + 4) - 1.0f / 60.0f) > 1e-6f) {
                continue;
            }
            found.push_back({r, f, table, flag});
        }
    }
    if (found.size() != 1) {
        std::printf("vcore: Dreams frame rate: %zu matching site pairs (need exactly 1): not "
                    "applied\n",
                    found.size());
        return std::nullopt;
    }
    return found[0];
}

int TargetRate() {
    if (const char* fps = std::getenv("DREAMS_FPS"); fps && *fps) {
        const int n = std::atoi(fps);
        if (n >= 60 && n <= 240) {
            return n;
        }
        if (n != 0 && n != 30) {
            std::printf("vcore: DREAMS_FPS=%s ignored (60..240)\n", fps);
        }
        return 0;
    }
    if (const char* on = std::getenv("DREAMS_60HZ"); on && on[0] == '1') {
        return 60;
    }
    return 0;
}

} // namespace

namespace Vcore {

void PatchFrameRate(unsigned char* image, std::uint64_t size) {
    const int rate = TargetRate();
    if (rate == 0) {
        return;
    }
    const auto sites = FindSites(image, size);
    if (!sites) {
        return;
    }
    // xor eax, eax: flip rate 0 (every vblank); the following jmp is kept.
    static constexpr u8 RatePatch[7] = {0x31, 0xc0, 0x90, 0x90, 0x90, 0x90, 0x90};
    // cmp eax, eax: ZF set, so the game's sete picks table entry 1 (the 60 Hz floor).
    static constexpr u8 FloorPatch[7] = {0x39, 0xc0, 0x90, 0x90, 0x90, 0x90, 0x90};
    std::memcpy(image + sites->rate, RatePatch, sizeof(RatePatch));
    std::memcpy(image + sites->floor, FloorPatch, sizeof(FloorPatch));
    if (rate > 60) {
        // Only this site reads the table (checked on v2.52): its 1/60 becomes 1/rate.
        const float floor = 1.0f / static_cast<float>(rate);
        std::memcpy(image + sites->table + 4, &floor, 4);
    }
    std::printf("vcore: Dreams frame rate %d: flip rate 0 at eboot +%#llx, render-clock floor 1/%d "
                "at +%#llx (table +%#llx, flag +%#llx); the 30 Hz gameplay worker is unchanged\n",
                rate, static_cast<unsigned long long>(sites->rate), rate,
                static_cast<unsigned long long>(sites->floor),
                static_cast<unsigned long long>(sites->table),
                static_cast<unsigned long long>(sites->flag));
}

} // namespace Vcore
