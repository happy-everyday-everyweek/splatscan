#include "render/vulkan_renderer.h"

#include <android/log.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "render/shader_registry.h"

#define SPLATSCAN_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "SplatScanVk", __VA_ARGS__)
#define SPLATSCAN_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SplatScanVk", __VA_ARGS__)

namespace splatscan {

namespace {

constexpr int32_t kTileSize = 16;

bool isExtensionSupported(const std::vector<VkExtensionProperties>& available, const char* name) {
    for (const VkExtensionProperties& item : available) {
        if (std::strcmp(item.extensionName, name) == 0) return true;
    }
    return false;
}

}  // namespace

VulkanRenderer::~VulkanRenderer() { shutdown(); }

bool VulkanRenderer::initialize(ANativeWindow* window, int32_t renderWidth, int32_t renderHeight) {
    if (ready_) return true;
    window_ = window;
    renderWidth_ = std::max(16, renderWidth);
    renderHeight_ = std::max(16, renderHeight);

    if (!createInstance()) return false;
    if (!createSurface(window)) return false;
    if (!pickPhysicalDevice()) return false;
    if (!createDevice()) return false;
    if (!createSwapchain()) return false;
    if (!createRenderPass()) return false;
    if (!createFramebuffers()) return false;
    if (!createCommands()) return false;
    if (!createStorageImage()) return false;
    if (!createSampler()) return false;
    if (!createDescriptors()) return false;
    if (!createPipelines()) return false;
    if (!createBuffers()) return false;

    ready_ = true;
    SPLATSCAN_LOGI("渲染器就绪：%dx%d 渲染，%dx%d 输出", renderWidth_, renderHeight_,
                   swapchainExtent_.width, swapchainExtent_.height);
    return true;
}

bool VulkanRenderer::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "SplatScan";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "SplatScanCore";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = extensions;

    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
        SPLATSCAN_LOGE("创建 Vulkan 实例失败");
        return false;
    }
    return true;
}

bool VulkanRenderer::createSurface(ANativeWindow* window) {
    if (window == nullptr) return false;
    VkAndroidSurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    createInfo.window = window;
    if (vkCreateAndroidSurfaceKHR(instance_, &createInfo, nullptr, &surface_) != VK_SUCCESS) {
        SPLATSCAN_LOGE("创建 Android 表面失败");
        return false;
    }
    return true;
}

bool VulkanRenderer::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        SPLATSCAN_LOGE("没有可用的 Vulkan 物理设备");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    for (VkPhysicalDevice candidate : devices) {
        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());

        for (uint32_t i = 0; i < queueCount; ++i) {
            const bool supportsGraphics = (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            const bool supportsCompute = (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            if (!supportsGraphics || !supportsCompute) continue;

            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &presentSupport);
            if (presentSupport != VK_TRUE) continue;

            uint32_t extensionCount = 0;
            vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr);
            std::vector<VkExtensionProperties> extensions(extensionCount);
            vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, extensions.data());
            if (!isExtensionSupported(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

            uint32_t formatCount = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface_, &formatCount, nullptr);
            if (formatCount == 0) continue;

            physicalDevice_ = candidate;
            queueFamily_ = i;
            return true;
        }
    }
    SPLATSCAN_LOGE("没有同时支持图形、计算与呈现的队列族");
    return false;
}

bool VulkanRenderer::createDevice() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = 1;
    createInfo.ppEnabledExtensionNames = extensions;

    if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
        SPLATSCAN_LOGE("创建 Vulkan 逻辑设备失败");
        return false;
    }
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    return queue_ != VK_NULL_HANDLE;
}

