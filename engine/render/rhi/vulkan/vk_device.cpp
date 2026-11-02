#include "render/rhi/vulkan/vk_device.h"

#include "core/allocator.h"
#include "core/log.h"
#include "core/profile.h"
#include "platform/window.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_vulkan.h>
#include <VkBootstrap.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>
#include <utility>

namespace de::rhi {

namespace {

// One Device per process: volk keeps one set of device-level function pointers.
std::atomic<bool> g_device_alive{false};

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data) {
    auto* device = static_cast<DeviceImpl*>(user_data);
    const char* id = data->pMessageIdName != nullptr ? data->pMessageIdName : "";
    const char* message = data->pMessage != nullptr ? data->pMessage : "";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        device->validation_errors.fetch_add(1, std::memory_order_relaxed);
        DE_LOG_ERROR("vulkan", "[%s] %s", id, message);
    } else {
        device->validation_warnings.fetch_add(1, std::memory_order_relaxed);
        DE_LOG_WARN("vulkan", "[%s] %s", id, message);
    }
    return VK_FALSE; // never abort the call
}

bool instance_layer_available(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    core::Array<VkLayerProperties> layers(core::heap_allocator(core::MemoryTag::Render));
    layers.resize(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.begin() + count,
                       [name](const VkLayerProperties& layer) { return std::strcmp(layer.layerName, name) == 0; });
}

bool instance_extension_available(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    core::Array<VkExtensionProperties> extensions(core::heap_allocator(core::MemoryTag::Render));
    extensions.resize(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    return std::any_of(extensions.begin(), extensions.begin() + count, [name](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

void copy_text(char (&dest)[256], const char* src) {
    const size_t length = std::min(std::strlen(src), sizeof(dest) - 1);
    std::memcpy(dest, src, length);
    dest[length] = '\0';
}

GpuVendor vendor_from_id(uint32_t vendor_id, VkPhysicalDeviceType type) {
    if (type == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        return GpuVendor::Software;
    }
    switch (vendor_id) {
    case 0x10DE:
        return GpuVendor::Nvidia;
    case 0x1002:
        return GpuVendor::Amd;
    case 0x8086:
        return GpuVendor::Intel;
    case 0x13B5:
        return GpuVendor::Arm;
    case 0x5143:
        return GpuVendor::Qualcomm;
    case 0x106B:
        return GpuVendor::Apple;
    default:
        return GpuVendor::Unknown;
    }
}

GpuType gpu_type_from_vk(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return GpuType::Discrete;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return GpuType::Integrated;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return GpuType::Virtual;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return GpuType::Cpu;
    default:
        return GpuType::Other;
    }
}

FormatSupport format_support_from_vk(VkFormatFeatureFlags features) {
    FormatSupport support = FormatSupport::None;
    const auto add = [&](VkFormatFeatureFlags bit, FormatSupport flag) {
        if ((features & bit) != 0) {
            support |= flag;
        }
    };
    add(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, FormatSupport::Sampled);
    add(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, FormatSupport::LinearFilter);
    add(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, FormatSupport::Storage);
    add(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, FormatSupport::RenderTarget);
    add(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, FormatSupport::Blend);
    add(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, FormatSupport::DepthStencil);
    add(VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, FormatSupport::CopySrc);
    add(VK_FORMAT_FEATURE_TRANSFER_DST_BIT, FormatSupport::CopyDst);
    return support;
}

void fill_capabilities(DeviceImpl& impl, bool ray_query) {
    Capabilities& caps = impl.capabilities;

    VkPhysicalDeviceVulkan12Properties props12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceVulkan11Properties props11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
                                               .pNext = &props12};
    VkPhysicalDeviceProperties2 props{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &props11};
    vkGetPhysicalDeviceProperties2(impl.physical_device, &props);
    const VkPhysicalDeviceProperties& p = props.properties;
    const VkPhysicalDeviceLimits& limits = p.limits;

    copy_text(caps.device_name, p.deviceName);
    copy_text(caps.driver_name, props12.driverName);
    copy_text(caps.driver_info, props12.driverInfo);
    caps.vendor = vendor_from_id(p.vendorID, p.deviceType);
    caps.type = gpu_type_from_vk(p.deviceType);
    caps.vendor_id = p.vendorID;
    caps.device_id = p.deviceID;
    caps.api_version_major = VK_API_VERSION_MAJOR(p.apiVersion);
    caps.api_version_minor = VK_API_VERSION_MINOR(p.apiVersion);

    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(impl.physical_device, &memory);
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            caps.device_local_memory_bytes =
                std::max<uint64_t>(caps.device_local_memory_bytes, memory.memoryHeaps[i].size);
        }
    }

    const auto family = [&](QueueType queue) { return impl.queue_families[static_cast<size_t>(queue)]; };
    caps.async_compute = family(QueueType::Compute) != family(QueueType::Graphics);
    caps.dedicated_transfer = family(QueueType::Transfer) != family(QueueType::Graphics) &&
                              family(QueueType::Transfer) != family(QueueType::Compute);
    caps.ray_query = ray_query;

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(impl.physical_device, &family_count, nullptr);
    core::Array<VkQueueFamilyProperties> families(core::heap_allocator(core::MemoryTag::Render));
    families.resize(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(impl.physical_device, &family_count, families.data());
    caps.timestamps = limits.timestampComputeAndGraphics == VK_TRUE && limits.timestampPeriod > 0.0f &&
                      families[family(QueueType::Graphics)].timestampValidBits > 0;
    caps.timestamp_period_ns = limits.timestampPeriod;

    caps.max_texture_size_2d = limits.maxImageDimension2D;
    caps.max_texture_size_3d = limits.maxImageDimension3D;
    caps.max_texture_size_cube = limits.maxImageDimensionCube;
    caps.max_texture_layers = limits.maxImageArrayLayers;
    caps.max_bindless_textures = props12.maxDescriptorSetUpdateAfterBindSampledImages;
    caps.max_bindless_buffers = props12.maxDescriptorSetUpdateAfterBindStorageBuffers;
    caps.max_push_constant_bytes = limits.maxPushConstantsSize;
    for (int i = 0; i < 3; ++i) {
        caps.max_compute_group_size[i] = limits.maxComputeWorkGroupSize[i];
    }
    caps.max_compute_group_invocations = limits.maxComputeWorkGroupInvocations;
    caps.subgroup_size = props11.subgroupSize;
    caps.uniform_buffer_alignment = limits.minUniformBufferOffsetAlignment;
    caps.storage_buffer_alignment = limits.minStorageBufferOffsetAlignment;
    caps.copy_row_pitch_alignment = limits.optimalBufferCopyRowPitchAlignment;
    caps.max_anisotropy = limits.maxSamplerAnisotropy;

    for (size_t i = 1; i < kFormatCount; ++i) {
        VkFormatProperties format_props{};
        vkGetPhysicalDeviceFormatProperties(impl.physical_device, to_vk_format(static_cast<Format>(i)), &format_props);
        caps.formats[i] = format_support_from_vk(format_props.optimalTilingFeatures);
    }
}

void destroy_impl(DeviceImpl* impl) {
    impl->~DeviceImpl();
    core::heap_allocator(core::MemoryTag::Render).deallocate(impl, sizeof(DeviceImpl), alignof(DeviceImpl));
}

} // namespace

