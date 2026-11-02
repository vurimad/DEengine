#include "render/rhi/mips.h"

#include "core/profile.h"
#include "render/rhi/device.h"

namespace de::rhi {

bool can_generate_mips(const Device& device, const TextureDesc& desc) {
    return !format_info(desc.format).depth &&
           device.capabilities().supports(desc.format, FormatSupport::Blit | FormatSupport::LinearFilter) &&
           core::has_all(desc.usage, TextureUsage::CopySrc | TextureUsage::CopyDst);
}

void record_mip_chain(CommandList& list, TextureHandle texture, const TextureDesc& desc, uint32_t source_mip) {
    for (uint32_t mip = source_mip + 1; mip < desc.mip_count; ++mip) {
        if (mip > source_mip + 1) {
            // The mip above is complete: read it, write this one.
            list.barrier(TextureBarrier{.texture = texture,
                                        .before = ResourceState::CopyDst,
                                        .after = ResourceState::CopySrc,
                                        .range = {.first_mip = mip - 1, .mip_count = 1}});
        }
        list.blit_texture(texture, texture, {.src_mip = mip - 1, .dst_mip = mip});
    }
}

namespace {

bool stages_named(ResourceState state, ShaderStages stages) {
    return !core::has_any(state, kShaderStates) || stages != ShaderStages::None;
}

} // namespace

core::Result<void> generate_mips(Device& device, CommandList& list, TextureHandle texture,
                                 const MipGeneration& generation) {
    DE_PROFILE_ZONE("generate mips");
    const TextureDesc* desc = device.texture_desc(texture);
    if (desc == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "mip generation for a stale texture handle");
    }
    if (!can_generate_mips(device, *desc)) {
        return core::fail(core::ErrorCode::Unsupported,
                          "the texture can't generate mips (its format can't be blitted, or it lacks copy usage)");
    }
    if (generation.source_mip + 1 >= desc->mip_count) {
        return core::fail(core::ErrorCode::OutOfRange, "no mips after the source to generate");
    }
    if (generation.before == ResourceState::Undefined || !is_valid_state(generation.before, true) ||
        generation.after == ResourceState::Undefined || !is_valid_state(generation.after, true) ||
        !stages_named(generation.before, generation.before_stages) ||
        !stages_named(generation.after, generation.after_stages)) {
        return core::fail(core::ErrorCode::InvalidArgument,
                          "mip generation needs valid states (the source's not Undefined, shader states with stages)");
    }

    const ScopedDebugLabel label(list, "generate mips");
    const uint32_t first = generation.source_mip;
    const uint32_t last = desc->mip_count - 1;
    // The source to CopySrc, the generated mips to CopyDst (old contents discarded), the chain.
    BarrierBatch prepare;
    prepare.add(TextureBarrier{.texture = texture,
                               .before = generation.before,
                               .after = ResourceState::CopySrc,
                               .before_stages = generation.before_stages,
                               .range = {.first_mip = first, .mip_count = 1}});
    prepare.add(TextureBarrier{.texture = texture,
                               .before = generation.before,
                               .after = ResourceState::CopyDst,
                               .before_stages = generation.before_stages,
                               .range = {.first_mip = first + 1, .mip_count = last - first},
                               .discard = true});
    prepare.flush(list);
    record_mip_chain(list, texture, *desc, first);
    BarrierBatch finish;
    finish.add(TextureBarrier{.texture = texture,
                              .before = ResourceState::CopySrc,
                              .after = generation.after,
                              .after_stages = generation.after_stages,
                              .range = {.first_mip = first, .mip_count = last - first}});
    finish.add(TextureBarrier{.texture = texture,
                              .before = ResourceState::CopyDst,
                              .after = generation.after,
                              .after_stages = generation.after_stages,
                              .range = {.first_mip = last, .mip_count = 1}});
    finish.flush(list);
    return {};
}

} // namespace de::rhi
