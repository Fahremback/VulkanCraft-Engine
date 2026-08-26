// VkRayTracer — backend HARDWARE (GPU) do IRayTracer usando Vulkan ray
// tracing via VK_KHR_ray_query (compute). Mesma interface pública; semântica
// single-sided IDÊNTICA ao Embree (cull de backface no shader:
// dot(normal, dir) >= 0 -> descarta o candidato).
//
// Por que ray query e não VK_KHR_ray_tracing_pipeline? O pipeline RT exige
// SBT + hit/miss/raygen shaders e, na driver desta máquina (NVIDIA 610.88),
// o trace com hit-groups causava VK_ERROR_DEVICE_LOST de forma reprodutível
// (debugado com validation layers + probes isolados). O ray query roda num
// compute pipeline comum (vkCmdDispatch): mesmo BLAS/TLAS, zero SBT, robusto.
//
// Seleção data-driven em create_ray_tracer_preferred(): GPU RT quando a GPU
// suporta, fallback automático para o software (Embree) — sem mudança de
// contrato (A.8). SPIR-V embutido (RayTracingGpuShaders.hpp, padrão BUG-010).
#include "engine/rendering/IRayTracer.hpp"
#include "RayTracingGpuShaders.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace vc::rendering {

namespace {

constexpr std::uint32_t kMaxRays = 1024;    // rays/results buffers fixos (host-visible)
constexpr std::size_t kRayBytes = 48;       // 3 x vec4 (org, dir, tminmax)
constexpr std::size_t kResultBytes = 16;    // uint hit + float t + int prim + pad

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
    void* mapped = nullptr;
};

class VkRayTracerImpl final : public IRayTracer {
public:
    VkRayTracerImpl() { init(); }
    ~VkRayTracerImpl() override { teardown(); }
    VkRayTracerImpl(const VkRayTracerImpl&) = delete;
    VkRayTracerImpl& operator=(const VkRayTracerImpl&) = delete;

    bool isReady() const {
        return instance_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE &&
               pipeline_ != VK_NULL_HANDLE;
    }

