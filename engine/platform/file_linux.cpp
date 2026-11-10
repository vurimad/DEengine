#include "platform/file.h"

#include <SDL3/SDL_filesystem.h>
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <climits>
#include <csignal>
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

core::Result<FileInfo> file_info(const char* path) {
    struct stat info = {};
    if (::stat(path, &info) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return fail(ErrorCode::NotFound, "no file at this path");
        }
        return fail(ErrorCode::IoError, "file attributes could not be read");
    }
    return FileInfo{
        .size = static_cast<uint64_t>(info.st_size),
        .modified_time = (static_cast<uint64_t>(info.st_mtim.tv_sec) * 1'000'000'000ULL) +
                         static_cast<uint64_t>(info.st_mtim.tv_nsec),
        .is_directory = S_ISDIR(info.st_mode),
    };
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

core::Result<void> move_path(const char* from, const char* to) {
    struct stat info = {};
    if (::lstat(from, &info) != 0) {
        return fail(ErrorCode::NotFound, "nothing to move there");
    }
    if (::lstat(to, &info) == 0) {
        return fail(ErrorCode::AlreadyExists, "something is there already");
    }
    PathString parent(to);
    const size_t slash = parent.view().find_last_of('/');
    if (slash != std::string_view::npos && slash > 0) {
        parent.truncate(slash);
        if (const core::Result<void> made = create_directories(parent.c_str()); !made) {
            return made;
        }
    }
    if (::rename(from, to) != 0) {
        return fail(error_from_errno(), "could not be moved");
    }
    return {};
}

core::Result<void> move_to_trash(const char* path) {
    // The freedesktop.org trash: the file under files/, a note of where it was under info/.
    char absolute[PATH_MAX];
    if (::realpath(path, absolute) == nullptr) {
        return fail(error_from_errno(), "nothing to delete there");
    }
    const char* data = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    PathString trash;
    if (data != nullptr && data[0] != '\0') {
        trash.assign(data);
    } else if (home == nullptr || !trash.assign(home) || !trash.append("/.local/share")) {
        return fail(ErrorCode::NotFound, "no trash folder (HOME isn't set)");
    }
    if (!trash.append("/Trash")) {
        return fail(ErrorCode::InvalidArgument, "path is too long");
    }
    PathString files = trash;
    PathString info = trash;
    if (!files.append("/files") || !info.append("/info") || !create_directories(files.c_str()).ok() ||
        !create_directories(info.c_str()).ok()) {
        return fail(ErrorCode::IoError, "the trash folder could not be made");
    }
    const std::string_view full(absolute);
    const std::string_view leaf = full.substr(full.find_last_of('/') + 1);
    // A name not taken there yet.
    for (uint32_t attempt = 0; attempt < 1000; ++attempt) {
        char name[kMaxPathLength];
        const int length =
            attempt == 0
                ? std::snprintf(name, sizeof(name), "%.*s", static_cast<int>(leaf.size()), leaf.data())
                : std::snprintf(name, sizeof(name), "%.*s.%u", static_cast<int>(leaf.size()), leaf.data(), attempt);
        PathString target = files;
        PathString note = info;
        if (length <= 0 || !target.append('/') || !target.append(name) || !note.append('/') || !note.append(name) ||
            !note.append(".trashinfo")) {
            return fail(ErrorCode::InvalidArgument, "path is too long");
        }
        struct stat existing = {};
        if (::lstat(target.c_str(), &existing) == 0 || ::lstat(note.c_str(), &existing) == 0) {
            continue;
        }
        char text[kMaxPathLength + 128];
        const int text_length =
            std::snprintf(text, sizeof(text), "[Trash Info]\nPath=%s\nDeletionDate=1970-01-01T00:00:00\n", absolute);
        if (text_length <= 0 ||
            !write_file_atomic(note.c_str(), std::as_bytes(std::span(text, static_cast<size_t>(text_length)))).ok()) {
            return fail(ErrorCode::IoError, "the trash note could not be written");
        }
        if (::rename(absolute, target.c_str()) != 0) {
            static_cast<void>(::remove(note.c_str()));
            return fail(error_from_errno(), "could not be moved to the trash");
        }
        return {};
    }
    return fail(ErrorCode::IoError, "the trash has too many of that name");
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

const char* program_suffix() {
    return "";
}

} // namespace de::platform