DeviceImpl::DeviceImpl()
    : textures(core::heap_allocator(core::MemoryTag::Render)), buffers(core::heap_allocator(core::MemoryTag::Render)) {}

core::Result<void> DeviceImpl::init(const DeviceDesc& desc) {
    DE_PROFILE_ZONE("rhi create device");
    window = desc.window;
    want_vsync = desc.vsync;

    if (volkInitialize() != VK_SUCCESS) {
        return core::fail(core::ErrorCode::Unsupported, "no Vulkan loader on this system");
    }
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        return core::fail(core::ErrorCode::Unsupported, "the Vulkan loader is older than 1.3");
    }

    // --- Instance ---
    vkb::InstanceBuilder instance_builder(vkGetInstanceProcAddr);
    instance_builder.set_app_name(desc.app_name)
        .set_engine_name("DEengine")
        .require_api_version(1, 3, 0)
        .set_headless(window == nullptr);
    if (window != nullptr) {
        uint32_t count = 0;
        const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
        if (extensions == nullptr) {
            DE_LOG_ERROR("vulkan", "SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
            return core::fail(core::ErrorCode::Unsupported, "no Vulkan surface support for the window");
        }
        instance_builder.enable_extensions(count, extensions);
    }
    validation = desc.validation && instance_layer_available("VK_LAYER_KHRONOS_validation");
    if (desc.validation && !validation) {
        DE_LOG_WARN("vulkan", "validation requested but VK_LAYER_KHRONOS_validation is not installed");
    }
    debug_utils = instance_extension_available(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (validation) {
        instance_builder.enable_validation_layers(true)
            .set_debug_callback(debug_callback)
            .set_debug_callback_user_data_pointer(this)
            .set_debug_messenger_severity(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            .set_debug_messenger_type(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT);
        if (desc.sync_validation) {
            instance_builder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
        }
        debug_utils = true; // the layer provides it
    } else if (debug_utils) {
        // Object names and labels for RenderDoc and Nsight, even without validation.
        instance_builder.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    auto vkb_instance = instance_builder.build();
    if (!vkb_instance) {
        DE_LOG_ERROR("vulkan", "instance creation failed: %s (%s)", vkb_instance.error().message().c_str(),
                     vk_result_name(vkb_instance.vk_result()));
        return core::fail(core::ErrorCode::Unsupported, "Vulkan 1.3 instance creation failed");
    }
    instance = vkb_instance->instance;
    messenger = vkb_instance->debug_messenger;
    volkLoadInstanceOnly(instance);

    // --- Surface ---
    if (window != nullptr && !SDL_Vulkan_CreateSurface(window->native_handle(), instance, nullptr, &surface)) {
        DE_LOG_ERROR("vulkan", "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return core::fail(core::ErrorCode::Unsupported, "the window surface could not be created");
    }

    // --- Physical device: Vulkan 1.3 with the features docs/design.md requires ---
    const VkPhysicalDeviceFeatures features10{
        .multiDrawIndirect = VK_TRUE,
        .drawIndirectFirstInstance = VK_TRUE,
        .samplerAnisotropy = VK_TRUE,
        .textureCompressionBC = VK_TRUE,
        .shaderInt64 = VK_TRUE,
    };
    const VkPhysicalDeviceVulkan11Features features11{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .shaderDrawParameters = VK_TRUE,
    };
    const VkPhysicalDeviceVulkan12Features features12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .drawIndirectCount = VK_TRUE,
        .descriptorIndexing = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
        .shaderStorageBufferArrayNonUniformIndexing = VK_TRUE,
        .shaderStorageImageArrayNonUniformIndexing = VK_TRUE,
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingStorageImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE,
        .descriptorBindingUpdateUnusedWhilePending = VK_TRUE,
        .descriptorBindingPartiallyBound = VK_TRUE,
        .descriptorBindingVariableDescriptorCount = VK_TRUE,
        .runtimeDescriptorArray = VK_TRUE,
        .scalarBlockLayout = VK_TRUE,
        .hostQueryReset = VK_TRUE,
        .timelineSemaphore = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE,
    };
    const VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
        .maintenance4 = VK_TRUE,
    };

    vkb::PhysicalDeviceSelector selector(vkb_instance.value());
    selector.set_minimum_version(1, 3)
        .set_required_features(features10)
        .set_required_features_11(features11)
        .set_required_features_12(features12)
        .set_required_features_13(features13)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
        .allow_any_gpu_device_type(true)
        .require_present(surface != VK_NULL_HANDLE);
    if (surface != VK_NULL_HANDLE) {
        selector.set_surface(surface).add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    auto vkb_physical = selector.select();
    if (!vkb_physical) {
        DE_LOG_ERROR("vulkan", "no suitable GPU: %s", vkb_physical.error().message().c_str());
        for (const auto& reason : vkb_physical.detailed_failure_reasons()) {
            DE_LOG_ERROR("vulkan", "  %s", reason.c_str());
        }
        return core::fail(core::ErrorCode::Unsupported, "no GPU supports the Vulkan 1.3 features the engine needs");
    }
    physical_device = vkb_physical->physical_device;

    // Optional features.
    memory_budget = vkb_physical->enable_extension_if_present(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    bool ray_query = false;
    {
        const char* const rt_extensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                             VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                             VK_KHR_RAY_QUERY_EXTENSION_NAME};
        const VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
            .accelerationStructure = VK_TRUE,
        };
        const VkPhysicalDeviceRayQueryFeaturesKHR ray_query_features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
            .rayQuery = VK_TRUE,
        };
        ray_query = vkb_physical->enable_extensions_if_present(std::size(rt_extensions), rt_extensions) &&
                    vkb_physical->enable_extension_features_if_present(acceleration_features) &&
                    vkb_physical->enable_extension_features_if_present(ray_query_features);
    }

    // --- Logical device and queues ---
    auto vkb_device = vkb::DeviceBuilder(vkb_physical.value()).build();
    if (!vkb_device) {
        DE_LOG_ERROR("vulkan", "device creation failed: %s (%s)", vkb_device.error().message().c_str(),
                     vk_result_name(vkb_device.vk_result()));
        return core::fail(core::ErrorCode::Unsupported, "Vulkan device creation failed");
    }
    device = vkb_device->device;
    volkLoadDevice(device);

    const auto graphics = vkb_device->get_queue_and_index(vkb::QueueType::graphics);
    if (!graphics) {
        return core::fail(core::ErrorCode::Unsupported, "the GPU has no graphics queue");
    }
    const auto pick = [&](vkb::QueueType type) {
        if (auto dedicated = vkb_device->get_dedicated_queue_and_index(type)) {
            return dedicated.value();
        }
        if (auto separate = vkb_device->get_queue_and_index(type)) {
            return separate.value();
        }
        return graphics.value(); // the graphics queue does it all
    };
    const std::pair<VkQueue, uint32_t> queue_list[] = {graphics.value(), pick(vkb::QueueType::compute),
                                                       pick(vkb::QueueType::transfer)};
    for (size_t i = 0; i < std::size(queue_list); ++i) {
        queues[i] = queue_list[i].first;
        queue_families[i] = queue_list[i].second;
    }

    // --- Memory ---
    VmaAllocatorCreateInfo allocator_info{
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
                 (memory_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0U),
        .physicalDevice = physical_device,
        .device = device,
        .instance = instance,
        .vulkanApiVersion = VK_API_VERSION_1_3,
    };
    VmaVulkanFunctions vma_functions{};
    DE_VK_CHECK(vmaImportVulkanFunctionsFromVolk(&allocator_info, &vma_functions));
    allocator_info.pVulkanFunctions = &vma_functions;
    DE_VK_CHECK(vmaCreateAllocator(&allocator_info, &allocator));

    fill_capabilities(*this, ray_query);

    // --- Frame pacing ---
    VkSemaphoreTypeCreateInfo timeline_type{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    const VkSemaphoreCreateInfo timeline_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                              .pNext = &timeline_type};
    DE_VK_CHECK(vkCreateSemaphore(device, &timeline_info, nullptr, &frame_timeline));
    set_debug_name(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(frame_timeline), "frame timeline");

    const VkSemaphoreCreateInfo binary_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = queue_families[static_cast<size_t>(QueueType::Graphics)],
    };
    for (FrameSlot& slot : frames) {
        DE_VK_CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &slot.command_pool));
        DE_VK_CHECK(vkCreateSemaphore(device, &binary_info, nullptr, &slot.acquire_semaphore));
    }

    const Capabilities& caps = capabilities;
    DE_LOG_INFO("render", "GPU: %s (%s, %s), driver %s %s, Vulkan %u.%u, %.1f GB VRAM", caps.device_name,
                gpu_vendor_name(caps.vendor), gpu_type_name(caps.type), caps.driver_name, caps.driver_info,
                caps.api_version_major, caps.api_version_minor,
                static_cast<double>(caps.device_local_memory_bytes) / (1024.0 * 1024.0 * 1024.0));
    DE_LOG_INFO("render", "validation %s, async compute %s, dedicated transfer %s, ray query %s",
                validation ? (desc.sync_validation ? "on (with sync)" : "on") : "off",
                caps.async_compute ? "yes" : "no", caps.dedicated_transfer ? "yes" : "no",
                caps.ray_query ? "yes" : "no");

    // --- Swapchain ---
    if (window != nullptr && !window->minimized() && window->pixel_width() > 0 && window->pixel_height() > 0) {
        if (auto created = create_swapchain(); !created) {
            return created;
        }
    }
    return {};
}

