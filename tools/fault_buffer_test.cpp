// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the production fault parser on a CPU Vulkan device. No game or physical GPU.
/* Build: g++ -std=c++20 -O2 tools/fault_buffer_test.cpp -lvulkan
 *   -Ivcore/build/host_shaders/include -o out/tests/fault-buffer-test */
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include "video_core/host_shaders/fault_buffer_process_comp.h"

#define VK_CHECK(call) do { if ((call)!=VK_SUCCESS) { \
    std::fprintf(stderr,"Vulkan failure at line %d: %s\n",__LINE__,#call); std::exit(2); } } while (0)
#define EXPECT(ok) do { if (!(ok)) { \
    std::fprintf(stderr,"Failed at line %d: %s\n",__LINE__,#ok); std::exit(1); } } while (0)

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
    VkInstance instance{}; VK_CHECK(vkCreateInstance(&ici,nullptr,&instance));
    uint32_t n=8; VkPhysicalDevice devices[8]{};
    VK_CHECK(vkEnumeratePhysicalDevices(instance,&n,devices));
    VkPhysicalDevice physical{};
    for (uint32_t i=0;i<n;++i) {
        VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(devices[i],&p);
        if (p.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU) {
            physical=devices[i]; std::printf("CPU device: %s\n",p.deviceName); break;
        }
    }
    EXPECT(physical);
    uint32_t family_count=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&family_count,nullptr);
    auto *families=new VkQueueFamilyProperties[family_count];
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&family_count,families);
    uint32_t family=0;
    while (family<family_count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) ++family;
    delete[] families; EXPECT(family<family_count);
    float priority=1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
    VkPhysicalDeviceFeatures features{}; features.shaderInt64=VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.synchronization2=VK_TRUE;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&f13};
    di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi; di.pEnabledFeatures=&features;
    VkDevice device{}; VK_CHECK(vkCreateDevice(physical,&di,nullptr,&device));
    VkQueue queue{}; vkGetDeviceQueue(device,family,0,&queue);

    constexpr uint32_t Words=2048, MaxFaults=1024, PageBits=16;
    constexpr VkDeviceSize Bytes=Words*sizeof(uint32_t);
    std::array<VkBuffer,2> buffers{}; std::array<VkDeviceMemory,2> memory{};
    std::array<void*,2> mapped{};
    VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    for (unsigned i=0;i<2;++i) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size=Bytes; bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VK_CHECK(vkCreateBuffer(device,&bi,nullptr,&buffers[i]));
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,buffers[i],&req);
        uint32_t type=0;
        constexpr auto wanted=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while (type<mp.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) ||
               (mp.memoryTypes[type].propertyFlags&wanted)!=wanted)) ++type;
        EXPECT(type<mp.memoryTypeCount);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize=req.size; ai.memoryTypeIndex=type;
        VK_CHECK(vkAllocateMemory(device,&ai,nullptr,&memory[i]));
        VK_CHECK(vkBindBufferMemory(device,buffers[i],memory[i],0));
        VK_CHECK(vkMapMemory(device,memory[i],0,Bytes,0,&mapped[i]));
    }
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (unsigned i=0;i<2;++i)
        bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount=2; li.pBindings=bindings;
    VkDescriptorSetLayout layout{}; VK_CHECK(vkCreateDescriptorSetLayout(device,&li,nullptr,&layout));
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount=1; pli.pSetLayouts=&layout;
    VkPipelineLayout pl{}; VK_CHECK(vkCreatePipelineLayout(device,&pli,nullptr,&pl));
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize=sizeof(FAULT_BUFFER_PROCESS_COMP); mi.pCode=FAULT_BUFFER_PROCESS_COMP;
    VkShaderModule module{}; VK_CHECK(vkCreateShaderModule(device,&mi,nullptr,&module));
    const uint32_t values[2]{PageBits,MaxFaults};
    const VkSpecializationMapEntry entries[2]{{0,0,4},{1,4,4}};
    VkSpecializationInfo spec{2,entries,sizeof(values),values};
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module=module;
    ci.stage.pName="main"; ci.stage.pSpecializationInfo=&spec; ci.layout=pl;
    VkPipeline pipeline{}; VK_CHECK(vkCreateComputePipelines(device,{},1,&ci,nullptr,&pipeline));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets=1; dpi.poolSizeCount=1; dpi.pPoolSizes=&ps;
    VkDescriptorPool pool{}; VK_CHECK(vkCreateDescriptorPool(device,&dpi,nullptr,&pool));
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sai.descriptorPool=pool; sai.descriptorSetCount=1; sai.pSetLayouts=&layout;
    VkDescriptorSet set{}; VK_CHECK(vkAllocateDescriptorSets(device,&sai,&set));
    const VkDescriptorBufferInfo info[2]{{buffers[0],0,Bytes},{buffers[1],0,Bytes}};
    VkWriteDescriptorSet writes[2]{};
    for (unsigned i=0;i<2;++i) {
        writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet=set;
        writes[i].dstBinding=i; writes[i].descriptorCount=1;
        writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo=&info[i];
    }
    vkUpdateDescriptorSets(device,2,writes,0,nullptr);
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.queueFamilyIndex=family;
    VkCommandPool commands{}; VK_CHECK(vkCreateCommandPool(device,&cpi,nullptr,&commands));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool=commands; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1;
    VkCommandBuffer cb{}; VK_CHECK(vkAllocateCommandBuffers(device,&cai,&cb));
    auto *bitmap=static_cast<uint32_t*>(mapped[0]);
    auto *faults=static_cast<uint64_t*>(mapped[1]);
    const auto run=[&](bool initialize) {
        std::memset(faults,0,Bytes);
        VK_CHECK(vkResetCommandPool(device,commands,0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cb,&begin));
        if (initialize) {
            // Same fill and transfer-to-shader dependency as FaultManager's constructor.
            vkCmdFillBuffer(cb,buffers[0],0,Bytes,0);
            VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask=VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT;
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer=buffers[0]; barrier.size=Bytes;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount=1; dependency.pBufferMemoryBarriers=&barrier;
            vkCmdPipelineBarrier2(cb,&dependency);
        }
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&set,0,nullptr);
        vkCmdDispatch(cb,Words/64,1,1);
        VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        host.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; host.srcAccessMask=VK_ACCESS_2_SHADER_WRITE_BIT;
        host.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT; host.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount=1; dependency.pMemoryBarriers=&host;
        vkCmdPipelineBarrier2(cb,&dependency);
        VK_CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cb;
        VK_CHECK(vkQueueSubmit(queue,1,&submit,{})); VK_CHECK(vkQueueWaitIdle(queue));
    };
    std::memset(bitmap,0xff,Bytes); run(false);
    EXPECT(faults[0]>=MaxFaults-1);
    std::puts("PASS negative control: dirty initial memory generates phantom faults");
    std::memset(bitmap,0xff,Bytes); run(true);
    EXPECT(faults[0]==0 && std::all_of(bitmap,bitmap+Words,[](uint32_t w){return w==0;}));
    std::puts("PASS initialized bitmap: no phantom faults after transfer-to-shader barrier");
    bitmap[0]=0x80000001; bitmap[Words-1]=0x80000000; run(false);
    EXPECT(faults[0]==3);
    std::set<uint64_t> actual{faults[1],faults[2],faults[3]};
    EXPECT((actual==std::set<uint64_t>{0,31ull<<PageBits,(Words*32ull-1)<<PageBits}));
    std::puts("PASS real faults: first and last pages survive initialization");
    std::fill(bitmap,bitmap+Words,1);
    std::set<uint64_t> observed;
    for (unsigned batch=0;batch<8;++batch) {
        run(false); const auto count=std::min<uint64_t>(faults[0],MaxFaults-1);
        for (unsigned i=1;i<=count;++i) EXPECT(observed.insert(faults[i]).second);
        if (!faults[0]) break;
    }
    EXPECT(observed.size()==Words);
    for (unsigned i=0;i<Words;++i) EXPECT(observed.contains(uint64_t(i*32)<<PageBits));
    std::puts("PASS overflow: requeued faults drain without loss or duplicates");
    vkDestroyCommandPool(device,commands,nullptr); vkDestroyDescriptorPool(device,pool,nullptr);
    vkDestroyPipeline(device,pipeline,nullptr); vkDestroyShaderModule(device,module,nullptr);
    vkDestroyPipelineLayout(device,pl,nullptr); vkDestroyDescriptorSetLayout(device,layout,nullptr);
    for (unsigned i=0;i<2;++i) { vkUnmapMemory(device,memory[i]); vkDestroyBuffer(device,buffers[i],nullptr); vkFreeMemory(device,memory[i],nullptr); }
    vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr);
}
