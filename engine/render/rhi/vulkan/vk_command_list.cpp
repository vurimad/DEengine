#include "core/inline_array.h"
#include "render/rhi/vulkan/vk_device.h"

#include <algorithm>
#include <cstddef>

namespace de::rhi {

namespace {

static_assert(kWholeBuffer == VK_WHOLE_SIZE, "kWholeBuffer passes straight through as VK_WHOLE_SIZE");

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
    DE_ASSERT(region.mip < desc.mip_count);
    DE_ASSERT_MSG(region.layer_count > 0 && region.layer < desc.layer_count &&
                      region.layer_count <= desc.layer_count - region.layer,
                  "copy layers outside the texture");
    const MipExtent extent = mip_extent(desc, region.mip);
    const uint32_t mip_width = extent.width;
    const uint32_t mip_height = extent.height;
    const uint32_t mip_depth = extent.depth;
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
                             .layerCount = region.layer_count},
        .imageOffset = {.x = static_cast<int32_t>(region.x),
                        .y = static_cast<int32_t>(region.y),
                        .z = static_cast<int32_t>(region.z)},
        .imageExtent = {.width = region.width != 0 ? region.width : mip_width - region.x,
                        .height = region.height != 0 ? region.height : mip_height - region.y,
                        .depth = region.depth != 0 ? region.depth : mip_depth - region.z},
    };
}

// The mips [first, end) and layers [first, end) a range covers in this texture.
struct ResolvedRange {
    uint32_t mip_begin;
    uint32_t mip_end;
    uint32_t layer_begin;
    uint32_t layer_end;
};

ResolvedRange resolve(const Texture& texture, const TextureRange& range) {
    const uint32_t mips = range.mip_count == kAllMips ? texture.desc.mip_count - range.first_mip : range.mip_count;
    const uint32_t layers =
        range.layer_count == kAllLayers ? texture.desc.layer_count - range.first_layer : range.layer_count;
    return {.mip_begin = range.first_mip,
            .mip_end = range.first_mip + mips,
            .layer_begin = range.first_layer,
            .layer_end = range.first_layer + layers};
}

// Barriers in one command are unordered: two that touch the same subresources or bytes are a bug
// (debug check, quadratic in the batch size).
bool no_overlaps(DeviceImpl& device, std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers) {
    for (size_t i = 0; i < textures.size(); ++i) {
        for (size_t j = i + 1; j < textures.size(); ++j) {
            if (textures[i].texture != textures[j].texture) {
                continue;
            }
            const Texture& texture = device.texture(textures[i].texture);
            const ResolvedRange a = resolve(texture, textures[i].range);
            const ResolvedRange b = resolve(texture, textures[j].range);
            if (a.mip_begin < b.mip_end && b.mip_begin < a.mip_end && a.layer_begin < b.layer_end &&
                b.layer_begin < a.layer_end) {
                return false;
            }
        }
    }
    for (size_t i = 0; i < buffers.size(); ++i) {
        for (size_t j = i + 1; j < buffers.size(); ++j) {
            if (buffers[i].buffer != buffers[j].buffer) {
                continue;
            }
            const uint64_t a_end = buffers[i].size == kWholeBuffer ? UINT64_MAX : buffers[i].offset + buffers[i].size;
            const uint64_t b_end = buffers[j].size == kWholeBuffer ? UINT64_MAX : buffers[j].offset + buffers[j].size;
            if (buffers[i].offset < b_end && buffers[j].offset < a_end) {
                return false;
            }
        }
    }
    return true;
}

// A side with shader states must name at least one shader stage.
bool stages_named(ResourceState state, ShaderStages stages) {
    return !core::has_any(state, kShaderStates) || stages != ShaderStages::None;
}

// What a barrier waits for and makes wait, with queue ownership applied: a release doesn't make
// anything on its queue wait (the acquire does); an acquire waits at every stage, with no access,
// so its layout change chains after the semaphore wait that ordered it behind the release (the
// device's semaphore waits are at all stages).
struct BarrierSides {
    VkStateInfo before;
    VkStateInfo after;
    uint32_t src_family = VK_QUEUE_FAMILY_IGNORED;
    uint32_t dst_family = VK_QUEUE_FAMILY_IGNORED;
};

