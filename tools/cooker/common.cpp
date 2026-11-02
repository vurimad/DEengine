#include "cooker/common.h"

#include "core/log.h"
#include "platform/vfs.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <iterator>

namespace de::cooker {

core::FixedString<kMaxSourcePath + 8> src_path(std::string_view relative) {
    core::FixedString<kMaxSourcePath + 8> path("src:/");
    if (!path.append(relative)) {
        path.clear();
    }
    return path;
}

// NOLINTNEXTLINE(modernize-avoid-variadic-functions): printf-style by design, checked by DE_PRINTF_FORMAT
void Diagnostics::error(std::string_view path, const char* format, ...) {
    va_list args;
    va_start(args, format);
    add(Severity::Error, path, format, args);
    va_end(args);
}

// NOLINTNEXTLINE(modernize-avoid-variadic-functions): printf-style by design, checked by DE_PRINTF_FORMAT
void Diagnostics::warning(std::string_view path, const char* format, ...) {
    va_list args;
    va_start(args, format);
    add(Severity::Warning, path, format, args);
    va_end(args);
}

void Diagnostics::add(Severity severity, std::string_view path, const char* format, va_list args) {
    Diagnostic diagnostic{.severity = severity};
    if (!diagnostic.path.assign(path)) {
        diagnostic.path.assign(path.substr(path.size() - kMaxSourcePath)); // keep the end, the file's name
    }
    char text[kMaxMessageLength + 1];
    std::vsnprintf(text, sizeof(text), format, args); // cut short if long: messages are for people
    diagnostic.message.assign(text);
    if (severity == Severity::Error) {
        DE_LOG_ERROR("cooker", "%s: %s", diagnostic.path.empty() ? "cook" : diagnostic.path.c_str(), text);
    } else {
        DE_LOG_WARN("cooker", "%s: %s", diagnostic.path.empty() ? "cook" : diagnostic.path.c_str(), text);
    }
    const std::scoped_lock lock(mutex_);
    items_.push(diagnostic);
    errors_ += severity == Severity::Error ? 1 : 0;
}

size_t Diagnostics::error_count() const {
    const std::scoped_lock lock(mutex_);
    return errors_;
}

size_t Diagnostics::warning_count() const {
    const std::scoped_lock lock(mutex_);
    return items_.size() - errors_;
}

core::Array<Diagnostic> Diagnostics::all() const {
    const std::scoped_lock lock(mutex_);
    core::Array<Diagnostic> copy(tools_heap());
    copy.assign(items_.span());
    return copy;
}

core::Result<core::Array<std::byte>> read_whole_file(const platform::Vfs& vfs, const char* path) {
    const core::Result<uint64_t> size = vfs.file_size(path);
    if (!size) {
        return core::fail(size.error());
    }
    core::Array<std::byte> bytes(tools_heap());
    bytes.resize_uninit(static_cast<size_t>(*size));
    const core::Result<size_t> read = vfs.read_file(path, bytes.span());
    if (!read) {
        return core::fail(read.error());
    }
    if (*read != bytes.size()) {
        return core::fail(core::ErrorCode::IoError, "the file changed size while it was read");
    }
    return bytes;
}

std::string_view extension_of(std::string_view path) {
    const std::string_view name = file_name_of(path);
    const size_t dot = name.find_last_of('.');
    return dot == std::string_view::npos || dot == 0 ? std::string_view() : name.substr(dot);
}

std::string_view folder_of(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
}

std::string_view file_name_of(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

bool join_relative(std::string_view folder, std::string_view reference, SourcePath& out) {
    // Segments of the result, as views into folder and reference.
    std::string_view segments[64];
    size_t count = 0;
    const auto push_all = [&](std::string_view text) {
        while (!text.empty()) {
            const size_t slash = text.find('/');
            const std::string_view segment = text.substr(0, slash);
            text = slash == std::string_view::npos ? std::string_view() : text.substr(slash + 1);
            if (segment.empty() || segment == ".") {
                continue;
            }
            if (segment == "..") {
                if (count == 0) {
                    return false;
                }
                --count;
                continue;
            }
            if (count == std::size(segments)) {
                return false;
            }
            segments[count++] = segment;
        }
        return true;
    };
    if (!push_all(folder) || !push_all(reference) || count == 0) {
        return false;
    }
    out.clear();
    for (size_t i = 0; i < count; ++i) {
        if ((i > 0 && !out.append('/')) || !out.append(segments[i])) {
            return false;
        }
    }
    return true;
}

core::FixedString<24> number_text(uint64_t value) {
    char digits[24];
    size_t length = 0;
    do {
        digits[length++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    std::reverse(digits, digits + length);
    return core::FixedString<24>(std::string_view(digits, length));
}

bool glob_match(std::string_view pattern, std::string_view name) {
    // Iterative, with one backtrack point for the last '*'.
    size_t p = 0;
    size_t n = 0;
    size_t star = std::string_view::npos;
    size_t resume = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

} // namespace de::cooker
