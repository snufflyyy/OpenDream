# Texture backing-page lookup and GPU fault bitmap initialization

## Target
`port/src/runtime_memory/gpu_traps.c`: borrowed backing-span API.
`port/vcore/shim/memory.cpp`: move BackingPageTable::find to a small, testable translation unit and avoid copying complete pages.

## Dependents
- shadPS4 `texture_cache.cpp::HashBackingMemory` consumes each returned page immediately. It hashes image contents for refresh/dirty checks at six call sites.
- No shader, descriptor, readback policy, draw ordering or game executable changes.
- The existing copy path remains for split mappings and diagnostic A/B runs.

## Affected stories
No release-plan/story metadata exists. Project goals: smooth high-fps rendering without introducing texture corruption.

## Test coverage
Add CPU-only tests for direct/flexible aliases, mapping boundaries, holes/reservations, GPU-protected guest pages, invalid inputs and private backing rejection. Test the page-table adapter with fake runtime backing APIs. Run the runtime suite and sandboxed high-fps A/B boots; inspect captures.

## Risk: Medium
Texture hashes affect image refresh decisions. Only lend complete pages from the permanent shared backing view; never lend private guest addresses that can be unmapped or protected. The pointer must be consumed immediately, not cached across remaps. As before, this is not an atomic snapshot against concurrent guest writes.

## Additional target: fault bitmap initialization
`ref/shadps4/src/video_core/buffer_cache/fault_manager.cpp` allocates the GPU fault bitmap with undefined contents. Its parser interprets every set bit as a demand for sparse residency; constructor initialization is missing. Add a one-time Vulkan fill and transfer-to-shader dependency before the first use. This is generic, not title-specific. No changes to fault overflow handling, readback policy or sparse residency semantics.

Evidence: two sandboxed runs progressively degrade below 2 FPS. Read-only stacks show `FaultManager::ProcessFaultBuffer -> ObtainBuffer -> EnsureResident -> radv_alloc_memory -> amdgpu_vamgr_find_va`. Explicitly clearing the bitmap is a correctness fix; its connection to the slowdown remains a hypothesis until the sandbox comparison.

Risk: Medium (GPU synchronization). Validate with the existing guarded sandbox repro, compare overflow warnings, FPS and images; keep the transfer barrier. Tests belong in `port/`, not the upstream reference repository.

## Recommended action
Add failing tests first. Preserve the existing fallback and an explicit copy-mode diagnostic switch. Leave unfinished `port-perf`/`ref/shadps4-perf` readback experiments untouched.