BarrierSides barrier_sides(const CommandListImpl& list, ResourceState before, ShaderStages before_stages,
                           ResourceState after, ShaderStages after_stages, QueueOwnership ownership,
                           QueueType other_queue) {
    BarrierSides sides{.before = vk_state_info(before, before_stages), .after = vk_state_info(after, after_stages)};
    if (ownership != QueueOwnership::None) {
        const uint32_t own = list.device->queue_families[static_cast<size_t>(list.queue)];
        const uint32_t other = list.device->queue_families[static_cast<size_t>(other_queue)];
        DE_ASSERT_MSG(own != other, "a queue handover between queues of one family: use a plain barrier");
        if (ownership == QueueOwnership::Release) {
            sides.src_family = own;
            sides.dst_family = other;
            sides.after.stages = VK_PIPELINE_STAGE_2_NONE;
            sides.after.access = VK_ACCESS_2_NONE;
        } else {
            sides.src_family = other;
            sides.dst_family = own;
            sides.before.stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            sides.before.access = VK_ACCESS_2_NONE;
        }
    }
    // A transfer queue only runs copies: its barriers can't name graphics or compute stages.
    constexpr VkPipelineStageFlags2 kTransferStages = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                                                      VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_HOST_BIT |
                                                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    DE_ASSERT_MSG(list.queue != QueueType::Transfer ||
                      ((sides.before.stages | sides.after.stages) & ~kTransferStages) == 0,
                  "a transfer list's barriers can only involve copy states (or a queue handover)");
    return sides;
}

} // namespace

void CommandList::barriers(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers,
                           std::span<const GlobalBarrier> globals) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(!impl_->in_rendering, "barriers inside begin_rendering/end_rendering");
    DeviceImpl& device = *impl_->device;
    DE_ASSERT_MSG(no_overlaps(device, textures, buffers),
                  "two barriers in one command touch the same subresources or bytes: split them into two commands");

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
        DE_ASSERT_MSG(!core::has_any(barrier.before | barrier.after, ResourceState::Present) || texture.swapchain,
                      "only swapchain textures present");
        DE_ASSERT_MSG(stages_named(barrier.before, barrier.before_stages) &&
                          stages_named(barrier.after, barrier.after_stages),
                      "a shader state needs at least one shader stage");
        DE_ASSERT_MSG(barrier.ownership == QueueOwnership::None || !barrier.discard,
                      "a queue handover keeps the contents: it can't discard");
        const BarrierSides sides = barrier_sides(*impl_, barrier.before, barrier.before_stages, barrier.after,
                                                 barrier.after_stages, barrier.ownership, barrier.other_queue);
        image_barriers.push({
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = sides.before.stages,
            .srcAccessMask = sides.before.access,
            .dstStageMask = sides.after.stages,
            .dstAccessMask = sides.after.access,
            // Discarding still waits for the previous use (stages and accesses), it just doesn't
            // keep the contents through the layout change.
            .oldLayout = barrier.discard ? VK_IMAGE_LAYOUT_UNDEFINED : sides.before.layout,
            .newLayout = sides.after.layout,
            .srcQueueFamilyIndex = sides.src_family,
            .dstQueueFamilyIndex = sides.dst_family,
            .image = texture.image,
            .subresourceRange = subresource_range(texture, barrier.range),
        });
    }
    for (const BufferBarrier& barrier : buffers) {
        const Buffer& buffer = device.buffer(barrier.buffer);
        DE_ASSERT_MSG(is_valid_state(barrier.before, false) && is_valid_state(barrier.after, false),
                      "buffer states: one write state, or buffer read states");
        DE_ASSERT_MSG(barrier.offset < buffer.desc.size &&
                          (barrier.size == kWholeBuffer ||
                           (barrier.size > 0 && barrier.size <= buffer.desc.size - barrier.offset)),
                      "buffer barrier range is empty or outside the buffer");
        DE_ASSERT_MSG(stages_named(barrier.before, barrier.before_stages) &&
                          stages_named(barrier.after, barrier.after_stages),
                      "a shader state needs at least one shader stage");
        const BarrierSides sides = barrier_sides(*impl_, barrier.before, barrier.before_stages, barrier.after,
                                                 barrier.after_stages, barrier.ownership, barrier.other_queue);
        buffer_barriers.push({
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = sides.before.stages,
            .srcAccessMask = sides.before.access,
            .dstStageMask = sides.after.stages,
            .dstAccessMask = sides.after.access,
            .srcQueueFamilyIndex = sides.src_family,
            .dstQueueFamilyIndex = sides.dst_family,
            .buffer = buffer.buffer,
            .offset = barrier.offset,
            .size = barrier.size,
        });
    }
    for (const GlobalBarrier& barrier : globals) {
        DE_ASSERT_MSG((barrier.before & ~kAllResourceStates) == ResourceState::Undefined &&
                          (barrier.after & ~kAllResourceStates) == ResourceState::Undefined,
                      "unknown resource state bits");
        DE_ASSERT_MSG(stages_named(barrier.before, barrier.before_stages) &&
                          stages_named(barrier.after, barrier.after_stages),
                      "a shader state needs at least one shader stage");
        const BarrierSides sides = barrier_sides(*impl_, barrier.before, barrier.before_stages, barrier.after,
                                                 barrier.after_stages, QueueOwnership::None, QueueType::Graphics);
        const VkStateInfo& before = sides.before;
        const VkStateInfo& after = sides.after;
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
    DE_ASSERT_MSG(impl_->queue != QueueType::Transfer, "transfer lists only copy");
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
        DE_ASSERT_MSG(core::has_any(texture.desc.usage, TextureUsage::RenderTarget),
                      "a color attachment needs RenderTarget usage");
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
        DE_ASSERT_MSG(core::has_any(texture.desc.usage, TextureUsage::DepthStencil),
                      "a depth attachment needs DepthStencil usage");
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
    DE_ASSERT_MSG(impl_->queue != QueueType::Transfer, "transfer lists only copy");
    const Pipeline& pipeline = impl_->device->pipeline(handle);
    DE_ASSERT_MSG(impl_->in_rendering == (pipeline.bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS),
                  "graphics pipelines bind inside begin_rendering, compute pipelines outside it");
    vkCmdBindPipeline(impl_->cmd, pipeline.bind_point, pipeline.pipeline);
    if (pipeline.bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        vkCmdSetCullMode(impl_->cmd, pipeline.cull);
        impl_->graphics_bound = true;
    } else {
        impl_->compute_bound = true;
    }
}

