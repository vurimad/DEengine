#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/vulkan/vk_device.h"

#include <algorithm>

namespace de::rhi {

namespace {

constexpr uint32_t kTableCount = static_cast<uint32_t>(BindlessTable::Count);

bool is_sampled_table(BindlessTable table) {
    return table <= BindlessTable::Texture3D;
}

bool is_sampler_table(BindlessTable table) {
    return table == BindlessTable::Sampler || table == BindlessTable::ComparisonSampler;
}

uint32_t table_capacity(BindlessTable table) {
    if (is_sampled_table(table)) {
        return kBindlessTextureCapacity;
    }
    return is_sampler_table(table) ? kBindlessSamplerCapacity : kBindlessStorageCapacity;
}

VkDescriptorType table_type(BindlessTable table) {
    if (is_sampled_table(table)) {
        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    }
    return is_sampler_table(table) ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
}

// One-channel only: the integer table is RWTexture2D<uint> (create_texture refuses the others).
bool is_integer_format(Format format) {
    return format == Format::R32Uint;
}

BindlessTable sampled_table_for(const TextureDesc& desc) {
    if (desc.dimension == TextureDimension::Texture3D) {
        return BindlessTable::Texture3D;
    }
    if (desc.dimension == TextureDimension::Cube) {
        return desc.layer_count > 6 ? BindlessTable::TextureCubeArray : BindlessTable::TextureCube;
    }
    return desc.layer_count > 1 ? BindlessTable::Texture2DArray : BindlessTable::Texture2D;
}

// Storage views of cubes are arrays of faces (vk_view_type(desc, true)).
BindlessTable storage_table_for(const TextureDesc& desc) {
    if (desc.dimension == TextureDimension::Texture3D) {
        return BindlessTable::Storage3D;
    }
    return desc.layer_count > 1 ? BindlessTable::Storage2DArray : BindlessTable::Storage2D;
}

VkSamplerCreateInfo sampler_info(DefaultSampler sampler, float max_anisotropy) {
    const bool point = sampler == DefaultSampler::PointWrap || sampler == DefaultSampler::PointClamp;
    const bool wrap = sampler == DefaultSampler::LinearWrap || sampler == DefaultSampler::PointWrap ||
                      sampler == DefaultSampler::AnisotropicWrap;
    const bool anisotropic = sampler == DefaultSampler::AnisotropicWrap || sampler == DefaultSampler::AnisotropicClamp;
    const bool shadow = sampler == DefaultSampler::ShadowCompare;
    const VkSamplerAddressMode address = wrap ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = point ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
        .minFilter = point ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
        .mipmapMode = point ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = address,
        .addressModeV = address,
        .addressModeW = address,
        .anisotropyEnable = anisotropic ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = anisotropic ? std::min(16.0f, max_anisotropy) : 1.0f,
        // Reverse Z: a stored depth passes when the compared one is at least as near.
        .compareEnable = shadow ? VK_TRUE : VK_FALSE,
        .compareOp = shadow ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
}

const char* sampler_name(DefaultSampler sampler) {
    switch (sampler) {
    case DefaultSampler::LinearWrap:
        return "sampler linear wrap";
    case DefaultSampler::LinearClamp:
        return "sampler linear clamp";
    case DefaultSampler::PointWrap:
        return "sampler point wrap";
    case DefaultSampler::PointClamp:
        return "sampler point clamp";
    case DefaultSampler::AnisotropicWrap:
        return "sampler anisotropic wrap";
    case DefaultSampler::AnisotropicClamp:
        return "sampler anisotropic clamp";
    case DefaultSampler::ShadowCompare:
        return "sampler shadow compare";
    case DefaultSampler::Count:
        break;
    }
    return "sampler";
}

// The fallback images: what each table's entry 0, and every free entry, points at.
struct FallbackImage {
    const char* name;
    VkFormat format;
    VkImageType type;
    uint32_t layers;
    bool cube;
    bool storage; // GENERAL layout and zeroed; otherwise sampled (read-only layout) and magenta
};

constexpr FallbackImage kFallbackImageDescs[DeviceImpl::kFallbackImages] = {
    {.name = "fallback texture",
     .format = VK_FORMAT_R8G8B8A8_UNORM,
     .type = VK_IMAGE_TYPE_2D,
     .layers = 6,
     .cube = true,
     .storage = false},
    {.name = "fallback texture 3d",
     .format = VK_FORMAT_R8G8B8A8_UNORM,
     .type = VK_IMAGE_TYPE_3D,
     .layers = 1,
     .cube = false,
     .storage = false},
    {.name = "fallback storage",
     .format = VK_FORMAT_R8G8B8A8_UNORM,
     .type = VK_IMAGE_TYPE_2D,
     .layers = 1,
     .cube = false,
     .storage = true},
    {.name = "fallback storage 3d",
     .format = VK_FORMAT_R8G8B8A8_UNORM,
     .type = VK_IMAGE_TYPE_3D,
     .layers = 1,
     .cube = false,
     .storage = true},
    {.name = "fallback storage uint",
     .format = VK_FORMAT_R32_UINT,
     .type = VK_IMAGE_TYPE_2D,
     .layers = 1,
     .cube = false,
     .storage = true},
};

// Which fallback image each texture table views, and how.
struct FallbackView {
    uint32_t image;
    VkImageViewType type;
    uint32_t layers;
};

constexpr FallbackView kFallbackViews[] = {
    {.image = 0, .type = VK_IMAGE_VIEW_TYPE_2D, .layers = 1},         // Texture2D
    {.image = 0, .type = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .layers = 1},   // Texture2DArray
    {.image = 0, .type = VK_IMAGE_VIEW_TYPE_CUBE, .layers = 6},       // TextureCube
    {.image = 0, .type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, .layers = 6}, // TextureCubeArray
    {.image = 1, .type = VK_IMAGE_VIEW_TYPE_3D, .layers = 1},         // Texture3D
    {.image = 2, .type = VK_IMAGE_VIEW_TYPE_2D, .layers = 1},         // Storage2D
    {.image = 2, .type = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .layers = 1},   // Storage2DArray
    {.image = 3, .type = VK_IMAGE_VIEW_TYPE_3D, .layers = 1},         // Storage3D
    {.image = 4, .type = VK_IMAGE_VIEW_TYPE_2D, .layers = 1},         // Storage2DUint
};
static_assert(std::size(kFallbackViews) == static_cast<size_t>(BindlessTable::Sampler),
              "one fallback view per texture table");

} // namespace

void BindlessSlots::init(const char* name, uint32_t capacity) {
    name_ = name;
    capacity_ = capacity;
    next_ = 1;
    used_ = 1;
    warned_ = false;
    freed_.clear();
    freed_head_ = 0;
}

uint32_t BindlessSlots::allocate() {
    uint32_t entry = 0;
    if (next_ < capacity_) {
        entry = next_++;
    } else if (freed_head_ < freed_.size()) {
        entry = freed_[freed_head_++];
        if (freed_head_ == freed_.size()) {
            freed_.clear(); // drained: start the queue over, keeping its memory
            freed_head_ = 0;
        }
    } else {
        return 0;
    }
    ++used_;
    if (!warned_ && uint64_t{used_} * 5 >= uint64_t{capacity_} * 4) {
        warned_ = true;
        DE_LOG_WARN("render", "bindless %s tables are 80%% full: %u of %u entries in use", name_, used_, capacity_);
    }
    return entry;
}

void BindlessSlots::release(uint32_t entry) {
    DE_ASSERT(entry > 0 && entry < next_ && used_ > 1);
    freed_.push(entry);
    --used_;
}

core::Result<void> DeviceImpl::create_bindless() {
    DE_PROFILE_ZONE("rhi create bindless");
    VkPhysicalDeviceVulkan12Properties props12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 props{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &props12};
    vkGetPhysicalDeviceProperties2(physical_device, &props);

    // Fixed sizes, far below the spec's minimums for descriptor indexing; checked anyway.
    constexpr uint32_t kSampled = 5 * kBindlessTextureCapacity;
    constexpr uint32_t kStorage = 4 * kBindlessStorageCapacity;
    constexpr uint32_t kSamplers = 2 * kBindlessSamplerCapacity;
    if (props12.maxDescriptorSetUpdateAfterBindSampledImages < kSampled ||
        props12.maxPerStageDescriptorUpdateAfterBindSampledImages < kSampled ||
        props12.maxDescriptorSetUpdateAfterBindStorageImages < kStorage ||
        props12.maxPerStageDescriptorUpdateAfterBindStorageImages < kStorage ||
        props12.maxDescriptorSetUpdateAfterBindSamplers < kSamplers ||
        props12.maxPerStageDescriptorUpdateAfterBindSamplers < kSamplers ||
        props12.maxPerStageUpdateAfterBindResources < kSampled + kStorage + kSamplers) {
        DE_LOG_ERROR("render", "the GPU's bindless limits are below %u sampled, %u storage textures and %u samplers",
                     kSampled, kStorage, kSamplers);
        return core::fail(core::ErrorCode::Unsupported, "the GPU's bindless tables are too small");
    }

    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                           VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorBindingFlags binding_flags[kTableCount];
    VkDescriptorSetLayoutBinding bindings[kTableCount];
    for (uint32_t i = 0; i < kTableCount; ++i) {
        const auto table = static_cast<BindlessTable>(i);
        binding_flags[i] = flags;
        bindings[i] = {.binding = i,
                       .descriptorType = table_type(table),
                       .descriptorCount = table_capacity(table),
                       .stageFlags = VK_SHADER_STAGE_ALL};
    }
    const VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = kTableCount,
        .pBindingFlags = binding_flags,
    };
    const VkDescriptorSetLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &flags_info,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = kTableCount,
        .pBindings = bindings,
    };
    DE_VK_CHECK(vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &bindless_layout));
    set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, reinterpret_cast<uint64_t>(bindless_layout),
                   "bindless layout");

    const VkDescriptorPoolSize pool_sizes[] = {
        {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = kSampled},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = kStorage},
        {.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = kSamplers},
    };
    const VkDescriptorPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
        .poolSizeCount = 3,
        .pPoolSizes = pool_sizes,
    };
    DE_VK_CHECK(vkCreateDescriptorPool(device, &pool_info, nullptr, &bindless_pool));
    const VkDescriptorSetAllocateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = bindless_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &bindless_layout,
    };
    DE_VK_CHECK(vkAllocateDescriptorSets(device, &set_info, &bindless_set));
    set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<uint64_t>(bindless_set), "bindless set");

    // --- The fallbacks: created, cleared on the GPU and left in the layout their tables expect ---
    for (uint32_t i = 0; i < kFallbackImages; ++i) {
        const FallbackImage& fallback = kFallbackImageDescs[i];
        const VkImageCreateInfo image_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags = fallback.cube ? static_cast<VkImageCreateFlags>(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) : 0U,
            .imageType = fallback.type,
            .format = fallback.format,
            .extent = {.width = 1, .height = 1, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = fallback.layers,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = (fallback.storage ? static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_STORAGE_BIT)
                                       : static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_SAMPLED_BIT)) |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        DE_VK_CHECK(vmaCreateImage(allocator, &image_info, &allocation, &fallback_images[i], &fallback_allocations[i],
                                   nullptr));
        set_debug_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(fallback_images[i]), fallback.name);
    }
    for (uint32_t table = 0; table < std::size(kFallbackViews); ++table) {
        const FallbackView& view = kFallbackViews[table];
        const VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = fallback_images[view.image],
            .viewType = view.type,
            .format = kFallbackImageDescs[view.image].format,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = view.layers},
        };
        DE_VK_CHECK(vkCreateImageView(device, &view_info, nullptr, &fallback_views[table]));
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    const VkCommandBufferAllocateInfo cmd_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = frames[0].command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    DE_VK_CHECK(vkAllocateCommandBuffers(device, &cmd_info, &cmd));
    const VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    DE_VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    VkImageMemoryBarrier2 barriers[kFallbackImages];
    for (uint32_t i = 0; i < kFallbackImages; ++i) {
        barriers[i] = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = fallback_images[i],
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .levelCount = 1,
                                 .layerCount = kFallbackImageDescs[i].layers},
        };
    }
    const VkDependencyInfo to_clear{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                    .imageMemoryBarrierCount = kFallbackImages,
                                    .pImageMemoryBarriers = barriers};
    vkCmdPipelineBarrier2(cmd, &to_clear);
    for (uint32_t i = 0; i < kFallbackImages; ++i) {
        const VkClearColorValue magenta{.float32 = {1.0f, 0.0f, 1.0f, 1.0f}};
        const VkClearColorValue zero{.uint32 = {0, 0, 0, 0}};
        const VkImageSubresourceRange range = barriers[i].subresourceRange;
        vkCmdClearColorImage(cmd, fallback_images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             kFallbackImageDescs[i].storage ? &zero : &magenta, 1, &range);
        barriers[i].srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        barriers[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barriers[i].dstAccessMask = kFallbackImageDescs[i].storage
                                        ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                        : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i].newLayout =
            kFallbackImageDescs[i].storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    }
    const VkDependencyInfo to_use{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                  .imageMemoryBarrierCount = kFallbackImages,
                                  .pImageMemoryBarriers = barriers};
    vkCmdPipelineBarrier2(cmd, &to_use);
    DE_VK_CHECK(vkEndCommandBuffer(cmd));
    const VkCommandBufferSubmitInfo cmd_submit{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                               .commandBuffer = cmd};
    const VkSubmitInfo2 submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cmd_submit};
    VkQueue queue = queues[static_cast<size_t>(QueueType::Graphics)];
    DE_VK_CHECK(vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE));
    DE_VK_CHECK(vkQueueWaitIdle(queue)); // once, at startup
    vkFreeCommandBuffers(device, frames[0].command_pool, 1, &cmd);

    // --- Every entry of every table starts valid: the fallback, or a default sampler ---
    for (uint32_t i = 0; i < static_cast<uint32_t>(DefaultSampler::Count); ++i) {
        const VkSamplerCreateInfo info = sampler_info(static_cast<DefaultSampler>(i), capabilities.max_anisotropy);
        DE_VK_CHECK(vkCreateSampler(device, &info, nullptr, &samplers[i]));
        set_debug_name(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<uint64_t>(samplers[i]),
                       sampler_name(static_cast<DefaultSampler>(i)));
    }
    core::Array<VkDescriptorImageInfo> infos(core::heap_allocator(core::MemoryTag::Render));
    for (uint32_t i = 0; i < kTableCount; ++i) {
        const auto table = static_cast<BindlessTable>(i);
        infos.clear();
        infos.resize(table_capacity(table));
        for (uint32_t entry = 0; entry < infos.size(); ++entry) {
            if (is_sampler_table(table)) {
                // Entries past the defaults repeat a sampler that suits the table (a comparing one
                // for the comparison table), so even a wrong index samples sensibly.
                const auto padding = table == BindlessTable::ComparisonSampler ? DefaultSampler::ShadowCompare
                                                                               : DefaultSampler::LinearWrap;
                const uint32_t sampler =
                    entry < static_cast<uint32_t>(DefaultSampler::Count) ? entry : static_cast<uint32_t>(padding);
                infos[entry] = {.sampler = samplers[sampler]};
            } else {
                infos[entry] = {.imageView = fallback_views[i],
                                .imageLayout = is_sampled_table(table) ? VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL
                                                                       : VK_IMAGE_LAYOUT_GENERAL};
            }
        }
        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = bindless_set,
            .dstBinding = i,
            .descriptorCount = static_cast<uint32_t>(infos.size()),
            .descriptorType = table_type(table),
            .pImageInfo = infos.data(),
        };
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    sampled_slots.init("sampled texture", kBindlessTextureCapacity);
    storage_slots.init("storage texture", kBindlessStorageCapacity);
    DE_LOG_INFO("render", "bindless tables: %u sampled and %u storage textures per table, %u samplers",
                kBindlessTextureCapacity, kBindlessStorageCapacity, kBindlessSamplerCapacity);
    return {};
}

