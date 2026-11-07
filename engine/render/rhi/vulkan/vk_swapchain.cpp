#include "core/log.h"
#include "core/profile.h"
#include "platform/window.h"
#include "render/rhi/vulkan/vk_device.h"

#include <algorithm>
#include <cstdio>

namespace de::rhi {

namespace {

const char* present_mode_name(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR:
        return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return "fifo relaxed";
    default:
        return "other";
    }
}

// Picks the swapchain format. Returns the query's result (fails only when the surface is lost);
// format stays UNDEFINED when the surface offers nothing usable.
VkResult choose_surface_format(VkPhysicalDevice physical_device, VkSurfaceKHR surface, VkSurfaceFormatKHR& format) {
    format = {.format = VK_FORMAT_UNDEFINED, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    uint32_t count = 0;
    VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, nullptr);
    if (result != VK_SUCCESS) {
        return result;
    }
    core::Array<VkSurfaceFormatKHR> formats(core::heap_allocator(core::MemoryTag::Render));
    formats.resize(count);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, formats.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return result;
    }
    // sRGB targets, so shaders write linear color and the hardware encodes it.
    for (const VkFormat wanted : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}) {
        for (uint32_t i = 0; i < count; ++i) {
            if (formats[i].format == wanted && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                format = formats[i];
                return VK_SUCCESS;
            }
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (from_vk_format(formats[i].format) != Format::Unknown &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            format = formats[i];
            DE_LOG_WARN("render", "the window offers no sRGB format: presenting %s, colors will look too dark",
                        format_info(from_vk_format(format.format)).name);
            return VK_SUCCESS;
        }
    }
    return VK_SUCCESS;
}

