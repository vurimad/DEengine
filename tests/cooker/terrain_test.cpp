// Terrain cooking, checked against the source by code of its own: what the renderer and the streamer rely on is that a
// sample is the source's at that point, that neighbours agree to the bit, and that a patch's bounds hold whatever it
// can draw. The maps are small and generated here, with enough detail that no sample passes by luck.

#include "asset/environment_format.h"
#include "asset/registry.h"
#include "asset/terrain_format.h"
#include "asset/world_format.h"
#include "cooker/cook_state.h"
#include "cooker/cooker.h"
#include "cooker/terrain_cooker.h"
#include "cooker/world_cooker.h"
#include "core/job_system.h"
#include "platform/vfs.h"
#include "support/cook_kit.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace de::asset;
using de::cooker::CookReport;
using de::platform::Vfs;
using de::test::Cooked;
using de::test::cooker_vfs;
using de::test::guid_of;
using de::test::has_message;
using de::test::remove_src;
using de::test::run_cook;
using de::test::Sandbox;
using de::test::write_src;

namespace {

constexpr AssetId kId = AssetId::from_value(0x7e44a1);
constexpr int64_t kSamples = kTerrainNodeSamples;
constexpr int64_t kBorder = kTerrainNodeBorder;
constexpr int64_t kQuads = kTerrainNodeQuads;

// --- Maps -------------------------------------------------------------------------------------

// A height map and what its pixels mean.
struct Map {
    uint32_t width = 0;
    uint32_t depth = 0;
    double spacing_m = 2.0;
    double lowest_m = -50.0;
    double highest_m = 550.0;
    double origin_m[2] = {0.0, 0.0};
    std::vector<uint16_t> pixels{};

    uint16_t& at(uint32_t x, uint32_t z) { return pixels[(size_t{z} * width) + x]; }

    // The source sample nearest a point on the source's grid, which may lie past the edge.
    uint16_t nearest(int64_t x, int64_t z) const {
        const int64_t column = std::clamp<int64_t>(x, 0, int64_t{width} - 1);
        const int64_t row = std::clamp<int64_t>(z, 0, int64_t{depth} - 1);
        return pixels[static_cast<size_t>((row * width) + column)];
    }

    double height_m(uint16_t pixel) const {
        return lowest_m + (static_cast<double>(pixel) / 65535.0 * (highest_m - lowest_m));
    }
};

// Waves of several sizes and a fine grain that's different along X and Z, so a sample from the wrong place shows. Its
// whole range is 300 spacings, well inside what a node holds.
Map bumpy_map(uint32_t width, uint32_t depth, double spacing_m = 2.0) {
    Map map{.width = width, .depth = depth, .spacing_m = spacing_m, .highest_m = -50.0 + (300.0 * spacing_m)};
    map.pixels.resize(size_t{width} * depth);
    for (uint32_t z = 0; z < depth; ++z) {
        for (uint32_t x = 0; x < width; ++x) {
            const double wave =
                (9000.0 * std::sin(x * 0.043)) + (7000.0 * std::cos(z * 0.031)) + (2000.0 * std::sin((x + z) * 0.17));
            const uint32_t grain = ((x * 73856093U) ^ (z * 19349663U)) % 97U;
            map.at(x, z) = static_cast<uint16_t>(30000.0 + wave + grain);
        }
    }
    return map;
}

Map flat_map(uint32_t width, uint32_t depth, uint16_t pixel) {
    Map map{.width = width, .depth = depth};
    map.pixels.assign(size_t{width} * depth, pixel);
    return map;
}

de::cooker::TerrainSource source_of(const Map& map) {
    de::cooker::TerrainSource source;
    source.meters_per_pixel = map.spacing_m;
    source.lowest_m = map.lowest_m;
    source.highest_m = map.highest_m;
    source.origin_m[0] = map.origin_m[0];
    source.origin_m[1] = map.origin_m[1];
    return source;
}

de::cooker::HeightImage image_of(const Map& map) {
    de::cooker::HeightImage image;
    image.width = map.width;
    image.depth = map.depth;
    image.samples.assign(map.pixels);
    return image;
}

double step_m(const TerrainLevel& level) {
    return static_cast<double>(level.spacing_m) / static_cast<double>(kTerrainStepsPerSpacing);
}

// A height rounded as a level of this spacing rounds it, in steps and back in meters.
int64_t steps_for(double height_m, double spacing_m) {
    return std::llround(height_m / (spacing_m / static_cast<double>(kTerrainStepsPerSpacing)));
}

double rounded_m(double height_m, double spacing_m) {
    return static_cast<double>(steps_for(height_m, spacing_m)) *
           (spacing_m / static_cast<double>(kTerrainStepsPerSpacing));
}

// The bounds a float can give an exact height.
float float_below(double value) {
    auto rounded = static_cast<float>(value);
    while (static_cast<double>(rounded) > value) {
        rounded = std::nextafter(rounded, -std::numeric_limits<float>::infinity());
    }
    return rounded;
}

float float_above(double value) {
    auto rounded = static_cast<float>(value);
    while (static_cast<double>(rounded) < value) {
        rounded = std::nextafter(rounded, std::numeric_limits<float>::infinity());
    }
    return rounded;
}

// A map cooked in memory, read back without the cooker's help.
class CookedMap {
public:
    explicit CookedMap(const Map& map, de::core::JobSystem* jobs = nullptr) {
        de::cooker::Diagnostics diagnostics(false);
        const auto cooked = de::cooker::cook_terrain(source_of(map), image_of(map), kId, jobs, "test", diagnostics);
        REQUIRE(cooked.ok());
        size_ = cooked->terrain.size();
        file_.resize((size_ + 15) / 16);
        std::memcpy(file_.data(), cooked->terrain.data(), size_);
        heights_.assign(cooked->heights.begin(), cooked->heights.end());
        const auto loaded = load_in_place<TerrainAsset>(terrain_file());
        REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? "" : loaded.error().message));
        terrain_ = *loaded;
    }

    std::span<std::byte> terrain_file() { return {reinterpret_cast<std::byte*>(file_.data()), size_}; }
    const std::vector<std::byte>& heights_file() const { return heights_; }
    const TerrainAsset& terrain() const { return *terrain_; }
    std::span<const TerrainLevel> levels() const { return terrain_->levels.get(file_.data()); }
    std::span<const TerrainNode> nodes() const { return terrain_->nodes.get(file_.data()); }
    std::span<const TerrainPatch> patches() const { return terrain_->patches.get(file_.data()); }

    const TerrainNode& node_at(const TerrainLevel& level, int64_t x, int64_t z) const {
        return nodes()[level.first_node + static_cast<size_t>((z * level.nodes_x) + x)];
    }
    const TerrainPatch& patch_at(const TerrainLevel& level, int64_t x, int64_t z) const {
        return patches()[level.first_patch + static_cast<size_t>((z * level.nodes_x * kTerrainPatchesPerNode) + x)];
    }
    // A block's sample, the border included, row and column from its north-west.
    uint16_t sample_of(const TerrainNode& node, int64_t row, int64_t column) const {
        uint16_t value = 0;
        const size_t at = static_cast<size_t>(terrain_block_offset(node.block)) +
                          static_cast<size_t>(((row * kSamples) + column) * 2);
        std::memcpy(&value, heights_.data() + at, 2);
        return value;
    }
    // A block's sample in steps, as a shared edge compares them.
    int64_t steps_of(const TerrainNode& node, int64_t row, int64_t column) const {
        return int64_t{node.base_steps} + sample_of(node, row, column);
    }
    // A level's sample by where it lies, in samples from the map's first: from the node that has it as its own.
    int64_t level_steps(const TerrainLevel& level, int64_t x, int64_t z) const {
        const int64_t node_x = std::min<int64_t>(x / kQuads, int64_t{level.nodes_x} - 1);
        const int64_t node_z = std::min<int64_t>(z / kQuads, int64_t{level.nodes_z} - 1);
        return steps_of(node_at(level, node_x, node_z), z - (node_z * kQuads) + kBorder,
                        x - (node_x * kQuads) + kBorder);
    }

private:
    struct alignas(16) Block {
        std::byte bytes[16];
    };
    std::vector<Block> file_;
    std::vector<std::byte> heights_;
    size_t size_ = 0;
    const TerrainAsset* terrain_ = nullptr;
};

// --- Sources in a sandbox ---------------------------------------------------------------------

void append(std::vector<std::byte>& out, std::span<const uint8_t> bytes) {
    for (const uint8_t byte : bytes) {
        out.push_back(static_cast<std::byte>(byte));
    }
}

void append_big_endian(std::vector<uint8_t>& out, uint32_t value) {
    for (const int shift : {24, 16, 8, 0}) {
        out.push_back(static_cast<uint8_t>(value >> static_cast<uint32_t>(shift)));
    }
}

uint32_t crc32_of(std::span<const uint8_t> bytes) {
    uint32_t crc = 0xffffffffU;
    for (const uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0 ? (crc >> 1U) ^ 0xedb88320U : crc >> 1U;
        }
    }
    return ~crc;
}

void append_chunk(std::vector<std::byte>& out, const char* kind, std::span<const uint8_t> data) {
    std::vector<uint8_t> chunk;
    append_big_endian(chunk, static_cast<uint32_t>(data.size()));
    chunk.insert(chunk.end(), kind, kind + 4);
    chunk.insert(chunk.end(), data.begin(), data.end());
    append_big_endian(chunk, crc32_of(std::span(chunk).subspan(4)));
    append(out, chunk);
}

// A PNG of a map's pixels, stored uncompressed (stb's writer has no 16 bits): gray, gray and alpha, or RGB, each
// channel the pixel, at 16 or 8 bits (their high bytes).
std::vector<std::byte> png_of(uint32_t width, uint32_t depth, std::span<const uint16_t> pixels, uint8_t bits = 16,
                              uint8_t channels = 1) {
    std::vector<uint8_t> raw;
    for (uint32_t z = 0; z < depth; ++z) {
        raw.push_back(0); // no filter
        for (uint32_t x = 0; x < width; ++x) {
            const uint16_t pixel = pixels[(size_t{z} * width) + x];
            for (uint8_t c = 0; c < channels; ++c) {
                raw.push_back(static_cast<uint8_t>(pixel >> 8U));
                if (bits == 16) {
                    raw.push_back(static_cast<uint8_t>(pixel & 0xffU));
                }
            }
        }
    }
    std::vector<uint8_t> zlib = {0x78, 0x01};
    size_t at = 0;
    do {
        const size_t length = std::min<size_t>(raw.size() - at, 65535);
        zlib.push_back(at + length == raw.size() ? 1 : 0);
        zlib.push_back(static_cast<uint8_t>(length & 0xffU));
        zlib.push_back(static_cast<uint8_t>(length >> 8U));
        zlib.push_back(static_cast<uint8_t>(~length & 0xffU));
        zlib.push_back(static_cast<uint8_t>((~length >> 8U) & 0xffU));
        zlib.insert(zlib.end(), raw.begin() + static_cast<ptrdiff_t>(at),
                    raw.begin() + static_cast<ptrdiff_t>(at + length));
        at += length;
    } while (at < raw.size());
    uint32_t adler_low = 1;
    uint32_t adler_high = 0;
    for (const uint8_t byte : raw) {
        adler_low = (adler_low + byte) % 65521U;
        adler_high = (adler_high + adler_low) % 65521U;
    }
    append_big_endian(zlib, (adler_high << 16U) | adler_low);

    std::vector<std::byte> out;
    static constexpr uint8_t kSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    append(out, kSignature);
    std::vector<uint8_t> header;
    append_big_endian(header, width);
    append_big_endian(header, depth);
    header.push_back(bits);
    header.push_back(channels == 1 ? 0 : (channels == 2 ? 4 : 2));
    header.insert(header.end(), {0, 0, 0});
    append_chunk(out, "IHDR", header);
    append_chunk(out, "IDAT", zlib);
    append_chunk(out, "IEND", {});
    return out;
}