    bool build(const RayTracerTriangle* triangles, int32_t count) override {
        teardownGeometry();
        if (count <= 0 || triangles == nullptr || !isReady()) return false;

        const std::uint32_t triCount = static_cast<std::uint32_t>(count);
        const VkDeviceSize vertBytes = static_cast<VkDeviceSize>(triCount) * 3u * 16u;

        GpuBuffer verts, instanceBuf;
        VkBuffer blasBuf = VK_NULL_HANDLE, tlasBuf = VK_NULL_HANDLE;
        VkDeviceMemory blasBufMem = VK_NULL_HANDLE, tlasBufMem = VK_NULL_HANDLE;
        VkBuffer scratchBuf = VK_NULL_HANDLE;
        VkDeviceMemory scratchBufMem = VK_NULL_HANDLE;
        VkAccelerationStructureKHR blasAS = VK_NULL_HANDLE, tlasAS = VK_NULL_HANDLE;
        bool success = false;

        auto fail = [&]() {
            destroyGpuBuffer(verts); destroyGpuBuffer(instanceBuf);
            if (blasBuf) vkDestroyBuffer(device_, blasBuf, nullptr);
            if (blasBufMem) vkFreeMemory(device_, blasBufMem, nullptr);
            if (tlasBuf) vkDestroyBuffer(device_, tlasBuf, nullptr);
            if (tlasBufMem) vkFreeMemory(device_, tlasBufMem, nullptr);
            if (scratchBuf) vkDestroyBuffer(device_, scratchBuf, nullptr);
            if (scratchBufMem) vkFreeMemory(device_, scratchBufMem, nullptr);
            if (blasAS) pfDestroyAccelerationStructure_(device_, blasAS, nullptr);
            if (tlasAS) pfDestroyAccelerationStructure_(device_, tlasAS, nullptr);
        };

        // --- Vértices (stride 16, usado pela BLAS E pelo SSBO do shader) ---
        if (!createBuffer(vertBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, verts)) return false;
        {
            std::vector<float> packed(static_cast<size_t>(triCount) * 3u * 4u, 0.0f);
            for (std::uint32_t i = 0; i < triCount; ++i) {
                const RayTracerTriangle& t = triangles[i];
                float* dst = packed.data() + static_cast<size_t>(i) * 12u;
                dst[0] = t.v0[0]; dst[1] = t.v0[1]; dst[2] = t.v0[2];
                dst[4] = t.v1[0]; dst[5] = t.v1[1]; dst[6] = t.v1[2];
                dst[8] = t.v2[0]; dst[9] = t.v2[1]; dst[10] = t.v2[2];
            }
            if (!upload(verts, packed.data(), packed.size() * sizeof(float))) { fail(); return false; }
        }

        // --- Instância única (transform identidade) ---
        if (!createBuffer(sizeof(VkAccelerationStructureInstanceKHR),
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, instanceBuf)) return false;
        {
            VkAccelerationStructureInstanceKHR inst{};
            inst.transform.matrix[0][0] = 1.0f;
            inst.transform.matrix[1][1] = 1.0f;
            inst.transform.matrix[2][2] = 1.0f;
            inst.instanceCustomIndex = 0;
            inst.mask = 0xFFu;
            inst.instanceShaderBindingTableRecordOffset = 0;
            inst.flags = 0;
            inst.accelerationStructureReference = 0;  // preenchido após a BLAS
            if (!upload(instanceBuf, &inst, sizeof(inst))) { fail(); return false; }
        }

        // --- BLAS ---
        VkAccelerationStructureGeometryKHR blasGeo{};
        blasGeo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        blasGeo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        blasGeo.flags = 0;  // NÃO-opaco: o shader de ray query decide o hit.
        blasGeo.geometry.triangles.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        blasGeo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        blasGeo.geometry.triangles.vertexData.deviceAddress = verts.address;
        blasGeo.geometry.triangles.vertexStride = 16;
        blasGeo.geometry.triangles.maxVertex = triCount * 3u - 1u;
        blasGeo.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

        VkAccelerationStructureBuildGeometryInfoKHR blasBuild{};
        blasBuild.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        blasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        blasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        blasBuild.geometryCount = 1u;
        blasBuild.pGeometries = &blasGeo;

        VkAccelerationStructureBuildSizesInfoKHR blasSizes{};
        blasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        pfGetAccelerationStructureBuildSizes_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                              &blasBuild, &triCount, &blasSizes);
        if (!createBuffer(blasSizes.accelerationStructureSize,
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &blasBuf, &blasBufMem)) { fail(); return false; }

        // --- TLAS ---
        VkAccelerationStructureGeometryKHR tlasGeo{};
        tlasGeo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        tlasGeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlasGeo.geometry.instances.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        tlasGeo.geometry.instances.arrayOfPointers = VK_FALSE;
        tlasGeo.geometry.instances.data.deviceAddress = instanceBuf.address;

        VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{};
        tlasBuild.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        tlasBuild.geometryCount = 1u;
        tlasBuild.pGeometries = &tlasGeo;

