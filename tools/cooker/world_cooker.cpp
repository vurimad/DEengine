#include "cooker/world_cooker.h"

#include "cooker/blob_writer.h"
#include "cooker/json.h"
#include "cooker/source_tree.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace de::cooker {

namespace {

constexpr double kRadPerDeg = std::numbers::pi / 180.0;

// A path relative to from's folder, as a source of the tree with one of the kinds; null (reported)
// if it isn't one.
const Source* find_relative(const SourceTree& tree, std::string_view from, std::string_view reference, SourceKind kind,
                            const char* what, Diagnostics& diagnostics) {
    SourcePath path;
    const Source* found = join_relative(folder_of(from), reference, path) ? tree.find(path.view()) : nullptr;
    if (found == nullptr || found->kind != kind) {
        diagnostics.error(from, "'%.*s' should name %s, relative to this file", static_cast<int>(reference.size()),
                          reference.data(), what);
        return nullptr;
    }
    return found;
}

// A cell source's coord, without cooking it; false (reported) if it has none.
bool cell_coord(const Source& cell, std::string_view world, SourceReader read, int32_t (&coord)[2],
                Diagnostics& diagnostics) {
    const core::Result<core::Array<std::byte>> bytes = read(cell.path.view());
    if (!bytes) {
        return false; // reported by read
    }
    Diagnostics ignored(false); // the cell's own cook reports its errors
    const core::Result<JsonDoc> doc = JsonDoc::parse(bytes->span(), cell.path.view(), ignored);
    double read_coord[2] = {};
    if (!doc || !read_numbers(yyjson_obj_get(doc->root(), "coord"), read_coord, 2) ||
        read_coord[0] != std::floor(read_coord[0]) || read_coord[1] != std::floor(read_coord[1]) ||
        std::abs(read_coord[0]) > 1e6 || std::abs(read_coord[1]) > 1e6) {
        diagnostics.error(world, "cell %s has no coord", cell.path.c_str());
        return false;
    }
    coord[0] = static_cast<int32_t>(read_coord[0]);
    coord[1] = static_cast<int32_t>(read_coord[1]);
    return true;
}

// A required number, finite and in [least, most]; reported otherwise.
bool required(const JsonReader& reader, const char* key, double least, double most, double& out) {
    yyjson_val* value = yyjson_obj_get(reader.object, key);
    if (!yyjson_is_num(value) || !std::isfinite(yyjson_get_num(value)) || yyjson_get_num(value) < least ||
        yyjson_get_num(value) > most) {
        reader.diagnostics->error(reader.path, "%s should be a number from %g to %g", key, least, most);
        return false;
    }
    out = yyjson_get_num(value);
    return true;
}

// A color times its intensity, from {"color": [r, g, b], "intensity": x}, or a bare [r, g, b].
void read_light(const JsonReader& parent, const char* key, float (&out)[3]) {
    yyjson_val* value = yyjson_obj_get(parent.object, key);
    yyjson_val* color = value;
    double intensity = 1.0;
    if (yyjson_is_obj(value)) {
        const JsonReader reader{.object = value, .path = parent.path, .diagnostics = parent.diagnostics};
        static constexpr const char* kSunKeys[] = {"color", "intensity", "elevation_deg", "azimuth_deg"};
        reader.check_keys(std::span(kSunKeys).first(std::string_view(key) == "sun" ? 4 : 2), key);
        color = yyjson_obj_get(value, "color");
        if (reader.has("intensity")) {
            required(reader, "intensity", 0.0, 1e6, intensity);
        }
    }
    double rgb[3] = {};
    if (!read_numbers(color, rgb, 3) || !std::ranges::all_of(rgb, [](double c) { return c >= 0.0 && c <= 1e6; })) {
        parent.diagnostics->error(parent.path, "%s should have a color: [r, g, b], linear, not negative", key);
        return;
    }
    for (int i = 0; i < 3; ++i) {
        out[i] = static_cast<float>(rgb[i] * intensity);
    }
}

// The fog block: every setting required but the colours' intensities and low_camera.
void read_fog(const JsonReader& parent, asset::EnvironmentAsset& environment) {
    yyjson_val* fog = yyjson_obj_get(parent.object, "fog");
    if (!yyjson_is_obj(fog)) {
        parent.diagnostics->error(parent.path, "fog should be an object");
        return;
    }
    const JsonReader reader{.object = fog, .path = parent.path, .diagnostics = parent.diagnostics};
    using F = asset::FogLimits;
    struct Number {
        const char* key;
        double least;
        double most;
        float* out;
        bool read = false;
    };
    Number numbers[] = {
        {.key = "density_per_m", .least = 0.0, .most = F::kMaxDensityPerM, .out = &environment.fog_density_per_m},
        {.key = "height_density_per_m",
         .least = 0.0,
         .most = F::kMaxDensityPerM,
         .out = &environment.fog_height_density_per_m},
        {.key = "height_falloff_per_m",
         .least = F::kMinFalloffPerM,
         .most = F::kMaxFalloffPerM,
         .out = &environment.fog_height_falloff_per_m},
        {.key = "base_height_m",
         .least = -F::kMaxBaseHeightM,
         .most = F::kMaxBaseHeightM,
         .out = &environment.fog_base_height_m},
        {.key = "sun_height_attenuation",
         .least = 0.0,
         .most = F::kMaxAttenuation,
         .out = &environment.fog_sun_height_attenuation},
        {.key = "ambient_height_attenuation",
         .least = 0.0,
         .most = F::kMaxAttenuation,
         .out = &environment.fog_ambient_height_attenuation},
        {.key = "start_m", .least = 0.0, .most = F::kMaxDistanceM, .out = &environment.fog_start_m},
        {.key = "sun_start_m", .least = 0.0, .most = F::kMaxDistanceM, .out = &environment.fog_sun_start_m},
        {.key = "end_m", .least = 0.0, .most = F::kMaxDistanceM, .out = &environment.fog_end_m},
        {.key = "volume_distance_m",
         .least = 0.0,
         .most = F::kMaxVolumeDistanceM,
         .out = &environment.fog_volume_distance_m},
    };
    // The known keys: the numbers', and the colours'.
    constexpr size_t kNumberCount = std::size(numbers);
    const char* keys[kNumberCount + 5] = {"extinction_tint", "sun_forward", "sun_back", "ambient", "low_camera"};
    for (size_t i = 0; i < kNumberCount; ++i) {
        keys[5 + i] = numbers[i].key;
    }
    reader.check_keys(keys, "fog");
    // Optional: without it, the height fog is the same wherever the camera is.
    if (yyjson_val* low = yyjson_obj_get(fog, "low_camera")) {
        const JsonReader low_reader{.object = low, .path = parent.path, .diagnostics = parent.diagnostics};
        static constexpr const char* kLowKeys[] = {"keep", "below_m", "above_m"};
        low_reader.check_keys(kLowKeys, "low_camera");
        double keep = 1.0;
        double below = 0.0;
        double above = 0.0;
        if (required(low_reader, "keep", 0.0, 1.0, keep) &&
            required(low_reader, "below_m", -F::kMaxBaseHeightM, F::kMaxBaseHeightM, below) &&
            required(low_reader, "above_m", below, F::kMaxBaseHeightM, above)) {
            environment.fog_low_camera_keep = static_cast<float>(keep);
            environment.fog_low_camera_below_m = static_cast<float>(below);
            environment.fog_low_camera_above_m = static_cast<float>(above);
        }
    }
    for (Number& number : numbers) {
        double value = 0.0;
        number.read = required(reader, number.key, number.least, number.most, value);
        if (number.read) {
            *number.out = static_cast<float>(value);
        }
    }
    // Compared only when both were read: a typo in one is its own error.
    if (numbers[6].read && numbers[8].read && environment.fog_end_m <= environment.fog_start_m) {
        parent.diagnostics->error(parent.path, "fog's end_m should be past its start_m");
    }
    double tint[3] = {};
    if (!read_numbers(yyjson_obj_get(fog, "extinction_tint"), tint, 3) ||
        !std::ranges::all_of(tint, [](double t) { return t >= F::kMinExtinctionTint && t <= F::kMaxExtinctionTint; })) {
        parent.diagnostics->error(parent.path, "fog's extinction_tint should be [r, g, b], each from %g to %g",
                                  static_cast<double>(F::kMinExtinctionTint),
                                  static_cast<double>(F::kMaxExtinctionTint));
    } else {
        for (int i = 0; i < 3; ++i) {
            environment.fog_extinction_tint[i] = static_cast<float>(tint[i]);
        }
    }
    read_light(reader, "sun_forward", environment.fog_sun_forward);
    read_light(reader, "sun_back", environment.fog_sun_back);
    read_light(reader, "ambient", environment.fog_ambient);
}

} // namespace

