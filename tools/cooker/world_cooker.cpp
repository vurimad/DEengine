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
    static constexpr const char* kKeys[] = {"environment", "start", "cells"};
    reader.check_keys(kKeys, "world");

    const Source* environment = find_relative(tree, path, reader.string("environment", ""), SourceKind::Environment,
                                              "an environment source (.environment)", diagnostics);
    if (environment != nullptr) {
        build.environment = {.id = environment->id, .type = asset::AssetType::Environment};
        build.read.push(environment->path);
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
    asset.environment = 0;
    BlobWriter writer(sizeof(asset::WorldAsset));
    asset.cells = writer.append(build.cells.span());
    return writer.finish(asset, id, std::span(&build.environment, 1));
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
    static constexpr const char* kKeys[] = {"sun", "sky", "ground", "shadow_color", "exposure_ev", "shadows"};
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
