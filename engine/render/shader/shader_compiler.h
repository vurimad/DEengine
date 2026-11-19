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

// A preprocessor define for one permutation ("SKINNED" = "1").
struct ShaderDefine {
    const char* name = nullptr;
    const char* value = "1";
};

inline constexpr size_t kMaxShaderDefines = 16;
inline constexpr size_t kMaxShaderEntries = 4;
// A compile of every entry point of a module (the cooker's): at most this many.
inline constexpr size_t kMaxModuleEntries = 32;

struct ShaderEntry {
    const char* name = nullptr; // the entry point function, e.g. "vertex_main"
    rhi::ShaderStage stage = rhi::ShaderStage::Vertex;
};

// One program: several entry points of one module, compiled together (the module and its
// imports are parsed and checked once).
struct CompileRequest {
    // A module under engine:/shaders/, by Slang module name: "triangle" is shaders/triangle.slang,
    // "common.color" is shaders/common/color.slang.
    const char* module = nullptr;
    // Empty: every entry point the module marks with [shader("...")], in the order it defines them.
    std::span<const ShaderEntry> entries{};
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

struct CompiledStage {
    ShaderName entry;           // the entry point's name
    core::Array<uint32_t> code; // the backend's bytecode (SPIR-V words)
    ShaderReflection reflection;
};

struct CompiledProgram {
    core::Array<CompiledStage> stages; // in the order of CompileRequest::entries
};

// A source file a compile read, with its size and write time when it was read: if either
// changes, the shader is stale. A file that couldn't be read has a zero info.
struct ShaderDependency {
    platform::PathString path{}; // "engine:/shaders/common/color.slang"
    platform::FileInfo info{};
    uint64_t content_hash = 0; // of what was read (core::hash_bytes): what the cache knows the file by
};

// What a compile reports besides its result, failed compiles included: the files it read (so a
// fix to any of them can be noticed) and Slang's messages, with file, line and column.
struct CompileReport {
    core::Array<ShaderDependency> dependencies;
    core::Array<char> diagnostics; // null-terminated text; empty without messages
};

struct ShaderCompilerDesc {
    // Source-level debug info, so RenderDoc and Nsight can step through Slang code.
    bool debug_info = DE_BUILD_DEV != 0;
    // Defines DE_SHADER_CHECKS to 1 (0 otherwise): shaders check what they can afford to, such
    // as bindless indices against their table (shaders/common/bindless.slang).
    bool checks = DE_BUILD_DEV != 0;
    // A folder to keep compiled programs in between runs ("user:/shader_cache"), or null for none
    // (shader_cache.h). A program compiled before is then read back, not compiled, as long as every
    // source that compile read still has the same content; one that compiled with warnings isn't
    // kept, so its warnings are seen every time. For the engine, which compiles the same programs at
    // every start; tools that compile once (the cooker) leave it off.
    const char* cache_folder = nullptr;
    // How many compiles may run at once, from as many threads (1 to kMaxParallelCompiles). Each takes one of Slang's
    // global sessions, which is single-threaded: the first is made with the compiler, the others when that many
    // compiles first overlap (a fraction of a second and some tens of megabytes each, kept from then on). A caller
    // that asks for more at once waits its turn. 1: one at a time, as tools that compile in order want.
    uint32_t parallel_compiles = 1;
};

inline constexpr uint32_t kMaxParallelCompiles = 16;

struct ShaderCompilerStats {
    uint32_t compiled = 0;   // programs Slang compiled (failed compiles included)
    uint32_t from_cache = 0; // programs read back from the cache folder
};

// Compiles Slang shaders to the RHI backend's bytecode, with reflection (docs/design.md, Shaders,
// materials & VFX). Sources are read through the VFS from engine:/shaders/.
//
// Conventions for shaders: column-major matrices (mul(matrix, vector), like GLM), push constants
// in one [[vk::push_constant]] ConstantBuffer, resources reached bindlessly (by index or pointer).
//
// Thread-safe: up to ShaderCompilerDesc::parallel_compiles compiles run at once, each on a Slang
// global session of its own (one is single-threaded), and the rest wait their turn; run them as
// background jobs to keep long compiles off the thread that renders. Reading the cache doesn't wait
// for them. Move-only.
class ShaderCompiler {
public:
    // Fails if Slang can't start (its runtime library or standard modules are missing).
    static core::Result<ShaderCompiler> create(platform::Vfs& vfs, const ShaderCompilerDesc& desc = {});

    ~ShaderCompiler();
    ShaderCompiler(ShaderCompiler&& other) noexcept;
    ShaderCompiler& operator=(ShaderCompiler&& other) noexcept;
    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;

    // Errors and warnings are logged under "shader" and, with a report, collected in it.
    core::Result<CompiledProgram> compile(const CompileRequest& request, CompileReport* report = nullptr);

    // The program compile() would give, if the cache has it for the sources as they are now; NotFound if it
    // doesn't (or there's no cache folder), without compiling. Never waits for a compile running on another
    // thread: for a caller that would sooner compile in the background than block (ShaderLibrary's first builds).
    core::Result<CompiledProgram> read_cached(const CompileRequest& request, CompileReport* report = nullptr);

    // Lets go of the sessions no compile is using, all but one, with what each keeps loaded (some tens of megabytes).
    // For when a burst of compiles is over (a start with nothing cached); the next burst makes them again, a
    // fraction of a second each. Thread-safe.
    void release_idle_sessions();

    ShaderCompilerStats stats() const;
    // How many compiles run at once (ShaderCompilerDesc::parallel_compiles, held to its range).
    uint32_t parallel_compiles() const;

private:
    explicit ShaderCompiler(CompilerState* state) : state_(state) {}

    CompilerState* state_ = nullptr;
};

} // namespace de::shader