bool VulkanRenderer::createSwapchain() {
    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());

    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                                     VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                                : formats.front();
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM ||
            candidate.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen = candidate;
            break;
        }
    }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == 0xFFFFFFFFu || extent.height == 0xFFFFFFFFu) {
        extent.width = static_cast<uint32_t>(ANativeWindow_getWidth(window_));
        extent.height = static_cast<uint32_t>(ANativeWindow_getHeight(window_));
    }
    extent.width = std::max(1u, extent.width);
    extent.height = std::max(1u, extent.height);

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = chosen.format;
    createInfo.imageColorSpace = chosen.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_) != VK_SUCCESS) {
        SPLATSCAN_LOGE("创建交换链失败");
        return false;
    }
    swapchainFormat_ = chosen.format;
    swapchainExtent_ = extent;

    uint32_t actualCount = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &actualCount, nullptr);
    swapchainImages_.resize(actualCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &actualCount, swapchainImages_.data());

    swapchainViews_.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchainImages_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchainFormat_;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &viewInfo, nullptr, &swapchainViews_[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchainFormat_;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = 1;
    createInfo.pAttachments = &colorAttachment;
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;
    createInfo.dependencyCount = 1;
    createInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device_, &createInfo, nullptr, &renderPass_) != VK_SUCCESS) {
        SPLATSCAN_LOGE("创建渲染通道失败");
        return false;
    }
    return true;
}

bool VulkanRenderer::createFramebuffers() {
    framebuffers_.resize(swapchainViews_.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < swapchainViews_.size(); ++i) {
        VkFramebufferCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        createInfo.renderPass = renderPass_;
        createInfo.attachmentCount = 1;
        createInfo.pAttachments = &swapchainViews_[i];
        createInfo.width = swapchainExtent_.width;
        createInfo.height = swapchainExtent_.height;
        createInfo.layers = 1;
        if (vkCreateFramebuffer(device_, &createInfo, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::createCommands() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = commandPool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &allocateInfo, &commandBuffer_) != VK_SUCCESS) return false;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device_, &fenceInfo, nullptr, &fence_) != VK_SUCCESS) return false;

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable_) != VK_SUCCESS) {
        return false;
    }
    if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinished_) != VK_SUCCESS) {
        return false;
    }
    return true;
}

bool VulkanRenderer::createStorageImage() {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = static_cast<uint32_t>(renderWidth_);
    imageInfo.extent.height = static_cast<uint32_t>(renderHeight_);
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &imageInfo, nullptr, &storageImage_) != VK_SUCCESS) {
        SPLATSCAN_LOGE("创建存储图像失败");
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, storageImage_, &requirements);

    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);

    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const bool supported = (requirements.memoryTypeBits & (1u << i)) != 0;
        const bool deviceLocal =
            (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        if (supported && deviceLocal) {
            memoryType = i;
            break;
        }
    }
    if (memoryType == UINT32_MAX) return false;

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(device_, &allocateInfo, nullptr, &storageMemory_) != VK_SUCCESS) return false;
    if (vkBindImageMemory(device_, storageImage_, storageMemory_, 0) != VK_SUCCESS) return false;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = storageImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &viewInfo, nullptr, &storageView_) != VK_SUCCESS) return false;

    // 一次性转到通用布局，之后一直保持，渲染通道内再由屏障切到只读。
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &beginInfo);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = storageImage_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);

    vkEndCommandBuffer(commandBuffer_);
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer_;
    vkQueueSubmit(queue_, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    return true;
}

bool VulkanRenderer::createSampler() {
    VkSamplerCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    createInfo.magFilter = VK_FILTER_LINEAR;
    createInfo.minFilter = VK_FILTER_LINEAR;
    createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(device_, &createInfo, nullptr, &sampler_) != VK_SUCCESS) return false;
    return true;
}

