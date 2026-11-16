// Unit test of the vcore GPU hang guard on lavapipe (Mesa's CPU Vulkan driver: never touches the GPU).
// docs/dreams/GPU_HANG.md section 4.
//
// 1. Loop guard: runs a real guarded shader from the private shader cache (default
//    0x5c35f8ee: a compute shader whose loop bound is flatbuffer dword 6) with a bound of 2^31-1,
//    i.e. a loop that would take minutes. Expected: it ends after the guard's 1<<20 back edges and
//    its report slot counts one trip per invocation and holds the shader hash. Also: with a normal
//    bound (no trip, no report) and without the specialization constant (guard still ends the loop,
//    no report).
// 2. Indirect clamp: runs host_shaders/vcore_indirect_clamp.comp (the build's generated header) on
//    garbage dispatch and draw arguments and checks the clamped copies and the report area.
//
// Build: g++ -std=c++20 -O1 tools/loop_guard_test.cpp -o out/loop-guard-test -lvulkan
//          -I vcore/build/host_shaders/include
// Run:   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json out/loop-guard-test
//          ../work/vcore-shader-cache/build-*-cache-v4/loop-guard-1048576/CUSA08010/0x000000005c35f8ee_0.spv
#include <vulkan/vulkan.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "video_core/host_shaders/vcore_indirect_clamp_comp.h"

namespace {

// Same values as ref/shadps4 src/shader_recompiler/loop_guard.h.
constexpr uint32_t AddressSpecId = 0x4c47;
constexpr uint32_t SlotCount = 4096;
constexpr uint32_t SlotDwords = 4;
constexpr uint32_t IndirectReportOffset = SlotCount * SlotDwords * 4;
constexpr uint32_t ReportBytes = IndirectReportOffset + 256;
constexpr uint32_t SlotOf(uint64_t h) {
    return uint32_t((h * 0x9e3779b97f4a7c15ULL) >> 52) & (SlotCount - 1);
}

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        VkResult r_ = (x);                                                                         \
        if (r_ != VK_SUCCESS) {                                                                    \
            std::fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__);                    \
            std::exit(2);                                                                          \
        }                                                                                          \
    } while (0)

int failures = 0;
void Expect(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    failures += !ok;
}

struct Ctx {
    VkInstance instance{};
    VkPhysicalDevice phys{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t qfam{};
    VkCommandPool pool{};
};

struct Buf {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* map{};
    VkDeviceAddress address{};
    VkDeviceSize size{};
};

Buf MakeBuffer(Ctx& c, VkDeviceSize size) {
    Buf b{};
    b.size = size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    CHECK(vkCreateBuffer(c.device, &bi, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.device, b.buffer, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(c.phys, &mp);
    uint32_t type = 0;
    for (; type < mp.memoryTypeCount; ++type) {
        const auto want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((req.memoryTypeBits & (1u << type)) && (mp.memoryTypes[type].propertyFlags & want) == want)
            break;
    }
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    CHECK(vkAllocateMemory(c.device, &ai, nullptr, &b.memory));
    CHECK(vkBindBufferMemory(c.device, b.buffer, b.memory, 0));
    CHECK(vkMapMemory(c.device, b.memory, 0, size, 0, &b.map));
    std::memset(b.map, 0, size);
    VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    dai.buffer = b.buffer;
    b.address = vkGetBufferDeviceAddress(c.device, &dai);
    return b;
}

void Init(Ctx& c) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    CHECK(vkCreateInstance(&ici, nullptr, &c.instance));
    uint32_t n = 8;
    VkPhysicalDevice devs[8];
    CHECK(vkEnumeratePhysicalDevices(c.instance, &n, devs));
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            c.phys = devs[i];
            std::printf("device: %s\n", p.deviceName);
        }
    }
    if (!c.phys) {
        std::fprintf(stderr, "no CPU (lavapipe) device: set VK_ICD_FILENAMES to lvp_icd\n");
        std::exit(2);
    }
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(c.phys, &f2); // enable everything the device has
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = 0;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    CHECK(vkCreateDevice(c.phys, &di, nullptr, &c.device));
    vkGetDeviceQueue(c.device, 0, 0, &c.queue);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    CHECK(vkCreateCommandPool(c.device, &pi, nullptr, &c.pool));
}

