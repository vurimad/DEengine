#include "platform/file.h"

#include <SDL3/SDL_filesystem.h>
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace de::platform {

namespace {

using core::ErrorCode;
using core::fail;

struct WidePath {
    wchar_t text[kMaxPathLength + 1];
};

bool to_wide(const char* path, WidePath& out) {
    return MultiByteToWideChar(CP_UTF8, 0, path, -1, out.text, static_cast<int>(kMaxPathLength + 1)) > 0;
}

ErrorCode error_from_last() {
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ErrorCode::NotFound : ErrorCode::IoError;
}

HANDLE native(intptr_t handle) {
    return reinterpret_cast<HANDLE>(handle); // NOLINT(performance-no-int-to-ptr): the OS handle round-trips
}

// ReadFile and WriteFile take DWORD sizes: big transfers go in chunks.
constexpr DWORD kMaxChunk = 1U << 30;

} // namespace

core::Result<File> File::open_read(const char* path) {
    WidePath wide;
    if (!to_wide(path, wide)) {
        return fail(ErrorCode::InvalidArgument, "path is not valid UTF-8 or too long");
    }
    HANDLE handle = CreateFileW(wide.text, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return fail(error_from_last(), "file could not be opened for reading");
    }
    return File(reinterpret_cast<intptr_t>(handle));
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
        CloseHandle(native(handle_));
        handle_ = kInvalid;
    }
}

core::Result<uint64_t> File::size() const {
    LARGE_INTEGER size;
    if (GetFileSizeEx(native(handle_), &size) == 0) {
        return fail(ErrorCode::IoError, "file size could not be read");
    }
    return static_cast<uint64_t>(size.QuadPart);
}

core::Result<size_t> File::read_at(uint64_t offset, std::span<std::byte> dest) const {
    size_t total = 0;
    while (total < dest.size()) {
        const auto chunk = static_cast<DWORD>(std::min<size_t>(dest.size() - total, kMaxChunk));
        const uint64_t position = offset + total;
        OVERLAPPED overlapped = {};
        overlapped.Offset = static_cast<DWORD>(position & 0xffffffffU);
        overlapped.OffsetHigh = static_cast<DWORD>(position >> 32U);
        DWORD read = 0;
        if (ReadFile(native(handle_), dest.data() + total, chunk, &read, &overlapped) == 0) {
            if (GetLastError() == ERROR_HANDLE_EOF) {
                break;
            }
            return fail(ErrorCode::IoError, "file read failed");
        }
        total += read;
        if (read < chunk) {
            break; // end of file
        }
    }
    return total;
}