bool VulkanRenderer::createDescriptors() {
    VkDescriptorSetLayoutBinding computeBindings[4]{};
    for (int i = 0; i < 3; ++i) {
        computeBindings[i].binding = static_cast<uint32_t>(i);
        computeBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        computeBindings[i].descriptorCount = 1;
        computeBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    computeBindings[3].binding = 3;
    computeBindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    computeBindings[3].descriptorCount = 1;
    computeBindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo computeLayoutInfo{};
    computeLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    computeLayoutInfo.bindingCount = 4;
    computeLayoutInfo.pBindings = computeBindings;
    if (vkCreateDescriptorSetLayout(device_, &computeLayoutInfo, nullptr, &computeSetLayout_) !=
        VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetLayoutBinding graphicsBinding{};
    graphicsBinding.binding = 0;
    graphicsBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    graphicsBinding.descriptorCount = 1;
    graphicsBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo graphicsLayoutInfo{};
    graphicsLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    graphicsLayoutInfo.bindingCount = 1;
    graphicsLayoutInfo.pBindings = &graphicsBinding;
    if (vkCreateDescriptorSetLayout(device_, &graphicsLayoutInfo, nullptr, &graphicsSetLayout_) !=
        VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[0].descriptorCount = 6;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[1].descriptorCount = 1;

    VkDescriptorPoolSize samplerSize{};
    samplerSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerSize.descriptorCount = 1;

    VkDescriptorPoolSize allSizes[3] = {sizes[0], sizes[1], samplerSize};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = allSizes;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo computeAllocate{};
    computeAllocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    computeAllocate.descriptorPool = descriptorPool_;
    computeAllocate.descriptorSetCount = 1;
    computeAllocate.pSetLayouts = &computeSetLayout_;
    if (vkAllocateDescriptorSets(device_, &computeAllocate, &computeSet_) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo graphicsAllocate{};
    graphicsAllocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    graphicsAllocate.descriptorPool = descriptorPool_;
    graphicsAllocate.descriptorSetCount = 1;
    graphicsAllocate.pSetLayouts = &graphicsSetLayout_;
    if (vkAllocateDescriptorSets(device_, &graphicsAllocate, &graphicsSet_) != VK_SUCCESS) return false;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = storageView_;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.sampler = sampler_;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = graphicsSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return true;
}

bool VulkanRenderer::createPipelines() {
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(uint32_t) * 4;

    VkPipelineLayoutCreateInfo computeLayout{};
    computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    computeLayout.setLayoutCount = 1;
    computeLayout.pSetLayouts = &computeSetLayout_;
    computeLayout.pushConstantRangeCount = 1;
    computeLayout.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device_, &computeLayout, nullptr, &computePipelineLayout_) !=
        VK_SUCCESS) {
        return false;
    }

    VkPipelineLayoutCreateInfo graphicsLayout{};
    graphicsLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    graphicsLayout.setLayoutCount = 1;
    graphicsLayout.pSetLayouts = &graphicsSetLayout_;
    if (vkCreatePipelineLayout(device_, &graphicsLayout, nullptr, &graphicsPipelineLayout_) !=
        VK_SUCCESS) {
        return false;
    }

    size_t computeSize = 0;
    const uint32_t* computeCode = shaders::splatCompute(&computeSize);
    VkShaderModuleCreateInfo computeModuleInfo{};
    computeModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    computeModuleInfo.codeSize = computeSize;
    computeModuleInfo.pCode = computeCode;
    VkShaderModule computeModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &computeModuleInfo, nullptr, &computeModule) != VK_SUCCESS) {
        return false;
    }

    VkComputePipelineCreateInfo computeInfo{};
    computeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computeInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    computeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    computeInfo.stage.module = computeModule;
    computeInfo.stage.pName = "main";
    computeInfo.layout = computePipelineLayout_;
    const VkResult computeResult =
        vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &computeInfo, nullptr, &computePipeline_);
    vkDestroyShaderModule(device_, computeModule, nullptr);
    if (computeResult != VK_SUCCESS) {
        SPLATSCAN_LOGE("创建计算管线失败");
        return false;
    }

    size_t vertexSize = 0;
    size_t fragmentSize = 0;
    const uint32_t* vertexCode = shaders::blitVertex(&vertexSize);
    const uint32_t* fragmentCode = shaders::blitFragment(&fragmentSize);

    VkShaderModuleCreateInfo vertexModuleInfo{};
    vertexModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertexModuleInfo.codeSize = vertexSize;
    vertexModuleInfo.pCode = vertexCode;
    VkShaderModule vertexModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &vertexModuleInfo, nullptr, &vertexModule) != VK_SUCCESS) {
        return false;
    }

    VkShaderModuleCreateInfo fragmentModuleInfo{};
    fragmentModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragmentModuleInfo.codeSize = fragmentSize;
    fragmentModuleInfo.pCode = fragmentCode;
    VkShaderModule fragmentModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &fragmentModuleInfo, nullptr, &fragmentModule) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vertexModule, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo graphicsInfo{};
    graphicsInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    graphicsInfo.stageCount = 2;
    graphicsInfo.pStages = stages;
    graphicsInfo.pVertexInputState = &vertexInput;
    graphicsInfo.pInputAssemblyState = &inputAssembly;
    graphicsInfo.pViewportState = &viewportState;
    graphicsInfo.pRasterizationState = &rasterizer;
    graphicsInfo.pMultisampleState = &multisample;
    graphicsInfo.pColorBlendState = &colorBlend;
    graphicsInfo.pDynamicState = &dynamicState;
    graphicsInfo.layout = graphicsPipelineLayout_;
    graphicsInfo.renderPass = renderPass_;
    graphicsInfo.subpass = 0;

    const VkResult graphicsResult =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &graphicsInfo, nullptr, &graphicsPipeline_);
    vkDestroyShaderModule(device_, fragmentModule, nullptr);
    vkDestroyShaderModule(device_, vertexModule, nullptr);
    if (graphicsResult != VK_SUCCESS) {
        SPLATSCAN_LOGE("创建图形管线失败");
        return false;
    }
    return true;
}

