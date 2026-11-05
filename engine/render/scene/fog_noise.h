#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::render {

// The noise fog volumes are shaped by: one channel in a cube that tiles on every axis. Small, since
// volumes are only as sharp as the froxel grid, and made when the renderer starts.
inline constexpr uint32_t kFogNoiseSize = 32;
inline constexpr uint32_t kFogNoiseMips = 6; // down to 1 texel: the mean
// All its mips, back to back: 32^3 + 16^3 + 8^3 + 4^3 + 2^3 + 1.
inline constexpr size_t kFogNoiseBytes = 37449;

// Fills kFogNoiseBytes (largest mip first; in each, x fastest, then y, then z): soft gradient noise in three
// octaves with billows cut into it, stretched to the whole range, and each mip the mean of the one above,
// wrapping. Coarse sampling reads a coarse mip, so a far slice sees big soft shapes, not noise it can't hold.
// The same every time.
void make_fog_noise(std::span<uint8_t> out);

} // namespace de::render