VkPresentModeKHR choose_present_mode(VkPhysicalDevice physical_device, VkSurfaceKHR surface, bool vsync) {
    if (vsync) {
        return VK_PRESENT_MODE_FIFO_KHR; // always supported
    }
    VkPresentModeKHR modes[16] = {};
    uint32_t count = 16;
    const VkResult result = vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, modes);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    // Mailbox: no tearing and no waiting. Immediate tears but never waits either.
    for (const VkPresentModeKHR wanted : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
        if (std::find(modes, modes + count, wanted) != modes + count) {
            return wanted;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

} // namespace

core::Result<void> DeviceImpl::create_surface() {
    auto created = platform::create_vulkan_surface(*window, instance);
    if (!created) {
        return core::fail(created.error());
    }
    surface = reinterpret_cast<VkSurfaceKHR>(*created); // NOLINT(performance-no-int-to-ptr): a Vulkan handle
    return {};
}

core::Result<void> DeviceImpl::recreate_surface() {
    DE_PROFILE_ZONE("rhi recreate surface");
    DE_LOG_WARN("render", "the window surface was lost: recreating it");
    DE_VK_CHECK(vkDeviceWaitIdle(device));
    destroy_swapchain();
    vkDestroySurfaceKHR(instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
    if (auto created = create_surface(); !created) {
        return created; // still lost: try again next frame
    }
    surface_lost = false;
    if (!graphics_queue_can_present()) {
        DE_FATAL("render", "the new window surface can't be presented from the graphics queue");
    }
    return {};
}

bool DeviceImpl::graphics_queue_can_present() const {
    VkBool32 supported = VK_FALSE;
    const VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(
        physical_device, queue_families[static_cast<size_t>(QueueType::Graphics)], surface, &supported);
    return result == VK_SUCCESS && supported == VK_TRUE;
}

bool DeviceImpl::swapchain_needs_recreation() const {
    return swapchain.swapchain == VK_NULL_HANDLE || swapchain_out_of_date ||
           (swapchain_suboptimal && !ignore_suboptimal) || swapchain.vsync != want_vsync ||
           swapchain.requested_width != window->pixel_width() || swapchain.requested_height != window->pixel_height();
}

core::Result<void> DeviceImpl::create_swapchain() {
    DE_PROFILE_ZONE("rhi create swapchain");
    const bool recreating = swapchain.swapchain != VK_NULL_HANDLE;
    const uint32_t window_width = window->pixel_width();
    const uint32_t window_height = window->pixel_height();
    // Rebuilt for being suboptimal and nothing else: if the new one is suboptimal too, keep it.
    const bool only_suboptimal = recreating && swapchain_suboptimal && !swapchain_out_of_date &&
                                 swapchain.vsync == want_vsync && swapchain.requested_width == window_width &&
                                 swapchain.requested_height == window_height;
    if (recreating) {
        // Rare (resizes, mode changes): a full stall keeps it simple and safe.
        DE_VK_CHECK(vkDeviceWaitIdle(device));
    }

    VkSurfaceCapabilitiesKHR caps{};
    VkSurfaceFormatKHR surface_format{};
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &caps);
    if (result == VK_SUCCESS) {
        result = choose_surface_format(physical_device, surface, surface_format);
    }
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        surface_lost = true;
        return core::fail(core::ErrorCode::Unsupported, "the window surface was lost");
    }
    DE_VK_CHECK(result);
    if (surface_format.format == VK_FORMAT_UNDEFINED) {
        return core::fail(core::ErrorCode::Unsupported, "the window surface offers no usable format");
    }
    const VkPresentModeKHR present_mode = choose_present_mode(physical_device, surface, want_vsync);

    // The surface decides the size when it knows it; otherwise (Wayland) the window does.
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(window_width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(window_height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        return core::fail(core::ErrorCode::Unsupported, "the window has no area to present to");
    }

    // One more than the minimum, so the CPU rarely waits for an image.
    if (caps.minImageCount > kMaxSwapchainImages) {
        return core::fail(core::ErrorCode::Unsupported, "the window needs more swapchain images than supported");
    }
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0) {
        image_count = std::min(image_count, caps.maxImageCount);
    }
    image_count = std::min(image_count, kMaxSwapchainImages);

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    TextureUsage texture_usage = TextureUsage::RenderTarget;
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        texture_usage |= TextureUsage::CopyDst;
    }
    if (want_swapchain_copy_src && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        texture_usage |= TextureUsage::CopySrc;
    }
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((caps.supportedCompositeAlpha & composite) == 0) {
        for (const VkCompositeAlphaFlagBitsKHR alpha :
             {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
            if ((caps.supportedCompositeAlpha & alpha) != 0) {
                composite = alpha;
                break;
            }
        }
    }

    VkSwapchainKHR old_swapchain = swapchain.swapchain;
    const VkSwapchainCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = composite,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = old_swapchain,
    };
    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    result = vkCreateSwapchainKHR(device, &info, nullptr, &new_swapchain);
    // The old swapchain is retired either way: release it and its images now.
    destroy_swapchain();
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        surface_lost = true;
    }
    if (result != VK_SUCCESS) {
        DE_LOG_ERROR("render", "swapchain creation failed: %s", vk_result_name(result));
        return core::fail(core::ErrorCode::Unsupported, "the swapchain could not be created");
    }

    // Drivers may make more images than asked for.
    uint32_t count = 0;
    DE_VK_CHECK(vkGetSwapchainImagesKHR(device, new_swapchain, &count, nullptr));
    if (count > kMaxSwapchainImages) {
        vkDestroySwapchainKHR(device, new_swapchain, nullptr);
        DE_LOG_ERROR("render", "the driver made %u swapchain images; at most %u are supported", count,
                     kMaxSwapchainImages);
        return core::fail(core::ErrorCode::Unsupported, "the swapchain has too many images");
    }
    VkImage images[kMaxSwapchainImages] = {};
    DE_VK_CHECK(vkGetSwapchainImagesKHR(device, new_swapchain, &count, images));

    swapchain.swapchain = new_swapchain;
    swapchain.format = from_vk_format(surface_format.format);
    swapchain.vk_format = surface_format.format;
    swapchain.width = extent.width;
    swapchain.height = extent.height;
    swapchain.requested_width = window_width;
    swapchain.requested_height = window_height;
    swapchain.vsync = want_vsync;
    swapchain.image_count = count;
    swapchain.on_refresh = present_mode == VK_PRESENT_MODE_FIFO_KHR;
    swapchain_out_of_date = false;
    swapchain_suboptimal = false;
    ignore_suboptimal = only_suboptimal;

    const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < count; ++i) {
        Texture texture{
            .image = images[i],
            .aspects = VK_IMAGE_ASPECT_COLOR_BIT,
            .desc = {.width = extent.width,
                     .height = extent.height,
                     .format = swapchain.format,
                     .usage = texture_usage},
            .swapchain = true,
        };
        const VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = surface_format.format,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
        };
        DE_VK_CHECK(vkCreateImageView(device, &view_info, nullptr, &texture.view));
        char name[32];
        std::snprintf(name, sizeof(name), "swapchain %u", i);
        set_debug_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(texture.image), name);
        set_debug_name(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(texture.view), name);
        swapchain.textures[i] = textures.insert(texture);
        if (present_semaphores[i] == VK_NULL_HANDLE) {
            DE_VK_CHECK(vkCreateSemaphore(device, &semaphore_info, nullptr, &present_semaphores[i]));
        }
    }

    if (recreating) {
        ++swapchain_recreations;
    }
    DE_LOG_INFO("render", "swapchain %ux%u %s, %s, %u images", extent.width, extent.height,
                format_info(swapchain.format).name, present_mode_name(present_mode), count);
    return {};
}