std::vector<std::byte> png_of(const Map& map) {
    return png_of(map.width, map.depth, map.pixels);
}

std::string terrain_text(const Map& map, std::string_view height) {
    char text[512];
    std::snprintf(
        text, sizeof(text),
        R"({"height": "%.*s", "meters_per_pixel": %.17g, "lowest_m": %.17g, "highest_m": %.17g, "origin": [%.17g, %.17g]})",
        static_cast<int>(height.size()), height.data(), map.spacing_m, map.lowest_m, map.highest_m, map.origin_m[0],
        map.origin_m[1]);
    return text;
}

// content/land/<name>.terrain and <name>.heightpng.
void write_map(const Sandbox& sandbox, const std::string& name, const Map& map) {
    write_src(sandbox, "content/land/" + name + ".heightpng", png_of(map));
    write_src(sandbox, "content/land/" + name + ".terrain", terrain_text(map, name + ".heightpng"));
}

AssetId id_of(const Vfs& vfs, const std::string& name) {
    return asset_id(guid_of(vfs, ("src:/meta/land/" + name + ".terrain.json").c_str()));
}

bool terrain_cooked(const Vfs& vfs, AssetId id) {
    return vfs.exists(cooked_path(id, AssetType::Terrain).c_str());
}

bool heights_cooked(const Vfs& vfs, AssetId id) {
    return vfs.exists(cooked_path(id, AssetType::TerrainHeights).c_str());
}

std::vector<std::byte> read_all(const Vfs& vfs, const char* path) {
    const auto size = vfs.file_size(path);
    REQUIRE(size.ok());
    std::vector<std::byte> bytes(static_cast<size_t>(*size));
    REQUIRE(vfs.read_file(path, bytes).ok());
    return bytes;
}

constexpr const char* kEnvironment = R"({
  "sun": {"elevation_deg": 30, "azimuth_deg": 90, "color": [1, 0.5, 0.25], "intensity": 4},
  "sky": {"color": [0.3, 0.4, 0.6]},
  "ground": {"color": [0.1, 0.1, 0.1], "intensity": 0.5},
  "shadow_color": [0.05, 0.06, 0.09],
  "exposure_ev": -1,
  "shadows": {"resolution": 1024, "distance_m": 100, "split_lambda": 0.5, "blend_band": 0.1,
              "caster_reach_m": 150, "penumbra_per_m": 0.05, "max_penumbra_texels": 4},
  "fog": {"density_per_m": 0.0001, "height_density_per_m": 0.01, "height_falloff_per_m": 0.1, "base_height_m": -20,
          "extinction_tint": [0.8, 1, 1.25], "sun_forward": {"color": [1, 0.5, 0.25], "intensity": 2},
          "sun_back": {"color": [0.1, 0.2, 0.3]}, "sun_height_attenuation": 0.5,
          "ambient": {"color": [0.2, 0.3, 0.4], "intensity": 0.5}, "ambient_height_attenuation": 2,
          "start_m": 1, "sun_start_m": 4, "end_m": 5000, "volume_distance_m": 90}
})";

} // namespace

// --- The pyramid ------------------------------------------------------------------------------

TEST_CASE("terrain: levels run down to the source's spacing, each with the grid of nodes touching the map") {
    using Grid = std::array<uint32_t, 2>; // x, z
    struct Case {
        uint32_t width;
        uint32_t depth;
        double spacing_m;
        std::vector<Grid> grids; // coarsest first
    };
    const Case cases[] = {
        // Not square, either way.
        {.width = 1100, .depth = 700, .spacing_m = 2.0, .grids = {{2, 1}, {3, 2}, {5, 3}, {9, 6}}},
        {.width = 700, .depth = 1100, .spacing_m = 2.0, .grids = {{1, 2}, {2, 3}, {3, 5}, {6, 9}}},
        // Ending exactly on a node's edge: that edge is the last node's, with no node past it...
        {.width = 129, .depth = 257, .spacing_m = 2.0, .grids = {{1, 1}, {1, 1}, {1, 1}, {1, 2}}},
        {.width = 129, .depth = 2, .spacing_m = 16.0, .grids = {{1, 1}}},
        // ...and one sample more needs one.
        {.width = 130, .depth = 258, .spacing_m = 2.0, .grids = {{1, 1}, {1, 1}, {1, 2}, {2, 3}}},
        {.width = 130, .depth = 130, .spacing_m = 16.0, .grids = {{2, 2}}},
        // Every spacing there is room for, and the smallest map.
        {.width = 33,
         .depth = 17,
         .spacing_m = 0.125,
         .grids = {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}}},
        {.width = 2, .depth = 2, .spacing_m = 1.0, .grids = {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}}},
    };
    for (const Case& test : cases) {
        INFO(test.width, " x ", test.depth, " at ", test.spacing_m, " m");
        Map map = bumpy_map(test.width, test.depth, test.spacing_m);
        map.origin_m[0] = 1.5;
        map.origin_m[1] = -7.25;
        const CookedMap cooked(map);
        const TerrainAsset& terrain = cooked.terrain();
        CHECK(terrain.origin_m[0] == 1.5);
        CHECK(terrain.origin_m[1] == -7.25);
        CHECK(static_cast<double>(terrain.width_m) == (test.width - 1) * test.spacing_m);
        CHECK(static_cast<double>(terrain.depth_m) == (test.depth - 1) * test.spacing_m);
        REQUIRE(cooked.levels().size() == test.grids.size());
        uint32_t next_node = 0;
        float spacing = 16.0f;
        for (size_t l = 0; l < test.grids.size(); ++l) {
            const TerrainLevel& level = cooked.levels()[l];
            CHECK(level.spacing_m == spacing);
            CHECK(level.nodes_x == test.grids[l][0]);
            CHECK(level.nodes_z == test.grids[l][1]);
            CHECK(level.first_node == next_node);
            CHECK(level.first_patch == next_node * 16);
            next_node += level.nodes_x * level.nodes_z;
            spacing /= 2.0f;
        }
        CHECK(static_cast<double>(cooked.levels().back().spacing_m) == test.spacing_m);
        CHECK(terrain.block_count == next_node);
        CHECK(cooked.nodes().size() == next_node);
        CHECK(cooked.patches().size() == size_t{next_node} * 16);
        for (uint32_t n = 0; n < next_node; ++n) {
            CHECK(cooked.nodes()[n].block == n);
        }
        CHECK(cooked.heights_file().size() == terrain_block_offset(next_node));
    }
}

TEST_CASE("terrain: every sample is the source's at that point, in whole steps, with its block's lowest as the base") {
    const Map map = bumpy_map(1100, 700);
    const CookedMap cooked(map);
    size_t samples = 0;
    size_t wrong = 0;
    size_t wrong_bases = 0;
    size_t wrong_padding = 0;
    size_t off_the_grid = 0; // samples whose exact height isn't a whole number of steps: the rounding is at work
    for (const TerrainLevel& level : cooked.levels()) {
        const auto stride = static_cast<int64_t>(std::llround(static_cast<double>(level.spacing_m) / map.spacing_m));
        const double step = step_m(level);
        for (int64_t node_z = 0; node_z < int64_t{level.nodes_z}; ++node_z) {
            for (int64_t node_x = 0; node_x < int64_t{level.nodes_x}; ++node_x) {
                const TerrainNode& node = cooked.node_at(level, node_x, node_z);
                int64_t lowest = INT64_MAX;
                for (int64_t row = 0; row < kSamples; ++row) {
                    for (int64_t column = 0; column < kSamples; ++column) {
                        const int64_t x = ((node_x * kQuads) + column - kBorder) * stride;
                        const int64_t z = ((node_z * kQuads) + row - kBorder) * stride;
                        const double height = map.height_m(map.nearest(x, z));
                        const int64_t want = std::llround(height / step);
                        wrong += cooked.steps_of(node, row, column) != want ? 1 : 0;
                        off_the_grid += static_cast<double>(want) * step != height ? 1 : 0;
                        lowest = std::min(lowest, want);
                        ++samples;
                    }
                }
                wrong_bases += node.base_steps != lowest ? 1 : 0;
                const auto end = static_cast<size_t>(terrain_block_offset(node.block + 1));
                const size_t padding = kTerrainNodeBytes - (size_t{kTerrainNodeSamples} * kTerrainNodeSamples * 2);
                for (size_t b = end - padding; b < end; ++b) {
                    wrong_padding += cooked.heights_file()[b] != std::byte{0} ? 1 : 0;
                }
            }
        }
    }
    CHECK(samples == cooked.nodes().size() * kTerrainNodeSamples * kTerrainNodeSamples);
    CHECK(wrong == 0);
    CHECK(wrong_bases == 0);
    CHECK(wrong_padding == 0);
    CHECK(off_the_grid > samples / 2);
}

