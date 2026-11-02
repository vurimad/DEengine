#include "core/inline_array.h"
#include "render/rhi/vulkan/vk_device.h"

#include <algorithm>

namespace de::rhi {

namespace {

// Batches beyond this spill to the heap: fine for rare big batches, the common case stays on the stack.
constexpr size_t kInlineBarriers = 32;

VkImageSubresourceRange subresource_range(const Texture& texture, const TextureRange& range) {
    return {
        .aspectMask = texture.aspects,
        .baseMipLevel = range.first_mip,
        .levelCount = range.mip_count == kAllMips ? VK_REMAINING_MIP_LEVELS : range.mip_count,
        .baseArrayLayer = range.first_layer,
        .layerCount = range.layer_count == kAllLayers ? VK_REMAINING_ARRAY_LAYERS : range.layer_count,
    };
}

VkAttachmentLoadOp to_vk(LoadOp op) {
    switch (op) {
    case LoadOp::Load:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::Clear:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::DontCare:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

VkAttachmentStoreOp to_vk(StoreOp op) {
    return op == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

// Clip space is +Y up on every backend: Vulkan's is +Y down, so the viewport is flipped
// (a negative height, core since Vulkan 1.1).
VkViewport to_vk(const Viewport& viewport) {
    return {
        .x = viewport.x,
        .y = viewport.y + viewport.height,
        .width = viewport.width,
        .height = -viewport.height,
        .minDepth = viewport.min_depth,
        .maxDepth = viewport.max_depth,
    };
}

VkBufferImageCopy2 to_vk(const Texture& texture, const BufferTextureCopy& region) {
    const TextureDesc& desc = texture.desc;
    DE_ASSERT(region.mip < desc.mip_count && region.layer < desc.layer_count);
    const uint32_t mip_width = std::max(desc.width >> region.mip, 1U);
    const uint32_t mip_height = std::max(desc.height >> region.mip, 1U);
    const uint32_t mip_depth = std::max(desc.depth >> region.mip, 1U);
    DE_ASSERT_MSG(region.x < mip_width && region.y < mip_height && region.z < mip_depth,
                  "copy region starts outside the mip");
    DE_ASSERT_MSG(region.x + region.width <= mip_width && region.y + region.height <= mip_height &&
                      region.z + region.depth <= mip_depth,
                  "copy region ends outside the mip");
    // Copies move one aspect at a time: depth for depth-stencil formats (stencil copies aren't exposed yet).
    const VkImageAspectFlags aspect = (texture.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0
                                          ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT)
                                          : texture.aspects;
    return {
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset = region.buffer_offset,
        .bufferRowLength = region.buffer_row_texels,
        .bufferImageHeight = 0,
        .imageSubresource = {.aspectMask = aspect,
                             .mipLevel = region.mip,
                             .baseArrayLayer = region.layer,
                             .layerCount = 1},
        .imageOffset = {.x = static_cast<int32_t>(region.x),
                        .y = static_cast<int32_t>(region.y),
                        .z = static_cast<int32_t>(region.z)},
        .imageExtent = {.width = region.width != 0 ? region.width : mip_width - region.x,
                        .height = region.height != 0 ? region.height : mip_height - region.y,
                        .depth = region.depth != 0 ? region.depth : mip_depth - region.z},
    };
}

} // namespace

void CommandList::barriers(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers,
                           std::span<const GlobalBarrier> globals) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(!impl_->in_rendering, "barriers inside begin_rendering/end_rendering");
    DeviceImpl& device = *impl_->device;

    core::InlineArray<VkImageMemoryBarrier2, kInlineBarriers> image_barriers(
        core::heap_allocator(core::MemoryTag::Render));
    core::InlineArray<VkBufferMemoryBarrier2, kInlineBarriers> buffer_barriers(
        core::heap_allocator(core::MemoryTag::Render));
    core::InlineArray<VkMemoryBarrier2, 2> memory_barriers(core::heap_allocator(core::MemoryTag::Render));
    image_barriers.reserve(textures.size());
    buffer_barriers.reserve(buffers.size());

    for (const TextureBarrier& barrier : textures) {
        const Texture& texture = device.texture(barrier.texture);
        DE_ASSERT_MSG(is_valid_state(barrier.before, true) && is_valid_state(barrier.after, true),
                      "texture states: one write state, or read states that share a layout");
        DE_ASSERT_MSG(barrier.after != ResourceState::Undefined, "a texture can't move to the Undefined state");
        DE_ASSERT_MSG(!core::has_any(barrier.after, ResourceState::Present) || texture.swapchain,
                      "only swapchain textures present");
        const VkStateInfo before = vk_state_info(barrier.before, barrier.before_stages);
        const VkStateInfo after = vk_state_info(barrier.after, barrier.after_stages);
        image_barriers.push({
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = before.stages,
            .srcAccessMask = before.access,
            .dstStageMask = after.stages,
            .dstAccessMask = after.access,
            // Discarding still waits for the previous use (stages and accesses), it just doesn't
            // keep the contents through the layout change.
            .oldLayout = barrier.discard ? VK_IMAGE_LAYOUT_UNDEFINED : before.layout,
            .newLayout = after.layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture.image,
            .subresourceRange = subresource_range(texture, barrier.range),
        });
    }
    for (const BufferBarrier& barrier : buffers) {
        const Buffer& buffer = device.buffer(barrier.buffer);
        DE_ASSERT_MSG(is_valid_state(barrier.before, false) && is_valid_state(barrier.after, false),
                      "buffer states: one write state, or buffer read states");
        DE_ASSERT_MSG(barrier.offset < buffer.desc.size &&
                          (barrier.size == kWholeBuffer || barrier.offset + barrier.size <= buffer.desc.size),
                      "buffer barrier range outside the buffer");
        const VkStateInfo before = vk_state_info(barrier.before, barrier.before_stages);
        const VkStateInfo after = vk_state_info(barrier.after, barrier.after_stages);
        buffer_barriers.push({
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = before.stages,
            .srcAccessMask = before.access,
            .dstStageMask = after.stages,
            .dstAccessMask = after.access,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffer.buffer,
            .offset = barrier.offset,
            .size = barrier.size == kWholeBuffer ? VK_WHOLE_SIZE : barrier.size,
        });
    }
    for (const GlobalBarrier& barrier : globals) {
        const VkStateInfo before = vk_state_info(barrier.before, barrier.before_stages);
        const VkStateInfo after = vk_state_info(barrier.after, barrier.after_stages);
        memory_barriers.push({
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = before.stages,
            .srcAccessMask = before.access,
            .dstStageMask = after.stages,
            .dstAccessMask = after.access,
        });
    }
    if (image_barriers.empty() && buffer_barriers.empty() && memory_barriers.empty()) {
        return;
    }
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = static_cast<uint32_t>(memory_barriers.size()),
        .pMemoryBarriers = memory_barriers.data(),
        .bufferMemoryBarrierCount = static_cast<uint32_t>(buffer_barriers.size()),
        .pBufferMemoryBarriers = buffer_barriers.data(),
        .imageMemoryBarrierCount = static_cast<uint32_t>(image_barriers.size()),
        .pImageMemoryBarriers = image_barriers.data(),
    };
    vkCmdPipelineBarrier2(impl_->cmd, &dependency);
}

void CommandList::barrier(const TextureBarrier& barrier) {
    barriers(std::span(&barrier, 1));
}

void CommandList::barrier(const BufferBarrier& barrier) {
    barriers({}, std::span(&barrier, 1));
}

void CommandList::barrier(const GlobalBarrier& barrier) {
    barriers({}, {}, std::span(&barrier, 1));
}

void CommandList::barrier(TextureHandle texture, ResourceState before, ResourceState after) {
    barrier(TextureBarrier{.texture = texture, .before = before, .after = after});
}

void CommandList::barrier(BufferHandle buffer, ResourceState before, ResourceState after) {
    barrier(BufferBarrier{.buffer = buffer, .before = before, .after = after});
}

void CommandList::begin_rendering(const RenderingDesc& desc) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(!impl_->in_rendering, "begin_rendering twice without end_rendering");
    DE_ASSERT_MSG(desc.colors.size() <= kMaxColorAttachments, "too many color attachments");
    DE_ASSERT_MSG(!desc.colors.empty() || desc.depth != nullptr, "rendering needs at least one attachment");
    DeviceImpl& device = *impl_->device;

