#include "image_diff/image_diff.h"

#include "core/log.h"
#include "platform/file.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <system_error>

namespace {

void* tools_malloc(size_t size) {
    return de::core::tagged_malloc(de::core::MemoryTag::Tools, size);
}

void* tools_realloc(void* pointer, size_t size) {
    return de::core::tagged_realloc(de::core::MemoryTag::Tools, pointer, size);
}

void tools_free(void* pointer) {
    de::core::tagged_free(de::core::MemoryTag::Tools, pointer);
}

} // namespace

// stb's own names and configuration. Static: the cooker, linked into the tests with this, has an stb_image of its own,
// and the tests an stb_image_write.
// NOLINTBEGIN(readability-identifier-naming, bugprone-macro-parentheses)
#define STBI_MALLOC(size) tools_malloc(size)
#define STBI_REALLOC(pointer, size) tools_realloc(pointer, size)
#define STBI_FREE(pointer) tools_free(pointer)
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_FAILURE_USERMSG
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBIW_MALLOC(size) tools_malloc(size)
#define STBIW_REALLOC(pointer, size) tools_realloc(pointer, size)
#define STBIW_FREE(pointer) tools_free(pointer)
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
// NOLINTEND(readability-identifier-naming, bugprone-macro-parentheses)

#include <stb_image.h>
#include <stb_image_write.h>

namespace de::image_diff {

namespace {

constexpr size_t kChannels = 4;
constexpr uint8_t kMaskCutoff = 128;

core::Allocator& tools_heap() {
    return core::heap_allocator(core::MemoryTag::Tools);
}

// Where stb's encoder hands over what it made: all of it, in one go.
void keep_png(void* context, void* data, int size) {
    auto& png = *static_cast<core::Array<std::byte>*>(context);
    png.resize(static_cast<size_t>(size));
    std::memcpy(png.data(), data, static_cast<size_t>(size));
}

core::Result<core::Array<std::byte>> read_file(const char* path) {
    auto file = platform::File::open_read(path);
    if (!file) {
        return core::fail(file.error());
    }
    const core::Result<uint64_t> size = file->size();
    if (!size) {
        return core::fail(size.error());
    }
    core::Array<std::byte> bytes(tools_heap());
    bytes.resize(static_cast<size_t>(*size));
    const core::Result<size_t> read = file->read_at(0, bytes.span());
    if (!read) {
        return core::fail(read.error());
    }
    if (*read != bytes.size()) {
        return core::fail(core::ErrorCode::IoError, "the file got shorter while it was read");
    }
    return bytes;
}

core::Result<Image> read_png(const char* path) {
    const core::Result<core::Array<std::byte>> bytes = read_file(path);
    if (!bytes) {
        return core::fail(bytes.error());
    }
    return decode_png(bytes->span());
}

template <typename Whole>
bool parse_whole(std::string_view word, Whole& out) {
    const auto [stop, error] = std::from_chars(word.data(), word.data() + word.size(), out);
    return error == std::errc{} && stop == word.data() + word.size();
}

// What the command line says.
struct Arguments {
    const char* a = nullptr;
    const char* b = nullptr;
    const char* mask = nullptr;
    const char* out = nullptr;
    uint32_t threshold = 0;
    uint64_t max_pixels = 0;
};

bool parse_arguments(int argc, char** argv, Arguments& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view word(argv[i]);
        const bool is_flag = word.starts_with("--");
        if (is_flag && i + 1 == argc) {
            DE_LOG_ERROR("image_diff", "%s needs a value", argv[i]);
            return false;
        }
        if (word == "--mask") {
            out.mask = argv[++i];
        } else if (word == "--out") {
            out.out = argv[++i];
        } else if (word == "--threshold") {
            if (!parse_whole(argv[++i], out.threshold) || out.threshold > 255) {
                DE_LOG_ERROR("image_diff", "--threshold must be a whole number from 0 to 255: '%s'", argv[i]);
                return false;
            }
        } else if (word == "--max-pixels") {
            if (!parse_whole(argv[++i], out.max_pixels)) {
                DE_LOG_ERROR("image_diff", "--max-pixels must be a whole number: '%s'", argv[i]);
                return false;
            }
        } else if (is_flag) {
            DE_LOG_ERROR("image_diff", "unknown flag %s", argv[i]);
            return false;
        } else if (out.a == nullptr) {
            out.a = argv[i];
        } else if (out.b == nullptr) {
            out.b = argv[i];
        } else {
            DE_LOG_ERROR("image_diff", "two pictures to compare; a third: %s", argv[i]);
            return false;
        }
    }
    if (out.b == nullptr) {
        DE_LOG_ERROR("image_diff",
                     "usage: image_diff A.png B.png [--mask M.png] [--threshold N] [--max-pixels N] [--out DIFF.png]");
        return false;
    }
    return true;
}

bool write_difference(const char* path, const Image& difference) {
    const core::Result<core::Array<std::byte>> png = encode_png(difference);
    if (!png) {
        DE_LOG_ERROR("image_diff", "%s: %s", path, png.error().message);
        return false;
    }
    const std::string_view text(path);
    if (const size_t slash = text.find_last_of("/\\"); slash != std::string_view::npos && slash != 0) {
        const platform::PathString parent(text.substr(0, slash));
        if (const core::Result<void> made = platform::create_directories(parent.c_str()); !made) {
            DE_LOG_ERROR("image_diff", "%s: %s", parent.c_str(), made.error().message);
            return false;
        }
    }
    if (const core::Result<void> written = platform::write_file_atomic(path, png->span()); !written) {
        DE_LOG_ERROR("image_diff", "%s: %s", path, written.error().message);
        return false;
    }
    return true;
}

} // namespace