bool VulkanRenderer::createBuffers() {
    return ensureBufferCapacity(4096, 4096 * 2, 4096 * 8);
}

bool VulkanRenderer::ensureBufferCapacity(size_t splatCount, size_t rangeCount, size_t itemCount) {
    auto createBuffer = [this](VkDeviceSize size, VkBuffer* buffer, VkDeviceMemory* memory) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bufferInfo, nullptr, buffer) != VK_SUCCESS) return false;

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, *buffer, &requirements);

        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);

        uint32_t type = UINT32_MAX;
        const VkMemoryPropertyFlags wanted =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            const bool supported = (requirements.memoryTypeBits & (1u << i)) != 0;
            if (supported && (memoryProperties.memoryTypes[i].propertyFlags & wanted) == wanted) {
                type = i;
                break;
            }
        }
        if (type == UINT32_MAX) return false;

        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = type;
        if (vkAllocateMemory(device_, &allocateInfo, nullptr, memory) != VK_SUCCESS) return false;
        return vkBindBufferMemory(device_, *buffer, *memory, 0) == VK_SUCCESS;
    };

    if (splatCount > splatCapacity_) {
        const size_t wanted = std::max(splatCount, splatCapacity_ * 2);
        if (splatBuffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, splatBuffer_, nullptr);
            vkFreeMemory(device_, splatMemory_, nullptr);
            splatBuffer_ = VK_NULL_HANDLE;
            splatMemory_ = VK_NULL_HANDLE;
        }
        if (!createBuffer(wanted * sizeof(SplatGpu), &splatBuffer_, &splatMemory_)) return false;
        splatCapacity_ = wanted;
    }
    if (rangeCount > rangeCapacity_) {
        const size_t wanted = std::max(rangeCount, rangeCapacity_ * 2);
        if (rangeBuffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, rangeBuffer_, nullptr);
            vkFreeMemory(device_, rangeMemory_, nullptr);
            rangeBuffer_ = VK_NULL_HANDLE;
            rangeMemory_ = VK_NULL_HANDLE;
        }
        if (!createBuffer(wanted * sizeof(uint32_t), &rangeBuffer_, &rangeMemory_)) return false;
        rangeCapacity_ = wanted;
    }
    if (itemCount > itemCapacity_) {
        const size_t wanted = std::max(itemCount, itemCapacity_ * 2);
        if (itemBuffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, itemBuffer_, nullptr);
            vkFreeMemory(device_, itemMemory_, nullptr);
            itemBuffer_ = VK_NULL_HANDLE;
            itemMemory_ = VK_NULL_HANDLE;
        }
        if (!createBuffer(wanted * sizeof(uint32_t), &itemBuffer_, &itemMemory_)) return false;
        itemCapacity_ = wanted;
    }
    return true;
}

