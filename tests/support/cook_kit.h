#pragma once

#include "asset/asset_id.h"
#include "asset/format.h"
#include "cooker/cooker.h"
#include "core/job_system.h"
#include "platform/file.h"
#include "platform/vfs.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace de::test {

// What cooker tests share: sources written to a sandbox, a cook over them, and reading the results.

inline void write_src(const Sandbox& sandbox, std::string_view relative, std::span<const std::byte> bytes) {
    platform::PathString path = sandbox.folder("src");
    REQUIRE(path.append('/'));
    REQUIRE(path.append(relative));
    const platform::PathString parent(path.view().substr(0, path.view().find_last_of('/')));
    REQUIRE(platform::create_directories(parent.c_str()).ok());
    REQUIRE(platform::write_file_atomic(path.c_str(), bytes).ok());
}

inline void write_src(const Sandbox& sandbox, std::string_view relative, std::string_view text) {
    write_src(sandbox, relative, bytes_of(text));
}

inline void remove_src(const Sandbox& sandbox, std::string_view relative) {
    platform::PathString path = sandbox.folder("src");
    REQUIRE(path.append('/'));
    REQUIRE(path.append(relative));
    REQUIRE(platform::delete_tree(path.c_str()).ok());
}

inline std::string read_text(const platform::Vfs& vfs, const char* path) {
    const auto size = vfs.file_size(path);
    REQUIRE(size.ok());
    std::string text(static_cast<size_t>(*size), '\0');
    REQUIRE(vfs.read_file(path, std::as_writable_bytes(std::span(text.data(), text.size()))).ok());
    return text;
}

inline platform::Vfs cooker_vfs(const Sandbox& sandbox, bool writable = true) {
    return sandbox.make_vfs({.game_writable = writable});
}

inline cooker::CookReport run_cook(platform::Vfs& vfs, cooker::CookOptions options = {}) {
    core::JobSystem jobs({.worker_count = 2});
    return cooker::cook(vfs, jobs, nullptr, options);
}

inline bool has_message(const cooker::CookReport& report, std::string_view path, std::string_view text) {
    for (const cooker::Diagnostic& diagnostic : report.diagnostics) {
        if (diagnostic.path == path && diagnostic.message.view().find(text) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

inline asset::Guid guid_of(const platform::Vfs& vfs, const char* meta_path) {
    const std::string text = read_text(vfs, meta_path);
    const size_t at = text.find(R"("guid": ")");
    REQUIRE(at != std::string::npos);
    asset::Guid guid;
    REQUIRE(asset::parse_guid(std::string_view(text).substr(at + 9, asset::kGuidTextLength), guid));
    return guid;
}

// A cooked file in aligned memory.
class Cooked {
public:
    Cooked(const platform::Vfs& vfs, asset::AssetId id, asset::AssetType type) {
        const asset::CookedPath path = asset::cooked_path(id, type);
        const auto size = vfs.file_size(path.c_str());
        REQUIRE_MESSAGE(size.ok(), path.c_str(), " is missing");
        blocks_.resize((*size + 15) / 16);
        size_ = static_cast<size_t>(*size);
        REQUIRE(vfs.read_file(path.c_str(), bytes()).ok());
    }
    std::span<std::byte> bytes() { return {reinterpret_cast<std::byte*>(blocks_.data()), size_}; }
    const void* data() { return blocks_.data(); }

    template <typename T>
    const T& load() {
        const auto loaded = asset::load_in_place<T>(bytes());
        REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? "" : loaded.error().message));
        CHECK(asset::verify_content_hash(bytes()));
        return **loaded;
    }

private:
    struct alignas(16) Block {
        std::byte bytes[16];
    };
    std::vector<Block> blocks_;
    size_t size_ = 0;
};

} // namespace de::test
