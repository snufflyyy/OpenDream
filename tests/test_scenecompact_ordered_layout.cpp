// SPDX-License-Identifier: GPL-2.0-or-later
// SceneCompact (Dreams 0x3937a849) ordered-count collect/prefix/replay layout.
//
// The exact path collects one OrderedEntryDwords entry per workgroup in each of the 64
// (wave, DOC) streams, then a host matrix scan computes exclusive prefixes in workgroup-creation
// order. Two properties decide whether a dispatch may take that path:
//   1. every stream slice stays inside the reused private-GDS checkpoint range, and
//   2. an entry for workgroup g stays inside its own stream slice (the shader indexes
//      scratch_base + stream * stride + g * OrderedEntryDwords with no bounds check).
// This test pins the constants (including the Ancient Dangers dispatch of 2371 workgroups) and
// models the scan, so a regression to a stride smaller than the accepted group count fails here
// instead of scattering records on the GPU. It never opens a GPU or the game.
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "shader_recompiler/dreams_compat.h"

namespace DC = Shader::DreamsCompat;

namespace {
unsigned failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// Ancient Dangers dispatches 2371 x workgroups (work/logs/run-20261116-155844.log:740).
constexpr std::uint32_t AncientDangersWorkgroups = 2371;

// The dispatch gate must reject a group count whose entries would not fit their stream slice.
bool PrefixFits(std::uint32_t max_workgroups, std::uint32_t groups) {
    const std::uint64_t stride = std::uint64_t{max_workgroups} * DC::OrderedEntryDwords;
    return std::uint64_t{groups} * DC::OrderedEntryDwords <= stride;
}

// Host model of the prefix shader (host_shaders/dreams_ordered_prefix.comp, matrix mode): seed
// from the guest counter, then exclusive prefix over entries in group order with uint32 wrap.
struct ScanResult {
    std::vector<std::uint32_t> prefix;
    std::uint32_t published{};
    std::uint32_t odd_prefixes{};
};

ScanResult ScanStream(std::uint32_t groups, std::uint32_t seed,
                      const std::function<std::uint32_t(std::uint32_t)>& payload) {
    ScanResult result;
    result.prefix.resize(groups);
    std::uint32_t running = seed;
    for (std::uint32_t group = 0; group < groups; ++group) {
        result.prefix[group] = running;
        const std::uint32_t previous = running;
        running += payload(group);
        if (running < previous) {
            ++result.odd_prefixes; // uint32 wrap inside one dispatch would be a layout bug
        }
    }
    result.published = running;
    return result;
}
} // namespace