core::Result<WorldBuild> import_world(const SourceTree& tree, const Source& source, std::span<const std::byte> bytes,
                                      SourceReader read, Diagnostics& diagnostics) {
    const std::string_view path = source.path.view();
    const size_t errors_before = diagnostics.error_count();
    const core::Result<JsonDoc> doc = JsonDoc::parse(bytes, path, diagnostics);
    if (!doc) {
        return core::fail(doc.error());
    }
    WorldBuild build;
    const JsonReader reader{.object = doc->root(), .path = path, .diagnostics = &diagnostics};
    if (!yyjson_is_obj(doc->root())) {
        diagnostics.error(path, "should be a JSON object");
        return core::fail(core::ErrorCode::InvalidArgument, "the world has errors");
    }
    static constexpr const char* kKeys[] = {"environment", "terrain", "terrain_layers", "sea", "start", "cells"};
    reader.check_keys(kKeys, "world");

    const Source* environment = find_relative(tree, path, reader.string("environment", ""), SourceKind::Environment,
                                              "an environment source (.environment)", diagnostics);
    if (environment != nullptr) {
        build.environment = {.id = environment->id, .type = asset::AssetType::Environment};
        build.read.push(environment->path);
    }
    if (reader.has("terrain")) {
        const Source* terrain = find_relative(tree, path, reader.string("terrain", ""), SourceKind::Terrain,
                                              "a terrain source (.terrain)", diagnostics);
        if (terrain != nullptr) {
            build.terrain = {.id = terrain->id, .type = asset::AssetType::Terrain};
            build.read.push(terrain->path);
        }
    }
    if (reader.has("terrain_layers")) {
        if (!reader.has("terrain")) {
            diagnostics.error(path, "terrain_layers needs a terrain to look like that");
        }
        const Source* layers = find_relative(tree, path, reader.string("terrain_layers", ""), SourceKind::TerrainLayers,
                                             "terrain layers (.terrainlayers)", diagnostics);
        if (layers != nullptr) {
            build.terrain_layers = {.id = layers->id, .type = asset::AssetType::TerrainLayers};
            build.read.push(layers->path);
        }
    }

    if (reader.has("sea")) {
        yyjson_val* sea = yyjson_obj_get(doc->root(), "sea");
        const JsonReader sea_reader{.object = sea, .path = path, .diagnostics = &diagnostics};
        double level_m = 0.0;
        if (!yyjson_is_obj(sea)) {
            diagnostics.error(path, "sea should be an object: {\"level_m\": 0}");
        } else {
            static constexpr const char* kSeaKeys[] = {"level_m", "shore", "foam"};
            sea_reader.check_keys(kSeaKeys, "sea");
            const auto most = static_cast<double>(asset::WorldAsset::kMostSeaLevelM);
            if (required(sea_reader, "level_m", -most, most, level_m)) {
                build.sea = true;
                build.sea_level_m = static_cast<float>(level_m);
            }
            const auto material = [&](const char* key, asset::AssetRef& out) {
                if (sea_reader.has(key)) {
                    const Source* found = find_relative(tree, path, sea_reader.string(key, ""), SourceKind::Material,
                                                        "a material source (.material)", diagnostics);
                    if (found != nullptr) {
                        out = {.id = found->id, .type = asset::AssetType::Material};
                        build.read.push(found->path);
                    }
                }
            };
            material("shore", build.sea_shore);
            material("foam", build.sea_foam);
        }
    }

    yyjson_val* start = yyjson_obj_get(doc->root(), "start");
    const JsonReader start_reader{.object = start, .path = path, .diagnostics = &diagnostics};
    if (!yyjson_is_obj(start) || !read_numbers(yyjson_obj_get(start, "position"), build.start_position, 3) ||
        !std::ranges::all_of(build.start_position, [](double p) { return std::isfinite(p); })) {
        diagnostics.error(path, "start should have a position: [x, y, z]");
    } else {
        static constexpr const char* kStartKeys[] = {"position", "yaw_deg", "pitch_deg"};
        start_reader.check_keys(kStartKeys, "start");
        double yaw_deg = 0.0;
        double pitch_deg = 0.0;
        if (start_reader.has("yaw_deg")) {
            required(start_reader, "yaw_deg", -360.0, 360.0, yaw_deg);
        }
        if (start_reader.has("pitch_deg")) {
            required(start_reader, "pitch_deg", -89.0, 89.0, pitch_deg);
        }
        build.start_yaw_rad = static_cast<float>(yaw_deg * kRadPerDeg);
        build.start_pitch_rad = static_cast<float>(pitch_deg * kRadPerDeg);
    }

    yyjson_val* cells = yyjson_obj_get(doc->root(), "cells");
    if (!yyjson_is_arr(cells)) {
        diagnostics.error(path, "cells should be an array of cell sources");
    }
    size_t i = 0;
    size_t n = 0;
    yyjson_val* item = nullptr;
    yyjson_arr_foreach(cells, i, n, item) {
        const std::string_view reference =
            yyjson_is_str(item) ? std::string_view(yyjson_get_str(item), yyjson_get_len(item)) : std::string_view();
        const Source* cell =
            find_relative(tree, path, reference, SourceKind::Cell, "a cell source (.cell)", diagnostics);
        int32_t coord[2] = {};
        if (cell == nullptr || !cell_coord(*cell, path, read, coord, diagnostics)) {
            continue;
        }
        build.read.push(cell->path);
        build.cells.push({.cell = cell->id, .coord_x = coord[0], .coord_z = coord[1]});
    }
    std::ranges::sort(build.cells.span(), [](const asset::WorldCell& a, const asset::WorldCell& b) {
        return a.coord_z != b.coord_z ? a.coord_z < b.coord_z : a.coord_x < b.coord_x;
    });
    for (size_t c = 1; c < build.cells.size(); ++c) {
        if (build.cells[c].coord_x == build.cells[c - 1].coord_x &&
            build.cells[c].coord_z == build.cells[c - 1].coord_z) {
            diagnostics.error(path, "two cells at [%d, %d]", build.cells[c].coord_x, build.cells[c].coord_z);
        }
    }
    if (diagnostics.error_count() != errors_before) {
        return core::fail(core::ErrorCode::InvalidArgument, "the world has errors");
    }
    return build;
}