TEST_CASE("terrain: every pair of neighbouring nodes on a level shares edges and borders to the bit") {
    const CookedMap cooked(bumpy_map(1100, 700));
    size_t pairs = 0;
    size_t edges_wrong = 0;
    size_t overlaps_wrong = 0;
    for (const TerrainLevel& level : cooked.levels()) {
        // Each pair once: east, south, and both diagonals, which share a corner's samples and border.
        static constexpr int64_t kNeighbours[][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
        for (int64_t node_z = 0; node_z < int64_t{level.nodes_z}; ++node_z) {
            for (int64_t node_x = 0; node_x < int64_t{level.nodes_x}; ++node_x) {
                const TerrainNode& a = cooked.node_at(level, node_x, node_z);
                for (const auto& offset : kNeighbours) {
                    const int64_t other_x = node_x + offset[0];
                    const int64_t other_z = node_z + offset[1];
                    if (other_x < 0 || other_z < 0 || other_x >= int64_t{level.nodes_x} ||
                        other_z >= int64_t{level.nodes_z}) {
                        continue;
                    }
                    const TerrainNode& b = cooked.node_at(level, other_x, other_z);
                    ++pairs;
                    size_t overlap = 0;
                    // A's row and column, as the other node numbers them: the same point of the land.
                    for (int64_t row = 0; row < kSamples; ++row) {
                        const int64_t other_row = row - (offset[1] * kQuads);
                        for (int64_t column = 0; column < kSamples; ++column) {
                            const int64_t other_column = column - (offset[0] * kQuads);
                            if (other_row < 0 || other_row >= kSamples || other_column < 0 ||
                                other_column >= kSamples) {
                                continue;
                            }
                            ++overlap;
                            // Whole steps of one size, so the same number is the same height to the bit.
                            edges_wrong +=
                                cooked.steps_of(a, row, column) != cooked.steps_of(b, other_row, other_column) ? 1 : 0;
                        }
                    }
                    // A's last 5 samples are B's first 5: the shared edge and a border each way.
                    constexpr size_t kBand = (2 * kTerrainNodeBorder) + 1;
                    const size_t want = offset[0] != 0 && offset[1] != 0 ? kBand * kBand : kBand * kTerrainNodeSamples;
                    overlaps_wrong += overlap != want ? 1 : 0;
                }
            }
        }
    }
    // Grids of 2 x 1, 3 x 2, 5 x 3 and 9 x 6: in each, x-1 by z pairs east, x by z-1 south, and 2 (x-1)(z-1) diagonal.
    CHECK(pairs == size_t{1 + (2 * 2 + 3 + 2 * 2) + (4 * 3 + 5 * 2 + 2 * 4 * 2) + (8 * 6 + 9 * 5 + 2 * 8 * 5)});
    CHECK(edges_wrong == 0);
    CHECK(overlaps_wrong == 0);
}

TEST_CASE("terrain: a coarse sample equals the fine sample at that point, within the two levels' rounding") {
    const CookedMap cooked(bumpy_map(1100, 700));
    REQUIRE(cooked.levels().size() == 4);
    for (size_t l = 1; l < cooked.levels().size(); ++l) {
        const TerrainLevel& coarse = cooked.levels()[l - 1];
        const TerrainLevel& fine = cooked.levels()[l];
        const double within = (step_m(coarse) + step_m(fine)) / 2.0;
        size_t compared = 0;
        size_t apart = 0;
        double worst = 0.0;
        for (int64_t z = 0; z <= int64_t{coarse.nodes_z} * kQuads; ++z) {
            for (int64_t x = 0; x <= int64_t{coarse.nodes_x} * kQuads; ++x) {
                if (x * 2 > int64_t{fine.nodes_x} * kQuads || z * 2 > int64_t{fine.nodes_z} * kQuads) {
                    continue; // past the finer level's grid: the coarse one reaches further
                }
                const double coarse_m = static_cast<double>(cooked.level_steps(coarse, x, z)) * step_m(coarse);
                const double fine_m = static_cast<double>(cooked.level_steps(fine, x * 2, z * 2)) * step_m(fine);
                worst = std::max(worst, std::abs(coarse_m - fine_m));
                apart += coarse_m != fine_m ? 1 : 0;
                ++compared;
            }
        }
        INFO("level ", l);
        CHECK(compared > 1000);
        CHECK(apart > 0);
        CHECK(worst <= within);
    }
}

TEST_CASE("terrain: past the map's edge a sample is the nearest source sample, on or off the level's grid") {
    // A ramp each way: any sample from the wrong place reads as another height.
    Map map{.width = 300, .depth = 200, .lowest_m = 0.0, .highest_m = 655.35};
    map.pixels.resize(size_t{map.width} * map.depth);
    for (uint32_t z = 0; z < map.depth; ++z) {
        for (uint32_t x = 0; x < map.width; ++x) {
            map.at(x, z) = static_cast<uint16_t>(100 * x + 37 * z); // a centimeter a unit
        }
    }
    const CookedMap cooked(map);
    const TerrainLevel& root = cooked.levels()[0];
    const TerrainNode& node = cooked.node_at(root, 0, 0);
    constexpr double kSpacing = 16.0;
    const auto steps = [&](uint32_t x, uint32_t z) { return steps_for(map.height_m(map.at(x, z)), kSpacing); };
    const auto at = [&](int64_t x, int64_t z) { return cooked.steps_of(node, z + kBorder, x + kBorder); };
    // Inside: every 8th source sample.
    CHECK(at(3, 2) == steps(24, 16));
    // East: index 38 of this level is source sample 304, past the last, 299, which isn't on this level's grid. The last
    // on its grid, 296, would be another height.
    REQUIRE(steps(299, 16) != steps(296, 16));
    CHECK(at(37, 2) == steps(296, 16));
    CHECK(at(38, 2) == steps(299, 16));
    CHECK(at(100, 2) == steps(299, 16));
    // South likewise: 200 samples end at 199.
    REQUIRE(steps(24, 199) != steps(24, 192));
    CHECK(at(3, 24) == steps(24, 192));
    CHECK(at(3, 25) == steps(24, 199));
    // West and north, in the border: the first.
    CHECK(at(-1, 2) == steps(0, 16));
    CHECK(at(-2, 2) == steps(0, 16));
    CHECK(at(3, -1) == steps(24, 0));
    CHECK(at(3, -2) == steps(24, 0));
    // The corners.
    CHECK(at(-2, -2) == steps(0, 0));
    CHECK(at(130, 130) == steps(299, 199));
}

TEST_CASE("terrain: a patch's bounds are exactly its samples and the coarser level's, its border's aren't in them") {
    const CookedMap cooked(bumpy_map(1100, 700));
    size_t patches = 0;
    size_t wrong = 0;
    size_t widened_by_coarser = 0;
    for (size_t l = 0; l < cooked.levels().size(); ++l) {
        const TerrainLevel& level = cooked.levels()[l];
        for (int64_t patch_z = 0; patch_z < int64_t{level.nodes_z} * kTerrainPatchesPerNode; ++patch_z) {
            for (int64_t patch_x = 0; patch_x < int64_t{level.nodes_x} * kTerrainPatchesPerNode; ++patch_x) {
                double own_lowest = std::numeric_limits<double>::infinity();
                double own_highest = -std::numeric_limits<double>::infinity();
                double lowest = own_lowest;
                double highest = own_highest;
                for (int64_t z = patch_z * kTerrainPatchQuads; z <= (patch_z + 1) * kTerrainPatchQuads; ++z) {
                    for (int64_t x = patch_x * kTerrainPatchQuads; x <= (patch_x + 1) * kTerrainPatchQuads; ++x) {
                        const double height = static_cast<double>(cooked.level_steps(level, x, z)) * step_m(level);
                        own_lowest = std::min(own_lowest, height);
                        own_highest = std::max(own_highest, height);
                        if (l > 0 && x % 2 == 0 && z % 2 == 0) {
                            const TerrainLevel& coarser = cooked.levels()[l - 1];
                            const double morph_to =
                                static_cast<double>(cooked.level_steps(coarser, x / 2, z / 2)) * step_m(coarser);
                            lowest = std::min(lowest, morph_to);
                            highest = std::max(highest, morph_to);
                        }
                    }
                }
                lowest = std::min(lowest, own_lowest);
                highest = std::max(highest, own_highest);
                const TerrainPatch& patch = cooked.patch_at(level, patch_x, patch_z);
                wrong += patch.min_m != float_below(lowest) || patch.max_m != float_above(highest) ? 1 : 0;
                widened_by_coarser += lowest < own_lowest || highest > own_highest ? 1 : 0;
                ++patches;
            }
        }
    }
    CHECK(patches == cooked.patches().size());
    CHECK(wrong == 0);
    CHECK(widened_by_coarser > 0);
}

TEST_CASE("terrain: a flat patch's bounds hold what morphing can reach, by the coarser level's rounding either way") {
    // 10.1 m everywhere: each level rounds it its own way, and the coarser level's rounding lies outside the finer's.
    Map map = flat_map(600, 600, 10100);
    map.lowest_m = 0.0;
    map.highest_m = 65.535;
    constexpr double kHeight = 10.1;
    REQUIRE(rounded_m(kHeight, 4.0) > rounded_m(kHeight, 2.0));  // above: only max_m shows it
    REQUIRE(rounded_m(kHeight, 16.0) < rounded_m(kHeight, 8.0)); // below: only min_m shows it
    const CookedMap cooked(map);
    const auto patch_of = [&](size_t l) { return cooked.patch_at(cooked.levels()[l], 0, 0); };
    CHECK(static_cast<double>(patch_of(3).min_m) == rounded_m(kHeight, 2.0));
    CHECK(static_cast<double>(patch_of(3).max_m) == rounded_m(kHeight, 4.0));
    CHECK(static_cast<double>(patch_of(1).min_m) == rounded_m(kHeight, 16.0));
    CHECK(static_cast<double>(patch_of(1).max_m) == rounded_m(kHeight, 8.0));
    // The root morphs to nothing.
    CHECK(static_cast<double>(patch_of(0).min_m) == rounded_m(kHeight, 16.0));
    CHECK(static_cast<double>(patch_of(0).max_m) == rounded_m(kHeight, 16.0));
}

TEST_CASE("terrain: a spike in a block's border sets its base, not the bounds of the patches beside it") {
    Map map = flat_map(600, 400, 30000);
    // Down in node [2, 0]'s own column 1, in the border of node [1, 0]; and up inside a patch, off the coarser grid.
    map.at(257, 10) = 20000;
    map.at(33, 300) = 40000;
    const CookedMap cooked(map);
    const TerrainLevel& fine = cooked.levels().back();
    REQUIRE(static_cast<double>(fine.spacing_m) == 2.0);
    const int64_t low = steps_for(map.height_m(20000), 2.0);
    const double low_m = rounded_m(map.height_m(20000), 2.0);
    const double high_m = rounded_m(map.height_m(40000), 2.0);
    // Flat land still reaches what it morphs to: the coarser level's rounding of it.
    const double flat_fine_m = rounded_m(map.height_m(30000), 2.0);
    const double flat_coarse_m = rounded_m(map.height_m(30000), 4.0);
    REQUIRE(flat_fine_m != flat_coarse_m);
    const double flat_min_m = std::min(flat_fine_m, flat_coarse_m);
    const double flat_max_m = std::max(flat_fine_m, flat_coarse_m);
    const auto is_flat = [&](int64_t patch_x, int64_t patch_z) {
        const TerrainPatch& patch = cooked.patch_at(fine, patch_x, patch_z);
        return static_cast<double>(patch.min_m) == flat_min_m && static_cast<double>(patch.max_m) == flat_max_m;
    };
    // The base counts the border: the spike is in both nodes' blocks, and not in the block of the node before them.
    CHECK(cooked.node_at(fine, 2, 0).base_steps == low);
    CHECK(cooked.node_at(fine, 1, 0).base_steps == low);
    CHECK(cooked.steps_of(cooked.node_at(fine, 1, 0), kBorder + 10, kBorder + 257 - kQuads) == low);
    CHECK(cooked.node_at(fine, 0, 0).base_steps != low);
    // ...but it's only in the patch that has it: patches are 32 samples wide, so x 257 is patch 8's, not patch 7's.
    CHECK(static_cast<double>(cooked.patch_at(fine, 8, 0).min_m) == low_m);
    CHECK(is_flat(7, 0));
    CHECK(is_flat(9, 0));
    // The same inside a node: x 33, z 300 is patch [1, 9]'s, one sample past patch [0, 9]'s last.
    CHECK(static_cast<double>(cooked.patch_at(fine, 1, 9).max_m) == high_m);
    CHECK(is_flat(0, 9));
    CHECK(is_flat(2, 9));
}

TEST_CASE("terrain: bounds round outward where a float can't hold a height") {
    // High up and finely spaced, a step is smaller than a float's precision there.
    Map map = bumpy_map(600, 600, 0.125);
    map.lowest_m = 40000.0;
    map.highest_m = 40037.5;
    const CookedMap cooked(map);
    const TerrainLevel& fine = cooked.levels().back();
    REQUIRE(fine.spacing_m == 0.125f);
    size_t below_needed = 0;
    size_t above_needed = 0;
    size_t wrong = 0;
    for (int64_t patch_z = 0; patch_z < int64_t{fine.nodes_z} * kTerrainPatchesPerNode; ++patch_z) {
        for (int64_t patch_x = 0; patch_x < int64_t{fine.nodes_x} * kTerrainPatchesPerNode; ++patch_x) {
            double lowest = std::numeric_limits<double>::infinity();
            double highest = -lowest;
            for (int64_t z = patch_z * kTerrainPatchQuads; z <= (patch_z + 1) * kTerrainPatchQuads; ++z) {
                for (int64_t x = patch_x * kTerrainPatchQuads; x <= (patch_x + 1) * kTerrainPatchQuads; ++x) {
                    const double height = static_cast<double>(cooked.level_steps(fine, x, z)) * step_m(fine);
                    lowest = std::min(lowest, height);
                    highest = std::max(highest, height);
                    if (x % 2 == 0 && z % 2 == 0) {
                        const TerrainLevel& coarser = cooked.levels()[cooked.levels().size() - 2];
                        const double coarse =
                            static_cast<double>(cooked.level_steps(coarser, x / 2, z / 2)) * step_m(coarser);
                        lowest = std::min(lowest, coarse);
                        highest = std::max(highest, coarse);
                    }
                }
            }
            const TerrainPatch& patch = cooked.patch_at(fine, patch_x, patch_z);
            wrong += static_cast<double>(patch.min_m) > lowest || static_cast<double>(patch.max_m) < highest ? 1 : 0;
            wrong += patch.min_m != float_below(lowest) || patch.max_m != float_above(highest) ? 1 : 0;
            // Where the nearest float would have cut the land off.
            below_needed += static_cast<double>(static_cast<float>(lowest)) > lowest ? 1 : 0;
            above_needed += static_cast<double>(static_cast<float>(highest)) < highest ? 1 : 0;
        }
    }
    CHECK(wrong == 0);
    CHECK(below_needed > 0);
    CHECK(above_needed > 0);
}

TEST_CASE("terrain: the same source cooks to the same bytes, on jobs or not") {
    const Map map = bumpy_map(600, 400);
    const CookedMap alone(map);
    de::core::JobSystem jobs({.worker_count = 3});
    CookedMap on_jobs(map, &jobs);
    CookedMap again(map);
    CHECK(alone.heights_file() == on_jobs.heights_file());
    CHECK(on_jobs.terrain_file().size() == again.terrain_file().size());
    CHECK(std::memcmp(on_jobs.terrain_file().data(), again.terrain_file().data(), again.terrain_file().size()) == 0);
    // And the heights say what they are.
    CHECK(verify_content_hash(alone.heights_file()));
    CHECK(alone.terrain().heights_hash == content_hash(alone.heights_file()));
    AssetHeader header;
    std::memcpy(&header, alone.heights_file().data(), sizeof(header));
    CHECK(header.magic == kAssetMagic);
    CHECK(header.type == AssetType::TerrainHeights);
    CHECK(header.format_version == TerrainAsset::kFormatVersion);
    CHECK(header.platform == Platform::Pc);
    CHECK(header.id == kId);
    CHECK(header.file_size == alone.heights_file().size());
    CHECK(header.dependencies.count == 0);
}

// --- Failing clearly --------------------------------------------------------------------------

TEST_CASE("terrain: a block that doesn't fit 16 bits fails, naming where and by how much") {
    const auto cook_cliff = [](double highest_m, uint32_t cliff_x) {
        // Half of it low, half of it the top of the range, at 2 m: a step is 1/32 m, so a block holds 65535 / 32 m.
        Map map = flat_map(600, 300, 0);
        map.lowest_m = 0.0;
        map.highest_m = highest_m;
        map.origin_m[0] = 10.0;
        map.origin_m[1] = 20.0;
        for (uint32_t z = 0; z < map.depth; ++z) {
            for (uint32_t x = cliff_x; x < map.width; ++x) {
                map.at(x, z) = 65535;
            }
        }
        struct Outcome {
            bool ok;
            std::string message;
        };
        de::cooker::Diagnostics diagnostics(false);
        const auto cooked = de::cooker::cook_terrain(source_of(map), image_of(map), kId, nullptr, "cliff", diagnostics);
        const auto findings = diagnostics.all();
        return Outcome{.ok = cooked.ok(), .message = findings.empty() ? "" : std::string(findings[0].message.view())};
    };
    constexpr double kHolds = 65535.0 / 32.0;
    REQUIRE(static_cast<double>(kTerrainStepsPerSpacing) == 64.0);

    // To the step: exactly what a block holds fits, and one step more doesn't.
    const auto fits = cook_cliff(kHolds, 300);
    CHECK(fits.ok);
    CHECK(fits.message.empty());
    const auto just_over = cook_cliff(kHolds + (1.0 / 32.0), 300);
    CHECK_FALSE(just_over.ok);
    CHECK(just_over.message.find("0.03 m too far") != std::string::npos);

    const auto far_over = cook_cliff(4000.0, 300);
    CHECK_FALSE(far_over.ok);
    INFO(far_over.message);
    CHECK(far_over.message.find("too steep at 2 m spacing") != std::string::npos);
    CHECK(far_over.message.find("3 node(s)") != std::string::npos); // a column 3 nodes deep: x 300 is only in node 2
    CHECK(far_over.message.find("2047.97 m a node holds") != std::string::npos);
    CHECK(far_over.message.find("node [2, 0]") != std::string::npos);
    CHECK(far_over.message.find("X 522.00 m, Z 20.00 m") != std::string::npos); // the origin plus two nodes
    CHECK(far_over.message.find("rises 4000.00 m: 1952.03 m too far") != std::string::npos);

    // The border counts: a cliff just past node 1's edge, in its border only, is in its block.
    const auto in_border = cook_cliff(4000.0, 257);
    CHECK_FALSE(in_border.ok);
    CHECK(in_border.message.find("6 node(s)") != std::string::npos);
    CHECK(in_border.message.find("node [1, 0]") != std::string::npos);

    // Each level that fails says so; coarser ones hold more.
    Map steep = flat_map(5000, 300, 0);
    steep.lowest_m = 0.0;
    steep.highest_m = 5000.0; // over 2 m's and 4 m's limits, inside 8 m's
    for (uint32_t z = 0; z < steep.depth; ++z) {
        for (uint32_t x = 2500; x < steep.width; ++x) {
            steep.at(x, z) = 65535;
        }
    }
    de::cooker::Diagnostics diagnostics(false);
    CHECK_FALSE(de::cooker::cook_terrain(source_of(steep), image_of(steep), kId, nullptr, "steep", diagnostics).ok());
    const auto findings = diagnostics.all();
    REQUIRE(findings.size() == 2);
    CHECK(findings[0].message.view().find("too steep at 4 m spacing") != std::string_view::npos);
    CHECK(findings[1].message.view().find("too steep at 2 m spacing") != std::string_view::npos);
}

TEST_CASE("terrain: a source that can't be cooked says why, and leaves nothing behind") {
    const Sandbox sandbox;
    const Map map = bumpy_map(64, 48);
    write_src(sandbox, "content/land/good.heightpng", png_of(map));
    struct Bad {
        const char* name;
        std::string text;
        const char* where; // empty: the source itself
        const char* message;
    };
    constexpr const char* kHeight = R"("height": "good.heightpng")";
    constexpr const char* kRange = R"("lowest_m": 0, "highest_m": 100)";
    const auto with = [&](const std::string& spacing) {
        return std::string("{") + kHeight + R"(, "meters_per_pixel": )" + spacing + ", " + kRange + "}";
    };
    const std::string spacing_rule = "meters_per_pixel should be 16 divided by a power of 2";
    const std::vector<Bad> bad = {
        // Wrong spacing.
        {.name = "three", .text = with("3"), .where = "", .message = "not 3"},
        {.name = "thirtytwo", .text = with("32"), .where = "", .message = "not 32"},
        {.name = "zero", .text = with("0"), .where = "", .message = "not 0"},
        {.name = "negative", .text = with("-2"), .where = "", .message = "not -2"},
        {.name = "too_fine", .text = with("0.0625"), .where = "", .message = "not 0.0625"},
        {.name = "not_a_power", .text = with("1.5"), .where = "", .message = spacing_rule.c_str()},
        {.name = "a_word", .text = with(R"("2")"), .where = "", .message = "meters_per_pixel should be a number"},
        {.name = "no_spacing",
         .text = std::string("{") + kHeight + ", " + kRange + "}",
         .where = "",
         .message = "meters_per_pixel should be a number"},
        // Missing or senseless range.
        {.name = "no_lowest",
         .text = std::string("{") + kHeight + R"(, "meters_per_pixel": 2, "highest_m": 100})",
         .where = "",
         .message = "lowest_m should be a number"},
        {.name = "no_highest",
         .text = std::string("{") + kHeight + R"(, "meters_per_pixel": 2, "lowest_m": 0})",
         .where = "",
         .message = "highest_m should be a number"},
        {.name = "no_range",
         .text = std::string("{") + kHeight + R"(, "meters_per_pixel": 2})",
         .where = "",
         .message = "lowest_m should be a number"},
        {.name = "flat_range",
         .text = std::string("{") + kHeight + R"(, "meters_per_pixel": 2, "lowest_m": 5, "highest_m": 5})",
         .where = "",
         .message = "should be above lowest_m"},
        {.name = "upside_down",
         .text = std::string("{") + kHeight + R"(, "meters_per_pixel": 2, "lowest_m": 5, "highest_m": 1})",
         .where = "",
         .message = "should be above lowest_m"},
        {.name = "too_high",
         .text = std::string("{") + kHeight + R"(, "meters_per_pixel": 2, "lowest_m": 0, "highest_m": 2e6})",
         .where = "",
         .message = "within 1e+06 m"},
        {.name = "too_low",
         .text = std::string("{") + kHeight + R"(, "meters_per_pixel": 2, "lowest_m": -2e6, "highest_m": 0})",
         .where = "",
         .message = "within 1e+06 m"},
        // The image's name.
        {.name = "no_height",
         .text = std::string("{") + R"("meters_per_pixel": 2, )" + kRange + "}",
         .where = "",
         .message = "height should name"},
        {.name = "a_texture",
         .text = std::string(R"({"height": "good.png", "meters_per_pixel": 2, )") + kRange + "}",
         .where = "",
         .message = "a .png would be cooked as a texture"},
        {.name = "out_of_the_tree",
         .text = std::string(R"({"height": "../../../../x.heightpng", "meters_per_pixel": 2, )") + kRange + "}",
         .where = "",
         .message = "height should name"},
        {.name = "no_image",
         .text = std::string(R"({"height": "nowhere.heightpng", "meters_per_pixel": 2, )") + kRange + "}",
         .where = "content/land/nowhere.heightpng",
         .message = "could not be read"},
        // The rest of it.
        {.name = "short_origin",
         .text = std::string("{") + kHeight + R"(, "meters_per_pixel": 2, )" + kRange + R"(, "origin": [1]})",
         .where = "",
         .message = "origin should be [x, z]"},
        {.name = "worded_origin",
         .text = std::string("{") + kHeight + R"(, "meters_per_pixel": 2, )" + kRange + R"(, "origin": ["a", "b"]})",
         .where = "",
         .message = "origin should be [x, z]"},
        {.name = "not_json", .text = "{", .where = "", .message = ""},
        {.name = "not_an_object", .text = "[1, 2]", .where = "", .message = "should be a JSON object"},
    };
    for (const Bad& source : bad) {
        write_src(sandbox, std::string("content/land/") + source.name + ".terrain", source.text);
    }
    write_src(sandbox, "content/land/not_a_power_rule.terrain", with("1.5"));
    // A key nobody reads is a warning, as in every other source: it cooks.
    write_src(sandbox, "content/land/typo.terrain",
              std::string("{") + kHeight + R"(, "meters_per_pixel": 2, )" + kRange + R"(, "orgin": [0, 0]})");

    // The images: each behind a source that's right.
    const std::vector<uint16_t> two(4, 100);
    const auto image_source = [&](const std::string& name, std::span<const std::byte> image) {
        write_src(sandbox, "content/land/" + name + ".heightpng", image);
        write_src(sandbox, "content/land/" + name + ".terrain", terrain_text(map, name + ".heightpng"));
    };
    image_source("eight_bit", png_of(map.width, map.depth, map.pixels, 8));
    image_source("rgb", png_of(map.width, map.depth, map.pixels, 16, 3));
    image_source("gray_alpha", png_of(map.width, map.depth, map.pixels, 16, 2));
    image_source("empty", png_of(0, 0, {}));
    image_source("one_wide", png_of(1, 2, two));
    image_source("one_deep", png_of(2, 1, two));
    image_source("one", png_of(1, 1, two));
    image_source("words", de::test::bytes_of("not an image at all"));
    image_source("nothing", {});
    std::vector<std::byte> truncated = png_of(map);
    truncated.resize(truncated.size() / 2);
    image_source("truncated", truncated);

    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    CHECK(has_message(report, "content/land/typo.terrain", "unknown terrain setting 'orgin'"));
    CHECK(terrain_cooked(vfs, id_of(vfs, "typo")));
    for (const Bad& source : bad) {
        INFO(std::string(source.name));
        const std::string path = std::string("content/land/") + source.name + ".terrain";
        CHECK(has_message(report, source.where[0] != '\0' ? std::string_view(source.where) : std::string_view(path),
                          source.message));
        const AssetId id = id_of(vfs, source.name);
        CHECK_FALSE(terrain_cooked(vfs, id));
        CHECK_FALSE(heights_cooked(vfs, id));
    }
    CHECK(has_message(report, "content/land/not_a_power_rule.terrain", spacing_rule));
    const auto refuses = [&](const char* name, const char* message) {
        INFO(std::string(name));
        CHECK(has_message(report, std::string("content/land/") + name + ".heightpng", message));
        const AssetId id = id_of(vfs, name);
        CHECK_FALSE(terrain_cooked(vfs, id));
        CHECK_FALSE(heights_cooked(vfs, id));
    };
    refuses("eight_bit", "has 8 bits a sample");
    refuses("rgb", "has 3 channels");
    refuses("gray_alpha", "has 2 channels");
    refuses("empty", "isn't a readable PNG");
    refuses("one_wide", "is 1 x 2 samples");
    refuses("one_deep", "is 2 x 1 samples");
    refuses("one", "is 1 x 1 samples");
    refuses("words", "isn't a PNG");
    refuses("nothing", "isn't a PNG");
    refuses("truncated", "could not be decoded");
    CHECK(report.stats.failed == bad.size() + 11);
    CHECK(report.stats.cooked == 1); // the typo
    CHECK_FALSE(report.ok());
}

TEST_CASE("terrain: every spacing the format has room for is read, with its origin, and an origin isn't required") {
    for (const double spacing_m : {16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125}) {
        Map map = flat_map(4, 4, 0);
        map.spacing_m = spacing_m;
        map.origin_m[0] = -3.5;
        map.origin_m[1] = 12.0;
        const std::string text = terrain_text(map, "a.heightpng");
        de::cooker::Diagnostics diagnostics(false);
        const auto source = de::cooker::import_terrain("content/x/a.terrain", de::test::bytes_of(text), diagnostics);
        REQUIRE(source.ok());
        CHECK(source->meters_per_pixel == spacing_m);
        CHECK((source->height == std::string_view("content/x/a.heightpng")));
        CHECK(source->lowest_m == map.lowest_m);
        CHECK(source->highest_m == map.highest_m);
        CHECK(source->origin_m[0] == -3.5);
        CHECK(source->origin_m[1] == 12.0);
    }
    de::cooker::Diagnostics diagnostics(false);
    const auto no_origin = de::cooker::import_terrain(
        "content/x/a.terrain",
        de::test::bytes_of(R"({"height": "../y/a.heightpng", "meters_per_pixel": 2, "lowest_m": 0, "highest_m": 1})"),
        diagnostics);
    REQUIRE(no_origin.ok());
    CHECK(no_origin->origin_m[0] == 0.0);
    CHECK(no_origin->origin_m[1] == 0.0);
    CHECK((no_origin->height == std::string_view("content/y/a.heightpng")));
}

// --- The cook ---------------------------------------------------------------------------------

TEST_CASE("terrain: a source cooks to two files of one ID, read back from a PNG as they were written") {
    const Sandbox sandbox;
    Map map = bumpy_map(300, 200);
    map.origin_m[0] = 1.0;
    map.origin_m[1] = 1.0;
    write_map(sandbox, "land", map);
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    CHECK(report.ok());
    CHECK(report.diagnostics.empty()); // no warning for the image, as for anything the tree doesn't know
    CHECK(report.stats.cooked == 1);
    CHECK_FALSE(vfs.exists("src:/meta/land/land.heightpng.json")); // read by its terrain: no meta of its own

    const AssetId id = id_of(vfs, "land");
    Cooked terrain_file(vfs, id, AssetType::Terrain);
    const auto& terrain = terrain_file.load<TerrainAsset>();
    CHECK(terrain.header.id == id);
    CHECK(terrain.origin_m[0] == 1.0);
    CHECK(terrain.width_m == 598.0f);
    CHECK(terrain.depth_m == 398.0f);
    // No heights in it: its tables and little else.
    CHECK(terrain_file.bytes().size() < sizeof(TerrainAsset) + (4 * sizeof(TerrainLevel)) +
                                            (terrain.nodes.count * sizeof(TerrainNode)) +
                                            (terrain.patches.count * sizeof(TerrainPatch)) + 64);

    Cooked heights_file(vfs, id, AssetType::TerrainHeights);
    CHECK(verify_content_hash(heights_file.bytes()));
    AssetHeader heights;
    std::memcpy(&heights, heights_file.bytes().data(), sizeof(heights));
    CHECK(terrain.check_heights_header(heights, heights_file.bytes().size()).ok());

    // The same bytes as cooking the map's pixels directly: the PNG's big-endian rows came through whole.
    de::cooker::Diagnostics direct_diagnostics(false);
    const auto expected =
        de::cooker::cook_terrain(source_of(map), image_of(map), id, nullptr, "direct", direct_diagnostics);
    REQUIRE(expected.ok());
    REQUIRE(expected->heights.size() == heights_file.bytes().size());
    CHECK(std::memcmp(expected->heights.data(), heights_file.bytes().data(), expected->heights.size()) == 0);

    de::cooker::Diagnostics diagnostics(false);
    const de::cooker::CookState state = de::cooker::CookState::load(vfs, diagnostics);
    CHECK(diagnostics.warning_count() == 0);
    const de::cooker::CookRecord* record = state.find("content/land/land.terrain");
    REQUIRE(record != nullptr);
    REQUIRE(record->outputs.size() == 2);
    CHECK(record->outputs[0].id == id);
    CHECK(record->outputs[1].id == id);
    CHECK(record->outputs[0].type != record->outputs[1].type);
    REQUIRE(record->dependencies.size() == 1);
    CHECK((record->dependencies[0] == std::string_view("content/land/land.heightpng")));
}

TEST_CASE("terrain: decoding a height image gives its samples, rows from the north") {
    const Map map = bumpy_map(70, 40);
    de::cooker::Diagnostics diagnostics(false);
    const std::vector<std::byte> png = png_of(map);
    const auto image = de::cooker::decode_height_image(png, "a.heightpng", diagnostics);
    REQUIRE(image.ok());
    CHECK(image->width == 70);
    CHECK(image->depth == 40);
    REQUIRE(image->samples.size() == map.pixels.size());
    CHECK(std::memcmp(image->samples.data(), map.pixels.data(), map.pixels.size() * 2) == 0);
}

TEST_CASE("terrain: a second cook skips it, and it cooks again when its text, its image or a file of it changes") {
    const Sandbox sandbox;
    Map map = bumpy_map(300, 200);
    write_map(sandbox, "land", map);
    Vfs vfs = cooker_vfs(sandbox);
    CHECK(run_cook(vfs).stats.cooked == 1);
    const AssetId id = id_of(vfs, "land");
    const std::vector<std::byte> first_heights = read_all(vfs, cooked_path(id, AssetType::TerrainHeights).c_str());

    CookReport report = run_cook(vfs);
    CHECK(report.stats.cooked == 0);
    CHECK(report.stats.up_to_date == 1);
    CHECK(report.diagnostics.empty()); // the cook state read both types back

    // Its image: the heights change, and the terrain says so.
    map.at(100, 100) = static_cast<uint16_t>(map.at(100, 100) + 5000);
    write_src(sandbox, "content/land/land.heightpng", png_of(map));
    report = run_cook(vfs);
    CHECK(report.stats.cooked == 1);
    const std::vector<std::byte> second_heights = read_all(vfs, cooked_path(id, AssetType::TerrainHeights).c_str());
    CHECK(second_heights != first_heights);
    {
        Cooked terrain_file(vfs, id, AssetType::Terrain);
        CHECK(terrain_file.load<TerrainAsset>().heights_hash == content_hash(second_heights));
    }
    CHECK(run_cook(vfs).stats.cooked == 0);

    // Its text.
    map.highest_m += 10.0;
    write_src(sandbox, "content/land/land.terrain", terrain_text(map, "land.heightpng"));
    CHECK(run_cook(vfs).stats.cooked == 1);
    const std::vector<std::byte> third_heights = read_all(vfs, cooked_path(id, AssetType::TerrainHeights).c_str());
    CHECK(third_heights != second_heights);
    CHECK(run_cook(vfs).stats.cooked == 0);

    // Its text naming another image, even of the same bytes.
    write_src(sandbox, "content/land/other.heightpng", png_of(map));
    write_src(sandbox, "content/land/land.terrain", terrain_text(map, "other.heightpng"));
    CHECK(run_cook(vfs).stats.cooked == 1);
    CHECK(run_cook(vfs).stats.cooked == 0);

    // Either file gone.
    REQUIRE(vfs.remove_file(cooked_path(id, AssetType::TerrainHeights).c_str()).ok());
    CHECK(run_cook(vfs).stats.cooked == 1);
    CHECK(heights_cooked(vfs, id));
    REQUIRE(vfs.remove_file(cooked_path(id, AssetType::Terrain).c_str()).ok());
    CHECK(run_cook(vfs).stats.cooked == 1);
    CHECK(terrain_cooked(vfs, id));
    CHECK(run_cook(vfs).stats.cooked == 0);

    // Forced.
    CHECK(run_cook(vfs, {.force = true}).stats.cooked == 1);
}

TEST_CASE("terrain: a source that stops cooking keeps its files, and cooks again once it can") {
    const Sandbox sandbox;
    const Map map = bumpy_map(300, 200);
    write_map(sandbox, "land", map);
    Vfs vfs = cooker_vfs(sandbox);
    REQUIRE(run_cook(vfs).ok());
    const AssetId id = id_of(vfs, "land");

    // Its image gone: reported, nothing removed, and tried again each time.
    remove_src(sandbox, "content/land/land.heightpng");
    CookReport report = run_cook(vfs);
    CHECK(report.stats.failed == 1);
    CHECK(report.stats.removed == 0);
    CHECK(has_message(report, "content/land/land.heightpng", "could not be read"));
    CHECK(terrain_cooked(vfs, id));
    CHECK(heights_cooked(vfs, id));
    report = run_cook(vfs);
    CHECK(report.stats.failed == 1);
    CHECK(terrain_cooked(vfs, id));
    CHECK(heights_cooked(vfs, id));

    // Its text broken.
    write_src(sandbox, "content/land/land.heightpng", png_of(map));
    write_src(sandbox, "content/land/land.terrain", "{");
    report = run_cook(vfs);
    CHECK(report.stats.failed == 1);
    CHECK(terrain_cooked(vfs, id));
    CHECK(heights_cooked(vfs, id));

    // Fixed: a failure never matches, so it cooks, and both files are the record's still.
    write_src(sandbox, "content/land/land.terrain", terrain_text(map, "land.heightpng"));
    report = run_cook(vfs);
    CHECK(report.ok());
    CHECK(report.stats.cooked == 1);
    CHECK(run_cook(vfs).stats.cooked == 0);
}

TEST_CASE("terrain: both files go with their source, and with its ID") {
    const Sandbox sandbox;
    write_map(sandbox, "land", bumpy_map(300, 200));
    write_map(sandbox, "kept", bumpy_map(200, 300));
    Vfs vfs = cooker_vfs(sandbox);
    REQUIRE(run_cook(vfs).ok());
    const AssetId id = id_of(vfs, "land");
    const AssetId kept = id_of(vfs, "kept");
    REQUIRE(terrain_cooked(vfs, id));
    REQUIRE(heights_cooked(vfs, id));

    // A new GUID: a new ID, and the old one's two files removed.
    remove_src(sandbox, "meta/land/land.terrain.json");
    CookReport report = run_cook(vfs);
    CHECK(report.ok());
    CHECK(report.stats.removed == 2);
    CHECK_FALSE(terrain_cooked(vfs, id));
    CHECK_FALSE(heights_cooked(vfs, id));
    const AssetId new_id = id_of(vfs, "land");
    CHECK(new_id != id);
    CHECK(terrain_cooked(vfs, new_id));
    CHECK(heights_cooked(vfs, new_id));

    // The source gone: both files, and nothing of the other terrain.
    remove_src(sandbox, "content/land/land.terrain");
    remove_src(sandbox, "content/land/land.heightpng");
    remove_src(sandbox, "meta/land/land.terrain.json");
    report = run_cook(vfs);
    CHECK(report.ok());
    CHECK(report.stats.removed == 2);
    CHECK_FALSE(terrain_cooked(vfs, new_id));
    CHECK_FALSE(heights_cooked(vfs, new_id));
    CHECK(terrain_cooked(vfs, kept));
    CHECK(heights_cooked(vfs, kept));
    CHECK(run_cook(vfs).stats.removed == 0);
}

TEST_CASE("terrain: --check reads every source, and what's cooked from one has to be whole and up to date") {
    const Sandbox sandbox;
    Map map = bumpy_map(300, 200);
    write_map(sandbox, "land", map);
    const char* text_path = "content/land/land.terrain";
    const char* image_path = "content/land/land.heightpng";

    // No meta yet: as for every source.
    {
        Vfs check = cooker_vfs(sandbox, false);
        const CookReport report = run_cook(check, {.check_only = true});
        CHECK(has_message(report, text_path, "has no meta"));
        CHECK_FALSE(check.exists("src:/meta/land/land.terrain.json"));
    }

    Vfs vfs = cooker_vfs(sandbox);
    REQUIRE(run_cook(vfs).ok());
    const AssetId id = id_of(vfs, "land");
    const CookedPath terrain_path = cooked_path(id, AssetType::Terrain);
    const CookedPath heights_path = cooked_path(id, AssetType::TerrainHeights);
    const auto check = [&] {
        Vfs read_only = cooker_vfs(sandbox, false);
        return run_cook(read_only, {.check_only = true});
    };
    const auto check_says = [&](const char* path, const char* message) {
        const CookReport report = check();
        CHECK_FALSE(report.ok());
        CHECK(has_message(report, path, message));
    };
    const CookReport clean = check();
    CHECK(clean.ok());
    CHECK(clean.diagnostics.empty());
    CHECK(clean.stats.cooked == 0);

    // Nothing cooked, as on CI: nothing stale, but the source still has to be readable.
    {
        const std::vector<std::byte> keep_state = read_all(vfs, "game:/cook_state.json");
        REQUIRE(vfs.remove_file("game:/cook_state.json").ok());
        CHECK(check().ok());
        REQUIRE(vfs.write_file("game:/cook_state.json", keep_state).ok());
    }

    // The source as it is now: its image, then its text. A cook would fix both.
    Map edited = map;
    edited.at(5, 5) = static_cast<uint16_t>(edited.at(5, 5) + 100);
    write_src(sandbox, image_path, png_of(edited));
    check_says(text_path, "its cooked files are stale");
    write_src(sandbox, image_path, png_of(map));
    CHECK(check().ok());
    Map moved = map;
    moved.origin_m[0] = 4.0;
    write_src(sandbox, text_path, terrain_text(moved, "land.heightpng"));
    check_says(text_path, "its cooked files are stale");
    write_src(sandbox, text_path, terrain_text(map, "land.heightpng"));
    CHECK(check().ok());

    // Sources a cook couldn't read: reported, cooked or not.
    write_src(sandbox, text_path,
              R"({"height": "land.heightpng", "meters_per_pixel": 3, "lowest_m": 0, "highest_m": 1})");
    check_says(text_path, "meters_per_pixel should be 16 divided by a power of 2");
    write_src(sandbox, text_path, terrain_text(map, "land.heightpng"));
    write_src(sandbox, image_path, png_of(map.width, map.depth, map.pixels, 8));
    check_says(image_path, "has 8 bits a sample");
    remove_src(sandbox, image_path);
    check_says(image_path, "could not be read");
    write_src(sandbox, image_path, png_of(map));
    REQUIRE(check().ok());

    // Each file: gone, cut short, or its bytes changed.
    const std::vector<std::byte> good_terrain = read_all(vfs, terrain_path.c_str());
    const std::vector<std::byte> good_heights = read_all(vfs, heights_path.c_str());
    const auto put_terrain = [&](std::span<const std::byte> bytes) {
        REQUIRE(vfs.write_file(terrain_path.c_str(), bytes).ok());
    };
    const auto put_heights = [&](std::span<const std::byte> bytes) {
        REQUIRE(vfs.write_file(heights_path.c_str(), bytes).ok());
    };
    REQUIRE(vfs.remove_file(heights_path.c_str()).ok());
    check_says(text_path, "is missing or too short");
    put_heights(std::span(good_heights).first(16));
    check_says(text_path, "is missing or too short");
    put_heights(std::span(good_heights).first(good_heights.size() - 4));
    check_says(text_path, "isn't as long as its blocks");
    std::vector<std::byte> flipped = good_heights;
    flipped[flipped.size() / 2] ^= std::byte{1};
    put_heights(flipped);
    check_says(text_path, "its bytes don't hash as its header says");
    put_heights(good_heights);
    CHECK(check().ok());

    REQUIRE(vfs.remove_file(terrain_path.c_str()).ok());
    check_says(text_path, "is missing or too short");
    put_terrain(std::span(good_terrain).first(good_terrain.size() - 16));
    check_says(text_path, "size differs from its header");
    std::vector<std::byte> broken = good_terrain;
    TerrainAsset header;
    std::memcpy(&header, broken.data(), sizeof(header));
    header.width_m = -1.0f;
    std::memcpy(broken.data(), &header, sizeof(header));
    put_terrain(broken);
    check_says(text_path, "terrain has no size");
    put_terrain(good_terrain);
    CHECK(check().ok());

    // Both whole, but from different cooks.
    Map other = map;
    other.at(9, 9) = static_cast<uint16_t>(other.at(9, 9) + 100);
    write_src(sandbox, image_path, png_of(other));
    REQUIRE(run_cook(vfs).ok());
    const std::vector<std::byte> other_heights = read_all(vfs, heights_path.c_str());
    write_src(sandbox, image_path, png_of(map));
    REQUIRE(run_cook(vfs).ok());
    put_heights(other_heights);
    check_says(text_path, "isn't the one this terrain was cooked with");
    put_heights(good_heights);
    CHECK(check().ok());

    // A check writes nothing: a stale state stays stale until a cook.
    write_src(sandbox, image_path, png_of(other));
    check_says(text_path, "its cooked files are stale");
    check_says(text_path, "its cooked files are stale");
    write_src(sandbox, image_path, png_of(map));
    CHECK(check().ok());

    // The source gone, and its cooked files still there.
    remove_src(sandbox, text_path);
    remove_src(sandbox, image_path);
    remove_src(sandbox, "meta/land/land.terrain.json");
    check_says(text_path, "is gone, and its cooked files are still there");
    REQUIRE(run_cook(vfs).ok());
    CHECK(check().ok());
}

TEST_CASE("terrain: --check tells a terrain left out for its meta from one that's gone") {
    const Sandbox sandbox;
    write_map(sandbox, "land", bumpy_map(64, 48));
    Vfs vfs = cooker_vfs(sandbox);
    REQUIRE(run_cook(vfs).ok());
    write_src(sandbox, "meta/land/land.terrain.json", "not a meta");
    Vfs read_only = cooker_vfs(sandbox, false);
    const CookReport report = run_cook(read_only, {.check_only = true});
    CHECK_FALSE(report.ok());
    CHECK(has_message(report, "meta/land/land.terrain.json", ""));
    CHECK_FALSE(has_message(report, "content/land/land.terrain", "is gone"));
}

// --- The heights file's header ----------------------------------------------------------------

TEST_CASE("terrain: a streamer's check of the heights file's header refuses any other file") {
    const CookedMap cooked(bumpy_map(300, 200));
    const auto& terrain = cooked.terrain();
    AssetHeader good;
    std::memcpy(&good, cooked.heights_file().data(), sizeof(good));
    const uint64_t size = cooked.heights_file().size();
    REQUIRE(terrain.check_heights_header(good, size).ok());

    const auto refused = [&](auto change, uint64_t file_bytes) {
        AssetHeader header = good;
        change(header);
        const auto checked = terrain.check_heights_header(header, file_bytes);
        return checked.ok() ? std::string() : std::string(checked.error().message);
    };
    const auto same = [](AssetHeader&) {};
    CHECK(refused([](AssetHeader& h) { h.magic = 0; }, size) == "not a terrain heights file");
    CHECK(refused([](AssetHeader& h) { h.type = AssetType::Terrain; }, size) == "not a terrain heights file");
    CHECK(refused([](AssetHeader& h) { h.format_version = TerrainAsset::kFormatVersion + 1; }, size) ==
          "terrain heights file has another format version or platform");
    CHECK(refused([](AssetHeader& h) { h.platform = Platform::Unknown; }, size) ==
          "terrain heights file has another format version or platform");
    CHECK(refused([](AssetHeader& h) { h.id = AssetId::from_value(99); }, size) ==
          "terrain heights file isn't the one this terrain was cooked with");
    CHECK(refused([](AssetHeader& h) { h.content_hash ^= 1U; }, size) ==
          "terrain heights file isn't the one this terrain was cooked with");
    CHECK(refused([](AssetHeader& h) { h.file_size -= 2; }, size) ==
          "terrain heights file isn't as long as its blocks (truncated?)");
    CHECK(refused(same, size - 2) == "terrain heights file isn't as long as its blocks (truncated?)");
    // Header and file agree, and are wrong: not as long as the blocks this terrain says it has.
    CHECK(refused([&](AssetHeader& h) { h.file_size = size - kTerrainNodeBytes; }, size - kTerrainNodeBytes) ==
          "terrain heights file isn't as long as its blocks (truncated?)");
}

// --- Refusing corrupt files -------------------------------------------------------------------

TEST_CASE("terrain: a corrupt .deterrain is refused, each way for its own reason") {
    const Map map = bumpy_map(600, 400);
    CookedMap cooked(map);
    const std::span<std::byte> original = cooked.terrain_file();
    struct alignas(16) Copy {
        std::byte bytes[8192 * 4];
    };
    REQUIRE(original.size() <= sizeof(Copy));
    const auto copy = std::make_unique<Copy>();

    struct File {
        std::span<std::byte> bytes;
        TerrainAsset& asset() const { return *reinterpret_cast<TerrainAsset*>(bytes.data()); }
        TerrainLevel* levels() const { return reinterpret_cast<TerrainLevel*>(bytes.data() + asset().levels.offset); }
        TerrainNode* nodes() const { return reinterpret_cast<TerrainNode*>(bytes.data() + asset().nodes.offset); }
        TerrainPatch* patches() const { return reinterpret_cast<TerrainPatch*>(bytes.data() + asset().patches.offset); }
    };
    // What's left of the file after an edit: "" if it loads, or why it doesn't.
    const auto after = [&](auto edit) {
        std::memcpy(copy->bytes, original.data(), original.size());
        const File file{.bytes = std::span(copy->bytes, original.size())};
        edit(file);
        const auto loaded = load_in_place<TerrainAsset>(file.bytes);
        return loaded.ok() ? std::string() : std::string(loaded.error().message);
    };
    const float not_a_number = std::numeric_limits<float>::quiet_NaN();
    const float infinite = std::numeric_limits<float>::infinity();
    const double not_a_number_d = std::numeric_limits<double>::quiet_NaN();
    const double infinite_d = std::numeric_limits<double>::infinity();

    // Controls: untouched it loads, and so does what's unusual but fine.
    CHECK(after([](const File&) {}).empty());
    CHECK(after([&](const File& f) { f.nodes()[0].base_steps = INT32_MAX - 65535; }).empty());
    CHECK(after([&](const File& f) { f.nodes()[0].base_steps = INT32_MIN; }).empty());
    CHECK(after([&](const File& f) { f.patches()[0].max_m = f.patches()[0].min_m; }).empty());
    CHECK(after([&](const File& f) {
              // A node with no block, its number given to the last's, one block fewer.
              const uint32_t last = f.asset().nodes.count - 1;
              f.nodes()[3].block = kTerrainNoBlock;
              f.nodes()[last].block = 3;
              f.asset().block_count -= 1;
          }).empty());
    CHECK(after([&](const File& f) {
              // Every node without one: a terrain of bounds alone.
              for (uint32_t n = 0; n < f.asset().nodes.count; ++n) {
                  f.nodes()[n].block = kTerrainNoBlock;
              }
              f.asset().block_count = 0;
          }).empty());

    const std::string outside = "terrain's tables lie outside the file";
    CHECK(after([](const File& f) { f.asset().levels.offset = static_cast<uint32_t>(f.bytes.size()); }) == outside);
    CHECK(after([](const File& f) { f.asset().nodes.offset = static_cast<uint32_t>(f.bytes.size()); }) == outside);
    CHECK(after([](const File& f) { f.asset().patches.offset = static_cast<uint32_t>(f.bytes.size()); }) == outside);
    CHECK(after([](const File& f) { f.asset().levels.count = UINT32_MAX; }) == outside);
    CHECK(after([](const File& f) { f.asset().nodes.count = UINT32_MAX; }) == outside);
    CHECK(after([](const File& f) { f.asset().patches.count = UINT32_MAX; }) == outside);
    CHECK(after([](const File& f) { f.asset().levels.offset = 8; }) == outside); // inside the header
    CHECK(after([](const File& f) { f.asset().nodes.offset += 1; }) == outside); // not aligned

    CHECK(after([&](const File& f) { f.asset().origin_m[0] = not_a_number_d; }) == "terrain's origin isn't finite");
    CHECK(after([&](const File& f) { f.asset().origin_m[1] = infinite_d; }) == "terrain's origin isn't finite");

    const std::string no_size = "terrain has no size";
    CHECK(after([](const File& f) { f.asset().width_m = 0.0f; }) == no_size);
    CHECK(after([](const File& f) { f.asset().width_m = -1.0f; }) == no_size);
    CHECK(after([&](const File& f) { f.asset().width_m = not_a_number; }) == no_size);
    CHECK(after([&](const File& f) { f.asset().width_m = infinite; }) == no_size);
    CHECK(after([](const File& f) { f.asset().depth_m = 0.0f; }) == no_size);
    CHECK(after([](const File& f) { f.asset().depth_m = -1.0f; }) == no_size);
    CHECK(after([&](const File& f) { f.asset().depth_m = not_a_number; }) == no_size);
    CHECK(after([&](const File& f) { f.asset().depth_m = infinite; }) == no_size);

    const std::string level_count = "terrain has no levels, or more than it can have";
    CHECK(after([](const File& f) { f.asset().levels.count = 0; }) == level_count);
    CHECK(after([](const File& f) { f.asset().levels.count = kTerrainMaxLevels + 1; }) == level_count);

    const std::string spacing = "terrain's levels don't halve their spacing from the root's";
    CHECK(after([](const File& f) { f.levels()[0].spacing_m = 8.0f; }) == spacing);
    CHECK(after([](const File& f) { f.levels()[1].spacing_m = 16.0f; }) == spacing);
    CHECK(after([](const File& f) { f.levels()[3].spacing_m = 1.0f; }) == spacing);

    const std::string grid = "a terrain level's grid isn't the nodes its size touches";
    CHECK(after([](const File& f) { f.levels()[3].nodes_x += 1; }) == grid);
    CHECK(after([](const File& f) { f.levels()[3].nodes_z -= 1; }) == grid);
    CHECK(after([](const File& f) { f.asset().width_m *= 8.0f; }) == grid);
    CHECK(after([](const File& f) { f.asset().depth_m *= 8.0f; }) == grid);
    // A grid the size didn't call for, however large, asks for nothing: 2^32 - 1 by 2^32 - 1 nodes, as big as it
    // gets.
    CHECK(after([](const File& f) {
              f.levels()[0].nodes_x = UINT32_MAX;
              f.levels()[0].nodes_z = UINT32_MAX;
          }) == grid);

    const std::string belonging = "terrain's nodes and patches don't each belong to one level";
    CHECK(after([](const File& f) { f.levels()[1].first_node += 1; }) == belonging);
    CHECK(after([](const File& f) { f.levels()[3].first_node -= 1; }) == belonging);
    CHECK(after([](const File& f) { f.levels()[1].first_patch += 16; }) == belonging);
    CHECK(after([](const File& f) { f.levels()[3].first_patch -= 16; }) == belonging);
    CHECK(after([](const File& f) { f.asset().nodes.count -= 1; }) == belonging); // too few for the last level
    CHECK(after([](const File& f) { f.asset().nodes.count += 1; }) == belonging); // one no level has
    CHECK(after([](const File& f) { f.asset().patches.count -= 1; }) == belonging);
    CHECK(after([](const File& f) {
              f.asset().patches.offset -= 16; // room for more, inside the file
              f.asset().patches.count += 2;
          }) == belonging);
    // The last level left out: its nodes belong to none.
    CHECK(after([](const File& f) { f.asset().levels.count -= 1; }) == belonging);

    CHECK(after([](const File& f) { f.asset().block_count = f.asset().nodes.count + 1; }) ==
          "terrain has more blocks than nodes");
    CHECK(after([](const File& f) { f.nodes()[2].block = f.asset().block_count; }) ==
          "a terrain node's block lies past the heights file");
    CHECK(after([](const File& f) { f.nodes()[2].block = f.nodes()[1].block; }) == "two terrain nodes share a block");
    CHECK(after([](const File& f) { f.nodes()[2].block = kTerrainNoBlock; }) == "a terrain block belongs to no node");
    CHECK(after([](const File& f) { f.asset().block_count -= 1; }) ==
          "a terrain node's block lies past the heights file");
    CHECK(after([](const File& f) { f.nodes()[0].base_steps = INT32_MAX - 65534; }) ==
          "a terrain node's samples would overflow its base");

    const std::string heights = "a terrain patch's heights aren't finite and in order";
    CHECK(after([&](const File& f) { f.patches()[7].min_m = not_a_number; }) == heights);
    CHECK(after([&](const File& f) { f.patches()[7].max_m = not_a_number; }) == heights);
    CHECK(after([&](const File& f) { f.patches()[7].min_m = -infinite; }) == heights);
    CHECK(after([&](const File& f) { f.patches()[7].max_m = infinite; }) == heights);
    CHECK(after([](const File& f) { f.patches()[7].min_m = f.patches()[7].max_m + 1.0f; }) == heights);
    const uint32_t last_patch = cooked.terrain().patches.count - 1; // the end of the check's reach
    CHECK(after([&](const File& f) { f.patches()[last_patch].max_m = not_a_number; }) == heights);
}

// --- Worlds and the registry ------------------------------------------------------------------

TEST_CASE("terrain: a world names its terrain, which isn't cooked with it, and cooks again only for a new ID") {
    const Sandbox sandbox;
    write_map(sandbox, "land", bumpy_map(300, 200));
    write_src(sandbox, "content/land/evening.environment", kEnvironment);
    write_src(sandbox, "content/land/with.world", R"({
  "environment": "evening.environment", "terrain": "land.terrain",
  "start": {"position": [19400, 340, 6500], "yaw_deg": 90, "pitch_deg": -5}, "cells": []})");
    write_src(sandbox, "content/land/without.world", R"({
  "environment": "evening.environment", "start": {"position": [0, 0, 0]}, "cells": []})");
    write_src(sandbox, "content/land/wrong_kind.world", R"({
  "environment": "evening.environment", "terrain": "evening.environment", "start": {"position": [0, 0, 0]}, "cells": []})");
    write_src(sandbox, "content/land/no_such.world", R"({
  "environment": "evening.environment", "terrain": "nowhere.terrain", "start": {"position": [0, 0, 0]}, "cells": []})");
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    CHECK(has_message(report, "content/land/wrong_kind.world", "'evening.environment' should name a terrain source"));
    CHECK(has_message(report, "content/land/no_such.world", "'nowhere.terrain' should name a terrain source"));
    CHECK(report.stats.failed == 2);
    CHECK_FALSE(has_message(report, "content/land/with.world", ""));
    CHECK_FALSE(has_message(report, "content/land/without.world", ""));

    const AssetId terrain_id = id_of(vfs, "land");
    const AssetId environment_id = asset_id(guid_of(vfs, "src:/meta/land/evening.environment.json"));
    const AssetId world_id = asset_id(guid_of(vfs, "src:/meta/land/with.world.json"));
    {
        Cooked file(vfs, world_id, AssetType::World);
        const auto& world = file.load<WorldAsset>();
        CHECK(world.header.format_version == 2);
        const auto dependencies = world.header.dependencies.get(file.data());
        REQUIRE(dependencies.size() == 2);
        REQUIRE(world.environment != WorldAsset::kNone);
        REQUIRE(world.terrain != WorldAsset::kNone);
        CHECK(dependencies[world.environment] == AssetRef{.id = environment_id, .type = AssetType::Environment});
        CHECK(dependencies[world.terrain] == AssetRef{.id = terrain_id, .type = AssetType::Terrain});
        CHECK(world.start_position[0] == 19400.0);
        CHECK(world.cells.count == 0);
    }
    {
        Cooked file(vfs, asset_id(guid_of(vfs, "src:/meta/land/without.world.json")), AssetType::World);
        const auto& world = file.load<WorldAsset>();
        CHECK(world.terrain == WorldAsset::kNone);
        CHECK(world.header.dependencies.count == 1);
        CHECK(world.header.dependencies.get(file.data())[world.environment].type == AssetType::Environment);
    }

    // Without the two broken worlds, which fail each time, nothing cooks while nothing changes.
    remove_src(sandbox, "content/land/wrong_kind.world");
    remove_src(sandbox, "meta/land/wrong_kind.world.json");
    remove_src(sandbox, "content/land/no_such.world");
    remove_src(sandbox, "meta/land/no_such.world.json");
    REQUIRE(run_cook(vfs).ok());
    CHECK(run_cook(vfs).stats.cooked == 0);
    // New land: only the terrain cooks.
    write_src(sandbox, "content/land/land.heightpng", png_of(bumpy_map(310, 200)));
    CHECK(run_cook(vfs).stats.cooked == 1);
    // A new ID: the worlds naming it do too (one of the two here).
    remove_src(sandbox, "meta/land/land.terrain.json");
    CHECK(run_cook(vfs).stats.cooked == 2);
    Cooked file(vfs, world_id, AssetType::World);
    const auto& world = file.load<WorldAsset>();
    CHECK(world.header.dependencies.get(file.data())[world.terrain].id == id_of(vfs, "land"));
    CHECK(world.header.dependencies.get(file.data())[world.terrain].id != terrain_id);
}

