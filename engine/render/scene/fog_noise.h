#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::render {

// The noise fog volumes are shaped by: one channel in a cube that tiles on every axis. Small, since
// volumes are only as sharp as the froxel grid, and made when the renderer starts.
inline constexpr uint32_t kFogNoiseSize = 32;
inline constexpr size_t kFogNoiseBytes = size_t{kFogNoiseSize} * kFogNoiseSize * kFogNoiseSize;

// Fills kFogNoiseBytes (x fastest, then y, then z): soft gradient noise in three octaves with billows cut
// into it, stretched to the whole range. The same every time.
void make_fog_noise(std::span<uint8_t> out);

} // namespace de::render