void DeviceImpl::destroy_bindless() {
    for (VkSampler& sampler : samplers) {
        vkDestroySampler(device, sampler, nullptr);
        sampler = VK_NULL_HANDLE;
    }
    for (VkImageView& view : fallback_views) {
        vkDestroyImageView(device, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < kFallbackImages; ++i) {
        if (fallback_images[i] != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator, fallback_images[i], fallback_allocations[i]);
            fallback_images[i] = VK_NULL_HANDLE;
            fallback_allocations[i] = VK_NULL_HANDLE;
        }
    }
    vkDestroyDescriptorPool(device, bindless_pool, nullptr); // frees the set
    bindless_pool = VK_NULL_HANDLE;
    bindless_set = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(device, bindless_layout, nullptr);
    bindless_layout = VK_NULL_HANDLE;
}

core::Result<void> DeviceImpl::register_bindless(Texture& texture) {
    const TextureDesc& desc = texture.desc;
    TextureBindless& bindless = texture.bindless;
    const bool integer = is_integer_format(desc.format);
    if (core::has_any(desc.usage, TextureUsage::Sampled)) {
        const uint32_t entry = sampled_slots.allocate();
        if (entry == 0) {
            return core::fail(core::ErrorCode::OutOfMemory, "the bindless sampled texture tables are full");
        }
        const BindlessTable table = sampled_table_for(desc);
        bindless.sampled_index = make_bindless_index(table, entry);
        // Shaders sample one aspect: the depth of a depth-stencil texture.
        if ((texture.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
            const VkImageViewCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = texture.image,
                .viewType = vk_view_type(desc, false),
                .format = to_vk_format(desc.format),
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                     .levelCount = VK_REMAINING_MIP_LEVELS,
                                     .layerCount = VK_REMAINING_ARRAY_LAYERS},
            };
            DE_VK_CHECK(vkCreateImageView(device, &info, nullptr, &bindless.sampled_view));
        }
        write_descriptor(table, entry, bindless.sampled_view != VK_NULL_HANDLE ? bindless.sampled_view : texture.view);
    }

    if (core::has_any(desc.usage, TextureUsage::Storage)) {
        const BindlessTable table = integer ? BindlessTable::Storage2DUint : storage_table_for(desc);
        for (uint32_t mip = 0; mip < desc.mip_count; ++mip) {
            const uint32_t entry = storage_slots.allocate();
            if (entry == 0) {
                release_bindless(bindless);
                return core::fail(core::ErrorCode::OutOfMemory, "the bindless storage texture tables are full");
            }
            const VkImageViewCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = texture.image,
                .viewType = vk_view_type(desc, true),
                .format = to_vk_format(storage_format(desc.format)),
                .subresourceRange = {.aspectMask = texture.aspects,
                                     .baseMipLevel = mip,
                                     .levelCount = 1,
                                     .layerCount = VK_REMAINING_ARRAY_LAYERS},
            };
            DE_VK_CHECK(vkCreateImageView(device, &info, nullptr, &bindless.storage_views[mip]));
            bindless.storage_indices[mip] = make_bindless_index(table, entry);
            ++bindless.storage_mips;
            write_descriptor(table, entry, bindless.storage_views[mip]);
        }
    }
    return {};
}

