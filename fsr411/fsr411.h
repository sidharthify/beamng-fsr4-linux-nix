// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: FSR 4.1.1 (AMD's INT8 model of the 4.1.1 upscaler DLL) on Vulkan, replaying what the DLL
// does on D3D12. The asset sets hold the SPIR-V of every pass and the model weights.
//
// Plain Vulkan, no renderer types. Frames record into the caller's command buffer; the caller
// keeps the images in layout General and makes sure that the frame recorded kFramesInFlight
// frames earlier has completed before recording a new one (constant buffer ring).

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

namespace Fsr411 {

constexpr uint32_t kFramesInFlight = 8;

#define FSR411_INSTANCE_FUNCS(X) X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceProperties)
#define FSR411_DEVICE_FUNCS(X)                                                                     \
    X(AllocateMemory) X(FreeMemory) X(MapMemory) X(BindBufferMemory) X(BindImageMemory)            \
    X(GetBufferMemoryRequirements) X(GetImageMemoryRequirements) X(CreateBuffer) X(DestroyBuffer)  \
    X(CreateImage) X(DestroyImage) X(CreateImageView) X(DestroyImageView) X(CreateSampler)         \
    X(DestroySampler) X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout)    \
    X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout)                 \
    X(CreateComputePipelines) X(DestroyPipeline) X(CmdBindPipeline) X(CmdDispatch)                 \
    X(CmdPipelineBarrier) X(CmdClearColorImage) X(CmdFillBuffer) X(CmdCopyBuffer)                 \
    X(CmdPushDescriptorSetKHR)

/// The Vulkan entry points the upscaler calls. A layer fills it from the next element of its
/// chain so that the upscaler's own commands do not go back through the layer.
struct Dispatch {
#define FSR411_MEMBER(name) PFN_vk##name name = nullptr;
    FSR411_INSTANCE_FUNCS(FSR411_MEMBER)
    FSR411_DEVICE_FUNCS(FSR411_MEMBER)
#undef FSR411_MEMBER

    /// Loads every entry point; false if one is missing.
    bool Load(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, PFN_vkGetDeviceProcAddr gdpa,
              VkDevice device);
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE; ///< depth: a view of the depth aspect
    uint32_t width = 0, height = 0;
};

struct Frame {
    VkCommandBuffer cmdbuf = VK_NULL_HANDLE;
    Image color, depth, motion, output; ///< color RGBA16F, depth D32, motion RG16F (render pixels)
    uint32_t render_width = 0, render_height = 0;
    bool ultra_performance = false; ///< the second model (quality ratio 3)
    float jitter[2] = {};           ///< render pixels, as for FSR 3/4
    float motion_scale[2] = {1.0f, 1.0f};
    float pre_exposure = 1.0f;
    float sharpness = 0.0f; ///< 0..1
    bool sharpen = false;
    bool reset = false;
    bool auto_exposure = true;
};

class Upscaler {
public:
    /// `dir`: the asset sets (t1080_m0 ... t2160_m1). `mixed_float_dot`: the device enabled
    /// VK_VALVE_shader_mixed_float_dot_product (shaderMixedFloatDotProductFloat16AccFloat32);
    /// without it the prepass loads prepass_portable.spv, core SPIR-V.
    /// `subgroup_size_control`: the device enabled subgroupSizeControl and allows required sizes
    /// 32 and 64 for compute; some passes then request the wave size measured fastest on RDNA 2.
    Upscaler(const Dispatch& vk, VkPhysicalDevice physical, VkDevice device, std::string dir,
             bool mixed_float_dot = true, bool subgroup_size_control = false);
    ~Upscaler();

    /// Records one upscale; false with Error() set when it cannot (assets, device, sizes).
    bool Record(const Frame& frame);
    [[nodiscard]] const std::string& Error() const noexcept;
    /// The asset set and sizes in use, for logs ("t2160_m0 output 3840x2160").
    [[nodiscard]] std::string Describe() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Fsr411