void DeviceImpl::shutdown() {
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        destroy_swapchain();
        for (FrameSlot& slot : frames) {
            free_pending(slot.pending);
        }
        if (!textures.empty() || !buffers.empty()) {
            DE_LOG_WARN("render", "%zu textures and %zu buffers still alive at device shutdown", textures.size(),
                        buffers.size());
        }
        for (const Texture& texture : textures) {
            vkDestroyImageView(device, texture.view, nullptr);
            vmaDestroyImage(allocator, texture.image, texture.allocation);
        }
        textures.clear();
        for (const Buffer& buffer : buffers) {
            vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
        }
        buffers.clear();
        for (FrameSlot& slot : frames) {
            vkDestroyCommandPool(device, slot.command_pool, nullptr);
            vkDestroySemaphore(device, slot.acquire_semaphore, nullptr);
            slot = {};
        }
        vkDestroySemaphore(device, frame_timeline, nullptr);
        frame_timeline = VK_NULL_HANDLE;
        if (allocator != VK_NULL_HANDLE) {
            vmaDestroyAllocator(allocator);
            allocator = VK_NULL_HANDLE;
        }
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE) {
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        if (messenger != VK_NULL_HANDLE) {
            vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
            messenger = VK_NULL_HANDLE;
        }
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
}

