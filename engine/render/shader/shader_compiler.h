#pragma once

#include "core/array.h"
#include "core/fixed_string.h"
#include "core/result.h"
#include "platform/file.h"
#include "render/rhi/rhi.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::platform {
class Vfs;
}

namespace de::shader {

struct CompilerState;

inline constexpr size_t kMaxShaderNameLength = 63;
using ShaderName = core::FixedString<kMaxShaderNameLength>;

// A preprocessor define for one permutation ("SKINNED" = "1"). Both strings are copied.
struct ShaderDefine {
    const char* name = nullptr;
    const char* value = "1";
};

inline constexpr size_t kMaxShaderDefines = 16;

struct CompileRequest {
    // A module under src:/shaders/, by Slang module name: "triangle" is shaders/triangle.slang,
    // "common.color" is shaders/common/color.slang.
    const char* module = nullptr;
    const char* entry = nullptr; // the entry point function, e.g. "vertex_main"
    rhi::ShaderStage stage = rhi::ShaderStage::Vertex;
    std::span<const ShaderDefine> defines{};
};

// One field of the push constant block, as the shader lays it out.
struct PushConstantField {
    ShaderName name;
    uint32_t offset = 0;
    uint32_t size = 0;
};

// A resource the shader binds by descriptor slot instead of bindlessly. The engine's pipelines
// have no such slots (resources are reached by index and address), so the library rejects these.
struct ResourceBinding {
    ShaderName name;
    uint32_t set = 0;
    uint32_t binding = 0;
};

struct ShaderReflection {
    rhi::ShaderStage stage = rhi::ShaderStage::Vertex;
    uint32_t push_constant_bytes = 0;   // the size of the [[vk::push_constant]] block, 0 if none
    uint32_t thread_group_size[3] = {}; // compute only
    core::Array<PushConstantField> push_constants;
    core::Array<ResourceBinding> bindings;
};

// A source file a compile read, with its size and write time when it was read: if either
// changes, the shader is stale.
struct ShaderDependency {
    platform::PathString path; // "src:/shaders/common/color.slang"
    platform::FileInfo info;
};

struct CompiledShader {
    core::Array<uint32_t> code; // the backend's bytecode (SPIR-V words)
    ShaderReflection reflection;
    core::Array<ShaderDependency> dependencies;
};

struct ShaderCompilerDesc {
    // Source-level debug info, so RenderDoc and Nsight can step through Slang code.
    bool debug_info = DE_BUILD_DEV != 0;
};

// Compiles Slang shaders to the RHI backend's bytecode, with reflection (docs/design.md, Shaders,
// materials & VFX). Sources are read through the VFS from src:/shaders/.
//
// Conventions for shaders: column-major matrices (mul(matrix, vector), like GLM), push constants
// in one [[vk::push_constant]] ConstantBuffer, resources reached bindlessly (by index or pointer).
//
// Thread-safe: compiles run one at a time (Slang's global session is single-threaded), so call
// it from jobs to keep long compiles off the render thread. Move-only.
class ShaderCompiler {
public:
    // Fails if Slang can't start (its runtime library or standard modules are missing).
    static core::Result<ShaderCompiler> create(platform::Vfs& vfs, const ShaderCompilerDesc& desc = {});

    ~ShaderCompiler();
    ShaderCompiler(ShaderCompiler&& other) noexcept;
    ShaderCompiler& operator=(ShaderCompiler&& other) noexcept;
    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;

    // Compiles one entry point. Errors and warnings (with file and line) are logged under "shader"
    // and, if diagnostics is given, appended to it as text (null-terminated).
    core::Result<CompiledShader> compile(const CompileRequest& request, core::Array<char>* diagnostics = nullptr);

private:
    explicit ShaderCompiler(CompilerState* state) : state_(state) {}

    CompilerState* state_ = nullptr;
};

} // namespace de::shader
