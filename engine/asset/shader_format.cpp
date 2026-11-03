#include "asset/shader_format.h"

#include "core/hash.h"

#include <cstring>

namespace de::asset {

namespace {

bool terminated(const char (&text)[kShaderNameSize]) {
    return std::memchr(text, '\0', kShaderNameSize) != nullptr;
}

} // namespace

core::Result<void> ShaderAsset::validate(std::span<const std::byte> file) const {
    using core::ErrorCode;
    const uint64_t size = file.size();
    if (!terminated(module) || !span_in_file(permutations, size)) {
        return core::fail(ErrorCode::CorruptData, "shader module name or permutation table is broken");
    }
    const std::span<const ShaderPermutation> all = permutations.get(file.data());
    for (size_t i = 0; i < all.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (all[j].defines_hash == all[i].defines_hash) {
                return core::fail(ErrorCode::CorruptData, "shader has the same permutation twice");
            }
        }
        if (!span_in_file(all[i].entries, size)) {
            return core::fail(ErrorCode::CorruptData, "shader entry table lies outside the file");
        }
        for (const ShaderEntryCode& entry : all[i].entries.get(file.data())) {
            if (!terminated(entry.entry) || entry.stage > ShaderStageKind::Compute || entry.code.count == 0 ||
                !span_in_file(entry.code, size) || !span_in_file(entry.push_constants, size)) {
                return core::fail(ErrorCode::CorruptData, "shader entry point is broken");
            }
            // Drivers needn't reject what isn't SPIR-V; some crash on it.
            if (entry.code.get(file.data())[0] != kSpirvMagic) {
                return core::fail(ErrorCode::CorruptData, "shader code isn't SPIR-V");
            }
            for (const ShaderPushConstant& field : entry.push_constants.get(file.data())) {
                if (!terminated(field.name)) {
                    return core::fail(ErrorCode::CorruptData, "shader push constant name is broken");
                }
            }
        }
    }
    return {};
}

const ShaderPermutation* ShaderAsset::find_permutation(const void* file, uint64_t defines_hash) const {
    for (const ShaderPermutation& permutation : permutations.get(file)) {
        if (permutation.defines_hash == defines_hash) {
            return &permutation;
        }
    }
    return nullptr;
}

const ShaderEntryCode* ShaderAsset::find_entry(const void* file, const ShaderPermutation& permutation,
                                               std::string_view entry) {
    for (const ShaderEntryCode& code : permutation.entries.get(file)) {
        if (entry == code.entry) {
            return &code;
        }
    }
    return nullptr;
}

uint64_t shader_defines_hash(std::span<const ShaderDefineText> defines) {
    // XOR of one hash per define, so the order doesn't matter.
    uint64_t hash = 0;
    for (const ShaderDefineText& define : defines) {
        const std::string_view value = define.value.empty() ? std::string_view("1") : define.value;
        hash ^= core::mix64(core::fnv1a_64(define.name.data(), define.name.size()) ^
                            core::mix64(core::fnv1a_64(value.data(), value.size())));
    }
    return hash;
}

} // namespace de::asset