core::Array<std::byte> cook_world(const WorldBuild& build, asset::AssetId id) {
    asset::WorldAsset asset;
    std::copy_n(build.start_position, 3, asset.start_position);
    asset.start_yaw_rad = build.start_yaw_rad;
    asset.start_pitch_rad = build.start_pitch_rad;
    asset.sea = build.sea ? 1U : 0U;
    asset.sea_level_m = build.sea_level_m;
    asset.environment = 0;
    asset::AssetRef dependencies[5] = {build.environment};
    uint32_t dependency_count = 1;
    if (build.terrain.id.is_valid()) {
        asset.terrain = dependency_count;
        dependencies[dependency_count++] = build.terrain;
    }
    if (build.terrain_layers.id.is_valid()) {
        asset.terrain_layers = dependency_count;
        dependencies[dependency_count++] = build.terrain_layers;
    }
    if (build.sea_shore.id.is_valid()) {
        asset.sea_shore = dependency_count;
        dependencies[dependency_count++] = build.sea_shore;
    }
    if (build.sea_foam.id.is_valid()) {
        asset.sea_foam = dependency_count;
        dependencies[dependency_count++] = build.sea_foam;
    }
    BlobWriter writer(sizeof(asset::WorldAsset));
    asset.cells = writer.append(build.cells.span());
    return writer.finish(asset, id, std::span(dependencies).first(dependency_count));
}