void CommandList::set_cull_mode(CullMode cull) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->in_rendering && impl_->graphics_bound, "set_cull_mode needs a bound graphics pipeline");
    vkCmdSetCullMode(impl_->cmd, to_vk(cull));
}

uint32_t CommandList::write_timestamp() {
    DE_ASSERT(impl_ != nullptr);
    DeviceImpl& device = *impl_->device;
    VkQueryPool pool = device.current_slot().timestamps;
    if (pool == VK_NULL_HANDLE || impl_->queue != QueueType::Graphics) {
        return kNoTimestamp;
    }
    const uint32_t index = device.timestamps_used.fetch_add(1, std::memory_order_relaxed);
    if (index >= kMaxTimestampsPerFrame) {
        return kNoTimestamp;
    }
    vkCmdWriteTimestamp2(impl_->cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, index);
    return index;
}

void CommandList::push_constants(std::span<const std::byte> data, uint32_t offset) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->queue != QueueType::Transfer, "transfer lists only copy");
    DE_ASSERT_MSG(offset % 4 == 0 && data.size() % 4 == 0 && offset + data.size() <= kMaxPushConstantBytes,
                  "push constants: 4-byte aligned, within kMaxPushConstantBytes");
    vkCmdPushConstants(impl_->cmd, impl_->device->pipeline_layout, VK_SHADER_STAGE_ALL, offset,
                       static_cast<uint32_t>(data.size()), data.data());
}

void CommandList::draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->queue != QueueType::Transfer, "transfer lists only copy");
    DE_ASSERT_MSG(impl_->in_rendering && impl_->graphics_bound, "draws need begin_rendering and a graphics pipeline");
    vkCmdDraw(impl_->cmd, vertex_count, instance_count, first_vertex, first_instance);
}

