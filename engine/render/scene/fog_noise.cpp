#include "render/scene/fog_noise.h"

#include "core/allocator.h"
#include "core/array.h"
#include "core/assert.h"

#include <algorithm>
#include <cmath>

namespace de::render {

namespace {

// A well-mixed number for a lattice point (wrapped to the period by the caller, which is what tiles).
uint32_t lattice_hash(int x, int y, int z, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8DA6B343U ^ static_cast<uint32_t>(y) * 0xD8163841U ^
                 static_cast<uint32_t>(z) * 0xCB1AB31FU ^ seed;
    h ^= h >> 15U;
    h *= 0x2C1B3C6DU;
    h ^= h >> 12U;
    h *= 0x297A2D39U;
    h ^= h >> 15U;
    return h;
}

float unit(uint32_t bits) {
    return static_cast<float>(bits & 0xFFFFU) / 65535.0f;
}

int wrap(int value, int period) {
    return ((value % period) + period) % period;
}

float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// Gradient noise at a point, in lattice cells, about -1 to 1. Tiles every `period` cells.
float gradient_noise(float x, float y, float z, int period, uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int z0 = static_cast<int>(std::floor(z));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const float fz = z - static_cast<float>(z0);
    const auto corner = [&](int dx, int dy, int dz) {
        const uint32_t h = lattice_hash(wrap(x0 + dx, period), wrap(y0 + dy, period), wrap(z0 + dz, period), seed);
        // A direction from the sphere, from two of the hash's numbers.
        const float up = unit(h) * 2.0f - 1.0f;
        const float around = unit(h >> 16U) * 6.2831853f;
        const float flat = std::sqrt(std::max(1.0f - up * up, 0.0f));
        return flat * std::cos(around) * (fx - static_cast<float>(dx)) +
               flat * std::sin(around) * (fy - static_cast<float>(dy)) + up * (fz - static_cast<float>(dz));
    };
    const float u = fade(fx);
    const float v = fade(fy);
    const float w = fade(fz);
    const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    return mix(mix(mix(corner(0, 0, 0), corner(1, 0, 0), u), mix(corner(0, 1, 0), corner(1, 1, 0), u), v),
               mix(mix(corner(0, 0, 1), corner(1, 0, 1), u), mix(corner(0, 1, 1), corner(1, 1, 1), u), v), w) *
           1.5f;
}

// How near a point is to the closest of randomly placed points, one a cell: 1 at one, falling away. Billows.
float billows(float x, float y, float z, int period, uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int z0 = static_cast<int>(std::floor(z));
    float nearest = 1e30f;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const uint32_t h =
                    lattice_hash(wrap(x0 + dx, period), wrap(y0 + dy, period), wrap(z0 + dz, period), seed);
                const float px = static_cast<float>(x0 + dx) + unit(h);
                const float py = static_cast<float>(y0 + dy) + unit(h >> 16U);
                const float pz = static_cast<float>(z0 + dz) + unit(h * 0x9E3779B1U);
                const float ax = x - px;
                const float ay = y - py;
                const float az = z - pz;
                nearest = std::min(nearest, ax * ax + ay * ay + az * az);
            }
        }
    }
    return 1.0f - std::min(std::sqrt(nearest), 1.0f);
}

} // namespace

void make_fog_noise(std::span<uint8_t> out) {
    DE_VERIFY(out.size() == kFogNoiseBytes);
    core::Array<float> values(core::heap_allocator(core::MemoryTag::Render));
    values.resize(kFogNoiseBytes);
    const float per_texel = 1.0f / static_cast<float>(kFogNoiseSize);
    float lowest = 1e30f;
    float highest = -1e30f;
    for (uint32_t z = 0; z < kFogNoiseSize; ++z) {
        for (uint32_t y = 0; y < kFogNoiseSize; ++y) {
            for (uint32_t x = 0; x < kFogNoiseSize; ++x) {
                const float u = static_cast<float>(x) * per_texel;
                const float v = static_cast<float>(y) * per_texel;
                const float w = static_cast<float>(z) * per_texel;
                float soft = 0.0f;
                float weight = 1.0f;
                int cells = 3;
                for (uint32_t octave = 0; octave < 3; ++octave) {
                    soft += weight * gradient_noise(u * static_cast<float>(cells), v * static_cast<float>(cells),
                                                    w * static_cast<float>(cells), cells, 11U + octave);
                    weight *= 0.5f;
                    cells *= 2;
                }
                const float billow = billows(u * 4.0f, v * 4.0f, w * 4.0f, 4, 97U);
                const float value = soft * 0.5f + billow * 0.8f;
                values[size_t{z} * kFogNoiseSize * kFogNoiseSize + size_t{y} * kFogNoiseSize + x] = value;
                lowest = std::min(lowest, value);
                highest = std::max(highest, value);
            }
        }
    }
    const float scale = 255.0f / std::max(highest - lowest, 1e-6f);
    for (size_t i = 0; i < kFogNoiseBytes; ++i) {
        out[i] = static_cast<uint8_t>((values[i] - lowest) * scale + 0.5f);
    }
}

} // namespace de::render
