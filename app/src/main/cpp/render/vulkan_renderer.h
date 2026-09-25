#pragma once

#include <android/native_window.h>

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

#include "gs/tile_builder.h"

namespace splatscan {

/**
 * Vulkan 渲染器：计算管线逐 tile 做高斯泼溅写进存储图像，图形管线把它铺满交换链。
 *
 * 设计取舍：投影、排序与分桶都在 CPU 完成，GPU 只做逐像素合成。这样显存占用与
 * 实现复杂度都可控，代价是每帧要做一次 O(N log N) 排序，对端上模型规模足够。
 * 初始化失败（没有 Vulkan 1.1 或不支持交换链）时保持不可用，调用方回退到 CPU 预览。
 */
class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    bool initialize(ANativeWindow* window, int32_t renderWidth, int32_t renderHeight);
    void shutdown();
    void setRenderSize(int32_t renderWidth, int32_t renderHeight);

    bool isReady() const { return ready_; }

    /** 用一帧的 tile 数据渲染并呈现。返回 false 表示这一帧没能画出来。 */
    bool renderFrame(const TileBundle& bundle);

private:
    bool createInstance();
    bool createSurface(ANativeWindow* window);
    bool pickPhysicalDevice();
    bool createDevice();
    bool createSwapchain();
    bool createRenderPass();
    bool createFramebuffers();
    bool createStorageImage();
    bool createSampler();
    bool createSetLayouts();
    bool createDescriptorResources();
    bool createComputePipeline();
    bool createBlitPipeline();
    void recreateSwapchain();
    bool createBuffers();
    bool createCommands();
    void destroySwapchainResources();
    bool ensureBufferCapacity(size_t splatCount, size_t rangeCount, size_t itemCount);
    void upload(const TileBundle& bundle);

    ANativeWindow* window_ = nullptr;
    int32_t renderWidth_ = 160;
    int32_t renderHeight_ = 120;
    bool ready_ = false;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapchainExtent_{0, 0};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    std::vector<VkFramebuffer> framebuffers_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout computeSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout graphicsSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout computePipelineLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout graphicsPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline computePipeline_ = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet computeSet_ = VK_NULL_HANDLE;
    VkDescriptorSet graphicsSet_ = VK_NULL_HANDLE;

    VkImage storageImage_ = VK_NULL_HANDLE;
    VkDeviceMemory storageMemory_ = VK_NULL_HANDLE;
    VkImageView storageView_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    VkBuffer splatBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory splatMemory_ = VK_NULL_HANDLE;
    size_t splatCapacity_ = 0;
    VkBuffer rangeBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory rangeMemory_ = VK_NULL_HANDLE;
    size_t rangeCapacity_ = 0;
    VkBuffer itemBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory itemMemory_ = VK_NULL_HANDLE;
    size_t itemCapacity_ = 0;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkSemaphore renderFinished_ = VK_NULL_HANDLE;

    uint32_t lastTileCount_ = 0;
};

}  // namespace splatscan