void CommandList::bind_index_buffer(BufferHandle buffer, uint64_t offset, IndexType type) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->queue != QueueType::Transfer, "transfer lists only copy");
    vkCmdBindIndexBuffer(impl_->cmd, impl_->device->buffer(buffer).buffer, offset,
                         type == IndexType::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void CommandList::draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                               int32_t vertex_offset, uint32_t first_instance) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->queue != QueueType::Transfer, "transfer lists only copy");
    DE_ASSERT_MSG(impl_->in_rendering && impl_->graphics_bound, "draws need begin_rendering and a graphics pipeline");
    vkCmdDrawIndexed(impl_->cmd, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void CommandList::dispatch(uint32_t groups_x, uint32_t groups_y, uint32_t groups_z) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->queue != QueueType::Transfer, "transfer lists only copy");
    DE_ASSERT_MSG(!impl_->in_rendering && impl_->compute_bound,
                  "dispatches need a compute pipeline, outside begin_rendering");
    vkCmdDispatch(impl_->cmd, groups_x, groups_y, groups_z);
}

// As the arguments lie in a buffer: what Vulkan reads is what rhi.h says, field by field.
static_assert(sizeof(DrawIndirectArgs) == sizeof(VkDrawIndirectCommand));
static_assert(offsetof(DrawIndirectArgs, vertex_count) == offsetof(VkDrawIndirectCommand, vertexCount));
static_assert(offsetof(DrawIndirectArgs, instance_count) == offsetof(VkDrawIndirectCommand, instanceCount));
static_assert(offsetof(DrawIndirectArgs, first_vertex) == offsetof(VkDrawIndirectCommand, firstVertex));
static_assert(offsetof(DrawIndirectArgs, first_instance) == offsetof(VkDrawIndirectCommand, firstInstance));
static_assert(sizeof(DrawIndexedIndirectArgs) == sizeof(VkDrawIndexedIndirectCommand));
static_assert(offsetof(DrawIndexedIndirectArgs, index_count) == offsetof(VkDrawIndexedIndirectCommand, indexCount));
static_assert(offsetof(DrawIndexedIndirectArgs, instance_count) ==
              offsetof(VkDrawIndexedIndirectCommand, instanceCount));
static_assert(offsetof(DrawIndexedIndirectArgs, first_index) == offsetof(VkDrawIndexedIndirectCommand, firstIndex));
static_assert(offsetof(DrawIndexedIndirectArgs, vertex_offset) == offsetof(VkDrawIndexedIndirectCommand, vertexOffset));
static_assert(offsetof(DrawIndexedIndirectArgs, first_instance) ==
              offsetof(VkDrawIndexedIndirectCommand, firstInstance));
static_assert(sizeof(DispatchIndirectArgs) == sizeof(VkDispatchIndirectCommand));
static_assert(offsetof(DispatchIndirectArgs, groups_x) == offsetof(VkDispatchIndirectCommand, x));
static_assert(offsetof(DispatchIndirectArgs, groups_y) == offsetof(VkDispatchIndirectCommand, y));
static_assert(offsetof(DispatchIndirectArgs, groups_z) == offsetof(VkDispatchIndirectCommand, z));

namespace {

// A stride of 0 is packed. Vulkan wants it spelled out for several draws, and at least the size of one.
uint32_t indirect_stride(uint32_t stride, uint32_t packed) {
    DE_ASSERT_MSG(stride == 0 || (stride >= packed && stride % 4 == 0),
                  "an indirect stride holds one draw, in 4-byte steps");
    return stride != 0 ? stride : packed;
}

// Whether a buffer can hold these arguments: its usage, and all of them inside it.
[[maybe_unused]] bool indirect_fits(const Buffer& buffer, uint64_t offset, uint32_t count, uint32_t stride,
                                    uint64_t one) {
    return core::has_any(buffer.desc.usage, BufferUsage::Indirect) && offset % 4 == 0 &&
           (count == 0 || (offset + (uint64_t{count} - 1) * stride + one <= buffer.desc.size));
}

} // namespace

