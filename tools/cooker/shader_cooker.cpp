#include "cooker/shader_cooker.h"

#include "cooker/blob_writer.h"
#include "core/profile.h"
#include "render/shader/shader_asset.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"

#include <cstring>

namespace de::cooker {

namespace {

constexpr std::string_view kShaders = "shaders/";
constexpr std::string_view kSlang = ".slang";
constexpr std::string_view kSrcPrefix = "src:/";

void copy_name(char (&out)[asset::kShaderNameSize], std::string_view name) {
    const size_t length = std::min(name.size(), asset::kShaderNameSize - 1);
    std::memcpy(out, name.data(), length);
    out[length] = '\0';
}

} // namespace

bool shader_module_of(std::string_view path, core::FixedString<asset::kShaderNameSize - 1>& module) {
    if (!path.starts_with(kShaders) || !path.ends_with(kSlang)) {
        return false;
    }
    const std::string_view name = path.substr(kShaders.size(), path.size() - kShaders.size() - kSlang.size());
    if (name.empty() || !module.assign(name)) {
        return false;
    }
    for (size_t i = 0; i < module.size(); ++i) {
        if (module[i] == '/') {
            module[i] = '.';
        }
    }
    return true;
}

bool defines_entry_points(std::span<const std::byte> source) {
    const std::string_view text(reinterpret_cast<const char*>(source.data()), source.size());
    return text.find("[shader(") != std::string_view::npos;
}

core::Result<core::Array<std::byte>> cook_shader(shader::ShaderCompiler& compiler, std::string_view module,
                                                 asset::ShaderFlags flags, core::Array<SourcePath>& dependencies,
                                                 std::string_view path, Diagnostics& diagnostics) {
    DE_PROFILE_ZONE("cook shader");
    const shader::ShaderName name(module);
    shader::CompileReport report{.dependencies = core::Array<shader::ShaderDependency>(tools_heap()),
                                 .diagnostics = core::Array<char>(tools_heap())};
    const core::Result<shader::CompiledProgram> compiled = compiler.compile({.module = name.c_str()}, &report);
    for (const shader::ShaderDependency& dependency : report.dependencies) {
        const std::string_view file = dependency.path.view();
        if (file.starts_with(kSrcPrefix) && file.substr(kSrcPrefix.size()) != path) {
            dependencies.push(SourcePath(file.substr(kSrcPrefix.size())));
        }
    }
    if (!compiled) {
        // The compiler's messages: null-terminated text (also in the log, whole).
        diagnostics.error(path, "doesn't compile: %.300s", report.diagnostics.empty() ? "" : report.diagnostics.data());
        return core::fail(compiled.error());
    }

    // Arrays of every entry point first; then the entries, which point at them.
    BlobWriter writer(sizeof(asset::ShaderAsset));
    core::Array<asset::ShaderEntryCode> entries(tools_heap());
    core::Array<asset::ShaderPushConstant> fields(tools_heap());
    bool fits_layout = true;
    for (const shader::CompiledStage& stage : compiled->stages) {
        const shader::ShaderReflection& reflection = stage.reflection;
        for (const shader::ResourceBinding& binding : reflection.bindings) {
            if (!shader::is_bindless_binding(binding)) {
                diagnostics.error(path,
                                  "%s binds '%s' to a descriptor slot (set %u, binding %u): reach textures through "
                                  "common.bindless and buffers through pointers",
                                  stage.entry.c_str(), binding.name.c_str(), binding.set, binding.binding);
                fits_layout = false;
            }
        }
        if (reflection.push_constant_bytes > rhi::kMaxPushConstantBytes) {
            diagnostics.error(path, "%s has %u bytes of push constants; the limit is %u", stage.entry.c_str(),
                              reflection.push_constant_bytes, rhi::kMaxPushConstantBytes);
            fits_layout = false;
        }
        asset::ShaderEntryCode& entry = entries.emplace();
        copy_name(entry.entry, stage.entry.view());
        entry.stage = shader::to_asset_stage(reflection.stage);
        entry.push_constant_bytes = reflection.push_constant_bytes;
        for (int axis = 0; axis < 3; ++axis) {
            entry.thread_group_size[axis] = reflection.thread_group_size[axis];
        }
        entry.code = writer.append(stage.code.span());
        fields.clear();
        for (const shader::PushConstantField& field : reflection.push_constants) {
            asset::ShaderPushConstant& out = fields.emplace();
            copy_name(out.name, field.name.view());
            out.offset = field.offset;
            out.size = field.size;
        }
        entry.push_constants = writer.append(fields.span());
    }
    if (!fits_layout) {
        return core::fail(core::ErrorCode::Unsupported, "the shader doesn't fit the engine's pipeline layout");
    }
    const asset::ShaderPermutation permutation{.defines_hash = 0, .entries = writer.append(entries.span())};
    asset::ShaderAsset shader;
    copy_name(shader.module, module);
    shader.flags = flags;
    shader.permutations = writer.append(std::span(&permutation, 1));
    return writer.finish(shader, asset::shader_asset_id(module), {});
}

} // namespace de::cooker
