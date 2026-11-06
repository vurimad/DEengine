#include "asset/shader_format.h"

#include "asset/asset_id.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/hash.h"
#include "core/text.h"
#include "platform/file.h"
#include "platform/vfs.h"

#include <algorithm>
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

core::Result<uint64_t> shader_sources_hash(const platform::Vfs& vfs) {
    using core::ErrorCode;
    constexpr std::string_view kRoot = "shaders";
    constexpr std::string_view kTests = "shaders/tests";
    constexpr std::string_view kExtension = ".slang";
    core::Allocator& heap = core::heap_allocator(core::MemoryTag::Tools);
    const auto src_path = [](std::string_view relative) {
        platform::PathString path("src:/");
        return path.append(relative) ? path : platform::PathString();
    };

    core::Array<platform::PathString> folders(heap);
    core::Array<platform::PathString> files(heap);
    folders.push(platform::PathString(kRoot));
    bool fits = true;
    while (!folders.empty()) {
        const platform::PathString folder = folders.back();
        folders.pop();
        const core::Result<void> listed =
            vfs.list(src_path(folder.view()).c_str(), [&](const char* name, bool is_directory) {
                if (name[0] == '.') {
                    return;
                }
                platform::PathString child = folder;
                if (!child.append('/') || !child.append(name)) {
                    fits = false;
                    return;
                }
                // Names against the path rules can't be read or imported: the cooker skips them too.
                if (!vfs.resolve(src_path(child.view()).c_str()).ok()) {
                    return;
                }
                if (is_directory && child.view() != kTests) {
                    folders.push(child);
                } else if (!is_directory && child.view().ends_with(kExtension)) {
                    files.push(child);
                }
            });
        if (!listed) {
            return core::fail(listed.error());
        }
    }
    if (!fits) {
        return core::fail(ErrorCode::OutOfRange, "a shader's path is too long");
    }
    // In the same order wherever the OS lists them from.
    std::ranges::sort(files,
                      [](const platform::PathString& a, const platform::PathString& b) { return a.view() < b.view(); });

    uint64_t hash = 0;
    for (const platform::PathString& file : files) {
        core::Result<core::Array<std::byte>> bytes = vfs.read_whole_file(src_path(file.view()).c_str(), heap);
        if (!bytes) {
            return core::fail(bytes.error());
        }
        // In place, CRLF to LF.
        size_t size = 0;
        for (size_t i = 0; i < bytes->size(); ++i) {
            const bool crlf =
                (*bytes)[i] == std::byte{'\r'} && i + 1 < bytes->size() && (*bytes)[i + 1] == std::byte{'\n'};
            if (!crlf) {
                (*bytes)[size++] = (*bytes)[i];
            }
        }
        const uint64_t size64 = size;
        hash = core::hash_bytes(file.c_str(), file.size(), hash);
        hash = core::hash_bytes(&size64, sizeof(size64), hash);
        hash = core::hash_bytes(bytes->data(), size, hash);
    }
    return hash;
}

core::Result<uint64_t> read_shader_stamp(const platform::Vfs& vfs) {
    using core::ErrorCode;
    if (!vfs.exists(kShaderStampPath)) {
        return core::fail(ErrorCode::NotFound, "game:/ has no shader stamp");
    }
    char text[32] = {};
    const core::Result<size_t> read = vfs.read_file(kShaderStampPath, std::as_writable_bytes(std::span(text)));
    if (!read) {
        return core::fail(ErrorCode::CorruptData, "game:/'s shader stamp can't be read as one");
    }
    std::string_view rest(text, *read);
    const std::string_view digits = core::trim(core::next_line(rest));
    uint64_t hash = 0;
    if (!rest.empty() || !parse_hex64(digits, hash)) {
        return core::fail(ErrorCode::CorruptData, "game:/'s shader stamp can't be read as one");
    }
    return hash;
}

core::Result<void> write_shader_stamp(const platform::Vfs& vfs, uint64_t sources_hash) {
    const AssetIdText text = format_asset_id(AssetId::from_value(sources_hash));
    return vfs.write_file(kShaderStampPath, std::as_bytes(std::span(text.c_str(), text.size())));
}

} // namespace de::asset