        const std::uint32_t one = 1u;
        VkAccelerationStructureBuildSizesInfoKHR tlasSizes{};
        tlasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        pfGetAccelerationStructureBuildSizes_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                              &tlasBuild, &one, &tlasSizes);
        if (!createBuffer(tlasSizes.accelerationStructureSize,
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &tlasBuf, &tlasBufMem)) { fail(); return false; }

        // --- Scratch (buffer device-local COM endereço) ---
        const VkDeviceSize scratchSize = blasSizes.buildScratchSize > tlasSizes.buildScratchSize
                                             ? blasSizes.buildScratchSize : tlasSizes.buildScratchSize;
        {
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, scratchSize,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
            if (vkCreateBuffer(device_, &bci, nullptr, &scratchBuf) != VK_SUCCESS) { fail(); return false; }
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(device_, scratchBuf, &req);
            const std::uint32_t mt = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mt == ~0u) { fail(); return false; }
            VkMemoryAllocateFlagsInfo allocFlags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                                 nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &allocFlags,
                                    req.size, mt};
            if (vkAllocateMemory(device_, &ai, nullptr, &scratchBufMem) != VK_SUCCESS) { fail(); return false; }
            if (vkBindBufferMemory(device_, scratchBuf, scratchBufMem, 0) != VK_SUCCESS) { fail(); return false; }
        }
        VkBufferDeviceAddressInfo bai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, scratchBuf};
        blasBuild.scratchData.deviceAddress = vkGetBufferDeviceAddress(device_, &bai);
        tlasBuild.scratchData.deviceAddress = blasBuild.scratchData.deviceAddress;

        // --- Criação das AS ---
        VkAccelerationStructureCreateInfoKHR asCI{};
        asCI.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        asCI.size = blasSizes.accelerationStructureSize;
        asCI.buffer = blasBuf;
        asCI.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (pfCreateAccelerationStructure_(device_, &asCI, nullptr, &blasAS) != VK_SUCCESS) { fail(); return false; }
        asCI.size = tlasSizes.accelerationStructureSize;
        asCI.buffer = tlasBuf;
        asCI.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (pfCreateAccelerationStructure_(device_, &asCI, nullptr, &tlasAS) != VK_SUCCESS) { fail(); return false; }
        blasBuild.dstAccelerationStructure = blasAS;
        tlasBuild.dstAccelerationStructure = tlasAS;

        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{};
        addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        addrInfo.accelerationStructure = blasAS;
        const VkDeviceAddress blasAddr = pfGetAccelerationStructureDeviceAddress_(device_, &addrInfo);
        addrInfo.accelerationStructure = tlasAS;
        const VkDeviceAddress tlasAddr = pfGetAccelerationStructureDeviceAddress_(device_, &addrInfo);
        if (blasAddr == 0 || tlasAddr == 0) { fail(); return false; }

        // Reenvia a instância com o endereço real da BLAS.
        {
            VkAccelerationStructureInstanceKHR inst{};
            inst.transform.matrix[0][0] = 1.0f;
            inst.transform.matrix[1][1] = 1.0f;
            inst.transform.matrix[2][2] = 1.0f;
            inst.instanceCustomIndex = 0;
            inst.mask = 0xFFu;
            inst.instanceShaderBindingTableRecordOffset = 0;
            inst.flags = 0;
            inst.accelerationStructureReference = blasAddr;
            if (!upload(instanceBuf, &inst, sizeof(inst))) { fail(); return false; }
        }

        // --- Build (copy -> barrier -> BLAS -> barrier -> TLAS) num submit ---
        VkAccelerationStructureBuildRangeInfoKHR blasRange{triCount, 0, 0, 0};
        VkAccelerationStructureBuildRangeInfoKHR tlasRange{1, 0, 0, 0};
        success = runCommands([&](VkCommandBuffer cmd) {
            VkMemoryBarrier bar1{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                 0, 1, &bar1, 0, nullptr, 0, nullptr);
            const VkAccelerationStructureBuildRangeInfoKHR* blasRangePtr = &blasRange;
            pfCmdBuildAccelerationStructures_(cmd, 1, &blasBuild, &blasRangePtr);
            VkMemoryBarrier bar2{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                 VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                                 VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                 0, 1, &bar2, 0, nullptr, 0, nullptr);
            const VkAccelerationStructureBuildRangeInfoKHR* tlasRangePtr = &tlasRange;
            pfCmdBuildAccelerationStructures_(cmd, 1, &tlasBuild, &tlasRangePtr);
        });
        if (!success) { fail(); return false; }

        // --- Descriptors: vértices (binding 2) + TLAS (binding 3) ---
        {
            VkDescriptorBufferInfo vertInfo{verts.buffer, 0, vertBytes};
            VkWriteDescriptorSet wv{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                                    descriptorSet_, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                    nullptr, &vertInfo, nullptr};
            VkWriteDescriptorSetAccelerationStructureKHR accelInfo{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR, nullptr,
                1, &tlasAS};
            VkWriteDescriptorSet wt{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &accelInfo,
                                    descriptorSet_, 3, 0, 1,
                                    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                                    nullptr, nullptr, nullptr};
            VkWriteDescriptorSet writes[2] = {wv, wt};
            vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
        }

        verts_ = verts;
        instanceBuf_ = instanceBuf;
        blasBuffer_ = blasBuf; blasMem_ = blasBufMem;
        tlasBuffer_ = tlasBuf; tlasMem_ = tlasBufMem;
        blas_ = blasAS;
        tlas_ = tlasAS;
        tlasAddr_ = tlasAddr;
        vkDestroyBuffer(device_, scratchBuf, nullptr);
        vkFreeMemory(device_, scratchBufMem, nullptr);
        built_ = true;
        return true;
    }

    RayTracerHit closestHit(const RayTracerRay& ray) const override {
        RayTracerHit out{};
        out.hit = false; out.t = 0.0f; out.primitiveIndex = -1;
        if (!built_) return out;
        writeRay(ray);
        if (!const_cast<VkRayTracerImpl*>(this)->trace(1u)) return out;
        const std::uint32_t* res = static_cast<const std::uint32_t*>(results_.mapped);
        if (res[0] != 0u) {
            out.hit = true;
            std::memcpy(&out.t, res + 1, sizeof(float));
            std::memcpy(&out.primitiveIndex, res + 2, sizeof(std::int32_t));
        }
        return out;
    }

    bool occluded(const RayTracerRay& ray) const override {
        if (!built_) return false;
        writeRay(ray);
        if (!const_cast<VkRayTracerImpl*>(this)->trace(1u)) return false;
        const std::uint32_t* res = static_cast<const std::uint32_t*>(results_.mapped);
        return res[0] != 0u;
    }