namespace {

// A rule of four numbers in order, from an array; false (reported) otherwise.
bool read_rule(const JsonReader& reader, const char* key, double least, double most, double (&out)[4]) {
    if (!read_numbers(yyjson_obj_get(reader.object, key), out, 4) ||
        !std::ranges::all_of(out,
                             [&](double value) { return std::isfinite(value) && value >= least && value <= most; }) ||
        !(out[0] <= out[1] && out[1] <= out[2] && out[2] <= out[3])) {
        reader.diagnostics->error(reader.path, "%s should be four numbers in order, from %g to %g", key, least, most);
        return false;
    }
    return true;
}

// Flat ground is the end of the range of slopes, and so is a sheer face: past either, the rule says all of it.
void slope_rule(const double (&slope_deg)[4], float (&out)[4]) {
    const auto cos_deg = [](double degrees) { return static_cast<float>(std::cos(degrees * kRadPerDeg)); };
    out[0] = slope_deg[1] <= 0.0 ? 2.0f : cos_deg(slope_deg[0]);
    out[1] = cos_deg(slope_deg[1]);
    out[2] = slope_deg[2] >= 90.0 ? -1.0f : cos_deg(slope_deg[2]);
    out[3] = slope_deg[2] >= 90.0 ? -2.0f : cos_deg(slope_deg[3]);
}

void read_terrain_grass(std::string_view path, yyjson_val* object, asset::TerrainGrass& grass,
                        Diagnostics& diagnostics) {
    const JsonReader reader{.object = object, .path = path, .diagnostics = &diagnostics};
    if (!yyjson_is_obj(object)) {
        diagnostics.error(path, "grass should be an object");
        return;
    }
    static constexpr const char* kKeys[] = {
        "density_per_m2", "height_m", "width_m", "where_height_m", "where_slope_deg", "not_where",
        "patches",        "base",     "tip",     "other"};
    reader.check_keys(kKeys, "grass");
    double density = 0.0;
    double width = 0.0;
    if (required(reader, "density_per_m2", 0.0, 2048.0, density)) {
        grass.density_per_m2 = static_cast<float>(density);
    }
    if (required(reader, "width_m", 0.0, 2.0, width)) {
        grass.width_m = static_cast<float>(width);
    }
    double height[2] = {};
    if (!read_numbers(yyjson_obj_get(object, "height_m"), height, 2) || !(height[0] >= 0.0) ||
        !(height[0] <= height[1]) || !(height[1] <= 16.0)) {
        diagnostics.error(path, "grass height_m should be its lowest and tallest, from 0 to 16");
    } else {
        grass.height_m[0] = static_cast<float>(height[0]);
        grass.height_m[1] = static_cast<float>(height[1]);
    }
    double where_height[4] = {-1e6, -1e6, 1e6, 1e6};
    if (reader.has("where_height_m")) {
        read_rule(reader, "where_height_m", -1e6, 1e6, where_height);
    }
    for (int i = 0; i < 4; ++i) {
        grass.where_height_m[i] = static_cast<float>(where_height[i]);
    }
    double where_slope[4] = {0.0, 0.0, 90.0, 90.0};
    if (reader.has("where_slope_deg")) {
        read_rule(reader, "where_slope_deg", 0.0, 90.0, where_slope);
    }
    slope_rule(where_slope, grass.where_slope_cos);
    for (const char channel : reader.string("not_where", "")) {
        const size_t index = std::string_view("rgba").find(channel);
        if (index == std::string_view::npos) {
            diagnostics.error(path, "grass not_where should be channels of the masks: r, g, b, a");
        } else {
            grass.excluded_masks |= 1U << index;
        }
    }
    double patches = 0.0;
    if (reader.has("patches") && required(reader, "patches", 0.0, 1.0, patches)) {
        grass.patches = static_cast<float>(patches);
    }
    const auto color = [&](yyjson_val* from, const char* key, float (&out)[3]) {
        double rgb[3] = {};
        if (!read_numbers(yyjson_obj_get(from, key), rgb, 3) ||
            !std::ranges::all_of(rgb, [](double c) { return c >= 0.0 && c <= 16.0; })) {
            diagnostics.error(path, "grass %s should be [r, g, b], linear, from 0 to 16", key);
        } else {
            std::ranges::transform(rgb, out, [](double c) { return static_cast<float>(c); });
        }
    };
    color(object, "base", grass.color_base);
    color(object, "tip", grass.color_tip);
    if (reader.has("other")) {
        yyjson_val* other = yyjson_obj_get(object, "other");
        const JsonReader other_reader{.object = other, .path = path, .diagnostics = &diagnostics};
        const std::string_view mask = other_reader.string("mask", "");
        const size_t channel = std::string_view("rgba").find(mask);
        if (mask.size() != 1 || channel == std::string_view::npos) {
            diagnostics.error(path, "grass other's mask should be one of r, g, b, a");
        } else {
            grass.other_mask = static_cast<uint32_t>(channel);
        }
        color(other, "base", grass.other_base);
        color(other, "tip", grass.other_tip);
    }
}

// Where a material is among the dependencies, adding it the first time.
uint32_t dependency_of(TerrainLayersBuild& build, const Source& material) {
    const asset::AssetRef ref{.id = material.id, .type = asset::AssetType::Material};
    for (uint32_t i = 0; i < build.dependencies.size(); ++i) {
        if (build.dependencies[i] == ref) {
            return i;
        }
    }
    build.dependencies.push(ref);
    build.read.push(material.path);
    return static_cast<uint32_t>(build.dependencies.size() - 1);
}

void read_terrain_layer(const SourceTree& tree, std::string_view path, yyjson_val* object, TerrainLayersBuild& build,
                        Diagnostics& diagnostics) {
    const JsonReader reader{.object = object, .path = path, .diagnostics = &diagnostics};
    if (!yyjson_is_obj(object)) {
        diagnostics.error(path, "a layer should be an object");
        return;
    }
    static constexpr const char* kKeys[] = {"material",    "tile_m",    "height_m",     "slope_deg",       "mask",
                                            "mask_range",  "detail",    "detail_range", "detail_coverage", "noise",
                                            "noise_large", "variation", "tint",         "projection"};
    reader.check_keys(kKeys, "terrain layer");
    asset::TerrainLayer layer;

    const Source* material = find_relative(tree, path, reader.string("material", ""), SourceKind::Material,
                                           "a material source (.material)", diagnostics);
    if (material != nullptr) {
        layer.material = dependency_of(build, *material);
    }
    double tile_m = 0.0;
    if (required(reader, "tile_m", 1.0 / 16.0, 4096.0, tile_m)) {
        layer.tile_m = static_cast<float>(tile_m);
    }

    double height_m[4] = {-1e6, -1e6, 1e6, 1e6};
    if (reader.has("height_m")) {
        read_rule(reader, "height_m", -1e6, 1e6, height_m);
    }
    double slope_deg[4] = {0.0, 0.0, 90.0, 90.0};
    if (reader.has("slope_deg")) {
        read_rule(reader, "slope_deg", 0.0, 90.0, slope_deg);
    }
    for (int i = 0; i < 4; ++i) {
        layer.height_m[i] = static_cast<float>(height_m[i]);
    }
    slope_rule(slope_deg, layer.slope_cos);

    // A channel of the masks or the details, and what it has to be.
    const auto channel_rule = [&](const char* key, const char* range_key, uint32_t& channel, float& from, float& to) {
        if (reader.has(key)) {
            const std::string_view name = reader.string(key, "");
            const size_t index = std::string_view("rgba").find(name);
            if (name.size() != 1 || index == std::string_view::npos) {
                diagnostics.error(path, "%s should be one of r, g, b, a", key);
            } else {
                channel = static_cast<uint32_t>(index);
            }
        }
        if (reader.has(range_key)) {
            double range[2] = {};
            if (!read_numbers(yyjson_obj_get(object, range_key), range, 2) ||
                !std::ranges::all_of(range, [](double v) { return v >= 0.0 && v <= 1.0; }) ||
                !(std::abs(range[1] - range[0]) >= 1e-3)) {
                diagnostics.error(
                    path, "%s should be two different numbers from 0 to 1: rising, or falling for where it's low",
                    range_key);
            } else {
                from = static_cast<float>(range[0]);
                to = static_cast<float>(range[1]);
            }
        }
    };
    channel_rule("mask", "mask_range", layer.mask, layer.mask_from, layer.mask_to);
    channel_rule("detail", "detail_range", layer.detail, layer.detail_from, layer.detail_to);
    if (reader.boolean("detail_coverage", false)) {
        if (layer.detail == asset::kTerrainNoMask) {
            diagnostics.error(path, "detail_coverage needs a detail channel");
        }
        layer.flags |= asset::TerrainLayerFlags::DetailCoverage;
    }
    const auto optional = [&](const char* key, double most, float& out) {
        double value = 0.0;
        if (reader.has(key) && required(reader, key, 0.0, most, value)) {
            out = static_cast<float>(value);
        }
    };
    optional("noise", 16.0, layer.noise);
    optional("noise_large", 1.0, layer.noise_large);
    optional("variation", 1.0, layer.variation);
    if (reader.has("tint")) {
        double tint[3] = {};
        if (!read_numbers(yyjson_obj_get(object, "tint"), tint, 3) ||
            !std::ranges::all_of(tint, [](double c) { return c >= 0.0 && c <= 16.0; })) {
            diagnostics.error(path, "tint should be [r, g, b], linear, from 0 to 16");
        } else {
            std::ranges::transform(tint, layer.tint, [](double c) { return static_cast<float>(c); });
        }
    }
    if (reader.has("projection")) {
        const std::string_view projection = reader.string("projection", "");
        if (projection == "sides") {
            layer.projection = asset::TerrainProjection::Sides;
        } else if (projection != "top") {
            diagnostics.error(path, "projection should be top or sides");
        }
    }
    build.layers.push(layer);
}

} // namespace