    Rect area = desc.area;
    const auto check_single_subresource = [](const Texture& texture) {
        DE_ASSERT_MSG(texture.desc.mip_count == 1 && texture.desc.layer_count == 1,
                      "attachments must have one mip and one layer for now");
        static_cast<void>(texture);
    };

    VkRenderingAttachmentInfo colors[kMaxColorAttachments];
    const auto color_count = static_cast<uint32_t>(std::min<size_t>(desc.colors.size(), kMaxColorAttachments));
    for (uint32_t i = 0; i < color_count; ++i) {
        const ColorAttachment& attachment = desc.colors[i];
        const Texture& texture = device.texture(attachment.texture);
        check_single_subresource(texture);
        if (area.width == 0) {
            area = {.width = texture.desc.width, .height = texture.desc.height};
        }
        colors[i] = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = texture.view,
            .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            .loadOp = to_vk(attachment.load),
            .storeOp = to_vk(attachment.store),
            .clearValue = {.color = {.float32 = {attachment.clear.r, attachment.clear.g, attachment.clear.b,
                                                 attachment.clear.a}}},
        };
    }

    VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool has_depth = false;
    bool has_stencil = false;
    if (desc.depth != nullptr) {
        const DepthAttachment& attachment = *desc.depth;
        const Texture& texture = device.texture(attachment.texture);
        check_single_subresource(texture);
        if (area.width == 0) {
            area = {.width = texture.desc.width, .height = texture.desc.height};
        }
        DE_ASSERT_MSG(!attachment.read_only || attachment.load != LoadOp::Clear,
                      "a read-only depth attachment can't be cleared: use LoadOp::Load");
        has_depth = true;
        has_stencil = (texture.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        depth = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = texture.view,
            .imageLayout =
                attachment.read_only ? VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            .loadOp = attachment.read_only ? VK_ATTACHMENT_LOAD_OP_LOAD : to_vk(attachment.load),
            .storeOp = attachment.read_only ? VK_ATTACHMENT_STORE_OP_NONE : to_vk(attachment.store),
            .clearValue = {.depthStencil = {.depth = attachment.clear_depth, .stencil = attachment.clear_stencil}},
        };
    }