int main() {
    using DC::GuestGdsDwords;
    using DC::OrderedEntryDwords;
    using DC::SculptOrderedStateBaseDword;
    using DC::SculptOrderedStateDwords;
    using DC::SceneCompactOrderedCounterBaseDword;
    using DC::SceneCompactOrderedCounterDword;
    using DC::SceneCompactOrderedCounterRowStrideDwords;
    using DC::SceneCompactOrderedDocCount;
    using DC::SceneCompactOrderedMaxWorkgroups;
    using DC::SceneCompactOrderedScratchBaseDword;
    using DC::SceneCompactOrderedScratchDwords;
    using DC::SceneCompactOrderedScratchStreamBaseDword;
    using DC::SceneCompactOrderedScratchStreamStrideDwords;
    using DC::SceneCompactOrderedStreamCount;
    using DC::SceneCompactOrderedThreadsPerWorkgroup;
    using DC::SceneCompactOrderedWavesPerWorkgroup;

    // Shape and counter rows: sixteen 64-lane waves, four DOC sites, a shared 4-dword row per wave.
    Check(SceneCompactOrderedThreadsPerWorkgroup == 1024, "1024-thread workgroup");
    Check(SceneCompactOrderedWavesPerWorkgroup == 16, "sixteen waves per workgroup");
    Check(SceneCompactOrderedStreamCount == 64, "64 wave/DOC streams");
    for (std::uint32_t stream = 0; stream < SceneCompactOrderedStreamCount; ++stream) {
        const std::uint32_t expected = SceneCompactOrderedCounterBaseDword +
                                       (stream / SceneCompactOrderedDocCount) *
                                           SceneCompactOrderedCounterRowStrideDwords +
                                       stream % SceneCompactOrderedDocCount;
        Check(SceneCompactOrderedCounterDword(stream) == expected, "counter row mapping");
        Check(SceneCompactOrderedCounterDword(stream) < GuestGdsDwords,
              "counter dword inside guest GDS");
    }

    // Scratch must fit the private checkpoint range it reuses; the cap matches 84aa's group
    // capacity, which already proved that range is enough for a wrapping ordered token.
    Check(SceneCompactOrderedMaxWorkgroups == DC::SculptOrderedMaxWorkgroups,
          "SceneCompact cap matches the 84aa group capacity");
    Check(SceneCompactOrderedScratchBaseDword + SceneCompactOrderedScratchDwords <=
              SculptOrderedStateBaseDword + SculptOrderedStateDwords,
          "scratch inside the 84aa checkpoint range");
    Check(SceneCompactOrderedScratchStreamStrideDwords ==
              SceneCompactOrderedMaxWorkgroups * OrderedEntryDwords,
          "stream stride matches the workgroup cap");

    // Streams are contiguous and disjoint.
    for (std::uint32_t stream = 0; stream + 1 < SceneCompactOrderedStreamCount; ++stream) {
        Check(SceneCompactOrderedScratchStreamBaseDword(stream) +
                      SceneCompactOrderedScratchStreamStrideDwords ==
                  SceneCompactOrderedScratchStreamBaseDword(stream + 1),
              "adjacent streams touch without overlap");
    }
    const std::uint32_t last_end =
        SceneCompactOrderedScratchStreamBaseDword(SceneCompactOrderedStreamCount - 1) +
        SceneCompactOrderedScratchStreamStrideDwords;
    Check(last_end == SceneCompactOrderedScratchBaseDword + SceneCompactOrderedScratchDwords,
          "last stream ends at the scratch end");

    // The 2048-bound layout did not fit the Ancient Dangers dispatch: entry 2048 of stream 0 landed
    // at the start of stream 1. The widened stride keeps it inside stream 0 for every accepted
    // count, up to the cap, which is also the limit of the prefix pass scan (MaxGroupCount 65536).
    Check(!PrefixFits(2048, AncientDangersWorkgroups),
          "the former 2048 stride cannot hold 2371 entries");
    for (const std::uint32_t groups : {AncientDangersWorkgroups, 4096u,
                                       SceneCompactOrderedMaxWorkgroups}) {
        Check(PrefixFits(SceneCompactOrderedMaxWorkgroups, groups),
              "current stride holds an accepted group count");
        Check(groups <= 65536u, "accepted count fits the prefix pass MaxGroupCount");
    }
    Check(!PrefixFits(SceneCompactOrderedMaxWorkgroups, SceneCompactOrderedMaxWorkgroups + 1),
          "one group above the cap no longer fits its stream");

    // Model the scan for the real dispatch size: per-stream payloads stay in workgroup order, the
    // published counter is the seed plus the payload sum, and no uint32 prefix wraps.
    for (std::uint32_t stream = 0; stream < SceneCompactOrderedStreamCount; ++stream) {
        const std::uint32_t seed = stream * 7u; // guest counters are nonzero state before a dispatch
        const auto payload = [stream](std::uint32_t group) {
            return 1u + (group + stream) % 3u;
        };
        const ScanResult scan = ScanStream(AncientDangersWorkgroups, seed, payload);
        std::uint32_t expected = seed;
        const unsigned failures_before = failures;
        for (std::uint32_t group = 0; group < AncientDangersWorkgroups; ++group) {
            Check(scan.prefix[group] == expected, "exclusive prefix in workgroup order");
            expected += payload(group);
        }
        Check(scan.published == expected, "published counter equals seed plus payload sum");
        Check(scan.odd_prefixes == 0, "no uint32 prefix wrap for 2371 workgroups");
        if (failures != failures_before) {
            std::printf("stream %u failed\n", stream);
            break; // one report is enough; the check is identical for every stream
        }
    }

    if (failures != 0) {
        std::printf("%u SceneCompact ordered-layout check(s) failed\n", failures);
        return 1;
    }
    std::printf("SceneCompact ordered layout OK: cap=%u groups, scratch=%u dwords, %u streams, "
                "scan model verified for %u groups\n",
                SceneCompactOrderedMaxWorkgroups, SceneCompactOrderedScratchDwords,
                SceneCompactOrderedStreamCount, AncientDangersWorkgroups);
    return 0;
}
