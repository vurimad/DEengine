#include "platform/file.h"

#include <SDL3/SDL_filesystem.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace de::platform {

namespace {

using core::ErrorCode;
using core::fail;

// Long enough for kMaxPathLength plus the \\?\UNC\ prefix.
constexpr DWORD kWideCapacity = kMaxPathLength + 16;

struct WidePath {
    wchar_t text[kWideCapacity];
};

// UTF-8 path -> absolute wide path with the \\?\ prefix, so the OS doesn't stop at MAX_PATH (260).
bool to_wide(const char* path, WidePath& out) {
    wchar_t relative[kWideCapacity];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, relative, static_cast<int>(kWideCapacity)) <= 0) {
        return false;
    }
    wchar_t absolute[kWideCapacity];
    const DWORD length = GetFullPathNameW(relative, kWideCapacity, absolute, nullptr); // also turns / into \.
    if (length == 0 || length >= kWideCapacity - 8) {
        return false;
    }
    const bool already_prefixed = std::wcsncmp(absolute, L"\\\\?\\", 4) == 0;
    const bool unc = !already_prefixed && std::wcsncmp(absolute, L"\\\\", 2) == 0;
    const wchar_t* prefix = already_prefixed ? L"" : (unc ? L"\\\\?\\UNC" : L"\\\\?\\");
    const wchar_t* rest = unc ? absolute + 1 : absolute; // \\server\share -> \\?\UNC\server\share
    const size_t prefix_length = std::wcslen(prefix);
    const size_t rest_length = std::wcslen(rest);
    if (prefix_length + rest_length + 1 > kWideCapacity) {
        return false;
    }
    std::memcpy(out.text, prefix, prefix_length * sizeof(wchar_t));
    std::memcpy(out.text + prefix_length, rest, (rest_length + 1) * sizeof(wchar_t));
    return true;
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
    add_number(GetCurrentProcessId());
    suffix[length++] = '-';
    add_number(counter.fetch_add(1, std::memory_order_relaxed));
    return out.append(std::string_view(suffix, length)) && out.append(".detmp");
}

} // namespace

core::Result<File> File::open_read(const char* path) {
    WidePath wide;
    if (!to_wide(path, wide)) {
        return fail(ErrorCode::InvalidArgument, "path is not valid UTF-8 or too long");
    }
    // Share everything: tools may hold files open for writing (logs), and the cooker replaces
    // files by rename while they're being read.
    HANDLE handle = CreateFileW(wide.text, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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
    PathString temp;
    WidePath wide;
    WidePath wide_temp;
    if (!temp_path_for(path, temp) || !to_wide(path, wide) || !to_wide(temp.c_str(), wide_temp)) {
        return fail(ErrorCode::InvalidArgument, "path is not valid UTF-8 or too long");
    }

    HANDLE handle = CreateFileW(wide_temp.text, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
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
    // Replacing a file fails for a moment while another rename onto it, an antivirus scan or an
    // indexer holds it: retry those transient errors briefly (up to about half a second).
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(wide_temp.text, wide.text, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
            return {};
        }
        const DWORD error = GetLastError();
        const bool transient = error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION;
        if (!transient || attempt == 100) {
            DeleteFileW(wide_temp.text);
            return fail(ErrorCode::IoError, "file could not be replaced");
        }
        Sleep(attempt < 10 ? 0 : 5);
    }
}

bool path_exists(const char* path) {
    WidePath wide;
    return to_wide(path, wide) && GetFileAttributesW(wide.text) != INVALID_FILE_ATTRIBUTES;
}

core::Result<FileInfo> file_info(const char* path) {
    WidePath wide;
    if (!to_wide(path, wide)) {
        return fail(ErrorCode::InvalidArgument, "path is not valid UTF-8 or too long");
    }
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (GetFileAttributesExW(wide.text, GetFileExInfoStandard, &data) == 0) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return fail(ErrorCode::NotFound, "no file at this path");
        }
        return fail(ErrorCode::IoError, "file attributes could not be read");
    }
    return FileInfo{
        .size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32U) | data.nFileSizeLow,
        .modified_time =
            (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32U) | data.ftLastWriteTime.dwLowDateTime,
        .is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
    };
}

core::Result<void> create_directories(const char* path) {
    WidePath wide;
    if (!to_wide(path, wide)) {
        return fail(ErrorCode::InvalidArgument, "path is not valid UTF-8 or too long");
    }
    // Create each level after the root: \\?\C:\a\b -> \\?\C:\a, \\?\C:\a\b.
    const size_t length = std::wcslen(wide.text);
    const wchar_t* root_end = std::wcschr(wide.text + 4, L'\\'); // first separator after \\?\X:
    if (root_end == nullptr) {
        return {}; // a drive root
    }
    for (size_t i = static_cast<size_t>(root_end - wide.text) + 1; i <= length; ++i) {
        const wchar_t c = wide.text[i];
        if (c != L'\\' && c != L'\0') {
            continue;
        }
        wide.text[i] = L'\0';
        if (CreateDirectoryW(wide.text, nullptr) == 0 && GetLastError() != ERROR_ALREADY_EXISTS) {
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
    if (length + 3 > kWideCapacity) {
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
            if (length + 1 + name_length >= kWideCapacity) {
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

core::Result<void> list_directory(const char* path,
                                  core::FunctionRef<void(const char* name, bool is_directory)> visit) {
    WidePath wide;
    if (!to_wide(path, wide)) {
        return fail(ErrorCode::InvalidArgument, "path is not valid UTF-8 or too long");
    }
    const size_t length = std::wcslen(wide.text);
    if (length + 3 > kWideCapacity) {
        return fail(ErrorCode::InvalidArgument, "path is too long");
    }
    std::memcpy(wide.text + length, L"\\*", sizeof(L"\\*"));
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(wide.text, &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return fail(error_from_last(), "directory could not be listed");
    }
    do {
        if (std::wcscmp(entry.cFileName, L".") == 0 || std::wcscmp(entry.cFileName, L"..") == 0) {
            continue;
        }
        char name[kMaxPathLength + 1];
        if (WideCharToMultiByte(CP_UTF8, 0, entry.cFileName, -1, name, sizeof(name), nullptr, nullptr) > 0) {
            visit(name, (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        }
    } while (FindNextFileW(find, &entry) != 0);
    FindClose(find);
    return {};
}

uint32_t current_process_id() {
    return GetCurrentProcessId();
}

bool process_alive(uint32_t process_id) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        return GetLastError() == ERROR_ACCESS_DENIED; // exists, but belongs to someone else
    }
    DWORD exit_code = 0;
    const bool running = GetExitCodeProcess(process, &exit_code) != 0 && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

core::Result<PathString> user_data_folder() {
    // SDL knows the Saved Games folder (FOLDERID_SavedGames).
    const char* saved_games = SDL_GetUserFolder(SDL_FOLDER_SAVEDGAMES);
    if (saved_games == nullptr) {
        return fail(ErrorCode::NotFound, "no Saved Games folder");
    }
    PathString folder(saved_games);
    folder.trim_trailing('\\', '/');
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
    folder.trim_trailing('\\', '/');
    return folder;
}

} // namespace de::platform