TEST_CASE("terrain: a broken world's terrain is refused when it's loaded") {
    struct alignas(16) Aligned {
        std::byte bytes[512];
    };
    const auto loads = [](const de::core::Array<std::byte>& cooked, auto edit) {
        Aligned aligned{};
        REQUIRE(cooked.size() <= sizeof(aligned.bytes));
        std::memcpy(aligned.bytes, cooked.data(), cooked.size());
        auto* world = reinterpret_cast<WorldAsset*>(aligned.bytes);
        edit(*world);
        return load_in_place<WorldAsset>(std::span(aligned.bytes, cooked.size())).ok();
    };
    de::cooker::WorldBuild build;
    build.environment = {.id = AssetId::from_value(1), .type = AssetType::Environment};
    build.terrain = {.id = AssetId::from_value(2), .type = AssetType::Terrain};
    const auto with_terrain = de::cooker::cook_world(build, AssetId::from_value(3));
    build.terrain = {};
    const auto without = de::cooker::cook_world(build, AssetId::from_value(3));
    const auto keep = [](WorldAsset&) {};
    CHECK(loads(with_terrain, keep));
    CHECK(loads(without, keep));
    // Past the dependencies, or naming the environment.
    CHECK_FALSE(loads(with_terrain, [](WorldAsset& world) { world.terrain = 2; }));
    CHECK_FALSE(loads(with_terrain, [](WorldAsset& world) { world.terrain = 0; }));
    CHECK_FALSE(loads(without, [](WorldAsset& world) { world.terrain = 1; }));
    CHECK(loads(with_terrain, [](WorldAsset& world) { world.terrain = WorldAsset::kNone; }));

    // A dependency of the wrong type: the terrain is a mesh.
    build.terrain = {.id = AssetId::from_value(2), .type = AssetType::Mesh};
    CHECK_FALSE(loads(de::cooker::cook_world(build, AssetId::from_value(3)), keep));
}

