#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/vulkan/vk_device.h"

#include <algorithm>
#include <cstring>

namespace de::rhi {

namespace {

// Table sizes: plenty for an open world's streamed materials, far below what GPUs allow.
constexpr uint32_t kWantedSampledTextures = 16384;
constexpr uint32_t kWantedStorageTextures = 4096;
constexpr uint32_t kWantedSamplers = 64;

// One binding per texture shape (shaders/common/bindless.slang), so a shader can only read a
// texture as the shape it is. A texture's one index is written into its shape's binding.
constexpr uint32_t kSampledBindings = 5; // 2D, 2D array, cube, cube array, 3D
constexpr uint32_t kStorageBindings = 3; // 2D, 2D array, 3D
constexpr uint32_t kStorageBindingBase = kSampledBindings;
constexpr uint32_t kSamplerBinding = kSampledBindings + kStorageBindings;
constexpr uint32_t kComparisonSamplerBinding = kSamplerBinding + 1;
constexpr uint32_t kBindingCount = kComparisonSamplerBinding + 1;

uint32_t sampled_binding(const TextureDesc& desc) {
    if (desc.dimension == TextureDimension::Texture3D) {
        return 4;
    }
    if (desc.dimension == TextureDimension::Cube) {
        return desc.layer_count > 6 ? 3 : 2;
    }
    return desc.layer_count > 1 ? 1 : 0;
}

// Storage views of cubes are arrays of faces.
uint32_t storage_binding(const TextureDesc& desc) {
    if (desc.dimension == TextureDimension::Texture3D) {
        return kStorageBindingBase + 2;
    }
    return kStorageBindingBase + (desc.layer_count > 1 ? 1 : 0);
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

VkImageViewType view_type(const TextureDesc& desc, bool storage) {
    if (desc.dimension == TextureDimension::Texture3D) {
        return VK_IMAGE_VIEW_TYPE_3D;
    }
    // Storage views of cubes are plain arrays of six faces.
    if (desc.dimension == TextureDimension::Cube && !storage) {
        return desc.layer_count > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    }
    return desc.layer_count > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
}

} // namespace

void BindlessTable::init(uint32_t capacity, uint32_t reserved) {
    capacity_ = capacity;
    next_ = reserved;
    used_ = reserved;
    free_.clear();
}

uint32_t BindlessTable::allocate() {
    if (!free_.empty()) {
        const uint32_t index = free_.back();
        free_.pop();
        ++used_;
        return index;
    }
    if (next_ == capacity_) {
        return kInvalidBindlessIndex;
    }
    ++used_;
    return next_++;
}

void BindlessTable::release(uint32_t index) {
    DE_ASSERT(index < next_ && used_ > 0);
    free_.push(index);
    --used_;
}

core::Result<void> DeviceImpl::create_bindless() {
    DE_PROFILE_ZONE("rhi create bindless");
    VkPhysicalDeviceVulkan12Properties props12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 props{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &props12};
    vkGetPhysicalDeviceProperties2(physical_device, &props);

    // Each binding counts against the GPU's limits for its descriptor type, per set and per stage.
    const auto fit = [](uint32_t wanted, uint32_t bindings, uint32_t set_limit, uint32_t stage_limit) {
        return std::min({wanted, set_limit / bindings, stage_limit / bindings});
    };
    const uint32_t samplers_count = fit(kWantedSamplers, 2, props12.maxDescriptorSetUpdateAfterBindSamplers,
                                        props12.maxPerStageDescriptorUpdateAfterBindSamplers);
    const uint32_t storage_count =
        fit(kWantedStorageTextures, kStorageBindings, props12.maxDescriptorSetUpdateAfterBindStorageImages,
            props12.maxPerStageDescriptorUpdateAfterBindStorageImages);
    uint32_t sampled_count =
        fit(kWantedSampledTextures, kSampledBindings, props12.maxDescriptorSetUpdateAfterBindSampledImages,
            props12.maxPerStageDescriptorUpdateAfterBindSampledImages);
    // Every stage sees the whole set, and some GPUs cap the total per stage.
    const uint64_t others = (uint64_t{2} * samplers_count) + (uint64_t{kStorageBindings} * storage_count);
    const uint64_t per_stage = props12.maxPerStageUpdateAfterBindResources;
    if (per_stage < others + (uint64_t{kSampledBindings} * sampled_count)) {
        sampled_count = per_stage > others ? static_cast<uint32_t>((per_stage - others) / kSampledBindings) : 0;
    }
    if (sampled_count < 1024 || storage_count < 64 || samplers_count < static_cast<uint32_t>(DefaultSampler::Count)) {
        return core::fail(core::ErrorCode::Unsupported, "the GPU's bindless tables are too small");
    }

    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                           VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorBindingFlags binding_flags[kBindingCount];
    VkDescriptorSetLayoutBinding bindings[kBindingCount];
    for (uint32_t i = 0; i < kBindingCount; ++i) {
        binding_flags[i] = flags;
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLER;
        uint32_t count = samplers_count;
        if (i < kSampledBindings) {
            type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            count = sampled_count;
        } else if (i < kSamplerBinding) {
            type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            count = storage_count;
        }
        bindings[i] = {
            .binding = i, .descriptorType = type, .descriptorCount = count, .stageFlags = VK_SHADER_STAGE_ALL};
    }
    const VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = kBindingCount,
        .pBindingFlags = binding_flags,
    };
    const VkDescriptorSetLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &flags_info,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = kBindingCount,
        .pBindings = bindings,
    };
    DE_VK_CHECK(vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &bindless_layout));
    set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, reinterpret_cast<uint64_t>(bindless_layout),
                   "bindless layout");

    const VkDescriptorPoolSize pool_sizes[] = {
        {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = kSampledBindings * sampled_count},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = kStorageBindings * storage_count},
        {.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 2 * samplers_count},
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

    // The default samplers, at their DefaultSampler values.
    for (uint32_t i = 0; i < static_cast<uint32_t>(DefaultSampler::Count); ++i) {
        const VkSamplerCreateInfo info = sampler_info(static_cast<DefaultSampler>(i), capabilities.max_anisotropy);
        DE_VK_CHECK(vkCreateSampler(device, &info, nullptr, &samplers[i]));
        set_debug_name(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<uint64_t>(samplers[i]),
                       sampler_name(static_cast<DefaultSampler>(i)));
        // In both sampler tables at the same index; only ShadowCompare is meant for comparisons.
        const VkDescriptorImageInfo image{.sampler = samplers[i]};
        for (const uint32_t binding : {kSamplerBinding, kComparisonSamplerBinding}) {
            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = bindless_set,
                .dstBinding = binding,
                .dstArrayElement = i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .pImageInfo = &image,
            };
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    }

    // The fallback texture: 1x1 magenta, uploaded once and left in the read-only layout.
    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {.width = 1, .height = 1, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo image_allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    DE_VK_CHECK(
        vmaCreateImage(allocator, &image_info, &image_allocation, &fallback_image, &fallback_allocation, nullptr));
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = fallback_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
    };
    DE_VK_CHECK(vkCreateImageView(device, &view_info, nullptr, &fallback_view));
    set_debug_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(fallback_image), "fallback texture");

    const VkBufferCreateInfo staging_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    const VmaAllocationCreateInfo staging_allocation{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation staging_memory = VK_NULL_HANDLE;
    VmaAllocationInfo staging_mapped{};
    DE_VK_CHECK(
        vmaCreateBuffer(allocator, &staging_info, &staging_allocation, &staging, &staging_memory, &staging_mapped));
    const uint8_t magenta[4] = {255, 0, 255, 255};
    std::memcpy(staging_mapped.pMappedData, magenta, sizeof(magenta));
    DE_VK_CHECK(vmaFlushAllocation(allocator, staging_memory, 0, VK_WHOLE_SIZE));

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
    const VkImageSubresourceRange range{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};
    const VkImageMemoryBarrier2 to_copy{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = fallback_image,
        .subresourceRange = range,
    };
    const VkDependencyInfo to_copy_dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_copy};
    vkCmdPipelineBarrier2(cmd, &to_copy_dependency);
    const VkBufferImageCopy2 region{
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
        .imageExtent = {.width = 1, .height = 1, .depth = 1},
    };
    const VkCopyBufferToImageInfo2 copy{
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer = staging,
        .dstImage = fallback_image,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1,
        .pRegions = &region,
    };
    vkCmdCopyBufferToImage2(cmd, &copy);
    VkImageMemoryBarrier2 to_read = to_copy;
    to_read.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_read.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    const VkDependencyInfo to_read_dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read};
    vkCmdPipelineBarrier2(cmd, &to_read_dependency);
    DE_VK_CHECK(vkEndCommandBuffer(cmd));
    const VkCommandBufferSubmitInfo cmd_submit{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                               .commandBuffer = cmd};
    const VkSubmitInfo2 submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cmd_submit};
    VkQueue queue = queues[static_cast<size_t>(QueueType::Graphics)];
    DE_VK_CHECK(vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE));
    DE_VK_CHECK(vkQueueWaitIdle(queue)); // once, at startup
    vkFreeCommandBuffers(device, frames[0].command_pool, 1, &cmd);
    vmaDestroyBuffer(allocator, staging, staging_memory);

    sampled_table.init(sampled_count, 1);
    storage_table.init(storage_count, 0);
    write_sampled(kFallbackBinding, kFallbackTextureIndex, fallback_view);

    DE_LOG_INFO("render", "bindless tables: %u sampled and %u storage textures per shape, %u samplers", sampled_count,
                storage_count, samplers_count);
    return {};
}

