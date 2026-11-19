#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "platform/file.h"
#include "render/shader/shader_compiler.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::shader {

// Compiled programs kept between runs (docs/design.md: shader caches live in the user's folder): what one cached
// program's file holds, and how it's named. ShaderCompiler reads and writes the files
// (ShaderCompilerDesc::cache_folder).
//
// A file is one request as one compiler setup compiled it: the SPIR-V and reflection of each stage, and every source
// the compile read with its content's hash. It's used again only while each of those sources still hashes the same, so
// an edit to a shader or to anything it imports recompiles it, and nothing else does.
//
// Bump when the files' layout changes, or when ShaderCompiler::compile asks something else of Slang for the same
// request (its target, its session's settings): files from before are then not for this build, and are compiled over.
inline constexpr uint32_t kShaderCacheVersion = 1;

// What names a cached program: the request (module, entry points, defines) and the setup it's compiled with (the
// compiler's options, Slang's version, kShaderCacheVersion), hashed twice from different seeds. file names the file;
// check is stored in it, so a file that another request's name happened to collide with isn't taken for this one's.
struct ShaderCacheKey {
    uint64_t file = 0;
    uint64_t check = 0;
};

// compiler_tag: Slang's build tag, which changes with its version.
ShaderCacheKey shader_cache_key(const CompileRequest& request, const ShaderCompilerDesc& desc,
                                const char* compiler_tag);

// A source a cached program was compiled from, and its content's hash (core::hash_bytes) then.
struct CachedSource {
    platform::PathString path;
    uint64_t content_hash = 0;
};

// A cached program's file. dependencies: what the compile read (ShaderDependency::content_hash set).
core::Array<std::byte> encode_cached_program(const ShaderCacheKey& key, const CompiledProgram& program,
                                             std::span<const ShaderDependency> dependencies,
                                             core::Allocator& allocator);

// Reads a file back, into arrays from allocator. False, with program and sources unspecified, if the bytes aren't a
// whole, undamaged file for this key from this version: cut short, changed on disk, another request's, an older
// layout's.
bool decode_cached_program(std::span<const std::byte> bytes, const ShaderCacheKey& key, core::Allocator& allocator,
                           CompiledProgram& program, core::Array<CachedSource>& sources);

} // namespace de::shader