Texture& DeviceImpl::texture(TextureHandle handle) {
    Texture* texture = textures.get(handle);
    DE_VERIFY_MSG(texture != nullptr, "stale or invalid TextureHandle");
    return *texture;
}

Buffer& DeviceImpl::buffer(BufferHandle handle) {
    Buffer* buffer = buffers.get(handle);
    DE_VERIFY_MSG(buffer != nullptr, "stale or invalid BufferHandle");
    return *buffer;
}

void DeviceImpl::set_debug_name(VkObjectType type, uint64_t object, const char* name) const {
    if (!debug_utils || name == nullptr || name[0] == '\0') {
        return;
    }
    const VkDebugUtilsObjectNameInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = type,
        .objectHandle = object,
        .pObjectName = name,
    };
    vkSetDebugUtilsObjectNameEXT(device, &info);
}

void DeviceImpl::free_pending(core::Array<PendingDestroy>& pending) {
    for (const PendingDestroy& entry : pending) {
        if (entry.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, entry.view, nullptr);
        }
        if (entry.image != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator, entry.image, entry.allocation);
        }
        if (entry.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, entry.buffer, entry.allocation);
        }
    }
    pending.clear();
}

CommandList DeviceImpl::begin_command_list(QueueType queue) {
    DE_ASSERT_MSG(in_frame, "begin_command_list outside begin_frame/end_frame");
    DE_ASSERT_MSG(queue == QueueType::Graphics, "only graphics command lists for now");
    static_cast<void>(queue);

    FrameSlot& slot = current_slot();
    DE_VERIFY_MSG(slot.list_count < kMaxCommandListsPerFrame, "too many command lists in one frame");
    const uint32_t index = slot.list_count++;
    if (index == slot.allocated_count) {
        const VkCommandBufferAllocateInfo info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = slot.command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        DE_VK_CHECK(vkAllocateCommandBuffers(device, &info, &slot.command_buffers[index]));
        ++slot.allocated_count;
    }

    CommandListImpl& list = slot.lists[index];
    list = CommandListImpl{.device = this, .cmd = slot.command_buffers[index]};
    const VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    DE_VK_CHECK(vkBeginCommandBuffer(list.cmd, &begin));
    return CommandList(&list);
}