void CommandList::draw_indirect(BufferHandle args, uint64_t offset, uint32_t draw_count, uint32_t stride) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->queue != QueueType::Transfer, "transfer lists only copy");
    DE_ASSERT_MSG(impl_->in_rendering && impl_->graphics_bound, "draws need begin_rendering and a graphics pipeline");
    const uint32_t used_stride = indirect_stride(stride, sizeof(DrawIndirectArgs));
    const Buffer& buffer = impl_->device->buffer(args);
    DE_ASSERT_MSG(indirect_fits(buffer, offset, draw_count, used_stride, sizeof(DrawIndirectArgs)),
                  "indirect arguments need Indirect usage, 4-byte alignment and to fit in the buffer");
    DE_ASSERT_MSG(draw_count <= impl_->device->capabilities.max_draw_indirect_count, "too many draws for one call");
    vkCmdDrawIndirect(impl_->cmd, buffer.buffer, offset, draw_count, used_stride);
}

void CommandList::draw_indexed_indirect(BufferHandle args, uint64_t offset, uint32_t draw_count, uint32_t stride) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->queue != QueueType::Transfer, "transfer lists only copy");
    DE_ASSERT_MSG(impl_->in_rendering && impl_->graphics_bound, "draws need begin_rendering and a graphics pipeline");
    const uint32_t used_stride = indirect_stride(stride, sizeof(DrawIndexedIndirectArgs));
    const Buffer& buffer = impl_->device->buffer(args);
    DE_ASSERT_MSG(indirect_fits(buffer, offset, draw_count, used_stride, sizeof(DrawIndexedIndirectArgs)),
                  "indirect arguments need Indirect usage, 4-byte alignment and to fit in the buffer");
    DE_ASSERT_MSG(draw_count <= impl_->device->capabilities.max_draw_indirect_count, "too many draws for one call");
    vkCmdDrawIndexedIndirect(impl_->cmd, buffer.buffer, offset, draw_count, used_stride);
}

void CommandList::draw_indirect_count(BufferHandle args, uint64_t offset, BufferHandle count_buffer,
                                      uint64_t count_offset, uint32_t max_draws, uint32_t stride) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->queue != QueueType::Transfer, "transfer lists only copy");
    DE_ASSERT_MSG(impl_->in_rendering && impl_->graphics_bound, "draws need begin_rendering and a graphics pipeline");
    const uint32_t used_stride = indirect_stride(stride, sizeof(DrawIndirectArgs));
    const Buffer& buffer = impl_->device->buffer(args);
    const Buffer& counts = impl_->device->buffer(count_buffer);
    DE_ASSERT_MSG(indirect_fits(buffer, offset, max_draws, used_stride, sizeof(DrawIndirectArgs)) &&
                      indirect_fits(counts, count_offset, 1, 4, 4),
                  "indirect arguments need Indirect usage, 4-byte alignment and to fit in the buffer");
    DE_ASSERT_MSG(max_draws <= impl_->device->capabilities.max_draw_indirect_count, "too many draws for one call");
    vkCmdDrawIndirectCount(impl_->cmd, buffer.buffer, offset, counts.buffer, count_offset, max_draws, used_stride);
}

void CommandList::draw_indexed_indirect_count(BufferHandle args, uint64_t offset, BufferHandle count_buffer,
                                              uint64_t count_offset, uint32_t max_draws, uint32_t stride) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->queue != QueueType::Transfer, "transfer lists only copy");
    DE_ASSERT_MSG(impl_->in_rendering && impl_->graphics_bound, "draws need begin_rendering and a graphics pipeline");
    const uint32_t used_stride = indirect_stride(stride, sizeof(DrawIndexedIndirectArgs));
    const Buffer& buffer = impl_->device->buffer(args);
    const Buffer& counts = impl_->device->buffer(count_buffer);
    DE_ASSERT_MSG(indirect_fits(buffer, offset, max_draws, used_stride, sizeof(DrawIndexedIndirectArgs)) &&
                      indirect_fits(counts, count_offset, 1, 4, 4),
                  "indirect arguments need Indirect usage, 4-byte alignment and to fit in the buffer");
    DE_ASSERT_MSG(max_draws <= impl_->device->capabilities.max_draw_indirect_count, "too many draws for one call");
    vkCmdDrawIndexedIndirectCount(impl_->cmd, buffer.buffer, offset, counts.buffer, count_offset, max_draws,
                                  used_stride);
}