private:
    // ---------- init / teardown ----------
    void init() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
                              "VulkanCraft RayTracer", VK_MAKE_VERSION(1, 0, 0),
                              "VulkanCraft", VK_MAKE_VERSION(1, 0, 0),
                              VK_API_VERSION_1_3};
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app,
                                 0, nullptr, 0, nullptr};
        if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) return;

        std::uint32_t gpuCount = 0;
        if (vkEnumeratePhysicalDevices(instance_, &gpuCount, nullptr) != VK_SUCCESS || gpuCount == 0)
            return;
        std::vector<VkPhysicalDevice> gpus(gpuCount);
        vkEnumeratePhysicalDevices(instance_, &gpuCount, gpus.data());

        for (VkPhysicalDevice g : gpus) {
            std::uint32_t extCount = 0;
            vkEnumerateDeviceExtensionProperties(g, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> exts(extCount);
            vkEnumerateDeviceExtensionProperties(g, nullptr, &extCount, exts.data());
            bool hasAS = false, hasRQ = false, hasDHO = false;
            for (const auto& e : exts) {
                if (std::strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) hasAS = true;
                if (std::strcmp(e.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0) hasRQ = true;
                if (std::strcmp(e.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) == 0) hasDHO = true;
            }
            if (!hasAS || !hasRQ || !hasDHO) continue;

            std::uint32_t qCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(g, &qCount, nullptr);
            std::vector<VkQueueFamilyProperties> qprops(qCount);
            vkGetPhysicalDeviceQueueFamilyProperties(g, &qCount, qprops.data());
            std::int32_t qf = -1;
            for (std::uint32_t i = 0; i < qCount; ++i) {
                if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = static_cast<std::int32_t>(i); break; }
            }
            if (qf < 0) continue;

            VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceAccelerationStructureFeaturesKHR fAS{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
            VkPhysicalDeviceRayQueryFeaturesKHR fRQ{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
            f12.pNext = &fAS;
            fAS.pNext = &fRQ;
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
            vkGetPhysicalDeviceFeatures2(g, &f2);
            if (!fAS.accelerationStructure || !fRQ.rayQuery || !f12.bufferDeviceAddress)
                continue;

            const char* devExts[] = {
                VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                VK_KHR_RAY_QUERY_EXTENSION_NAME,
                VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
            };
            f12.bufferDeviceAddress = VK_TRUE;
            fAS.accelerationStructure = VK_TRUE;
            fRQ.rayQuery = VK_TRUE;
            f2.pNext = &f12;
            const float prio = 1.0f;
            VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr,
                                        0, static_cast<std::uint32_t>(qf), 1, &prio};
            VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci,
                                   0, nullptr, 3, devExts, nullptr};
            if (vkCreateDevice(g, &dci, nullptr, &device_) != VK_SUCCESS) continue;

            physicalDevice_ = g;
            queueFamily_ = qf;
            vkGetDeviceQueue(device_, static_cast<std::uint32_t>(qf), 0, &queue_);
            loadProcs();
            vkGetPhysicalDeviceMemoryProperties(g, &memProps_);

            VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                         VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                         static_cast<std::uint32_t>(qf)};
            if (vkCreateCommandPool(device_, &cpci, nullptr, &cmdPool_) != VK_SUCCESS) return;
            VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
                                             cmdPool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
            if (vkAllocateCommandBuffers(device_, &cbai, &cmdBuffer_) != VK_SUCCESS) return;
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
            if (vkCreateFence(device_, &fci, nullptr, &fence_) != VK_SUCCESS) return;

            if (!createBuffer(kMaxRays * kRayBytes,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              rays_)) return;
            if (!createBuffer(kMaxRays * kResultBytes,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              results_)) return;

            VkDescriptorPoolSize poolSizes[2] = {
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3u},
                {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u},
            };
            VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr,
                                            0, 1, 2, poolSizes};
            if (vkCreateDescriptorPool(device_, &dpci, nullptr, &descPool_) != VK_SUCCESS) return;

            VkDescriptorSetLayoutBinding bindings[4] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {3, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            };
            VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  nullptr, 0, 4, bindings};
            if (vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &descSetLayout_) != VK_SUCCESS) return;
            VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr,
                                            0, 1, &descSetLayout_, 0, nullptr};
            if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipelineLayout_) != VK_SUCCESS) return;
            VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
                                             descPool_, 1, &descSetLayout_};
            if (vkAllocateDescriptorSets(device_, &dsai, &descriptorSet_) != VK_SUCCESS) return;

            {
                VkDescriptorBufferInfo raysInfo{rays_.buffer, 0, kMaxRays * kRayBytes};
                VkWriteDescriptorSet w0{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                                        descriptorSet_, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                        nullptr, &raysInfo, nullptr};
                VkDescriptorBufferInfo resInfo{results_.buffer, 0, kMaxRays * kResultBytes};
                VkWriteDescriptorSet w1{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                                        descriptorSet_, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                        nullptr, &resInfo, nullptr};
                VkWriteDescriptorSet writes[2] = {w0, w1};
                vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
            }

            if (!createPipeline()) return;
            return;  // sucesso
        }
    }

    void loadProcs() {
        pfCreateAccelerationStructure_ = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
            vkGetDeviceProcAddr(device_, "vkCreateAccelerationStructureKHR"));
        pfDestroyAccelerationStructure_ = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
            vkGetDeviceProcAddr(device_, "vkDestroyAccelerationStructureKHR"));
        pfGetAccelerationStructureBuildSizes_ = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureBuildSizesKHR"));
        pfCmdBuildAccelerationStructures_ = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            vkGetDeviceProcAddr(device_, "vkCmdBuildAccelerationStructuresKHR"));
        pfGetAccelerationStructureDeviceAddress_ = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureDeviceAddressKHR"));
    }

    bool createPipeline() {
        VkShaderModule module = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                     RT_QUERY_SPV_SIZE * sizeof(std::uint32_t), RT_QUERY_SPV};
        if (vkCreateShaderModule(device_, &sci, nullptr, &module) != VK_SUCCESS) return false;
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module,
                                              "main", nullptr};
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr,
                                         0, stage, pipelineLayout_, VK_NULL_HANDLE, -1};
        const VkResult r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                                    &pipeline_);
        vkDestroyShaderModule(device_, module, nullptr);
        return r == VK_SUCCESS;
    }

    // ---------- helpers de buffer ----------
    std::uint32_t findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags props) const {
        for (std::uint32_t i = 0; i < memProps_.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) &&
                (memProps_.memoryTypes[i].propertyFlags & props) == props)
                return i;
        }
        return ~0u;
    }

    void destroyGpuBuffer(GpuBuffer& b) {
        if (b.buffer) { vkDestroyBuffer(device_, b.buffer, nullptr); b.buffer = VK_NULL_HANDLE; }
        if (b.memory) { vkFreeMemory(device_, b.memory, nullptr); b.memory = VK_NULL_HANDLE; }
        b.mapped = nullptr;
        b.address = 0;
        b.size = 0;
    }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                      GpuBuffer& out) {
        return createBuffer(size, usage, props, &out.buffer, &out.memory, &out.address,
                            &out.mapped, &out.size);
    }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                      VkBuffer* buffer, VkDeviceMemory* memory) {
        GpuBuffer tmp;
        if (!createBuffer(size, usage, props, &tmp.buffer, &tmp.memory, &tmp.address,
                          &tmp.mapped, &tmp.size)) return false;
        *buffer = tmp.buffer; *memory = tmp.memory;
        return true;
    }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                      VkBuffer* buffer, VkDeviceMemory* memory, VkDeviceAddress* address,
                      void** mapped, VkDeviceSize* outSize) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,
                               size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
        if (vkCreateBuffer(device_, &bci, nullptr, buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, *buffer, &req);
        const std::uint32_t mt = findMemoryType(req.memoryTypeBits, props);
        if (mt == ~0u) { vkDestroyBuffer(device_, *buffer, nullptr); *buffer = VK_NULL_HANDLE; return false; }
        VkMemoryAllocateFlags allocFlags = 0;
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
            allocFlags |= VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateFlagsInfo flagsInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                            nullptr, allocFlags};
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                                req.size, mt};
        if (allocFlags != 0) ai.pNext = &flagsInfo;
        if (vkAllocateMemory(device_, &ai, nullptr, memory) != VK_SUCCESS) {
            vkDestroyBuffer(device_, *buffer, nullptr); *buffer = VK_NULL_HANDLE; return false;
        }
        if (vkBindBufferMemory(device_, *buffer, *memory, 0) != VK_SUCCESS) return false;
        if (address != nullptr && (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)) {
            VkBufferDeviceAddressInfo bai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, *buffer};
            *address = vkGetBufferDeviceAddress(device_, &bai);
        }
        if (mapped != nullptr && (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            if (vkMapMemory(device_, *memory, 0, size, 0, mapped) != VK_SUCCESS) return false;
        }
        if (outSize != nullptr) *outSize = size;
        return true;
    }

    bool upload(const GpuBuffer& dst, const void* data, VkDeviceSize bytes) {
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        void* mapped = nullptr;
        if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &staging, &stagingMem, nullptr, &mapped, nullptr)) return false;
        std::memcpy(mapped, data, static_cast<size_t>(bytes));
        const bool ok = runCommands([&](VkCommandBuffer cmd) {
            VkBufferCopy region{0, 0, bytes};
            vkCmdCopyBuffer(cmd, staging, dst.buffer, 1, &region);
        });
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, stagingMem, nullptr);
        return ok;
    }

    bool runCommands(const std::function<void(VkCommandBuffer)>& record) {
        vkResetCommandBuffer(cmdBuffer_, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, 0, nullptr};
        if (vkBeginCommandBuffer(cmdBuffer_, &bi) != VK_SUCCESS) return false;
        record(cmdBuffer_);
        if (vkEndCommandBuffer(cmdBuffer_) != VK_SUCCESS) return false;
        vkResetFences(device_, 1, &fence_);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr,
                        1, &cmdBuffer_, 0, nullptr};
        if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) return false;
        if (vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;
        return true;
    }

    void writeRay(const RayTracerRay& ray) const {
        float* dst = static_cast<float*>(rays_.mapped);
        dst[0] = ray.ox; dst[1] = ray.oy; dst[2] = ray.oz; dst[3] = 0.0f;
        dst[4] = ray.dx; dst[5] = ray.dy; dst[6] = ray.dz; dst[7] = 0.0f;
        dst[8] = ray.tMin; dst[9] = ray.tMax; dst[10] = 0.0f; dst[11] = 0.0f;
    }

    bool trace(std::uint32_t count) {
        if (!isReady() || count == 0) return false;
        const std::uint32_t groups = (count + 63u) / 64u;
        return runCommands([&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                                    0, 1, &descriptorSet_, 0, nullptr);
            vkCmdDispatch(cmd, groups, 1, 1);
        });
    }

    void teardownGeometry() {
        if (tlas_) { pfDestroyAccelerationStructure_(device_, tlas_, nullptr); tlas_ = VK_NULL_HANDLE; }
        if (blas_) { pfDestroyAccelerationStructure_(device_, blas_, nullptr); blas_ = VK_NULL_HANDLE; }
        if (tlasBuffer_) { vkDestroyBuffer(device_, tlasBuffer_, nullptr); tlasBuffer_ = VK_NULL_HANDLE; }
        if (blasBuffer_) { vkDestroyBuffer(device_, blasBuffer_, nullptr); blasBuffer_ = VK_NULL_HANDLE; }
        if (tlasMem_) { vkFreeMemory(device_, tlasMem_, nullptr); tlasMem_ = VK_NULL_HANDLE; }
        if (blasMem_) { vkFreeMemory(device_, blasMem_, nullptr); blasMem_ = VK_NULL_HANDLE; }
        destroyGpuBuffer(verts_);
        destroyGpuBuffer(instanceBuf_);
        tlasAddr_ = 0;
        built_ = false;
    }

    void teardown() {
        teardownGeometry();
        destroyGpuBuffer(rays_);
        destroyGpuBuffer(results_);
        if (pipeline_) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
        if (pipelineLayout_) { vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
        if (descSetLayout_) { vkDestroyDescriptorSetLayout(device_, descSetLayout_, nullptr); descSetLayout_ = VK_NULL_HANDLE; }
        if (descPool_) { vkDestroyDescriptorPool(device_, descPool_, nullptr); descPool_ = VK_NULL_HANDLE; }
        if (fence_) { vkDestroyFence(device_, fence_, nullptr); fence_ = VK_NULL_HANDLE; }
        if (cmdBuffer_) { vkFreeCommandBuffers(device_, cmdPool_, 1, &cmdBuffer_); cmdBuffer_ = VK_NULL_HANDLE; }
        if (cmdPool_) { vkDestroyCommandPool(device_, cmdPool_, nullptr); cmdPool_ = VK_NULL_HANDLE; }
        if (device_) { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
        if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
    }

    // ---------- estado ----------
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::int32_t queueFamily_ = -1;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkAccelerationStructureKHR blas_ = VK_NULL_HANDLE;
    VkAccelerationStructureKHR tlas_ = VK_NULL_HANDLE;
    VkBuffer blasBuffer_ = VK_NULL_HANDLE;
    VkBuffer tlasBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory blasMem_ = VK_NULL_HANDLE;
    VkDeviceMemory tlasMem_ = VK_NULL_HANDLE;
    VkDeviceAddress tlasAddr_ = 0;
    GpuBuffer verts_, instanceBuf_, rays_, results_;
    VkPhysicalDeviceMemoryProperties memProps_{};
    bool built_ = false;

    PFN_vkCreateAccelerationStructureKHR pfCreateAccelerationStructure_ = nullptr;
    PFN_vkDestroyAccelerationStructureKHR pfDestroyAccelerationStructure_ = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR pfGetAccelerationStructureBuildSizes_ = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR pfCmdBuildAccelerationStructures_ = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR pfGetAccelerationStructureDeviceAddress_ = nullptr;
};

}  // namespace

// Factory HARDWARE: nullptr se a GPU não expuser ray query (fallback honesto).
std::unique_ptr<IRayTracer> create_hw_ray_tracer() {
    auto impl = std::make_unique<VkRayTracerImpl>();
    if (!impl->isReady()) return nullptr;
    return impl;
}

// Seleção data-driven: preferHardware tenta a GPU RT e cai para o software
// (Embree) automaticamente — A.8 sem mudança de contrato.
std::unique_ptr<IRayTracer> create_ray_tracer_preferred(bool preferHardware) {
    if (preferHardware) {
        if (auto hw = create_hw_ray_tracer(); hw) return hw;
    }
    return create_ray_tracer();
}

}  // namespace vc::rendering