// --- Device ---

core::Result<Device> Device::create(const DeviceDesc& desc) {
    bool expected = false;
    if (!g_device_alive.compare_exchange_strong(expected, true)) {
        DE_LOG_ERROR("render", "only one rhi::Device may exist at a time");
        return core::fail(core::ErrorCode::AlreadyExists, "only one rhi::Device may exist at a time");
    }
    void* memory = core::heap_allocator(core::MemoryTag::Render).allocate(sizeof(DeviceImpl), alignof(DeviceImpl));
    auto* impl = ::new (memory) DeviceImpl();
    if (auto result = impl->init(desc); !result) {
        impl->shutdown();
        destroy_impl(impl);
        g_device_alive.store(false);
        return core::fail(result.error());
    }
    return Device(impl);
}

Device::~Device() {
    if (impl_ != nullptr) {
        impl_->shutdown();
        destroy_impl(impl_);
        g_device_alive.store(false);
    }
}

Device::Device(Device&& other) noexcept : impl_(std::exchange(other.impl_, nullptr)) {}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        if (impl_ != nullptr) {
            impl_->shutdown();
            destroy_impl(impl_);
            g_device_alive.store(false);
        }
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}

const Capabilities& Device::capabilities() const {
    return impl_->capabilities;
}

bool Device::validation_enabled() const {
    return impl_->validation;
}