void DeviceImpl::release_bindless(TextureBindless& bindless, bool rewrite_descriptors) {
    // Every entry back to its table's fallback, in one descriptor update.
    VkDescriptorImageInfo images[kMaxMips + 1];
    VkWriteDescriptorSet writes[kMaxMips + 1];
    uint32_t write_count = 0;
    const auto release = [&](uint32_t index, BindlessSlots& slots) {
        const BindlessTable table = bindless_table(index);
        const uint32_t entry = bindless_entry(index);
        images[write_count] = {
            .imageView = fallback_views[static_cast<size_t>(table)],
            .imageLayout = is_sampled_table(table) ? VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
        };
        writes[write_count] = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = bindless_set,
            .dstBinding = static_cast<uint32_t>(table),
            .dstArrayElement = entry,
            .descriptorCount = 1,
            .descriptorType = table_type(table),
            .pImageInfo = &images[write_count],
        };
        ++write_count;
        slots.release(entry);
    };
    if (bindless.sampled_index != kInvalidBindlessIndex) {
        release(bindless.sampled_index, sampled_slots);
    }
    for (uint32_t mip = 0; mip < bindless.storage_mips; ++mip) {
        release(bindless.storage_indices[mip], storage_slots);
    }
    if (rewrite_descriptors && write_count > 0) {
        vkUpdateDescriptorSets(device, write_count, writes, 0, nullptr);
    }
    vkDestroyImageView(device, bindless.sampled_view, nullptr);
    for (uint32_t mip = 0; mip < bindless.storage_mips; ++mip) {
        vkDestroyImageView(device, bindless.storage_views[mip], nullptr);
    }
    bindless = TextureBindless{};
}

void DeviceImpl::write_descriptor(BindlessTable table, uint32_t entry, VkImageView view) {
    DE_ASSERT(!is_sampler_table(table) && entry < table_capacity(table));
    const VkDescriptorImageInfo image{
        .imageView = view,
        .imageLayout = is_sampled_table(table) ? VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = bindless_set,
        .dstBinding = static_cast<uint32_t>(table),
        .dstArrayElement = entry,
        .descriptorCount = 1,
        .descriptorType = table_type(table),
        .pImageInfo = &image,
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

uint32_t Device::texture_index(TextureHandle handle) const {
    const Texture* texture = impl_->textures.get(handle);
    return texture != nullptr ? texture->bindless.sampled_index : kInvalidBindlessIndex;
}

uint32_t Device::storage_texture_index(TextureHandle handle, uint32_t mip) const {
    const Texture* texture = impl_->textures.get(handle);
    if (texture == nullptr || mip >= texture->bindless.storage_mips) {
        return kInvalidBindlessIndex;
    }
    return texture->bindless.storage_indices[mip];
}

} // namespace de::rhi
