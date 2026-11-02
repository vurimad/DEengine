#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/compiler.h"
#include "core/fixed_string.h"
#include "core/result.h"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>

namespace de::platform {
class Vfs;
}

namespace de::cooker {

// The cooker's memory: everything it allocates is tagged Tools.
inline core::Allocator& tools_heap() {
    return core::heap_allocator(core::MemoryTag::Tools);
}

// A path relative to the source tree's root (src:/): "content/rocks/rock.png", "shaders/imgui.slang".
inline constexpr size_t kMaxSourcePath = 255;
using SourcePath = core::FixedString<kMaxSourcePath>;

// "src:/" + path; fails (empty) if it doesn't fit, which a SourcePath always does.
core::FixedString<kMaxSourcePath + 8> src_path(std::string_view relative);

enum class Severity : uint8_t {
    Warning,
    Error,
};

inline constexpr size_t kMaxMessageLength = 383;

// One finding of a cook, about one file: what was expected and where (docs/design.md, AI-friendly:
// error messages say what was expected and where).
struct Diagnostic {
    Severity severity = Severity::Error;
    SourcePath path{}; // the file it is about; empty for the cook as a whole
    core::FixedString<kMaxMessageLength> message{};
};

// Findings collected over a cook, logged as they come (channel "cooker"). Thread-safe: texture and
// model cooks report from jobs.
class Diagnostics {
public:
    // NOLINTBEGIN(modernize-avoid-variadic-functions): printf-style by design, checked by DE_PRINTF_FORMAT
    void error(std::string_view path, const char* format, ...) DE_PRINTF_FORMAT(3, 4);
    void warning(std::string_view path, const char* format, ...) DE_PRINTF_FORMAT(3, 4);
    // NOLINTEND(modernize-avoid-variadic-functions)

    size_t error_count() const;
    size_t warning_count() const;
    // A copy, under the lock.
    core::Array<Diagnostic> all() const;

private:
    void add(Severity severity, std::string_view path, const char* format, va_list args);

    mutable std::mutex mutex_;
    core::Array<Diagnostic> items_{tools_heap()};
    size_t errors_ = 0;
};

// The whole file into memory (Tools heap). Blocking: for tools.
core::Result<core::Array<std::byte>> read_whole_file(const platform::Vfs& vfs, const char* path);

// The extension of a file name, lowercase as paths are, with the dot: ".png"; empty if none.
std::string_view extension_of(std::string_view path);
// The folder part of a path, without the trailing slash: "content/rocks"; empty for a bare name.
std::string_view folder_of(std::string_view path);
// The last segment: "rock.png".
std::string_view file_name_of(std::string_view path);

// Joins a relative reference (a glTF image URI, a material's texture) to the folder of the file
// that makes it, resolving "." and ".." segments: ("content/a/b", "../c/x.png") -> "content/a/c/x.png".
// Fails for a reference that climbs out of the tree, or one too long.
bool join_relative(std::string_view folder, std::string_view reference, SourcePath& out);

// Decimal digits of value.
core::FixedString<24> number_text(uint64_t value);

// Whether a file name matches a folder rule's pattern: '*' any run of characters, '?' one character.
bool glob_match(std::string_view pattern, std::string_view name);

} // namespace de::cooker