VkShaderModule Module(Ctx& c, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = bytes;
    si.pCode = code;
    VkShaderModule m;
    CHECK(vkCreateShaderModule(c.device, &si, nullptr, &m));
    return m;
}

// Records `record` into a one-time command buffer, submits and waits. Returns seconds.
template <typename F>
double Submit(Ctx& c, F&& record) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = c.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(c.device, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cb, &bi));
    record(cb);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    s.commandBufferCount = 1;
    s.pCommandBuffers = &cb;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(vkQueueSubmit(c.queue, 1, &s, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(c.queue));
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    vkFreeCommandBuffers(c.device, c.pool, 1, &cb);
    return secs;
}

// --- 1. loop guard on a real guarded shader --------------------------------------------------

struct GuardRun {
    double seconds;
    uint32_t count, hash_lo, hash_hi;
};

GuardRun RunGuarded(Ctx& c, VkShaderModule module, uint64_t pgm_hash, uint32_t bound,
                    bool specialize) {
    // ssbo_1 (binding 0): records of 4 dwords read per iteration; 1<<20 iterations read 16 MiB.
    Buf ssbo = MakeBuffer(c, (1ull << 20) * 16 + 4096);
    Buf flat = MakeBuffer(c, 256); // srt_flatbuf (binding 1): dword 6 = loop bound
    Buf report = MakeBuffer(c, ReportBytes);
    static_cast<uint32_t*>(flat.map)[6] = bound;

    VkDescriptorSetLayoutBinding b[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = b;
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(c.device, &li, nullptr, &dsl));
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(c.device, &pli, nullptr, &pl));

    const uint64_t address = report.address;
    VkSpecializationMapEntry me{AddressSpecId, 0, 8};
    VkSpecializationInfo spec{1, &me, 8, &address};
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.stage.pSpecializationInfo = specialize ? &spec : nullptr;
    ci.layout = pl;
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe));

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(c.device, &dpi, nullptr, &dp));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = dp;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl;
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(c.device, &dai, &set));
    VkDescriptorBufferInfo bufs[2]{{ssbo.buffer, 0, VK_WHOLE_SIZE}, {flat.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bufs[i];
    }
    vkUpdateDescriptorSets(c.device, 2, w, 0, nullptr);

    uint8_t push[128]{};
    const double secs = Submit(c, [&](VkCommandBuffer cb) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(cb, 1, 1, 1);
    });
    const auto* words = static_cast<const uint32_t*>(report.map);
    const uint32_t slot = SlotOf(pgm_hash) * SlotDwords;
    return {secs, words[slot], words[slot + 1], words[slot + 2]};
}

// --- 2. indirect clamp shader ----------------------------------------------------------------

struct ClampPush {
    uint64_t src, dst, report;
    uint32_t count, stride, mode, max_x, max_yz, max_total, max_elements, max_instances;
};

