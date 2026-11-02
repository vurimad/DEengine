#pragma once

#include "asset/shader_format.h"
#include "cooker/common.h"

#include <span>

namespace de::shader {
class ShaderCompiler;
}

namespace de::cooker {

// "shaders/common/color.slang" -> "common.color"; false for a path outside shaders/ or not .slang.
bool shader_module_of(std::string_view path, core::FixedString<asset::kShaderNameSize - 1>& module);

// Whether a module defines entry points ([shader("...")]); modules without any are libraries that
// others import, and aren't cooked on their own.
bool defines_entry_points(std::span<const std::byte> source);

// The .deshader of a module: every entry point compiled to SPIR-V with no defines, with its
// reflection, checked against the engine's pipeline layout (bindless only, push constants at most
// 128 bytes). dependencies receives every file the compile read ("shaders/common/bindless.slang").
core::Result<core::Array<std::byte>> cook_shader(shader::ShaderCompiler& compiler, std::string_view module,
                                                 asset::ShaderFlags flags, core::Array<SourcePath>& dependencies,
                                                 std::string_view path, Diagnostics& diagnostics);

} // namespace de::cooker
