#include "platform/file.h"

#include <SDL3/SDL_filesystem.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace de::platform {

namespace {

using core::ErrorCode;
using core::fail;

ErrorCode error_from_errno() {
    return errno == ENOENT || errno == ENOTDIR ? ErrorCode::NotFound : ErrorCode::IoError;
}

int native(intptr_t handle) {
    return static_cast<int>(handle);
}

} // namespace

core::Result<File> File::open_read(const char* path) {
    const int handle = ::open(path, O_RDONLY | O_CLOEXEC);
    if (handle < 0) {
        return fail(error_from_errno(), "file could not be opened for reading");
    }
    return File(handle);
}

File::~File() {
    close();
}

File::File(File&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalid;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalid;
    }
    return *this;
}

void File::close() {
    if (handle_ != kInvalid) {
        ::close(native(handle_));
        handle_ = kInvalid;
    }
}

core::Result<uint64_t> File::size() const {
    struct stat info = {};
    if (::fstat(native(handle_), &info) != 0) {
        return fail(ErrorCode::IoError, "file size could not be read");
    }
    return static_cast<uint64_t>(info.st_size);
}

core::Result<size_t> File::read_at(uint64_t offset, std::span<std::byte> dest) const {
    size_t total = 0;
    while (total < dest.size()) {
        const ssize_t read =
            ::pread(native(handle_), dest.data() + total, dest.size() - total, static_cast<off_t>(offset + total));
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(ErrorCode::IoError, "file read failed");
        }
        if (read == 0) {
            break; // end of file
        }
        total += static_cast<size_t>(read);
    }
    return total;
}

core::Result<void> write_file_atomic(const char* path, std::span<const std::byte> data) {
    PathString temp(path);
    if (!temp.append(".tmp")) {
        return fail(ErrorCode::InvalidArgument, "path is too long");
    }
    const int handle = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (handle < 0) {
        return fail(error_from_errno(), "file could not be created");
    }
    size_t written_total = 0;
    bool ok = true;
    while (ok && written_total < data.size()) {
        const ssize_t written = ::write(handle, data.data() + written_total, data.size() - written_total);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        ok = written > 0;
        written_total += ok ? static_cast<size_t>(written) : 0;
    }
    ok = ok && ::fsync(handle) == 0;
    ok = ::close(handle) == 0 && ok;
    if (!ok) {
        ::unlink(temp.c_str());
        return fail(ErrorCode::IoError, "file write failed");
    }
    if (::rename(temp.c_str(), path) != 0) {
        ::unlink(temp.c_str());
        return fail(ErrorCode::IoError, "file could not be replaced");
    }
    return {};
}

bool path_exists(const char* path) {
    struct stat info = {};
    return ::stat(path, &info) == 0;
}

core::Result<void> create_directories(const char* path) {
    PathString partial(path);
    if (partial.size() != std::strlen(path)) {
        return fail(ErrorCode::InvalidArgument, "path is too long");
    }
    // Create each level in turn: "/a/b/c" -> "/a", "/a/b", "/a/b/c".
    for (size_t i = 1; i <= partial.size(); ++i) {
        const bool at_end = i == partial.size();
        if (!at_end && partial[i] != '/') {
            continue;
        }
        PathString level(partial.view().substr(0, i));
        if (::mkdir(level.c_str(), 0755) != 0 && errno != EEXIST) {
            return fail(ErrorCode::IoError, "directory could not be created");
        }
    }
    return {};
}

namespace {

int remove_entry(const char* path, const struct stat* /*info*/, int /*type*/, struct FTW* /*walk*/) {
    return ::remove(path);
}

} // namespace

core::Result<void> delete_tree(const char* path) {
    if (!path_exists(path)) {
        return {};
    }
    // Depth first, and never follow symbolic links out of the tree.
    if (::nftw(path, &remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0) {
        return fail(ErrorCode::IoError, "directory tree could not be deleted");
    }
    return {};
}

core::Result<PathString> user_data_folder() {
    // Linux has no Saved Games folder: use the XDG data folder.
    PathString folder;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0') {
        folder.assign(xdg);
    } else if (const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME)) {
        folder.assign(home);
        while (folder.size() > 1 && folder[folder.size() - 1] == '/') {
            folder.assign(folder.view().substr(0, folder.size() - 1));
        }
        if (!folder.append("/.local/share")) {
            return fail(ErrorCode::InvalidArgument, "home folder path is too long");
        }
    } else {
        return fail(ErrorCode::NotFound, "no user data folder");
    }
    while (folder.size() > 1 && folder[folder.size() - 1] == '/') {
        folder.assign(folder.view().substr(0, folder.size() - 1));
    }
    return folder;
}

core::Result<PathString> system_temp_folder() {
    const char* temp = std::getenv("TMPDIR");
    PathString folder((temp != nullptr && temp[0] != '\0') ? temp : "/tmp");
    while (folder.size() > 1 && folder[folder.size() - 1] == '/') {
        folder.assign(folder.view().substr(0, folder.size() - 1));
    }
    return folder;
}

} // namespace de::platform