void VulkanRenderer::upload(const TileBundle& bundle) {
    auto writeBuffer = [this](VkDeviceMemory memory, const void* data, size_t bytes) {
        void* mapped = nullptr;
        if (vkMapMemory(device_, memory, 0, bytes, 0, &mapped) != VK_SUCCESS) return;
        std::memcpy(mapped, data, bytes);
        vkUnmapMemory(device_, memory);
    };

    if (!bundle.splats.empty()) {
        writeBuffer(splatMemory_, bundle.splats.data(), bundle.splats.size() * sizeof(SplatGpu));
    }
    if (!bundle.tileRanges.empty()) {
        writeBuffer(rangeMemory_, bundle.tileRanges.data(),
                    bundle.tileRanges.size() * sizeof(uint32_t));
    }
    if (!bundle.tileItems.empty()) {
        writeBuffer(itemMemory_, bundle.tileItems.data(),
                    bundle.tileItems.size() * sizeof(uint32_t));
    }

    VkDescriptorBufferInfo splatInfo{};
    splatInfo.buffer = splatBuffer_;
    splatInfo.offset = 0;
    splatInfo.range = bundle.splats.empty() ? sizeof(SplatGpu)
                                            : bundle.splats.size() * sizeof(SplatGpu);

    VkDescriptorBufferInfo rangeInfo{};
    rangeInfo.buffer = rangeBuffer_;
    rangeInfo.offset = 0;
    rangeInfo.range = bundle.tileRanges.empty() ? sizeof(uint32_t)
                                                : bundle.tileRanges.size() * sizeof(uint32_t);

    VkDescriptorBufferInfo itemInfo{};
    itemInfo.buffer = itemBuffer_;
    itemInfo.offset = 0;
    itemInfo.range = bundle.tileItems.empty() ? sizeof(uint32_t)
                                              : bundle.tileItems.size() * sizeof(uint32_t);

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = storageView_;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[4]{};
    for (int i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = computeSet_;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    writes[0].pBufferInfo = &splatInfo;
    writes[1].pBufferInfo = &rangeInfo;
    writes[2].pBufferInfo = &itemInfo;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = computeSet_;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[3].pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);
}

bool VulkanRenderer::renderFrame(const TileBundle& bundle) {
    if (!ready_ || bundle.empty()) return false;
    if (!ensureBufferCapacity(bundle.splats.size(), bundle.tileRanges.size(),
                              bundle.tileItems.size())) {
        return false;
    }
    upload(bundle);

    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &fence_);

    uint32_t imageIndex = 0;
    const VkResult acquire =
        vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_, VK_NULL_HANDLE, &imageIndex);
    if (acquire != VK_SUCCESS) {
        return false;
    }

    vkResetCommandBuffer(commandBuffer_, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &beginInfo);

    // 计算：逐 tile 做高斯泼溅
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline_);
    vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayout_,
                            0, 1, &computeSet_, 0, nullptr);
    const uint32_t pushConstants[4] = {
        static_cast<uint32_t>(renderWidth_),
        static_cast<uint32_t>(renderHeight_),
        static_cast<uint32_t>(bundle.tilesX),
        static_cast<uint32_t>(kTileSize),
    };
    vkCmdPushConstants(commandBuffer_, computePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(pushConstants), pushConstants);
    vkCmdDispatch(commandBuffer_, static_cast<uint32_t>(bundle.tilesX),
                  static_cast<uint32_t>(bundle.tilesY), 1);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = storageImage_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);

    VkClearValue clearValue{};
    clearValue.color.float32[0] = 0.06f;
    clearValue.color.float32[1] = 0.06f;
    clearValue.color.float32[2] = 0.08f;
    clearValue.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = renderPass_;
    passInfo.framebuffer = framebuffers_[imageIndex];
    passInfo.renderArea.offset = {0, 0};
    passInfo.renderArea.extent = swapchainExtent_;
    passInfo.clearValueCount = 1;
    passInfo.pClearValues = &clearValue;
    vkCmdBeginRenderPass(commandBuffer_, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchainExtent_.width);
    viewport.height = static_cast<float>(swapchainExtent_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer_, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchainExtent_;
    vkCmdSetScissor(commandBuffer_, 0, 1, &scissor);

    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_);
    vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelineLayout_,
                            0, 1, &graphicsSet_, 0, nullptr);
    vkCmdDraw(commandBuffer_, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer_);

    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);

    vkEndCommandBuffer(commandBuffer_);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &imageAvailable_;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer_;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderFinished_;
    if (vkQueueSubmit(queue_, 1, &submitInfo, fence_) != VK_SUCCESS) {
        return false;
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinished_;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;
    const VkResult present = vkQueuePresentKHR(queue_, &presentInfo);
    if (present != VK_SUCCESS && present != VK_SUBOPTIMAL_KHR) {
        SPLATSCAN_LOGE("呈现失败：%d", static_cast<int>(present));
        return false;
    }
    return true;
}