// Only once the GPU is idle. The present semaphores stay: see DeviceImpl::present_semaphores.
void DeviceImpl::destroy_swapchain() {
    for (uint32_t i = 0; i < swapchain.image_count; ++i) {
        if (const Texture* texture = textures.get(swapchain.textures[i])) {
            vkDestroyImageView(device, texture->view, nullptr);
            textures.remove(swapchain.textures[i]);
        }
    }
    if (swapchain.swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain.swapchain, nullptr);
    }
    swapchain = Swapchain{};
}

TextureHandle Device::acquire_swapchain_texture() {
    DE_PROFILE_ZONE("rhi acquire");
    DeviceImpl& impl = *impl_;
    DE_ASSERT_MSG(impl.in_frame, "acquire_swapchain_texture outside begin_frame/end_frame");
    DE_ASSERT_MSG(!impl.acquired, "acquire_swapchain_texture called twice in one frame");
    if (impl.window == nullptr || impl.window->minimized() || impl.window->pixel_width() == 0 ||
        impl.window->pixel_height() == 0) {
        return {};
    }

    const FrameSlot& slot = impl.current_slot();
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (impl.surface_lost && !impl.recreate_surface()) {
            return {};
        }
        if (impl.swapchain_needs_recreation() && !impl.create_swapchain()) {
            if (impl.surface_lost) {
                continue; // recreate the surface and try once more
            }
            return {};
        }
        uint32_t index = 0;
        VkResult result = VK_SUCCESS;
        {
            DE_PROFILE_ZONE("wait for swapchain image");
            result = vkAcquireNextImageKHR(impl.device, impl.swapchain.swapchain, UINT64_MAX, slot.acquire_semaphore,
                                           VK_NULL_HANDLE, &index);
        }
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            // Suboptimal still signals the semaphore: use the image, rebuild next frame.
            impl.swapchain_suboptimal = impl.swapchain_suboptimal || result == VK_SUBOPTIMAL_KHR;
            impl.acquired = true;
            impl.acquired_image = index;
            return impl.swapchain.textures[index];
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            impl.swapchain_out_of_date = true; // rebuild and try once more
            continue;
        }
        if (result == VK_ERROR_SURFACE_LOST_KHR) {
            impl.surface_lost = true;
            continue;
        }
        DE_VK_CHECK(result);
    }
    return {};
}

} // namespace de::rhi
