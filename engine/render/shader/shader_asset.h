#pragma once

#include "asset/shader_format.h"
#include "platform/file.h"
#include "render/rhi/rhi.h"
#include "render/shader/shader_compiler.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace de::shader {

// Between the RHI's shader stages and the numbers .deshader files store.
constexpr asset::ShaderStageKind to_asset_stage(rhi::ShaderStage stage) {
    switch (stage) {
    case rhi::ShaderStage::Fragment:
        return asset::ShaderStageKind::Fragment;
    case rhi::ShaderStage::Compute:
        return asset::ShaderStageKind::Compute;
    case rhi::ShaderStage::Vertex:
    case rhi::ShaderStage::Count:
        break;
    }
    return asset::ShaderStageKind::Vertex;
}

constexpr rhi::ShaderStage from_asset_stage(asset::ShaderStageKind stage) {
    switch (stage) {
    case asset::ShaderStageKind::Fragment:
        return rhi::ShaderStage::Fragment;
    case asset::ShaderStageKind::Compute:
        return rhi::ShaderStage::Compute;
    case asset::ShaderStageKind::Vertex:
        break;
    }
    return rhi::ShaderStage::Vertex;
}

// The permutation key a .deshader stores for these defines (asset::shader_defines_hash).
uint64_t defines_hash(std::span<const ShaderDefine> defines);

// A module's source file, as a path under the source tree: "common.color" -> "shaders/common/color.slang".
// False if it doesn't fit.
bool shader_module_path(std::string_view module, platform::PathString& path);
// The reverse: "shaders/common/color.slang" -> "common.color"; false for anything else.
bool shader_module_name(std::string_view path, ShaderName& module);

} // namespace de::shader