core::Result<Image> decode_png(std::span<const std::byte> file) {
    if (file.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return core::fail(core::ErrorCode::OutOfRange, "the file is too big for a PNG reader");
    }
    int width = 0;
    int height = 0;
    int channels_in_file = 0;
    stbi_uc* const pixels =
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(file.data()), static_cast<int>(file.size()), &width,
                              &height, &channels_in_file, static_cast<int>(kChannels));
    if (pixels == nullptr) {
        return core::fail(core::ErrorCode::CorruptData, stbi_failure_reason());
    }
    Image image;
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.rgba.resize(static_cast<size_t>(uint64_t{image.width} * image.height * kChannels));
    std::memcpy(image.rgba.data(), pixels, image.rgba.size());
    stbi_image_free(pixels);
    return image;
}

core::Result<core::Array<std::byte>> encode_png(const Image& image, uint32_t channels) {
    if ((channels != 3 && channels != 4) || image.width == 0 || image.height == 0 ||
        image.rgba.size() != uint64_t{image.width} * image.height * kChannels) {
        return core::fail(core::ErrorCode::InvalidArgument, "a picture to encode has 3 or 4 channels, and some pixels");
    }
    core::Array<uint8_t> packed(tools_heap());
    packed.resize(uint64_t{image.width} * image.height * channels);
    const size_t pixels = uint64_t{image.width} * image.height;
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        std::memcpy(packed.data() + (pixel * channels), image.rgba.data() + (pixel * kChannels), channels);
    }
    core::Array<std::byte> png(tools_heap());
    const int written =
        stbi_write_png_to_func(&keep_png, &png, static_cast<int>(image.width), static_cast<int>(image.height),
                               static_cast<int>(channels), packed.data(), static_cast<int>(image.width * channels));
    if (written == 0 || png.empty()) {
        return core::fail(core::ErrorCode::OutOfMemory, "the PNG couldn't be made");
    }
    return png;
}