void VulkanRenderer::setRenderSize(int32_t renderWidth, int32_t renderHeight) {
    const int32_t width = std::max(16, renderWidth);
    const int32_t height = std::max(16, renderHeight);
    if (width == renderWidth_ && height == renderHeight_) return;
    renderWidth_ = width;
    renderHeight_ = height;
    if (!ready_) return;

    vkDeviceWaitIdle(device_);
    if (storageView_ != VK_NULL_HANDLE) vkDestroyImageView(device_, storageView_, nullptr);
    if (storageImage_ != VK_NULL_HANDLE) vkDestroyImage(device_, storageImage_, nullptr);
    if (storageMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_, storageMemory_, nullptr);
    storageView_ = VK_NULL_HANDLE;
    storageImage_ = VK_NULL_HANDLE;
    storageMemory_ = VK_NULL_HANDLE;
    createStorageImage();
    createDescriptors();
}

void VulkanRenderer::destroySwapchainResources() {
    for (VkFramebuffer framebuffer : framebuffers_) {
        if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    framebuffers_.clear();
    for (VkImageView view : swapchainViews_) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device_, view, nullptr);
    }
    swapchainViews_.clear();
    swapchainImages_.clear();
}

void VulkanRenderer::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);

        destroySwapchainResources();
        if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;

        auto destroyBuffer = [this](VkBuffer& buffer, VkDeviceMemory& memory) {
            if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, buffer, nullptr);
            if (memory != VK_NULL_HANDLE) vkFreeMemory(device_, memory, nullptr);
            buffer = VK_NULL_HANDLE;
            memory = VK_NULL_HANDLE;
        };
        destroyBuffer(splatBuffer_, splatMemory_);
        destroyBuffer(rangeBuffer_, rangeMemory_);
        destroyBuffer(itemBuffer_, itemMemory_);

        if (storageView_ != VK_NULL_HANDLE) vkDestroyImageView(device_, storageView_, nullptr);
        if (storageImage_ != VK_NULL_HANDLE) vkDestroyImage(device_, storageImage_, nullptr);
        if (storageMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_, storageMemory_, nullptr);
        storageView_ = VK_NULL_HANDLE;
        storageImage_ = VK_NULL_HANDLE;
        storageMemory_ = VK_NULL_HANDLE;

        if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
        if (descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (computePipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, computePipeline_, nullptr);
        if (graphicsPipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
        if (computePipelineLayout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, computePipelineLayout_, nullptr);
        }
        if (graphicsPipelineLayout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, graphicsPipelineLayout_, nullptr);
        }
        if (computeSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, computeSetLayout_, nullptr);
        }
        if (graphicsSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, graphicsSetLayout_, nullptr);
        }
        if (renderPass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, renderPass_, nullptr);
        if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
        if (imageAvailable_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, imageAvailable_, nullptr);
        if (renderFinished_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, renderFinished_, nullptr);
        if (commandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, commandPool_, nullptr);

        vkDestroyDevice(device_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    commandPool_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    imageAvailable_ = VK_NULL_HANDLE;
    renderFinished_ = VK_NULL_HANDLE;
    renderPass_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    computePipeline_ = VK_NULL_HANDLE;
    graphicsPipeline_ = VK_NULL_HANDLE;
    computePipelineLayout_ = VK_NULL_HANDLE;
    graphicsPipelineLayout_ = VK_NULL_HANDLE;
    computeSetLayout_ = VK_NULL_HANDLE;
    graphicsSetLayout_ = VK_NULL_HANDLE;
    splatCapacity_ = 0;
    rangeCapacity_ = 0;
    itemCapacity_ = 0;

    if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    ready_ = false;
}

}  // namespace splatscan