    const VkRenderingInfo info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {.offset = {.x = area.x, .y = area.y}, .extent = {.width = area.width, .height = area.height}},
        .layerCount = 1,
        .colorAttachmentCount = color_count,
        .pColorAttachments = colors,
        .pDepthAttachment = has_depth ? &depth : nullptr,
        .pStencilAttachment = has_stencil ? &depth : nullptr,
    };
    vkCmdBeginRendering(impl_->cmd, &info);
    impl_->in_rendering = true;

    set_viewport({.x = static_cast<float>(area.x),
                  .y = static_cast<float>(area.y),
                  .width = static_cast<float>(area.width),
                  .height = static_cast<float>(area.height)});
    set_scissor(area);
}

void CommandList::end_rendering() {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->in_rendering, "end_rendering without begin_rendering");
    vkCmdEndRendering(impl_->cmd);
    impl_->in_rendering = false;
}

void CommandList::set_viewport(const Viewport& viewport) {
    DE_ASSERT(impl_ != nullptr);
    const VkViewport vk_viewport = to_vk(viewport);
    vkCmdSetViewport(impl_->cmd, 0, 1, &vk_viewport);
}

void CommandList::set_scissor(const Rect& rect) {
    DE_ASSERT(impl_ != nullptr);
    const VkRect2D scissor{.offset = {.x = rect.x, .y = rect.y},
                           .extent = {.width = rect.width, .height = rect.height}};
    vkCmdSetScissor(impl_->cmd, 0, 1, &scissor);
}

void CommandList::bind_pipeline(PipelineHandle handle) {
    DE_ASSERT(impl_ != nullptr);
    const Pipeline& pipeline = impl_->device->pipeline(handle);
    DE_ASSERT_MSG(impl_->in_rendering == (pipeline.bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS),
                  "graphics pipelines bind inside begin_rendering, compute pipelines outside it");
    vkCmdBindPipeline(impl_->cmd, pipeline.bind_point, pipeline.pipeline);
    if (pipeline.bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        impl_->graphics_bound = true;
    } else {
        impl_->compute_bound = true;
    }
}

void CommandList::push_constants(std::span<const std::byte> data, uint32_t offset) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(offset % 4 == 0 && data.size() % 4 == 0 && offset + data.size() <= kMaxPushConstantBytes,
                  "push constants: 4-byte aligned, within kMaxPushConstantBytes");
    vkCmdPushConstants(impl_->cmd, impl_->device->pipeline_layout, VK_SHADER_STAGE_ALL, offset,
                       static_cast<uint32_t>(data.size()), data.data());
}