core::Result<TerrainLayersBuild> import_terrain_layers(const SourceTree& tree, const Source& source,
                                                       std::span<const std::byte> bytes, Diagnostics& diagnostics) {
    const std::string_view path = source.path.view();
    const size_t errors_before = diagnostics.error_count();
    const core::Result<JsonDoc> doc = JsonDoc::parse(bytes, path, diagnostics);
    if (!doc) {
        return core::fail(doc.error());
    }
    TerrainLayersBuild build;
    const JsonReader reader{.object = doc->root(), .path = path, .diagnostics = &diagnostics};
    if (!yyjson_is_obj(doc->root())) {
        diagnostics.error(path, "should be a JSON object");
        return core::fail(core::ErrorCode::InvalidArgument, "the terrain layers have errors");
    }
    static constexpr const char* kKeys[] = {"masks",  "details",   "lighting",           "normals",
                                            "colors", "color_map", "color_map_strength", "blend",
                                            "layers", "grass"};
    reader.check_keys(kKeys, "terrain layers");
    const auto map = [&](const char* key, uint32_t& out) {
        if (reader.has(key)) {
            const Source* material = find_relative(tree, path, reader.string(key, ""), SourceKind::Material,
                                                   "a material source (.material)", diagnostics);
            if (material != nullptr) {
                out = dependency_of(build, *material);
            }
        }
    };
    map("masks", build.masks);
    map("details", build.details);
    map("lighting", build.lighting);
    map("normals", build.normals);
    map("colors", build.colors);
    map("color_map", build.color_map);
    double strength = build.color_map_strength;
    if (reader.has("color_map_strength") && required(reader, "color_map_strength", 0.0, 1.0, strength)) {
        build.color_map_strength = static_cast<float>(strength);
    }
    double blend = build.blend;
    if (reader.has("blend") && required(reader, "blend", 0.0, 1.0, blend)) {
        build.blend = static_cast<float>(blend);
    }
    yyjson_val* layers = yyjson_obj_get(doc->root(), "layers");
    if (!yyjson_is_arr(layers) || yyjson_arr_size(layers) == 0 || yyjson_arr_size(layers) > asset::kTerrainMaxLayers) {
        diagnostics.error(path, "layers should be an array of 1 to %u layers", asset::kTerrainMaxLayers);
    } else {
        size_t i = 0;
        size_t n = 0;
        yyjson_val* layer = nullptr;
        yyjson_arr_foreach(layers, i, n, layer) {
            read_terrain_layer(tree, path, layer, build, diagnostics);
        }
    }
    if (reader.has("grass")) {
        read_terrain_grass(path, yyjson_obj_get(doc->root(), "grass"), build.grass, diagnostics);
    }
    if (diagnostics.error_count() != errors_before) {
        return core::fail(core::ErrorCode::InvalidArgument, "the terrain layers have errors");
    }
    return build;
}

