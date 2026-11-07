#include "cooker/terrain_cooker.h"

#include "asset/terrain_format.h"
#include "cooker/blob_writer.h"
#include "cooker/json.h"
#include "core/job_system.h"
#include "core/profile.h"

#include <stb_image.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>

namespace de::cooker {

namespace {

using asset::kTerrainNodeBorder;
using asset::kTerrainNodeQuads;
using asset::kTerrainNodeSamples;
using asset::kTerrainPatchesPerNode;
using asset::kTerrainPatchQuads;

constexpr std::string_view kHeightExtension = ".heightpng";
constexpr uint32_t kPixelValues = 65536;
constexpr int64_t kMaxBlockRise = 65535; // steps a block's 16-bit samples can hold above its base
// Far past any land, and near enough that the finest level's steps stay well inside 32 bits.
constexpr double kMaxHeightM = 1'000'000.0;

// A required number, finite; reported otherwise.
bool required_number(const JsonReader& reader, const char* key, double& out) {
    yyjson_val* value = yyjson_obj_get(reader.object, key);
    if (!yyjson_is_num(value) || !std::isfinite(yyjson_get_num(value))) {
        reader.diagnostics->error(reader.path, "%s should be a number", key);
        return false;
    }
    out = yyjson_get_num(value);
    return true;
}

// The level count for a source spacing: the spacings 16 divided by a power of 2 that the format has room for.
uint32_t levels_for_spacing(double meters_per_pixel) {
    for (uint32_t levels = 1; levels <= asset::kTerrainMaxLevels; ++levels) {
        if (meters_per_pixel == static_cast<double>(asset::kTerrainRootSpacingM) / (1U << (levels - 1))) {
            return levels;
        }
    }
    return 0;
}

// The nearest float at or below value, and at or above it: a bound that stays one after rounding.
float float_at_or_below(double value) {
    const auto rounded = static_cast<float>(value);
    return static_cast<double>(rounded) > value ? std::nextafter(rounded, -std::numeric_limits<float>::infinity())
                                                : rounded;
}

float float_at_or_above(double value) {
    const auto rounded = static_cast<float>(value);
    return static_cast<double>(rounded) < value ? std::nextafter(rounded, std::numeric_limits<float>::infinity())
                                                : rounded;
}

// One level of the pyramid on its way to the files.
struct Level {
    double spacing_m = 0.0;
    double step_m = 0.0;
    uint32_t stride = 1; // source samples per sample of this level
    uint32_t nodes_x = 0;
    uint32_t nodes_z = 0;
    uint32_t first_node = 0;
    core::Array<int32_t> steps_of_pixel{tools_heap()}; // what each of the 65536 pixel values rounds to here
};

// A node that didn't fit, as a level remembers its worst.
struct Overflow {
    uint32_t count = 0;
    int64_t worst_rise = 0;
    uint32_t worst_x = 0;
    uint32_t worst_z = 0;
};

// What the node jobs share: each writes only its own node, block and patches.
struct NodeJob {
    const HeightImage* image = nullptr;
    std::span<const Level> levels;
    std::span<const uint32_t> level_of_node;
    std::span<asset::TerrainNode> nodes;
    std::span<asset::TerrainPatch> patches;
    std::span<int64_t> rise; // of each node's block, in its level's steps
    std::byte* blocks = nullptr;
};

// The source pixels of a block: its node's samples and the border, past the map's edge the nearest.
void read_block_pixels(const HeightImage& image, const Level& level, uint32_t node_x, uint32_t node_z, uint16_t* out) {
    const int64_t first_x = (int64_t{node_x} * kTerrainNodeQuads) - kTerrainNodeBorder;
    const int64_t first_z = (int64_t{node_z} * kTerrainNodeQuads) - kTerrainNodeBorder;
    const int64_t last_source_x = int64_t{image.width} - 1;
    const int64_t last_source_z = int64_t{image.depth} - 1;
    for (uint32_t row = 0; row < kTerrainNodeSamples; ++row) {
        const int64_t source_z = std::clamp((first_z + row) * level.stride, int64_t{0}, last_source_z);
        const uint16_t* source_row = image.samples.data() + (source_z * image.width);
        for (uint32_t column = 0; column < kTerrainNodeSamples; ++column) {
            const int64_t source_x = std::clamp((first_x + column) * level.stride, int64_t{0}, last_source_x);
            out[(row * kTerrainNodeSamples) + column] = source_row[source_x];
        }
    }
}

// A patch's reach: its samples, and those of the level above where it has them. What morphs reads is either: a vertex
// goes from its own height toward the coarser surface, and a coarse sample can lie outside the fine ones by its
// rounding. The border isn't the patch's.
asset::TerrainPatch patch_bounds(const uint16_t* pixels, const Level& level, const Level* coarser, uint32_t patch_x,
                                 uint32_t patch_z) {
    int32_t lowest = INT32_MAX;
    int32_t highest = INT32_MIN;
    int32_t coarse_lowest = INT32_MAX;
    int32_t coarse_highest = INT32_MIN;
    for (uint32_t z = 0; z <= kTerrainPatchQuads; ++z) {
        const uint32_t row = kTerrainNodeBorder + (patch_z * kTerrainPatchQuads) + z;
        for (uint32_t x = 0; x <= kTerrainPatchQuads; ++x) {
            const uint32_t column = kTerrainNodeBorder + (patch_x * kTerrainPatchQuads) + x;
            const uint16_t pixel = pixels[(row * kTerrainNodeSamples) + column];
            const int32_t steps = level.steps_of_pixel[pixel];
            lowest = std::min(lowest, steps);
            highest = std::max(highest, steps);
            // A node's patches start on even samples, so every other sample here is one of the coarser level's.
            if (coarser != nullptr && x % 2 == 0 && z % 2 == 0) {
                const int32_t coarse_steps = coarser->steps_of_pixel[pixel];
                coarse_lowest = std::min(coarse_lowest, coarse_steps);
                coarse_highest = std::max(coarse_highest, coarse_steps);
            }
        }
    }
    double min_m = lowest * level.step_m;
    double max_m = highest * level.step_m;
    if (coarser != nullptr) {
        min_m = std::min(min_m, coarse_lowest * coarser->step_m);
        max_m = std::max(max_m, coarse_highest * coarser->step_m);
    }
    return {.min_m = float_at_or_below(min_m), .max_m = float_at_or_above(max_m)};
}

void cook_nodes(size_t begin, size_t end, void* context) {
    DE_PROFILE_ZONE("cook terrain nodes");
    const NodeJob& job = *static_cast<const NodeJob*>(context);
    core::Array<uint16_t> pixels(tools_heap());
    pixels.resize_uninit(size_t{kTerrainNodeSamples} * kTerrainNodeSamples);
    core::Array<uint16_t> samples(tools_heap());
    samples.resize(size_t{kTerrainNodeSamples} * kTerrainNodeSamples);
    constexpr uint32_t kPatchesPerNode = kTerrainPatchesPerNode * kTerrainPatchesPerNode;
    for (size_t index = begin; index < end; ++index) {
        const uint32_t level_index = job.level_of_node[index];
        const Level& level = job.levels[level_index];
        const auto local = static_cast<uint32_t>(index - level.first_node);
        const uint32_t node_x = local % level.nodes_x;
        const uint32_t node_z = local / level.nodes_x;
        read_block_pixels(*job.image, level, node_x, node_z, pixels.data());

        int32_t lowest = INT32_MAX;
        int32_t highest = INT32_MIN;
        for (const uint16_t pixel : pixels) {
            const int32_t steps = level.steps_of_pixel[pixel];
            lowest = std::min(lowest, steps);
            highest = std::max(highest, steps);
        }
        const int64_t rise = int64_t{highest} - lowest;
        job.rise[index] = rise;
        if (rise > kMaxBlockRise) {
            continue; // reported once every node is done: nothing is written
        }
        for (size_t i = 0; i < pixels.size(); ++i) {
            samples[i] = static_cast<uint16_t>(level.steps_of_pixel[pixels[i]] - lowest);
        }
        std::memcpy(job.blocks + (index * asset::kTerrainNodeBytes), samples.data(), samples.span().size_bytes());
        job.nodes[index] = {.base_steps = lowest, .block = static_cast<uint32_t>(index)};

        // The patches of a level's first node come first, a row across every node at a time.
        const Level* coarser = level_index > 0 ? &job.levels[level_index - 1] : nullptr;
        const uint32_t patches_x = level.nodes_x * kTerrainPatchesPerNode;
        const size_t first_patch = size_t{level.first_node} * kPatchesPerNode;
        for (uint32_t patch_z = 0; patch_z < kTerrainPatchesPerNode; ++patch_z) {
            for (uint32_t patch_x = 0; patch_x < kTerrainPatchesPerNode; ++patch_x) {
                const size_t row = (size_t{node_z} * kTerrainPatchesPerNode) + patch_z;
                const size_t column = (size_t{node_x} * kTerrainPatchesPerNode) + patch_x;
                job.patches[first_patch + (row * patches_x) + column] =
                    patch_bounds(pixels.data(), level, coarser, patch_x, patch_z);
            }
        }
    }
}

// Every node that didn't fit is one too many, but one message a level, its worst, says what to fix.
void report_overflows(const TerrainSource& source, std::span<const Level> levels, std::span<const int64_t> rise,
                      std::string_view path, Diagnostics& diagnostics) {
    for (const Level& level : levels) {
        Overflow overflow;
        const size_t count = size_t{level.nodes_x} * level.nodes_z;
        for (size_t local = 0; local < count; ++local) {
            const int64_t node_rise = rise[level.first_node + local];
            if (node_rise > kMaxBlockRise) {
                ++overflow.count;
                if (node_rise > overflow.worst_rise) {
                    overflow.worst_rise = node_rise;
                    overflow.worst_x = static_cast<uint32_t>(local % level.nodes_x);
                    overflow.worst_z = static_cast<uint32_t>(local / level.nodes_x);
                }
            }
        }
        if (overflow.count == 0) {
            continue;
        }
        const double node_m = level.spacing_m * kTerrainNodeQuads;
        diagnostics.error(path,
                          "is too steep at %g m spacing: %u node(s) rise further than the %.2f m a node holds. The "
                          "worst, node [%u, %u], with its corner at X %.2f m, Z %.2f m, rises %.2f m: %.2f m too far",
                          level.spacing_m, overflow.count, static_cast<double>(kMaxBlockRise) * level.step_m,
                          overflow.worst_x, overflow.worst_z, source.origin_m[0] + (overflow.worst_x * node_m),
                          source.origin_m[1] + (overflow.worst_z * node_m),
                          static_cast<double>(overflow.worst_rise) * level.step_m,
                          static_cast<double>(overflow.worst_rise - kMaxBlockRise) * level.step_m);
    }
}

} // namespace

core::Result<TerrainSource> import_terrain(std::string_view path, std::span<const std::byte> bytes,
                                           Diagnostics& diagnostics) {
    const size_t errors_before = diagnostics.error_count();
    const core::Result<JsonDoc> doc = JsonDoc::parse(bytes, path, diagnostics);
    if (!doc) {
        return core::fail(doc.error());
    }
    if (!yyjson_is_obj(doc->root())) {
        diagnostics.error(path, "should be a JSON object");
        return core::fail(core::ErrorCode::InvalidArgument, "the terrain has errors");
    }
    const JsonReader reader{.object = doc->root(), .path = path, .diagnostics = &diagnostics};
    static constexpr const char* kKeys[] = {"height", "meters_per_pixel", "lowest_m", "highest_m", "origin"};
    reader.check_keys(kKeys, "terrain");

    TerrainSource source;
    const std::string_view height = reader.string("height", "");
    if (extension_of(height) != kHeightExtension || !join_relative(folder_of(path), height, source.height)) {
        diagnostics.error(path,
                          "height should name a 16-bit grayscale PNG with the extension %.*s, relative to this file "
                          "(a .png would be cooked as a texture too)",
                          static_cast<int>(kHeightExtension.size()), kHeightExtension.data());
    }
    if (required_number(reader, "meters_per_pixel", source.meters_per_pixel) &&
        levels_for_spacing(source.meters_per_pixel) == 0) {
        diagnostics.error(path,
                          "meters_per_pixel should be 16 divided by a power of 2 (16, 8, 4, 2, 1, 0.5, 0.25 or 0.125), "
                          "not %g",
                          source.meters_per_pixel);
    }
    // A PNG doesn't say what its pixels mean, and a guess would be wrong all over the map.
    const bool has_lowest = required_number(reader, "lowest_m", source.lowest_m);
    const bool has_highest = required_number(reader, "highest_m", source.highest_m);
    if (has_lowest && has_highest) {
        if (source.highest_m <= source.lowest_m) {
            diagnostics.error(path, "highest_m (%g) should be above lowest_m (%g)", source.highest_m, source.lowest_m);
        }
        if (std::abs(source.lowest_m) > kMaxHeightM || std::abs(source.highest_m) > kMaxHeightM) {
            diagnostics.error(path, "lowest_m and highest_m should be within %g m of sea level", kMaxHeightM);
        }
    }
    if (reader.has("origin") && (!read_numbers(yyjson_obj_get(doc->root(), "origin"), source.origin_m, 2) ||
                                 !std::isfinite(source.origin_m[0]) || !std::isfinite(source.origin_m[1]))) {
        diagnostics.error(path, "origin should be [x, z], where the first sample lies, in meters");
    }
    if (diagnostics.error_count() != errors_before) {
        return core::fail(core::ErrorCode::InvalidArgument, "the terrain has errors");
    }
    return source;
}

bool check_height_image(std::span<const std::byte> encoded, std::string_view path, Diagnostics& diagnostics) {
    // stb also reads 16-bit PSDs, and turns any other image into 16 bits of something.
    static constexpr uint8_t kPngSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (encoded.size() < sizeof(kPngSignature) || encoded.size() > static_cast<size_t>(INT_MAX) ||
        std::memcmp(encoded.data(), kPngSignature, sizeof(kPngSignature)) != 0) {
        diagnostics.error(path, "isn't a PNG under 2 GB: heights are 16-bit grayscale PNGs");
        return false;
    }
    const auto* data = reinterpret_cast<const stbi_uc*>(encoded.data());
    const int size = static_cast<int>(encoded.size());
    int width = 0;
    int depth = 0;
    int channels = 0;
    if (stbi_info_from_memory(data, size, &width, &depth, &channels) == 0) {
        diagnostics.error(path, "isn't a readable PNG: %s", stbi_failure_reason());
        return false;
    }
    if (stbi_is_16_bit_from_memory(data, size) == 0) {
        diagnostics.error(path, "has 8 bits a sample: heights need 16, so export it as a 16-bit grayscale PNG");
        return false;
    }
    if (channels != 1) {
        diagnostics.error(path, "has %d channels: heights are one, so export it as a 16-bit grayscale PNG", channels);
        return false;
    }
    if (width < 2 || depth < 2) {
        diagnostics.error(path, "is %d x %d samples: land needs at least 2 x 2", width, depth);
        return false;
    }
    return true;
}

core::Result<HeightImage> decode_height_image(std::span<const std::byte> encoded, std::string_view path,
                                              Diagnostics& diagnostics) {
    DE_PROFILE_ZONE("decode height image");
    if (!check_height_image(encoded, path, diagnostics)) {
        return core::fail(core::ErrorCode::InvalidArgument, "the height image isn't one the cooker takes");
    }
    int width = 0;
    int depth = 0;
    int channels = 0;
    stbi_us* samples = stbi_load_16_from_memory(reinterpret_cast<const stbi_uc*>(encoded.data()),
                                                static_cast<int>(encoded.size()), &width, &depth, &channels, 1);
    if (samples == nullptr) {
        diagnostics.error(path, "could not be decoded: %s", stbi_failure_reason());
        return core::fail(core::ErrorCode::CorruptData, "the height image could not be decoded");
    }
    HeightImage image;
    image.width = static_cast<uint32_t>(width);
    image.depth = static_cast<uint32_t>(depth);
    image.samples.resize_uninit(size_t{image.width} * image.depth);
    std::memcpy(image.samples.data(), samples, image.samples.span().size_bytes());
    stbi_image_free(samples);
    return image;
}

core::Result<CookedTerrain> cook_terrain(const TerrainSource& source, const HeightImage& image, asset::AssetId id,
                                         core::JobSystem* jobs, std::string_view path, Diagnostics& diagnostics) {
    DE_PROFILE_ZONE("cook terrain");
    const uint32_t level_count = levels_for_spacing(source.meters_per_pixel);
    DE_ASSERT(level_count != 0 && image.width >= 2 && image.depth >= 2); // import_terrain, decode_height_image
    const double width_m = static_cast<double>(image.width - 1) * source.meters_per_pixel;
    const double depth_m = static_cast<double>(image.depth - 1) * source.meters_per_pixel;

    core::Array<Level> levels(tools_heap());
    core::Array<asset::TerrainLevel> file_levels(tools_heap());
    uint64_t node_total = 0;
    for (uint32_t l = 0; l < level_count; ++l) {
        Level& level = levels.emplace();
        level.spacing_m = static_cast<double>(asset::kTerrainRootSpacingM) / (1U << l);
        level.step_m = level.spacing_m / static_cast<double>(asset::kTerrainStepsPerSpacing);
        level.stride = 1U << (level_count - 1 - l);
        const double node_m = level.spacing_m * kTerrainNodeQuads;
        level.nodes_x = static_cast<uint32_t>(std::ceil(width_m / node_m));
        level.nodes_z = static_cast<uint32_t>(std::ceil(depth_m / node_m));
        level.first_node = static_cast<uint32_t>(node_total);
        level.steps_of_pixel.resize_uninit(kPixelValues);
        for (uint32_t pixel = 0; pixel < kPixelValues; ++pixel) {
            const double height_m = source.lowest_m + (static_cast<double>(pixel) / (kPixelValues - 1) *
                                                       (source.highest_m - source.lowest_m));
            level.steps_of_pixel[pixel] = static_cast<int32_t>(std::llround(height_m / level.step_m));
        }
        node_total += uint64_t{level.nodes_x} * level.nodes_z;
        constexpr uint64_t kPatchesPerNode = uint64_t{kTerrainPatchesPerNode} * kTerrainPatchesPerNode;
        if (node_total * kPatchesPerNode > asset::kMaxAssetFileSize / sizeof(asset::TerrainPatch)) {
            diagnostics.error(path, "is %u x %u samples: more nodes than a terrain file can list", image.width,
                              image.depth);
            return core::fail(core::ErrorCode::OutOfRange, "the terrain is too large");
        }
        file_levels.push({.spacing_m = static_cast<float>(level.spacing_m),
                          .nodes_x = level.nodes_x,
                          .nodes_z = level.nodes_z,
                          .first_node = level.first_node,
                          .first_patch = static_cast<uint32_t>(uint64_t{level.first_node} * kPatchesPerNode)});
    }
    const auto node_count = static_cast<uint32_t>(node_total);

    core::Array<uint32_t> level_of_node(tools_heap());
    level_of_node.resize_uninit(node_count);
    for (uint32_t l = 0; l < level_count; ++l) {
        const Level& level = levels[l];
        const uint32_t count = level.nodes_x * level.nodes_z;
        for (uint32_t n = 0; n < count; ++n) {
            level_of_node[level.first_node + n] = l;
        }
    }
    core::Array<asset::TerrainNode> nodes(tools_heap());
    nodes.resize(node_count);
    core::Array<asset::TerrainPatch> patches(tools_heap());
    patches.resize(size_t{node_count} * kTerrainPatchesPerNode * kTerrainPatchesPerNode);
    core::Array<int64_t> rise(tools_heap());
    rise.resize(node_count);
    const uint64_t heights_size = asset::terrain_block_offset(node_count);
    core::Array<std::byte> heights(tools_heap());
    heights.resize(static_cast<size_t>(heights_size)); // the padding after each block's samples stays zero

    NodeJob job{.image = &image,
                .levels = levels.span(),
                .level_of_node = level_of_node.span(),
                .nodes = nodes.span(),
                .patches = patches.span(),
                .rise = rise.span(),
                .blocks = heights.data() + sizeof(asset::AssetHeader)};
    if (jobs != nullptr) {
        jobs->parallel_for(node_count, 8, &cook_nodes, &job, "cook terrain nodes");
    } else {
        cook_nodes(0, node_count, &job);
    }
    if (std::ranges::any_of(rise, [](int64_t node_rise) { return node_rise > kMaxBlockRise; })) {
        report_overflows(source, levels.span(), rise.span(), path, diagnostics);
        return core::fail(core::ErrorCode::OutOfRange, "the terrain is too steep");
    }

    asset::AssetHeader heights_header;
    heights_header.type = asset::AssetType::TerrainHeights;
    heights_header.format_version = asset::TerrainAsset::kFormatVersion;
    heights_header.platform = asset::Platform::Pc;
    heights_header.file_size = heights_size;
    heights_header.id = id;
    heights_header.content_hash = asset::content_hash(heights.span());
    std::memcpy(heights.data(), &heights_header, sizeof(heights_header));

    asset::TerrainAsset terrain;
    terrain.origin_m[0] = source.origin_m[0];
    terrain.origin_m[1] = source.origin_m[1];
    terrain.width_m = static_cast<float>(width_m); // exact: under 2^24 samples, times a power of 2
    terrain.depth_m = static_cast<float>(depth_m);
    terrain.block_count = node_count;
    terrain.heights_hash = heights_header.content_hash;
    BlobWriter writer(sizeof(asset::TerrainAsset));
    terrain.levels = writer.append(file_levels.span());
    terrain.nodes = writer.append(nodes.span());
    terrain.patches = writer.append(patches.span());

    CookedTerrain cooked;
    cooked.terrain = writer.finish(terrain, id, {});
    cooked.heights = std::move(heights);
    cooked.levels = level_count;
    cooked.nodes = node_count;
    return cooked;
}

} // namespace de::cooker