void CommandList::draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->in_rendering && impl_->graphics_bound, "draws need begin_rendering and a graphics pipeline");
    vkCmdDraw(impl_->cmd, vertex_count, instance_count, first_vertex, first_instance);
}

void CommandList::bind_index_buffer(BufferHandle buffer, uint64_t offset, IndexType type) {
    DE_ASSERT(impl_ != nullptr);
    vkCmdBindIndexBuffer(impl_->cmd, impl_->device->buffer(buffer).buffer, offset,
                         type == IndexType::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void CommandList::draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                               int32_t vertex_offset, uint32_t first_instance) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->in_rendering && impl_->graphics_bound, "draws need begin_rendering and a graphics pipeline");
    vkCmdDrawIndexed(impl_->cmd, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void CommandList::dispatch(uint32_t groups_x, uint32_t groups_y, uint32_t groups_z) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(!impl_->in_rendering && impl_->compute_bound,
                  "dispatches need a compute pipeline, outside begin_rendering");
    vkCmdDispatch(impl_->cmd, groups_x, groups_y, groups_z);
}

void CommandList::copy_buffer(BufferHandle src, uint64_t src_offset, BufferHandle dst, uint64_t dst_offset,
                              uint64_t size) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(!impl_->in_rendering, "copies inside begin_rendering/end_rendering");
    DeviceImpl& device = *impl_->device;
    const VkBufferCopy2 region{
        .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2, .srcOffset = src_offset, .dstOffset = dst_offset, .size = size};
    const VkCopyBufferInfo2 info{
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
        .srcBuffer = device.buffer(src).buffer,
        .dstBuffer = device.buffer(dst).buffer,
        .regionCount = 1,
        .pRegions = &region,
    };
    vkCmdCopyBuffer2(impl_->cmd, &info);
}

void CommandList::fill_buffer(BufferHandle buffer, uint64_t offset, uint64_t size, uint32_t value) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(!impl_->in_rendering, "fills inside begin_rendering/end_rendering");
    DE_ASSERT_MSG(offset % 4 == 0 && size % 4 == 0, "fill_buffer works in whole 32-bit words");
    vkCmdFillBuffer(impl_->cmd, impl_->device->buffer(buffer).buffer, offset, size, value);
}

void CommandList::copy_buffer_to_texture(BufferHandle src, TextureHandle dst, const BufferTextureCopy& region) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(!impl_->in_rendering, "copies inside begin_rendering/end_rendering");
    DeviceImpl& device = *impl_->device;
    const Texture& texture = device.texture(dst);
    const VkBufferImageCopy2 vk_region = to_vk(texture, region);
    const VkCopyBufferToImageInfo2 info{
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer = device.buffer(src).buffer,
        .dstImage = texture.image,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1,
        .pRegions = &vk_region,
    };
    vkCmdCopyBufferToImage2(impl_->cmd, &info);
}

void CommandList::copy_texture_to_buffer(TextureHandle src, BufferHandle dst, const BufferTextureCopy& region) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(!impl_->in_rendering, "copies inside begin_rendering/end_rendering");
    DeviceImpl& device = *impl_->device;
    const Texture& texture = device.texture(src);
    const VkBufferImageCopy2 vk_region = to_vk(texture, region);
    const VkCopyImageToBufferInfo2 info{
        .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
        .srcImage = texture.image,
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstBuffer = device.buffer(dst).buffer,
        .regionCount = 1,
        .pRegions = &vk_region,
    };
    vkCmdCopyImageToBuffer2(impl_->cmd, &info);
}

void CommandList::begin_debug_label(const char* name, Color color) {
    DE_ASSERT(impl_ != nullptr);
    ++impl_->label_depth;
    if (!impl_->device->debug_utils) {
        return;
    }
    const VkDebugUtilsLabelEXT label{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        .pLabelName = name,
        .color = {color.r, color.g, color.b, color.a},
    };
    vkCmdBeginDebugUtilsLabelEXT(impl_->cmd, &label);
}

void CommandList::end_debug_label() {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->label_depth > 0, "end_debug_label without begin_debug_label");
    --impl_->label_depth;
    if (impl_->device->debug_utils) {
        vkCmdEndDebugUtilsLabelEXT(impl_->cmd);
    }
}

} // namespace de::rhi
