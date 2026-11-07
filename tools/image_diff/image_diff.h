#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/result.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::image_diff {

// The process's exit code.
inline constexpr int kExitSame = 0;      // within the threshold
inline constexpr int kExitDifferent = 1; // more differs than allowed
inline constexpr int kExitError = 2;     // nothing was compared: bad arguments, an unreadable file, different sizes

// A picture in memory: 8 bits a channel, RGBA, the rows tightly packed.
struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    core::Array<uint8_t> rgba{core::heap_allocator(core::MemoryTag::Tools)};
};

// Any PNG: grey, palette or 16 bits come out as 8-bit RGBA, and alpha is opaque where the file has none.
core::Result<Image> decode_png(std::span<const std::byte> file);
// channels: 3 leaves alpha out, 4 keeps it.
core::Result<core::Array<std::byte>> encode_png(const Image& image, uint32_t channels = 3);

struct Report {
    uint64_t compared = 0;  // pixels the mask leaves
    uint64_t differing = 0; // of those, differing by more than the threshold
    // A pixel's difference is its largest channel's, alpha included, 0 to 255.
    uint32_t max_difference = 0;
    uint32_t max_x = 0; // the first pixel that differs by that much
    uint32_t max_y = 0;
    double mean_difference = 0.0; // over the pixels compared
};

// Compares two pictures of one size. mask: optional, of the same size; where it's black (red, green and blue all
// below 128), pixels aren't compared. difference: optional; made into a picture of what differs: pixels over
// the threshold in red, brighter for more, on a dim grey copy of a, and black where the mask leaves them out.
Report compare(const Image& a, const Image& b, const Image* mask, uint32_t threshold, Image* difference);

// The command line: image_diff A.png B.png [--mask M.png] [--threshold N] [--max-pixels N] [--out DIFF.png]
//   --threshold N   how much a pixel may differ, 0 to 255 (default 0: exactly the same)
//   --max-pixels N  how many pixels may differ by more than that (default 0)
//   --out PATH      write the picture of the differences, passing or not
// Paths are the OS's. Logs the numbers, and returns the exit code.
int run(int argc, char** argv);

} // namespace de::image_diff
