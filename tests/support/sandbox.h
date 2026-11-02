#pragma once

#include "platform/file.h"
#include "platform/time.h"
#include "platform/vfs.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

namespace de::test {

inline std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

// A scratch folder per test: game/, src/, user/ and temp/ roots under the OS temp folder, deleted
// at the end.
class Sandbox {
public:
    Sandbox() {
        static std::atomic<int> counter{0};
        root_ = *platform::system_temp_folder();
        REQUIRE(root_.append("/deengine_tests/"));
        char id[32];
        const unsigned long long stamp = platform::time_ns() + static_cast<unsigned long long>(counter.fetch_add(1));
        size_t length = 0;
        for (unsigned long long value = stamp; value != 0 || length == 0; value /= 10) {
            id[length++] = static_cast<char>('0' + value % 10);
        }
        while (length > 0) {
            REQUIRE(root_.append(id[--length]));
        }
        REQUIRE(platform::create_directories(game().c_str()).ok());
        REQUIRE(platform::create_directories(folder("src").c_str()).ok());
    }
    ~Sandbox() { static_cast<void>(platform::delete_tree(root_.c_str())); }
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;
    Sandbox(Sandbox&&) = delete;
    Sandbox& operator=(Sandbox&&) = delete;

    platform::PathString folder(std::string_view name) const {
        platform::PathString path = root_;
        REQUIRE(path.append('/'));
        REQUIRE(path.append(name));
        return path;
    }
    platform::PathString game() const { return folder("game"); }

    // Puts a file into the game data folder, as the cooker would.
    void add_game_file(std::string_view relative, std::string_view contents) const {
        platform::PathString path = game();
        REQUIRE(path.append('/'));
        REQUIRE(path.append(relative));
        const platform::PathString parent(path.view().substr(0, path.view().find_last_of('/')));
        REQUIRE(platform::create_directories(parent.c_str()).ok());
        REQUIRE(platform::write_file_atomic(path.c_str(), bytes_of(contents)).ok());
    }

    // A Vfs over the sandbox folders, src:/ included.
    platform::Vfs make_vfs(platform::VfsDesc desc = {}) const {
        const platform::PathString game_root = game();
        const platform::PathString user_root = folder("user");
        const platform::PathString temp_root = folder("temp");
        const platform::PathString src_root = folder("src");
        desc.game_root = game_root.c_str();
        desc.user_root = user_root.c_str();
        desc.temp_root = temp_root.c_str();
        desc.src_root = src_root.c_str();
        auto vfs = platform::Vfs::create(desc);
        REQUIRE(vfs.ok());
        return std::move(vfs).value();
    }

private:
    platform::PathString root_;
};

} // namespace de::test
