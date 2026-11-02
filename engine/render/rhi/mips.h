#pragma once

#include "core/result.h"
#include "render/rhi/command_list.h"
#include "render/rhi/rhi.h"

#include <cstdint>

namespace de::rhi {

class Device;

// GPU mip generation: each mip is a filtered blit of the one above it (a 2x2 box for even sizes,
// a linear filter otherwise; sRGB is filtered in linear space). Mip generation is for textures
// made at runtime (procedural, rendered); block-compressed and integer formats can't be blitted,
// and their mips come from the cooker.

// Whether the GPU can generate this texture's mips: a color format with FormatSupport::Blit and
// LinearFilter (depth can't be filtered), and CopySrc | CopyDst usage.
bool can_generate_mips(const Device& device, const TextureDesc& desc);

struct MipGeneration {
    // The mip that holds the contents; every mip after it is generated, each from the one above.
    uint32_t source_mip = 0;
    // The state of the mips from source_mip on: the source keeps its contents (so not Undefined),
    // the generated mips' contents are replaced. after is the state they all end in.
    ResourceState before = ResourceState::ShaderRead;
    ShaderStages before_stages = ShaderStages::All;
    ResourceState after = ResourceState::ShaderRead;
    ShaderStages after_stages = ShaderStages::All;
};

// Records the generation on a graphics list, barriers included. Fails, recording nothing, if the
// texture is stale, can't generate mips (can_generate_mips), the source is its last mip, or a state
// is invalid (or a shader state names no stages).
core::Result<void> generate_mips(Device& device, CommandList& list, TextureHandle texture,
                                 const MipGeneration& generation);

// The blits alone, for callers that manage the states around them (the uploader). Before: mip
// source_mip in CopySrc, the later ones in CopyDst. After: source_mip to the last but one in
// CopySrc, the last in CopyDst.
void record_mip_chain(CommandList& list, TextureHandle texture, const TextureDesc& desc, uint32_t source_mip);

} // namespace de::rhi