core::Result<void> write_file_atomic(const char* path, std::span<const std::byte> data) {
    WidePath wide;
    WidePath wide_temp;
    PathString temp(path);
    if (!temp.append(".tmp") || !to_wide(path, wide) || !to_wide(temp.c_str(), wide_temp)) {
        return fail(ErrorCode::InvalidArgument, "path is not valid UTF-8 or too long");
    }

    HANDLE handle =
        CreateFileW(wide_temp.text, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return fail(error_from_last(), "file could not be created");
    }
    size_t written_total = 0;
    bool ok = true;
    while (ok && written_total < data.size()) {
        const auto chunk = static_cast<DWORD>(std::min<size_t>(data.size() - written_total, kMaxChunk));
        DWORD written = 0;
        ok = WriteFile(handle, data.data() + written_total, chunk, &written, nullptr) != 0 && written == chunk;
        written_total += written;
    }
    ok = ok && FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    if (!ok) {
        DeleteFileW(wide_temp.text);
        return fail(ErrorCode::IoError, "file write failed");
    }
    if (MoveFileExW(wide_temp.text, wide.text, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        DeleteFileW(wide_temp.text);
        return fail(ErrorCode::IoError, "file could not be replaced");
    }
    return {};
}

bool path_exists(const char* path) {
    WidePath wide;
    return to_wide(path, wide) && GetFileAttributesW(wide.text) != INVALID_FILE_ATTRIBUTES;
}

core::Result<void> create_directories(const char* path) {
    WidePath wide;
    if (!to_wide(path, wide)) {
        return fail(ErrorCode::InvalidArgument, "path is not valid UTF-8 or too long");
    }
    // Create each level in turn: "C:/a/b/c" -> "C:/a", "C:/a/b", "C:/a/b/c".
    const size_t length = std::wcslen(wide.text);
    for (size_t i = 1; i <= length; ++i) {
        const wchar_t c = wide.text[i];
        if (c != L'/' && c != L'\\' && c != L'\0') {
            continue;
        }
        wide.text[i] = L'\0';
        const bool is_drive = i == 2 && wide.text[1] == L':';
        if (!is_drive && CreateDirectoryW(wide.text, nullptr) == 0 && GetLastError() != ERROR_ALREADY_EXISTS) {
            return fail(ErrorCode::IoError, "directory could not be created");
        }
        wide.text[i] = c;
    }
    return {};
}

namespace {

bool delete_tree_wide(WidePath& path) {
    const DWORD attributes = GetFileAttributesW(path.text);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return true; // already gone
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        // Files, and links to directories: remove the entry itself, never follow a link.
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? RemoveDirectoryW(path.text) != 0
                                                            : DeleteFileW(path.text) != 0;
    }
    const size_t length = std::wcslen(path.text);
    if (length + 3 > kMaxPathLength) {
        return false;
    }
    std::memcpy(path.text + length, L"\\*", sizeof(L"\\*"));
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(path.text, &entry);
    path.text[length] = L'\0';
    bool ok = true;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (std::wcscmp(entry.cFileName, L".") == 0 || std::wcscmp(entry.cFileName, L"..") == 0) {
                continue;
            }
            const size_t name_length = std::wcslen(entry.cFileName);
            if (length + 1 + name_length > kMaxPathLength) {
                ok = false;
                continue;
            }
            path.text[length] = L'\\';
            std::memcpy(path.text + length + 1, entry.cFileName, (name_length + 1) * sizeof(wchar_t));
            ok = delete_tree_wide(path) && ok;
            path.text[length] = L'\0';
        } while (FindNextFileW(find, &entry) != 0);
        FindClose(find);
    }
    return RemoveDirectoryW(path.text) != 0 && ok;
}

} // namespace

core::Result<void> delete_tree(const char* path) {
    WidePath wide;
    if (!to_wide(path, wide)) {
        return fail(ErrorCode::InvalidArgument, "path is not valid UTF-8 or too long");
    }
    if (!delete_tree_wide(wide)) {
        return fail(ErrorCode::IoError, "directory tree could not be deleted");
    }
    return {};
}

core::Result<PathString> user_data_folder() {
    // SDL knows the Saved Games folder (FOLDERID_SavedGames).
    const char* saved_games = SDL_GetUserFolder(SDL_FOLDER_SAVEDGAMES);
    if (saved_games == nullptr) {
        return fail(ErrorCode::NotFound, "no Saved Games folder");
    }
    PathString folder(saved_games);
    while (!folder.empty() && (folder[folder.size() - 1] == '\\' || folder[folder.size() - 1] == '/')) {
        folder.assign(folder.view().substr(0, folder.size() - 1));
    }
    return folder;
}

core::Result<PathString> system_temp_folder() {
    wchar_t wide[MAX_PATH + 1];
    const DWORD length = GetTempPathW(MAX_PATH + 1, wide);
    if (length == 0 || length > MAX_PATH) {
        return fail(ErrorCode::NotFound, "the temp folder could not be found");
    }
    char utf8[kMaxPathLength + 1];
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), nullptr, nullptr) == 0) {
        return fail(ErrorCode::InvalidArgument, "the temp folder path is too long");
    }
    PathString folder(utf8);
    while (!folder.empty() && (folder[folder.size() - 1] == '\\' || folder[folder.size() - 1] == '/')) {
        folder.assign(folder.view().substr(0, folder.size() - 1));
    }
    return folder;
}

} // namespace de::platform