core::Array<std::byte> cook_terrain_layers(const TerrainLayersBuild& build, asset::AssetId id) {
    asset::TerrainLayersAsset asset;
    asset.masks = build.masks;
    asset.details = build.details;
    asset.lighting = build.lighting;
    asset.normals = build.normals;
    asset.colors = build.colors;
    asset.color_map = build.color_map;
    asset.color_map_strength = build.color_map_strength;
    asset.blend = build.blend;
    asset.grass = build.grass;
    BlobWriter writer(sizeof(asset::TerrainLayersAsset));
    asset.layers = writer.append(build.layers.span());
    return writer.finish(asset, id, build.dependencies.span());
}

core::Result<asset::EnvironmentAsset> import_environment(const Source& source, std::span<const std::byte> bytes,
                                                         Diagnostics& diagnostics) {
    const std::string_view path = source.path.view();
    const size_t errors_before = diagnostics.error_count();
    const core::Result<JsonDoc> doc = JsonDoc::parse(bytes, path, diagnostics);
    if (!doc) {
        return core::fail(doc.error());
    }
    if (!yyjson_is_obj(doc->root())) {
        diagnostics.error(path, "should be a JSON object");
        return core::fail(core::ErrorCode::InvalidArgument, "the environment has errors");
    }
    const JsonReader reader{.object = doc->root(), .path = path, .diagnostics = &diagnostics};
    static constexpr const char* kKeys[] = {"sun",     "sky", "ground", "shadow_color", "exposure_ev",
                                            "shadows", "fog", "wind",   "sea"};
    reader.check_keys(kKeys, "environment");
    asset::EnvironmentAsset environment;

    yyjson_val* sun = yyjson_obj_get(doc->root(), "sun");
    const JsonReader sun_reader{.object = sun, .path = path, .diagnostics = &diagnostics};
    double elevation_deg = 0.0;
    double azimuth_deg = 0.0;
    if (!yyjson_is_obj(sun)) {
        diagnostics.error(path, "sun should be an object: elevation_deg, azimuth_deg, color, intensity");
    } else if (required(sun_reader, "elevation_deg", -90.0, 90.0, elevation_deg) &&
               required(sun_reader, "azimuth_deg", -360.0, 360.0, azimuth_deg)) {
        // Toward the sun: up by its elevation, across from north (-Z) toward east (+X) by its azimuth.
        const double elevation = elevation_deg * kRadPerDeg;
        const double azimuth = azimuth_deg * kRadPerDeg;
        environment.sun_direction[0] = static_cast<float>(std::cos(elevation) * std::sin(azimuth));
        environment.sun_direction[1] = static_cast<float>(std::sin(elevation));
        environment.sun_direction[2] = static_cast<float>(-std::cos(elevation) * std::cos(azimuth));
    }
    read_light(reader, "sun", environment.sun_color);
    read_light(reader, "sky", environment.sky_color);
    read_light(reader, "ground", environment.ground_color);
    read_light(reader, "shadow_color", environment.shadow_color);
    double exposure_ev = 0.0;
    required(reader, "exposure_ev", -20.0, 20.0, exposure_ev);
    environment.exposure_ev = static_cast<float>(exposure_ev);

    yyjson_val* shadows = yyjson_obj_get(doc->root(), "shadows");
    if (!yyjson_is_obj(shadows)) {
        diagnostics.error(path, "shadows should be an object");
    } else {
        const JsonReader shadow_reader{.object = shadows, .path = path, .diagnostics = &diagnostics};
        static constexpr const char* kShadowKeys[] = {"resolution",         "distance_m",     "split_lambda",
                                                      "blend_band",         "caster_reach_m", "penumbra_per_m",
                                                      "max_penumbra_texels"};
        shadow_reader.check_keys(kShadowKeys, "shadows");
        double values[7] = {};
        using L = asset::ShadowLimits;
        const double limits[7][2] = {{L::kMinResolution, L::kMaxResolution},
                                     {L::kMinDistanceM, L::kMaxDistanceM},
                                     {0, 1},
                                     {0, L::kMaxBlendBand},
                                     {0, L::kMaxCasterReachM},
                                     {0, L::kMaxPenumbraPerM},
                                     {L::kMinPenumbraTexels, L::kMaxPenumbraTexels}};
        for (size_t k = 0; k < 7; ++k) {
            required(shadow_reader, kShadowKeys[k], limits[k][0], limits[k][1], values[k]);
        }
        if (values[0] != std::floor(values[0])) {
            diagnostics.error(path, "resolution should be a whole number");
        }
        environment.shadow_resolution = static_cast<uint32_t>(values[0]);
        environment.shadow_distance_m = static_cast<float>(values[1]);
        environment.shadow_split_lambda = static_cast<float>(values[2]);
        environment.shadow_blend_band = static_cast<float>(values[3]);
        environment.shadow_caster_reach_m = static_cast<float>(values[4]);
        environment.shadow_penumbra_per_m = static_cast<float>(values[5]);
        environment.shadow_max_penumbra_texels = static_cast<float>(values[6]);
    }
    read_fog(reader, environment);
    // Optional: still air without it.
    if (yyjson_val* wind = yyjson_obj_get(doc->root(), "wind"); wind != nullptr) {
        const JsonReader wind_reader{.object = wind, .path = path, .diagnostics = &diagnostics};
        static constexpr const char* kWindKeys[] = {"toward_deg", "speed_mps", "gust_mps", "gust_size_m", "turbulence"};
        if (!yyjson_is_obj(wind)) {
            diagnostics.error(path,
                              "wind should be an object: toward_deg, speed_mps, gust_mps, gust_size_m, turbulence");
        } else {
            wind_reader.check_keys(kWindKeys, "wind");
            using W = asset::WindLimits;
            double toward_deg = 0.0;
            double speed = 0.0;
            double gust = 0.0;
            double gust_size = environment.wind_gust_size_m;
            double turbulence = 0.0;
            if (required(wind_reader, "toward_deg", -360.0, 360.0, toward_deg)) {
                // Across from north (-Z) toward east (+X), as the sun's azimuth.
                environment.wind_direction[0] = static_cast<float>(std::sin(toward_deg * kRadPerDeg));
                environment.wind_direction[1] = static_cast<float>(-std::cos(toward_deg * kRadPerDeg));
            }
            required(wind_reader, "speed_mps", 0.0, W::kMaxSpeedMps, speed);
            if (wind_reader.has("gust_mps")) {
                required(wind_reader, "gust_mps", 0.0, W::kMaxSpeedMps, gust);
            }
            if (wind_reader.has("gust_size_m")) {
                required(wind_reader, "gust_size_m", W::kMinGustSizeM, W::kMaxGustSizeM, gust_size);
            }
            if (wind_reader.has("turbulence")) {
                required(wind_reader, "turbulence", 0.0, 1.0, turbulence);
            }
            environment.wind_speed_mps = static_cast<float>(speed);
            environment.wind_gust_mps = static_cast<float>(gust);
            environment.wind_gust_size_m = static_cast<float>(gust_size);
            environment.wind_turbulence = static_cast<float>(turbulence);
        }
    }
    if (yyjson_val* sea = yyjson_obj_get(doc->root(), "sea"); sea != nullptr) {
        const JsonReader sea_reader{.object = sea, .path = path, .diagnostics = &diagnostics};
        static constexpr const char* kSeaKeys[] = {"swell_height_m", "swell_period_s", "swell_from_deg", "fetch_km",
                                                   "choppiness"};
        if (!yyjson_is_obj(sea)) {
            diagnostics.error(path,
                              "sea should be an object: swell_height_m, swell_period_s, swell_from_deg, fetch_km, "
                              "choppiness");
        } else {
            sea_reader.check_keys(kSeaKeys, "sea");
            using S = asset::SeaLimits;
            double height = environment.sea_swell_height_m;
            double period = environment.sea_swell_period_s;
            double from_deg = 180.0;
            double fetch_km = environment.sea_fetch_m / 1000.0;
            double choppiness = environment.sea_choppiness;
            const auto optional = [&](const char* key, double least, double most, double& out) {
                if (sea_reader.has(key)) {
                    required(sea_reader, key, least, most, out);
                }
            };
            optional("swell_height_m", 0.0, S::kMaxSwellHeightM, height);
            optional("swell_period_s", S::kMinSwellPeriodS, S::kMaxSwellPeriodS, period);
            optional("swell_from_deg", -360.0, 360.0, from_deg);
            optional("fetch_km", S::kMinFetchM / 1000.0, S::kMaxFetchM / 1000.0, fetch_km);
            optional("choppiness", 0.0, S::kMaxChoppiness, choppiness);
            // It travels away from where it comes from: across from north (-Z) toward east (+X), half a turn on.
            const double toward = (from_deg + 180.0) * kRadPerDeg;
            environment.sea_swell_direction[0] = static_cast<float>(std::sin(toward));
            environment.sea_swell_direction[1] = static_cast<float>(-std::cos(toward));
            environment.sea_swell_height_m = static_cast<float>(height);
            environment.sea_swell_period_s = static_cast<float>(period);
            environment.sea_fetch_m = static_cast<float>(fetch_km * 1000.0);
            environment.sea_choppiness = static_cast<float>(choppiness);
        }
    }
    if (diagnostics.error_count() != errors_before) {
        return core::fail(core::ErrorCode::InvalidArgument, "the environment has errors");
    }
    return environment;
}

core::Array<std::byte> cook_environment(const asset::EnvironmentAsset& environment, asset::AssetId id) {
    BlobWriter writer(sizeof(asset::EnvironmentAsset));
    return writer.finish(environment, id, {});
}

} // namespace de::cooker