void DeviceImpl::destroy_bindless() {
    for (VkSampler& sampler : samplers) {
        vkDestroySampler(device, sampler, nullptr);
        sampler = VK_NULL_HANDLE;
    }
    if (fallback_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, fallback_view, nullptr);
        fallback_view = VK_NULL_HANDLE;
    }
    if (fallback_image != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator, fallback_image, fallback_allocation);
        fallback_image = VK_NULL_HANDLE;
        fallback_allocation = VK_NULL_HANDLE;
    }
    vkDestroyDescriptorPool(device, bindless_pool, nullptr); // frees the set
    bindless_pool = VK_NULL_HANDLE;
    bindless_set = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(device, bindless_layout, nullptr);
    bindless_layout = VK_NULL_HANDLE;
}

core::Result<void> DeviceImpl::register_bindless(Texture& texture) {
    const TextureDesc& desc = texture.desc;
    if (core::has_any(desc.usage, TextureUsage::Sampled)) {
        texture.sampled_index = sampled_table.allocate();
        if (texture.sampled_index == kInvalidBindlessIndex) {
            return core::fail(core::ErrorCode::OutOfMemory, "the bindless sampled texture table is full");
        }
        // Shaders sample one aspect: the depth of a depth-stencil texture.
        if ((texture.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
            const VkImageViewCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = texture.image,
                .viewType = view_type(desc, false),
                .format = to_vk_format(desc.format),
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                     .levelCount = VK_REMAINING_MIP_LEVELS,
                                     .layerCount = VK_REMAINING_ARRAY_LAYERS},
            };
            DE_VK_CHECK(vkCreateImageView(device, &info, nullptr, &texture.sampled_view));
        }
        texture.sampled_binding = sampled_binding(desc);
        write_sampled(texture.sampled_binding, texture.sampled_index,
                      texture.sampled_view != VK_NULL_HANDLE ? texture.sampled_view : texture.view);
    }
    if (core::has_any(desc.usage, TextureUsage::Storage)) {
        texture.storage_index = storage_table.allocate();
        if (texture.storage_index == kInvalidBindlessIndex) {
            return core::fail(core::ErrorCode::OutOfMemory, "the bindless storage texture table is full");
        }
        const VkImageViewCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = texture.image,
            .viewType = view_type(desc, true),
            .format = to_vk_format(desc.format),
            .subresourceRange = {.aspectMask = texture.aspects,
                                 .levelCount = 1,
                                 .layerCount = VK_REMAINING_ARRAY_LAYERS},
        };
        DE_VK_CHECK(vkCreateImageView(device, &info, nullptr, &texture.storage_view));
        texture.storage_binding = storage_binding(desc);
        write_storage(texture.storage_binding, texture.storage_index, texture.storage_view);
    }
    return {};
}

void DeviceImpl::write_sampled(uint32_t binding, uint32_t index, VkImageView view) {
    const VkDescriptorImageInfo image{.imageView = view, .imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = bindless_set,
        .dstBinding = binding,
        .dstArrayElement = index,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo = &image,
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void DeviceImpl::write_storage(uint32_t binding, uint32_t index, VkImageView view) {
    const VkDescriptorImageInfo image{.imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = bindless_set,
        .dstBinding = binding,
        .dstArrayElement = index,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .pImageInfo = &image,
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

uint32_t Device::texture_index(TextureHandle handle) const {
    const Texture* texture = impl_->textures.get(handle);
    return texture != nullptr ? texture->sampled_index : kInvalidBindlessIndex;
}

uint32_t Device::storage_texture_index(TextureHandle handle) const {
    const Texture* texture = impl_->textures.get(handle);
    return texture != nullptr ? texture->storage_index : kInvalidBindlessIndex;
}

} // namespace de::rhi