void TestClamp(Ctx& c) {
    std::printf("indirect clamp shader:\n");
    VkShaderModule module = Module(c, VCORE_INDIRECT_CLAMP_COMP, sizeof(VCORE_INDIRECT_CLAMP_COMP));
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ClampPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(c.device, &pli, nullptr, &pl));
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = pl;
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe));

    Buf src = MakeBuffer(c, 4096), dst = MakeBuffer(c, 4096), report = MakeBuffer(c, 256);
    auto* s = static_cast<uint32_t*>(src.map);
    auto* d = static_cast<uint32_t*>(dst.map);
    auto* r = static_cast<uint32_t*>(report.map);
    const auto run = [&](uint32_t count, uint32_t stride, uint32_t mode, uint32_t max_total) {
        ClampPush push{src.address, dst.address, report.address, count, stride, mode,
                       0xffffffffu, 65535, max_total, 1u << 26, 1u << 24};
        Submit(c, [&](VkCommandBuffer cb) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cb, (count + 63) / 64, 1, 1);
        });
    };
    // Normal dispatch: unchanged, no report.
    s[0] = 100; s[1] = 20; s[2] = 3;
    run(1, 12, 0, 1u << 22);
    Expect(d[0] == 100 && d[1] == 20 && d[2] == 3 && r[0] == 0, "dispatch 100x20x3 passes unchanged");
    // Garbage dispatch.
    s[0] = 0xffffffffu; s[1] = 0x3f800000u; s[2] = 7;
    run(1, 12, 0, 1u << 22);
    const uint64_t total = uint64_t(d[0]) * d[1] * d[2];
    std::printf("    garbage dispatch -> %ux%ux%u (%llu groups)\n", d[0], d[1], d[2],
                (unsigned long long)total);
    Expect(total <= (1u << 22) && d[1] <= 65535 && d[0] >= 1 && d[1] >= 1 && d[2] >= 1,
           "garbage dispatch clamped to <= 1<<22 groups, y/z <= 65535");
    Expect(r[0] == 1 && r[1] == 0xffffffffu && r[2] == 0x3f800000u && r[3] == 7,
           "dispatch clamp reported with the original dims");
    // Zero-sized dispatch stays zero.
    s[0] = 0; s[1] = 0xffffffffu; s[2] = 0xffffffffu;
    run(1, 12, 0, 1u << 22);
    Expect(d[0] == 0, "dispatch with x=0 stays empty");
    // Indexed multi-draw, stride 32 (5 words used): one sane and one garbage command.
    std::memset(s, 0, 4096);
    std::memset(d, 0, 4096);
    const uint32_t a[5] = {3000, 2, 10, 0xfffffff0u, 1};
    const uint32_t b[5] = {0xffffffffu, 0xffffffffu, 5, 6, 7};
    std::memcpy(s, a, sizeof(a));
    std::memcpy(s + 8, b, sizeof(b));
    run(2, 32, 2, 1u << 28);
    Expect(std::memcmp(d, a, sizeof(a)) == 0, "sane indexed draw copied unchanged");
    Expect(d[8] == (1u << 26) && uint64_t(d[8]) * d[9] <= (1u << 28) && d[9] >= 1 &&
               d[10] == 5 && d[11] == 6 && d[12] == 7,
           "garbage indexed draw clamped (count <= 1<<26, product <= 1<<28), other words kept");
    Expect(r[4] == 1 && r[5] == 0xffffffffu && r[6] == 0xffffffffu && r[7] == 2,
           "draw clamp reported");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <guarded 0x...5c35f8ee_0.spv> [pgm_hash]\n", argv[0]);
        return 2;
    }
    const uint64_t hash = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 0x5c35f8eeULL;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::perror(argv[1]);
        return 2;
    }
    std::vector<uint32_t> spv;
    std::fseek(f, 0, SEEK_END);
    spv.resize(std::ftell(f) / 4);
    std::rewind(f);
    if (std::fread(spv.data(), 4, spv.size(), f) != spv.size()) return 2;
    std::fclose(f);

    Ctx c;
    Init(c);
    VkShaderModule module = Module(c, spv.data(), spv.size() * 4);
    std::printf("loop guard, shader %#llx (slot %u):\n", (unsigned long long)hash, SlotOf(hash));

    GuardRun normal = RunGuarded(c, module, hash, 64, true);
    std::printf("    bound 64: %.3f s, report count %u\n", normal.seconds, normal.count);
    Expect(normal.count == 0, "normal loop bound: no trip reported");

    GuardRun spin = RunGuarded(c, module, hash, 0x7fffffffu, true);
    std::printf("    bound 2^31-1: %.3f s, report count %u hash %#x:%#x\n", spin.seconds, spin.count,
                spin.hash_hi, spin.hash_lo);
    Expect(spin.count == 64, "endless loop ends, one trip per invocation (64) reported");
    Expect(spin.hash_lo == uint32_t(hash) && spin.hash_hi == uint32_t(hash >> 32),
           "report slot holds the shader hash");

    GuardRun unspec = RunGuarded(c, module, hash, 0x7fffffffu, false);
    std::printf("    bound 2^31-1, no specialization: %.3f s, report count %u\n", unspec.seconds,
                unspec.count);
    Expect(unspec.count == 0, "without the address specialization: loop still ends, no report");

    TestClamp(c);
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