Report compare(const Image& a, const Image& b, const Image* mask, uint32_t threshold, Image* difference) {
    DE_ASSERT(a.width == b.width && a.height == b.height && a.rgba.size() == b.rgba.size());
    DE_ASSERT(mask == nullptr || mask->rgba.size() == a.rgba.size());
    if (difference != nullptr) {
        difference->width = a.width;
        difference->height = a.height;
        difference->rgba.resize(a.rgba.size());
    }
    Report report;
    uint64_t total = 0;
    const size_t pixels = a.rgba.size() / kChannels;
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        const uint8_t* const pa = a.rgba.data() + (pixel * kChannels);
        const uint8_t* const pb = b.rgba.data() + (pixel * kChannels);
        uint8_t* const out = difference != nullptr ? difference->rgba.data() + (pixel * kChannels) : nullptr;
        if (mask != nullptr) {
            const uint8_t* const pm = mask->rgba.data() + (pixel * kChannels);
            if (pm[0] < kMaskCutoff && pm[1] < kMaskCutoff && pm[2] < kMaskCutoff) {
                if (out != nullptr) {
                    out[0] = out[1] = out[2] = 0;
                    out[3] = 255;
                }
                continue;
            }
        }
        uint32_t largest = 0;
        for (size_t channel = 0; channel < kChannels; ++channel) {
            const int gap = static_cast<int>(pa[channel]) - static_cast<int>(pb[channel]);
            largest = std::max(largest, static_cast<uint32_t>(std::abs(gap)));
        }
        ++report.compared;
        total += largest;
        if (largest > report.max_difference) {
            report.max_difference = largest;
            report.max_x = static_cast<uint32_t>(pixel % a.width);
            report.max_y = static_cast<uint32_t>(pixel / a.width);
        }
        const bool differs = largest > threshold;
        if (differs) {
            ++report.differing;
        }
        if (out != nullptr) {
            // Red for what differs, at least half bright so one level still shows; a quarter of a's grey for the rest.
            const auto grey = static_cast<uint8_t>(((uint32_t{pa[0]} + pa[1] + pa[2]) / 3) / 4);
            out[0] = differs ? static_cast<uint8_t>(128 + (largest / 2)) : grey;
            out[1] = differs ? uint8_t{0} : grey;
            out[2] = differs ? uint8_t{0} : grey;
            out[3] = 255;
        }
    }
    report.mean_difference =
        report.compared != 0 ? static_cast<double>(total) / static_cast<double>(report.compared) : 0.0;
    return report;
}

int run(int argc, char** argv) {
    Arguments arguments;
    if (!parse_arguments(argc, argv, arguments)) {
        return kExitError; // logged
    }
    const core::Result<Image> a = read_png(arguments.a);
    if (!a) {
        DE_LOG_ERROR("image_diff", "%s: %s", arguments.a, a.error().message);
        return kExitError;
    }
    const core::Result<Image> b = read_png(arguments.b);
    if (!b) {
        DE_LOG_ERROR("image_diff", "%s: %s", arguments.b, b.error().message);
        return kExitError;
    }
    if (a->width != b->width || a->height != b->height) {
        DE_LOG_ERROR("image_diff", "the pictures differ in size: %ux%u and %ux%u", a->width, a->height, b->width,
                     b->height);
        return kExitError;
    }
    core::Result<Image> mask = core::fail(core::ErrorCode::NotFound, "no mask");
    if (arguments.mask != nullptr) {
        mask = read_png(arguments.mask);
        if (!mask) {
            DE_LOG_ERROR("image_diff", "%s: %s", arguments.mask, mask.error().message);
            return kExitError;
        }
        if (mask->width != a->width || mask->height != a->height) {
            DE_LOG_ERROR("image_diff", "the mask's size is %ux%u, and the pictures' %ux%u", mask->width, mask->height,
                         a->width, a->height);
            return kExitError;
        }
    }
    Image difference;
    const Report report =
        compare(*a, *b, mask ? &*mask : nullptr, arguments.threshold, arguments.out != nullptr ? &difference : nullptr);
    if (report.compared == 0) {
        DE_LOG_ERROR("image_diff", "the mask leaves nothing to compare");
        return kExitError;
    }
    const bool same = report.differing <= arguments.max_pixels;
    DE_LOG_INFO("image_diff", "%s: %llu of %llu pixels differ by over %u (%llu allowed); max %u at %u,%u; mean %.6f",
                same ? "PASS" : "FAIL", static_cast<unsigned long long>(report.differing),
                static_cast<unsigned long long>(report.compared), arguments.threshold,
                static_cast<unsigned long long>(arguments.max_pixels), report.max_difference, report.max_x,
                report.max_y, report.mean_difference);
    if (arguments.out != nullptr && !write_difference(arguments.out, difference)) {
        return kExitError; // logged
    }
    return same ? kExitSame : kExitDifferent;
}

} // namespace de::image_diff
