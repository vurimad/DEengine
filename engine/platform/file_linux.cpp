#include "platform/file.h"

#include <SDL3/SDL_filesystem.h>
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
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

// "<path>.<process>-<n>.detmp": unique per write, and unlikely to clash with a real file.
bool temp_path_for(const char* path, PathString& out) {
    static std::atomic<uint32_t> counter{0};
    out.assign(path);
    char suffix[48];
    size_t length = 0;
    const auto add_number = [&](uint32_t value) {
        char digits[10];
        size_t count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count > 0) {
            suffix[length++] = digits[--count];
        }
    };
    suffix[length++] = '.';
    add_number(static_cast<uint32_t>(::getpid()));
    suffix[length++] = '-';
    add_number(counter.fetch_add(1, std::memory_order_relaxed));
    return out.append(std::string_view(suffix, length)) && out.append(".detmp");
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
    PathString temp;
    if (!temp_path_for(path, temp)) {
        return fail(ErrorCode::InvalidArgument, "path is too long");
    }
    const int handle = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
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
    const PathString full(path);
    if (full.size() != std::strlen(path)) {
        return fail(ErrorCode::InvalidArgument, "path is too long");
    }
    // Create each level in turn: "/a/b/c" -> "/a", "/a/b", "/a/b/c".
    for (size_t i = 1; i <= full.size(); ++i) {
        if (i < full.size() && full[i] != '/') {
            continue;
        }
        const PathString level(full.view().substr(0, i));
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
    struct stat info = {};
    if (::lstat(path, &info) != 0) {
        return {}; // already gone
    }
    // Depth first, and never follow symbolic links out of the tree.
    if (::nftw(path, &remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0) {
        return fail(ErrorCode::IoError, "directory tree could not be deleted");
    }
    return {};
}

core::Result<void> list_directory(const char* path,
                                  core::FunctionRef<void(const char* name, bool is_directory)> visit) {
    DIR* directory = ::opendir(path);
    if (directory == nullptr) {
        return fail(error_from_errno(), "directory could not be listed");
    }
    while (const dirent* entry = ::readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        bool is_directory = entry->d_type == DT_DIR;
        if (entry->d_type == DT_UNKNOWN) { // some file systems don't fill d_type
            PathString full(path);
            struct stat info = {};
            is_directory = full.append('/') && full.append(entry->d_name) && ::lstat(full.c_str(), &info) == 0 &&
                           S_ISDIR(info.st_mode);
        }
        visit(entry->d_name, is_directory);
    }
    ::closedir(directory);
    return {};
}

uint32_t current_process_id() {
    return static_cast<uint32_t>(::getpid());
}

bool process_alive(uint32_t process_id) {
    // Signal 0 checks for existence; EPERM means it exists but belongs to someone else.
    return ::kill(static_cast<pid_t>(process_id), 0) == 0 || errno == EPERM;
}

core::Result<PathString> user_data_folder() {
    // Linux has no Saved Games folder: use the XDG data folder.
    PathString folder;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0') {
        folder.assign(xdg);
    } else if (const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME)) {
        folder.assign(home);
        folder.trim_trailing('/', '/', 1);
        if (!folder.append("/.local/share")) {
            return fail(ErrorCode::InvalidArgument, "home folder path is too long");
        }
    } else {
        return fail(ErrorCode::NotFound, "no user data folder");
    }
    folder.trim_trailing('/', '/', 1);
    return folder;
}

core::Result<PathString> system_temp_folder() {
    const char* temp = std::getenv("TMPDIR");
    PathString folder((temp != nullptr && temp[0] != '\0') ? temp : "/tmp");
    folder.trim_trailing('/', '/', 1);
    return folder;
}

} // namespace de::platform
