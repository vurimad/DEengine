// A build step: writes the hash of shaders/ as a source file of the engine's shader library, with
// the code the cooker hashes them with (asset::shader_sources_hash), so the two agree.
//   de_shader_hash <repository root> <output folder>
// The file is rewritten only when the hash changes, so nothing relinks otherwise.

#include "asset/shader_format.h"
#include "core/allocator.h"
#include "core/log.h"
#include "platform/vfs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

int main(int argc, char** argv) {
    if (argc != 3) {
        DE_LOG_ERROR("shader", "usage: de_shader_hash <repository root> <output folder>");
        return EXIT_FAILURE;
    }
    constexpr const char* kOutput = "game:/shader_source_hash.cpp";
    de::core::set_log_level(de::core::LogLevel::Error); // quiet in the build's output
    const auto vfs = de::platform::Vfs::create({.game_root = argv[2],
                                                .user_root = argv[2],
                                                .temp_root = argv[2],
                                                .src_root = argv[1],
                                                .reader_threads = 1,
                                                .game_writable = true});
    if (!vfs) {
        return EXIT_FAILURE;
    }
    const de::core::Result<uint64_t> hash = de::asset::shader_sources_hash(*vfs);
    if (!hash) {
        DE_LOG_ERROR("shader", "%s/shaders could not be hashed: %s", argv[1], hash.error().message);
        return EXIT_FAILURE;
    }
    char text[256];
    const int length = std::snprintf(text, sizeof(text),
                                     "// Generated from shaders/ by tools/shader_hash. Don't edit.\n"
                                     "#include \"render/shader/shader_asset.h\"\n\n"
                                     "uint64_t de::shader::built_source_hash() {\n"
                                     "    return 0x%016llxULL;\n"
                                     "}\n",
                                     static_cast<unsigned long long>(*hash));
    const std::span<const std::byte> bytes = std::as_bytes(std::span(text, static_cast<size_t>(length)));
    const auto current = vfs->read_whole_file(kOutput, de::core::heap_allocator(de::core::MemoryTag::Tools));
    if (current && current->size() == bytes.size() && std::memcmp(current->data(), bytes.data(), bytes.size()) == 0) {
        return EXIT_SUCCESS;
    }
    if (const de::core::Result<void> written = vfs->write_file(kOutput, bytes); !written) {
        DE_LOG_ERROR("shader", "%s/%s could not be written: %s", argv[2], kOutput, written.error().message);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