TEST_CASE("terrain: it loads with its world, and its heights never load whole") {
    const Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 2});
    Vfs vfs = sandbox.make_vfs({.jobs = &jobs, .game_writable = true});
    const AssetId environment_id = AssetId::from_value(0x51);
    const AssetId terrain_id = AssetId::from_value(0x52);
    const AssetId world_id = AssetId::from_value(0x53);
    const AssetId broken_id = AssetId::from_value(0x54);

    const Map map = bumpy_map(300, 200);
    de::cooker::Diagnostics diagnostics(false);
    const auto cooked =
        de::cooker::cook_terrain(source_of(map), image_of(map), terrain_id, nullptr, "test", diagnostics);
    REQUIRE(cooked.ok());
    const CookedPath heights_path = cooked_path(terrain_id, AssetType::TerrainHeights);
    REQUIRE(vfs.write_file(cooked_path(terrain_id, AssetType::Terrain).c_str(), cooked->terrain.span()).ok());
    REQUIRE(vfs.write_file(heights_path.c_str(), cooked->heights.span()).ok());
    EnvironmentAsset environment;
    environment.shadow_distance_m = 100.0f;
    REQUIRE(vfs.write_file(cooked_path(environment_id, AssetType::Environment).c_str(),
                           de::cooker::cook_environment(environment, environment_id).span())
                .ok());
    de::cooker::WorldBuild build;
    build.environment = {.id = environment_id, .type = AssetType::Environment};
    build.terrain = {.id = terrain_id, .type = AssetType::Terrain};
    REQUIRE(
        vfs.write_file(cooked_path(world_id, AssetType::World).c_str(), de::cooker::cook_world(build, world_id).span())
            .ok());

    auto created = AssetRegistry::create({.vfs = &vfs, .jobs = &jobs, .hot_reload = false});
    REQUIRE(created.ok());
    AssetRegistry registry = std::move(created).value();
    const AssetHandle world_handle = registry.load<WorldAsset>(world_id);
    registry.wait_all();
    REQUIRE(registry.state(world_handle) == AssetState::Ready);
    const auto* world = registry.get<WorldAsset>(world_handle);
    REQUIRE(world != nullptr);
    const std::span<const AssetHandle> dependencies = registry.dependencies(world_handle);
    REQUIRE(dependencies.size() == 2);
    const AssetHandle terrain_handle = dependencies[world->terrain];
    CHECK(registry.state(terrain_handle) == AssetState::Ready);
    CHECK(registry.asset(terrain_handle) == AssetRef{.id = terrain_id, .type = AssetType::Terrain});
    const auto* terrain = registry.get<TerrainAsset>(terrain_handle);
    REQUIRE(terrain != nullptr);
    CHECK(terrain->block_count == 10);
    // Three assets in memory, the heights not among them.
    CHECK(registry.stats().assets == 3);
    CHECK(registry.stats().bytes < cooked->heights.size());

    // Even when they're asked for by name, and their file is there.
    REQUIRE(vfs.exists(heights_path.c_str()));
    const uint64_t read_before = vfs.stats().bytes_read;
    const AssetHandle heights_handle = registry.load({.id = terrain_id, .type = AssetType::TerrainHeights});
    registry.wait_all();
    CHECK(registry.state(heights_handle) == AssetState::Failed);
    CHECK(vfs.stats().bytes_read == read_before); // refused before a byte of it is read
    CHECK(registry.stats().bytes < cooked->heights.size());
    CHECK(registry.state(terrain_handle) == AssetState::Ready); // the same ID, as a terrain: another asset
    registry.release(heights_handle);

    // A corrupt one fails with a reason, and doesn't fail the rest.
    de::core::Array<std::byte> corrupt;
    corrupt.assign(cooked->terrain.span());
    TerrainAsset header;
    std::memcpy(&header, corrupt.data(), sizeof(header));
    header.header.id = broken_id;
    header.width_m = -1.0f;
    std::memcpy(corrupt.data(), &header, sizeof(header));
    REQUIRE(vfs.write_file(cooked_path(broken_id, AssetType::Terrain).c_str(), corrupt.span()).ok());
    const AssetHandle broken_handle = registry.load<TerrainAsset>(broken_id);
    registry.wait_all();
    CHECK(registry.state(broken_handle) == AssetState::Failed);
    CHECK(registry.state(terrain_handle) == AssetState::Ready);
    registry.release(broken_handle);
    registry.release(world_handle);
    registry.update();
    CHECK(registry.stats().assets == 0);
}