DeviceStats Device::stats() const {
    DeviceStats stats{
        .frame_index = impl_->frame_index,
        .live_textures = static_cast<uint32_t>(impl_->textures.size()),
        .live_buffers = static_cast<uint32_t>(impl_->buffers.size()),
        .swapchain_recreations = impl_->swapchain_recreations,
        .validation_errors = impl_->validation_errors.load(std::memory_order_relaxed),
        .validation_warnings = impl_->validation_warnings.load(std::memory_order_relaxed),
    };
    for (const FrameSlot& slot : impl_->frames) {
        stats.pending_destroys += static_cast<uint32_t>(slot.pending.size());
    }

    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(impl_->allocator, &memory);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(impl_->allocator, budgets);
    for (uint32_t i = 0; i < memory->memoryHeapCount; ++i) {
        if ((memory->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            stats.gpu_memory_used_bytes += budgets[i].usage;
            stats.gpu_memory_budget_bytes += budgets[i].budget;
        }
    }
    return stats;
}

// --- Resources ---

core::Result<TextureHandle> Device::create_texture(const TextureDesc& desc) {
    const Capabilities& caps = impl_->capabilities;
    const FormatInfo& info = format_info(desc.format);
    const bool cube = desc.dimension == TextureDimension::Cube;
    const bool volume = desc.dimension == TextureDimension::Texture3D;

    if (desc.format == Format::Unknown || desc.width == 0 || desc.height == 0 || desc.depth == 0 ||
        desc.mip_count == 0 || desc.layer_count == 0 || desc.usage == TextureUsage::None) {
        return core::fail(core::ErrorCode::InvalidArgument, "texture needs a format, a usage and non-zero sizes");
    }
    if ((volume && desc.layer_count != 1) || (!volume && desc.depth != 1) ||
        (cube && (desc.width != desc.height || desc.layer_count % 6 != 0))) {
        return core::fail(core::ErrorCode::InvalidArgument, "texture dimension doesn't match its sizes");
    }
    const uint32_t largest = std::max({desc.width, desc.height, desc.depth});
    uint32_t max_mips = 1;
    while ((largest >> max_mips) != 0) {
        ++max_mips;
    }
    if (desc.mip_count > max_mips) {
        return core::fail(core::ErrorCode::InvalidArgument, "more mips than the texture size allows");
    }
    const uint32_t max_size =
        volume ? caps.max_texture_size_3d : (cube ? caps.max_texture_size_cube : caps.max_texture_size_2d);
    if (largest > max_size || desc.layer_count > caps.max_texture_layers) {
        return core::fail(core::ErrorCode::Unsupported, "texture is larger than the GPU allows");
    }

    FormatSupport needed = FormatSupport::None;
    VkImageUsageFlags usage = 0;
    const auto use = [&](TextureUsage flag, VkImageUsageFlags vk_flag, FormatSupport support) {
        if (core::has_any(desc.usage, flag)) {
            usage |= vk_flag;
            needed |= support;
        }
    };
    use(TextureUsage::Sampled, VK_IMAGE_USAGE_SAMPLED_BIT, FormatSupport::Sampled);
    use(TextureUsage::Storage, VK_IMAGE_USAGE_STORAGE_BIT, FormatSupport::Storage);
    use(TextureUsage::RenderTarget, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, FormatSupport::RenderTarget);
    use(TextureUsage::DepthStencil, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, FormatSupport::DepthStencil);
    use(TextureUsage::CopySrc, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, FormatSupport::CopySrc);
    use(TextureUsage::CopyDst, VK_IMAGE_USAGE_TRANSFER_DST_BIT, FormatSupport::CopyDst);
    if (!caps.supports(desc.format, needed)) {
        DE_LOG_ERROR("render", "texture '%s': format %s doesn't support the requested usage on this GPU",
                     desc.debug_name != nullptr ? desc.debug_name : "", info.name);
        return core::fail(core::ErrorCode::Unsupported, "texture format doesn't support the requested usage");
    }

    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0U,
        .imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = to_vk_format(desc.format),
        .extent = {.width = desc.width, .height = desc.height, .depth = desc.depth},
        .mipLevels = desc.mip_count,
        .arrayLayers = desc.layer_count,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo allocation_info{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    Texture texture{.aspects = format_aspects(desc.format), .desc = desc};
    texture.desc.debug_name = nullptr;
    const VkResult result =
        vmaCreateImage(impl_->allocator, &image_info, &allocation_info, &texture.image, &texture.allocation, nullptr);
    if (result != VK_SUCCESS) {
        DE_LOG_ERROR("render", "texture '%s' (%ux%u %s): %s", desc.debug_name != nullptr ? desc.debug_name : "",
                     desc.width, desc.height, info.name, vk_result_name(result));
        return core::fail(core::ErrorCode::OutOfMemory, "GPU memory for the texture could not be allocated");
    }

    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    if (volume) {
        view_type = VK_IMAGE_VIEW_TYPE_3D;
    } else if (cube) {
        view_type = desc.layer_count > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    } else if (desc.layer_count > 1) {
        view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture.image,
        .viewType = view_type,
        .format = image_info.format,
        .subresourceRange = {.aspectMask = texture.aspects,
                             .baseMipLevel = 0,
                             .levelCount = VK_REMAINING_MIP_LEVELS,
                             .baseArrayLayer = 0,
                             .layerCount = VK_REMAINING_ARRAY_LAYERS},
    };
    DE_VK_CHECK(vkCreateImageView(impl_->device, &view_info, nullptr, &texture.view));

    impl_->set_debug_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(texture.image), desc.debug_name);
    impl_->set_debug_name(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(texture.view), desc.debug_name);
    if (desc.debug_name != nullptr) {
        vmaSetAllocationName(impl_->allocator, texture.allocation, desc.debug_name);
    }
    return impl_->textures.insert(texture);
}

core::Result<BufferHandle> Device::create_buffer(const BufferDesc& desc) {
    if (desc.size == 0 || desc.usage == BufferUsage::None) {
        return core::fail(core::ErrorCode::InvalidArgument, "buffer needs a size and a usage");
    }
    VkBufferUsageFlags usage = 0;
    const auto use = [&](BufferUsage flag, VkBufferUsageFlags vk_flag) {
        if (core::has_any(desc.usage, flag)) {
            usage |= vk_flag;
        }
    };
    use(BufferUsage::Vertex, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    use(BufferUsage::Index, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    use(BufferUsage::Uniform, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    use(BufferUsage::Storage, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    use(BufferUsage::Indirect, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    use(BufferUsage::CopySrc, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    use(BufferUsage::CopyDst, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    // Shaders reach every buffer they read by address (bindless, docs/design.md Rendering).
    if (core::has_any(desc.usage, BufferUsage::Vertex | BufferUsage::Index | BufferUsage::Uniform |
                                      BufferUsage::Storage | BufferUsage::Indirect)) {
        usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }

    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = desc.size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo allocation_info{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (desc.memory == MemoryLocation::Upload) {
        allocation_info = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                    VMA_ALLOCATION_CREATE_MAPPED_BIT,
                           .usage = VMA_MEMORY_USAGE_AUTO};
    } else if (desc.memory == MemoryLocation::Readback) {
        allocation_info = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                           .usage = VMA_MEMORY_USAGE_AUTO};
    }

    Buffer buffer{.desc = desc};
    buffer.desc.debug_name = nullptr;
    VmaAllocationInfo allocated{};
    const VkResult result = vmaCreateBuffer(impl_->allocator, &buffer_info, &allocation_info, &buffer.buffer,
                                            &buffer.allocation, &allocated);
    if (result != VK_SUCCESS) {
        DE_LOG_ERROR("render", "buffer '%s' (%llu bytes): %s", desc.debug_name != nullptr ? desc.debug_name : "",
                     static_cast<unsigned long long>(desc.size), vk_result_name(result));
        return core::fail(core::ErrorCode::OutOfMemory, "GPU memory for the buffer could not be allocated");
    }
    buffer.mapped = static_cast<std::byte*>(allocated.pMappedData);
    impl_->set_debug_name(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(buffer.buffer), desc.debug_name);
    if (desc.debug_name != nullptr) {
        vmaSetAllocationName(impl_->allocator, buffer.allocation, desc.debug_name);
    }
    return impl_->buffers.insert(buffer);
}

void Device::destroy(TextureHandle handle) {
    const Texture* texture = impl_->textures.get(handle);
    if (texture == nullptr) {
        return;
    }
    DE_ASSERT_MSG(!texture->swapchain, "swapchain textures belong to the device");
    if (texture->swapchain) {
        return;
    }
    impl_->current_slot().pending.push(
        PendingDestroy{.image = texture->image, .view = texture->view, .allocation = texture->allocation});
    impl_->textures.remove(handle);
}

void Device::destroy(BufferHandle handle) {
    const Buffer* buffer = impl_->buffers.get(handle);
    if (buffer == nullptr) {
        return;
    }
    impl_->current_slot().pending.push(PendingDestroy{.buffer = buffer->buffer, .allocation = buffer->allocation});
    impl_->buffers.remove(handle);
}

const TextureDesc* Device::texture_desc(TextureHandle handle) const {
    const Texture* texture = impl_->textures.get(handle);
    return texture != nullptr ? &texture->desc : nullptr;
}

const BufferDesc* Device::buffer_desc(BufferHandle handle) const {
    const Buffer* buffer = impl_->buffers.get(handle);
    return buffer != nullptr ? &buffer->desc : nullptr;
}

std::span<std::byte> Device::mapped(BufferHandle handle) {
    const Buffer& buffer = impl_->buffer(handle);
    if (buffer.mapped == nullptr) {
        return {};
    }
    return {buffer.mapped, static_cast<size_t>(buffer.desc.size)};
}

void Device::flush_mapped(BufferHandle handle, uint64_t offset, uint64_t size) {
    const Buffer& buffer = impl_->buffer(handle);
    DE_VK_CHECK(
        vmaFlushAllocation(impl_->allocator, buffer.allocation, offset, size == UINT64_MAX ? VK_WHOLE_SIZE : size));
}

void Device::invalidate_mapped(BufferHandle handle, uint64_t offset, uint64_t size) {
    const Buffer& buffer = impl_->buffer(handle);
    DE_VK_CHECK(vmaInvalidateAllocation(impl_->allocator, buffer.allocation, offset,
                                        size == UINT64_MAX ? VK_WHOLE_SIZE : size));
}

// --- Frames ---

void Device::begin_frame() {
    DE_PROFILE_ZONE("rhi begin_frame");
    DeviceImpl& impl = *impl_;
    DE_ASSERT_MSG(!impl.in_frame, "begin_frame twice without end_frame");
    ++impl.frame_index;
    impl.in_frame = true;
    impl.acquired = false;

    FrameSlot& slot = impl.current_slot();
    if (slot.timeline_value != 0) {
        DE_PROFILE_ZONE("wait for GPU");
        const VkSemaphoreWaitInfo wait{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &impl.frame_timeline,
            .pValues = &slot.timeline_value,
        };
        DE_VK_CHECK(vkWaitSemaphores(impl.device, &wait, UINT64_MAX));
    }
    impl.free_pending(slot.pending);
    DE_VK_CHECK(vkResetCommandPool(impl.device, slot.command_pool, 0));
    slot.list_count = 0;
}

CommandList Device::begin_command_list(QueueType queue) {
    return impl_->begin_command_list(queue);
}

void Device::end_frame() {
    DE_PROFILE_ZONE("rhi end_frame");
    DeviceImpl& impl = *impl_;
    DE_ASSERT_MSG(impl.in_frame, "end_frame without begin_frame");
    FrameSlot& slot = impl.current_slot();

    VkCommandBufferSubmitInfo command_buffers[kMaxCommandListsPerFrame];
    for (uint32_t i = 0; i < slot.list_count; ++i) {
        const CommandListImpl& list = slot.lists[i];
        DE_ASSERT_MSG(!list.in_rendering, "command list ended inside begin_rendering");
        DE_ASSERT_MSG(list.label_depth == 0, "command list ended with an open debug label");
        DE_VK_CHECK(vkEndCommandBuffer(list.cmd));
        command_buffers[i] = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = list.cmd};
    }

    const VkSemaphoreSubmitInfo wait{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = slot.acquire_semaphore,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    const VkSemaphoreSubmitInfo signals[] = {
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
         .semaphore = impl.frame_timeline,
         .value = impl.frame_index,
         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
         .semaphore = impl.acquired ? impl.swapchain.present_semaphores[impl.acquired_image] : VK_NULL_HANDLE,
         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
    };
    const VkSubmitInfo2 submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = impl.acquired ? 1U : 0U,
        .pWaitSemaphoreInfos = &wait,
        .commandBufferInfoCount = slot.list_count,
        .pCommandBufferInfos = command_buffers,
        .signalSemaphoreInfoCount = impl.acquired ? 2U : 1U,
        .pSignalSemaphoreInfos = signals,
    };
    {
        DE_PROFILE_ZONE("submit");
        DE_VK_CHECK(vkQueueSubmit2(impl.queues[static_cast<size_t>(QueueType::Graphics)], 1, &submit, VK_NULL_HANDLE));
    }
    slot.timeline_value = impl.frame_index;

    if (impl.acquired) {
        DE_PROFILE_ZONE("present");
        const VkPresentInfoKHR present{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &impl.swapchain.present_semaphores[impl.acquired_image],
            .swapchainCount = 1,
            .pSwapchains = &impl.swapchain.swapchain,
            .pImageIndices = &impl.acquired_image,
        };
        const VkResult result = vkQueuePresentKHR(impl.queues[static_cast<size_t>(QueueType::Graphics)], &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            impl.swapchain_out_of_date = true;
        } else if (result == VK_ERROR_SURFACE_LOST_KHR) {
            DE_LOG_ERROR("render", "the window surface was lost");
            impl.swapchain_out_of_date = true;
        } else {
            DE_VK_CHECK(result);
        }
        impl.acquired = false;
    }
    impl.in_frame = false;
}

void Device::wait_idle() {
    DE_PROFILE_ZONE("rhi wait_idle");
    DeviceImpl& impl = *impl_;
    DE_VK_CHECK(vkDeviceWaitIdle(impl.device));
    for (FrameSlot& slot : impl.frames) {
        // Inside a frame, the current slot's resources may still be used by unsubmitted commands.
        if (!impl.in_frame || &slot != &impl.current_slot()) {
            impl.free_pending(slot.pending);
        }
    }
}

void Device::set_vsync(bool vsync) {
    impl_->want_vsync = vsync;
}

Format Device::swapchain_format() const {
    return impl_->swapchain.format;
}

uint32_t Device::swapchain_width() const {
    return impl_->swapchain.width;
}

uint32_t Device::swapchain_height() const {
    return impl_->swapchain.height;
}

} // namespace de::rhi
