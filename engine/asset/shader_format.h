#pragma once

#include "asset/format.h"
#include "core/enum_flags.h"

#include <cstdint>
#include <string_view>

namespace de::platform {
class Vfs;
}

namespace de::asset {

// Numbers are stored in files: never renumber, only add.
enum class ShaderStageKind : uint8_t {
    Vertex = 0,
    Fragment = 1,
    Compute = 2,
};

inline constexpr size_t kShaderNameSize = 64;       // 63 characters and the terminator, as in the compiler
inline constexpr uint32_t kSpirvMagic = 0x07230203; // the first word of SPIR-V

// One field of the push constant block.
struct ShaderPushConstant {
    char name[kShaderNameSize] = {};
    uint32_t offset = 0;
    uint32_t size = 0;
};

// One entry point's bytecode and reflection.
struct ShaderEntryCode {
    char entry[kShaderNameSize] = {}; // "vertex_main"
    ShaderStageKind stage = ShaderStageKind::Vertex;
    uint8_t reserved[3] = {};
    uint32_t push_constant_bytes = 0;
    uint32_t thread_group_size[3] = {}; // compute only
    uint32_t reserved1 = 0;
    OffsetSpan<uint32_t> code; // SPIR-V words
    OffsetSpan<ShaderPushConstant> push_constants;
};
static_assert(sizeof(ShaderEntryCode) == 104);

// The module compiled with one set of defines.
struct ShaderPermutation {
    uint64_t defines_hash = 0; // shader_defines_hash() of the defines; 0 for none
    OffsetSpan<ShaderEntryCode> entries;
};

enum class ShaderFlags : uint32_t {
    None = 0,
    DebugInfo = 1 << 0, // source-level debug info in the SPIR-V
    Checks = 1 << 1,    // compiled with DE_SHADER_CHECKS=1
};
DE_ENUM_FLAGS(ShaderFlags)

// .deshader (docs/design.md, Asset formats): one Slang module's entry points in SPIR-V, per
// permutation, with their reflection. Its ID is named_asset_id("shader", module name), since code
// names shaders by module ("imgui", "common.color").
struct ShaderAsset {
    static constexpr AssetType kType = AssetType::Shader;
    static constexpr uint16_t kFormatVersion = 2; // 2: typed dependencies

    AssetHeader header;
    char module[kShaderNameSize] = {};
    ShaderFlags flags = ShaderFlags::None;
    uint32_t reserved = 0;
    OffsetSpan<ShaderPermutation> permutations;

    // Arrays in the file, names terminated, permutations unique.
    core::Result<void> validate(std::span<const std::byte> file) const;

    // Null if the module wasn't cooked with these defines, or has no such entry.
    const ShaderPermutation* find_permutation(const void* file, uint64_t defines_hash) const;
    static const ShaderEntryCode* find_entry(const void* file, const ShaderPermutation& permutation,
                                             std::string_view entry);
};
static_assert(sizeof(ShaderAsset) == 144);

inline constexpr std::string_view kShaderAssetKind = "shader";

inline AssetId shader_asset_id(std::string_view module) {
    return named_asset_id(kShaderAssetKind, module);
}

// The same for any order of the defines; 0 for none. A define without a value counts as "1".
struct ShaderDefineText {
    std::string_view name;
    std::string_view value;
};
uint64_t shader_defines_hash(std::span<const ShaderDefineText> defines);

// C++ and shaders share data layouts by hand, and a mismatch reaches the GPU as wild pointers. So
// cooked shaders and the engine both name the sources they came from, by this hash: every .slang
// under src:/shaders/ except shaders/tests/ (which games never load), by path and content, the same
// on a Windows and a Linux checkout (CRLF reads as LF). Any edit counts, even to a comment. Fails
// with NotFound if src:/shaders/ isn't there. Blocking, for tools.
core::Result<uint64_t> shader_sources_hash(const platform::Vfs& vfs);

// Written by the cooker once every module in game:/ is cooked from those sources, removed while it
// works on them: 16 hex digits of shader_sources_hash(). The engine compares it at startup.
inline constexpr const char* kShaderStampPath = "game:/shader_sources.hash";
// NotFound without a stamp, CorruptData for anything else that isn't one.
core::Result<uint64_t> read_shader_stamp(const platform::Vfs& vfs);
core::Result<void> write_shader_stamp(const platform::Vfs& vfs, uint64_t sources_hash);

} // namespace de::asset