void CommandList::dispatch_indirect(BufferHandle args, uint64_t offset) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->queue != QueueType::Transfer, "transfer lists only copy");
    DE_ASSERT_MSG(!impl_->in_rendering && impl_->compute_bound,
                  "dispatches need a compute pipeline, outside begin_rendering");
    const Buffer& buffer = impl_->device->buffer(args);
    DE_ASSERT_MSG(indirect_fits(buffer, offset, 1, sizeof(DispatchIndirectArgs), sizeof(DispatchIndirectArgs)),
                  "indirect arguments need Indirect usage, 4-byte alignment and to fit in the buffer");
    vkCmdDispatchIndirect(impl_->cmd, buffer.buffer, offset);
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

void CommandList::blit_texture(TextureHandle src, TextureHandle dst, const TextureBlit& blit) {
    DE_ASSERT(impl_ != nullptr);
    DE_ASSERT_MSG(impl_->queue != QueueType::Transfer, "blits need a graphics list");
    DE_ASSERT_MSG(!impl_->in_rendering, "copies inside begin_rendering/end_rendering");
    DeviceImpl& device = *impl_->device;
    const Texture& source = device.texture(src);
    const Texture& target = device.texture(dst);
    DE_ASSERT_MSG(blit.src_mip < source.desc.mip_count && blit.dst_mip < target.desc.mip_count,
                  "blit mips outside the textures");
    DE_ASSERT_MSG(src != dst || blit.src_mip != blit.dst_mip, "a blit from a mip into itself");
    DE_ASSERT_MSG(core::has_any(source.desc.usage, TextureUsage::CopySrc) &&
                      core::has_any(target.desc.usage, TextureUsage::CopyDst),
                  "blits need CopySrc usage on the source and CopyDst on the destination");
    DE_ASSERT_MSG(device.capabilities.supports(source.desc.format, FormatSupport::Blit | FormatSupport::LinearFilter) &&
                      device.capabilities.supports(target.desc.format, FormatSupport::Blit),
                  "the formats can't be blitted with a linear filter");
    // Filtered blits are for color: depth can't be filtered, and integer formats only blit to
    // integer formats (unfiltered).
    const auto integer = [](Format format) {
        return format == Format::R32Uint || format == Format::RG32Uint || format == Format::RGBA32Uint;
    };
    DE_ASSERT_MSG(!format_info(source.desc.format).depth && !format_info(target.desc.format).depth &&
                      !integer(source.desc.format) && !integer(target.desc.format),
                  "filtered blits are for float and normalized color formats");
    DE_ASSERT_MSG(blit.first_layer < source.desc.layer_count && blit.first_layer < target.desc.layer_count,
                  "blit layers outside the textures");
    const uint32_t layers =
        blit.layer_count == kAllLayers ? source.desc.layer_count - blit.first_layer : blit.layer_count;
    DE_ASSERT_MSG(layers > 0 && layers <= source.desc.layer_count - blit.first_layer &&
                      layers <= target.desc.layer_count - blit.first_layer,
                  "blit layers outside the textures");
    const MipExtent from = mip_extent(source.desc, blit.src_mip);
    const MipExtent to = mip_extent(target.desc, blit.dst_mip);
    const auto corner = [](const MipExtent& extent) {
        return VkOffset3D{.x = static_cast<int32_t>(extent.width),
                          .y = static_cast<int32_t>(extent.height),
                          .z = static_cast<int32_t>(extent.depth)};
    };
    const VkImageBlit2 region{
        .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
        .srcSubresource = {.aspectMask = source.aspects,
                           .mipLevel = blit.src_mip,
                           .baseArrayLayer = blit.first_layer,
                           .layerCount = layers},
        .srcOffsets = {VkOffset3D{}, corner(from)},
        .dstSubresource = {.aspectMask = target.aspects,
                           .mipLevel = blit.dst_mip,
                           .baseArrayLayer = blit.first_layer,
                           .layerCount = layers},
        .dstOffsets = {VkOffset3D{}, corner(to)},
    };
    const VkBlitImageInfo2 info{
        .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
        .srcImage = source.image,
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstImage = target.image,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1,
        .pRegions = &region,
        .filter = VK_FILTER_LINEAR,
    };
    vkCmdBlitImage2(impl_->cmd, &info);
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
