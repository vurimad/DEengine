#include "asset/cell_format.h"
#include "asset/collision_format.h"
#include "asset/prefab_format.h"
#include "cooker/prefab_cooker.h"
#include "support/cook_kit.h"
#include "support/gltf_source.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>
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
using de::test::run_cook;
using de::test::Sandbox;
using de::test::write_src;

namespace {

constexpr std::string_view kFlame = R"({"emitters": [{"name": "flame", "rate_per_s": 30, "lifetime_s": 0.5}]})";

// A candle: its wax, solid, and a flame a fifth of a meter up. Unlit, it has no flame.
constexpr std::string_view kCandle = R"({
  "parts": {
    "wax":   {"mesh": "wax.gltf", "collision": "mesh"},
    "flame": {"effect": "../fire/flame.vfx", "position": [0, 0.2, 0]}
  },
  "variants": {
    "unlit": {"flame": null}
  }
})";

// A table with two candles on it: one turned a quarter and twice the size, one unlit.
constexpr std::string_view kTable = R"({
  "parts": {
    "top":      {"mesh": "top.gltf"},
    "candle_a": {"prefab": "../candle/candle.prefab", "position": [0.4, 0.9, 0], "rotation_deg": [0, 90, 0], "scale": 2},
    "candle_b": {"prefab": "../candle/candle.prefab", "variant": "unlit", "position": [-0.4, 0.9, 0]}
  },
  "variants": {
    "bare": {"candle_a": null, "candle_b/wax": null}
  }
})";

void write_model(const Sandbox& sandbox, std::string_view path) {
    std::vector<de::test::Vertex> vertices;
    std::vector<uint32_t> indices;
    de::test::quad(vertices, indices);
    write_src(sandbox, path, de::test::gltf(vertices, indices, 0.0f, "[{}]", "[]", "[]"));
}

void write_props(const Sandbox& sandbox) {
    write_model(sandbox, "content/props/candle/wax.gltf");
    write_model(sandbox, "content/props/table/top.gltf");
    write_src(sandbox, "content/props/fire/flame.vfx", kFlame);
    write_src(sandbox, "content/props/candle/candle.prefab", kCandle);
    write_src(sandbox, "content/props/table/table.prefab", kTable);
}

AssetId id_of(const Vfs& vfs, const char* meta) {
    return asset_id(guid_of(vfs, meta));
}

} // namespace

TEST_CASE("a prefab cooks into one flat list of parts, the prefabs it nests placed among its own") {
    const Sandbox sandbox;
    write_props(sandbox);
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    CHECK(report.stats.failed == 0);
    const AssetId wax = id_of(vfs, "src:/meta/props/candle/wax.gltf.json");
    const AssetId top = id_of(vfs, "src:/meta/props/table/top.gltf.json");
    const AssetId flame = id_of(vfs, "src:/meta/props/fire/flame.vfx.json");
    {
        Cooked file(vfs, id_of(vfs, "src:/meta/props/candle/candle.prefab.json"), AssetType::Prefab);
        const auto& candle = file.load<PrefabAsset>();
        const std::span<const PrefabPart> parts = candle.parts.get(file.data());
        const std::span<const AssetRef> dependencies = candle.header.dependencies.get(file.data());
        REQUIRE(parts.size() == 2);
        REQUIRE(dependencies.size() == 3); // the wax's mesh and its collision, the flame
        CHECK((candle.name(file.data(), parts[0]) == "wax"));
        CHECK((dependencies[parts[0].mesh] == AssetRef{.id = wax, .type = AssetType::Mesh}));
        CHECK((dependencies[parts[0].collision] ==
               AssetRef{.id = collision_asset_id(wax), .type = AssetType::Collision}));
        CHECK(parts[0].effect == PrefabPart::kNone);
        CHECK(parts[0].absent_in == 0);
        CHECK((candle.name(file.data(), parts[1]) == "flame"));
        CHECK(parts[1].mesh == PrefabPart::kNone);
        CHECK((dependencies[parts[1].effect] == AssetRef{.id = flame, .type = AssetType::Effect}));
        CHECK(parts[1].position[1] == doctest::Approx(0.2f));
        CHECK(parts[1].id == prefab_part_id("flame"));
        CHECK(parts[0].id != parts[1].id);
        // Its one variant is the first bit, and leaves out the flame.
        const std::span<const PrefabVariant> variants = candle.variants.get(file.data());
        REQUIRE(variants.size() == 1);
        CHECK((candle.name(file.data(), variants[0]) == "unlit"));
        CHECK(variants[0].name_hash == prefab_variant_hash("unlit"));
        CHECK(parts[1].absent_in == 1U);
        CHECK(candle.variant_bit(file.data(), prefab_variant_hash("unlit")) == 1U);
        CHECK(candle.variant_bit(file.data(), 0) == 0);                          // no variant asked for
        CHECK(candle.variant_bit(file.data(), prefab_variant_hash("lit")) == 0); // one it doesn't have
        CHECK(candle.reach_m == doctest::Approx(0.2f));
    }
    {
        Cooked file(vfs, id_of(vfs, "src:/meta/props/table/table.prefab.json"), AssetType::Prefab);
        const auto& table = file.load<PrefabAsset>();
        const std::span<const PrefabPart> parts = table.parts.get(file.data());
        const std::span<const AssetRef> dependencies = table.header.dependencies.get(file.data());
        // Its top, then the first candle whole, then the second without the flame its variant leaves out. No part is
        // a prefab: what was nested is flat.
        REQUIRE(parts.size() == 4);
        CHECK((table.name(file.data(), parts[0]) == "top"));
        CHECK((table.name(file.data(), parts[1]) == "candle_a/wax"));
        CHECK((table.name(file.data(), parts[2]) == "candle_a/flame"));
        CHECK((table.name(file.data(), parts[3]) == "candle_b/wax"));
        CHECK(parts[2].id == prefab_part_id("candle_a/flame"));
        REQUIRE(dependencies.size() == 4); // two meshes, the wax's collision, the flame: each once
        CHECK((dependencies[parts[0].mesh] == AssetRef{.id = top, .type = AssetType::Mesh}));
        CHECK((dependencies[parts[1].mesh] == AssetRef{.id = wax, .type = AssetType::Mesh}));
        CHECK((dependencies[parts[3].mesh] == AssetRef{.id = wax, .type = AssetType::Mesh}));
        CHECK(parts[1].collision == parts[3].collision);
        // Placed from the table's origin: the candle's place, turn and size carried into its parts. Its flame is
        // 0.2 up in a candle twice the size: 0.4 above where the candle stands.
        CHECK(parts[1].position[0] == doctest::Approx(0.4f));
        CHECK(parts[1].position[1] == doctest::Approx(0.9f));
        CHECK(parts[2].position[0] == doctest::Approx(0.4f));
        CHECK(parts[2].position[1] == doctest::Approx(1.3f));
        CHECK(parts[2].scale[0] == doctest::Approx(2.0f));
        CHECK(parts[2].rotation[1] == doctest::Approx(std::sqrt(0.5f))); // a quarter turn about y
        CHECK(parts[2].rotation[3] == doctest::Approx(std::sqrt(0.5f)));
        CHECK(parts[3].position[0] == doctest::Approx(-0.4f));
        CHECK(parts[3].scale[0] == doctest::Approx(1.0f));
        CHECK(table.reach_m == doctest::Approx(std::hypot(0.4f, 1.3f)));
        // Bare: no first candle (both its parts), and not the second's wax. The top stays.
        REQUIRE(table.variants.count == 1);
        CHECK(parts[0].absent_in == 0);
        CHECK(parts[1].absent_in == 1U);
        CHECK(parts[2].absent_in == 1U);
        CHECK(parts[3].absent_in == 1U);
    }
    // Nothing changed: nothing cooks.
    CHECK(run_cook(vfs).stats.cooked == 0);
}

TEST_CASE("a cell places a prefab in one entity, and cooks again only when the prefab's variants change") {
    const Sandbox sandbox;
    write_props(sandbox);
    write_src(sandbox, "content/world/hall/cell_0_0.cell", R"({
  "coord": [0, 0],
  "entities": [
    {"name": "table", "prefab": "../../props/table/table.prefab", "position": [4, 0, 4]},
    {"name": "candle_1", "prefab": "../../props/candle/candle.prefab", "position": [1, 1, 1], "scale": 0.8},
    {"name": "candle_2", "prefab": "../../props/candle/candle.prefab", "variant": "unlit", "position": [2, 1, 1]}
  ]
})");
    write_src(sandbox, "content/world/hall/cell_1_0.cell", R"({
  "coord": [1, 0],
  "entities": [
    {"name": "both", "prefab": "../../props/candle/candle.prefab", "mesh": "../../props/candle/wax.gltf"},
    {"name": "nothing", "prefab": "../../props/candle/lamp.prefab"},
    {"name": "no_prefab", "variant": "unlit"},
    {"name": "no_variant", "prefab": "../../props/candle/candle.prefab", "variant": "dripping"}
  ]
})");
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    const auto said = [&](std::string_view text) {
        return has_message(report, "content/world/hall/cell_1_0.cell", text);
    };
    CHECK(said("both: places a prefab: a mesh, an effect, a light, fog or water belong to entities of their own"));
    CHECK(said("nothing: prefab '../../props/candle/lamp.prefab' should name a prefab source (.prefab)"));
    CHECK(said("no_prefab: names a variant, which only an entity that places a prefab has"));
    CHECK(report.stats.failed == 1);

    const AssetId candle = id_of(vfs, "src:/meta/props/candle/candle.prefab.json");
    const AssetId table = id_of(vfs, "src:/meta/props/table/table.prefab.json");
    {
        Cooked file(vfs, id_of(vfs, "src:/meta/world/hall/cell_0_0.cell.json"), AssetType::Cell);
        const auto& cell = file.load<CellAsset>();
        const std::span<const AssetRef> dependencies = cell.header.dependencies.get(file.data());
        const std::span<const CellEntity> entities = cell.entities.get(file.data());
        // The prefabs alone: what's in them is theirs to depend on.
        REQUIRE(dependencies.size() == 2);
        REQUIRE(entities.size() == 3);
        CHECK((dependencies[entities[0].prefab] == AssetRef{.id = table, .type = AssetType::Prefab}));
        CHECK((dependencies[entities[1].prefab] == AssetRef{.id = candle, .type = AssetType::Prefab}));
        CHECK(entities[1].prefab == entities[2].prefab);
        CHECK(entities[1].variant == 0);
        CHECK(entities[2].variant == prefab_variant_hash("unlit"));
        CHECK(entities[1].mesh == CellEntity::kNone);
        CHECK(entities[1].scale[0] == doctest::Approx(0.8f));
    }

    // The flame moved in the candle: the candle cooks, and the table that holds two of them. No cell does: a cell
    // says where a prefab is, not what's in it.
    std::string moved(kCandle);
    const std::string_view at = R"("position": [0, 0.2, 0])";
    moved.replace(moved.find(at), at.size(), R"("position": [0, 0.25, 0])");
    write_src(sandbox, "content/props/candle/candle.prefab", moved);
    const CookReport after_move = run_cook(vfs);
    CHECK(after_move.stats.cooked == 2);

    // Its variant renamed: the cell that asks for the old one cooks again, and says so; so does the table.
    std::string renamed = moved;
    const std::string_view old_name = R"("unlit": {"flame": null})";
    renamed.replace(renamed.find(old_name), old_name.size(), R"("dark": {"flame": null})");
    write_src(sandbox, "content/props/candle/candle.prefab", renamed);
    const CookReport after_rename = run_cook(vfs);
    CHECK(has_message(after_rename, "content/world/hall/cell_0_0.cell",
                      "candle_2: content/props/candle/candle.prefab has no variant 'unlit'"));
    CHECK(has_message(after_rename, "content/props/table/table.prefab",
                      "content/props/candle/candle.prefab has no variant 'unlit'"));
}

TEST_CASE("a prefab's mistakes are each reported with the part they're in") {
    const Sandbox sandbox;
    write_props(sandbox);
    write_src(sandbox, "content/bad/broken.prefab", R"({
  "parts": {
    "Wax":      {"mesh": "../props/candle/wax.gltf"},
    "empty":    {"position": [0, 1, 0]},
    "both":     {"prefab": "../props/candle/candle.prefab", "mesh": "../props/candle/wax.gltf"},
    "no_model": {"mesh": "../props/fire/flame.vfx"},
    "no_shape": {"effect": "../props/fire/flame.vfx", "collision": "mesh"},
    "variant":  {"effect": "../props/fire/flame.vfx", "variant": "unlit"},
    "squashed": {"effect": "../props/fire/flame.vfx", "scale": [1, 0, 1]},
    "typo":     {"effect": "../props/fire/flame.vfx", "postion": [0, 1, 0]},
    "candle":   {"prefab": "../props/candle/candle.prefab", "variant": "dripping"},
    "list":     [1, 2]
  },
  "variants": {
    "one": {"wick": null},
    "two": {"typo": {"effect": "x.vfx"}}
  }
})");
    // Each inside the other.
    write_src(sandbox, "content/bad/ring_a.prefab", R"({"parts": {"b": {"prefab": "ring_b.prefab"}}})");
    write_src(sandbox, "content/bad/ring_b.prefab", R"({"parts": {"a": {"prefab": "ring_a.prefab"}}})");
    write_src(sandbox, "content/bad/none.prefab", R"({"parts": {}})");
    write_src(sandbox, "content/bad/list.prefab", R"({"parts": []})");
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    const auto said = [&](std::string_view text) { return has_message(report, "content/bad/broken.prefab", text); };
    CHECK(said("part 'Wax' should have an ID of a-z, 0-9 and _"));
    CHECK(said("part 'empty': has nothing in it"));
    CHECK(said("part 'both': is a prefab: a mesh, collision, an effect and a light belong to parts of their own"));
    CHECK(said("no_model: mesh '../props/fire/flame.vfx' should name a model source"));
    CHECK(said("part 'no_shape': collision needs a mesh"));
    CHECK(said("part 'variant': names a variant, which only a part that's a prefab has"));
    CHECK(said("part 'squashed': scale has a zero"));
    CHECK(said("postion")); // not a key of a part
    CHECK(said("content/props/candle/candle.prefab has no variant 'dripping'"));
    CHECK(said("part 'list': should be an object"));
    CHECK(said("variant 'one' leaves out 'wick', which isn't a part of the prefab"));
    CHECK(said("variant 'two': 'typo' should be null"));
    CHECK(has_message(report, "content/bad/ring_a.prefab", "a prefab can't be inside itself"));
    CHECK(has_message(report, "content/bad/ring_b.prefab", "a prefab can't be inside itself"));
    CHECK(has_message(report, "content/bad/none.prefab", "parts should hold at least one part"));
    CHECK(has_message(report, "content/bad/list.prefab", "parts should be an object"));
    CHECK(report.stats.failed == 5);
}

TEST_CASE("a prefab's part may hold a light, alone or with a model or an effect; a cell's entity too") {
    const Sandbox sandbox;
    write_props(sandbox);
    // A lantern: its flame and the light it gives in one part, a spot beside it, and the candle nested with a light
    // of its own.
    write_src(sandbox, "content/props/candle/lit.prefab", R"({
  "parts": {
    "wax":   {"mesh": "wax.gltf"},
    "glow":  {"light": {"color": [1, 0.6, 0.3], "intensity": 0.8, "radius_m": 3, "flicker": 0.3, "flicker_hz": 9, "shadow": 0.5},
              "position": [0, 0.22, 0]}
  }
})");
    write_src(sandbox, "content/props/lantern.prefab", R"({
  "parts": {
    "flame":  {"effect": "fire/flame.vfx", "light": {"intensity": 2, "radius_m": 6}},
    "beam":   {"light": {"radius_m": 12, "cone_deg": 25, "cone_soft": 0.5}, "rotation_deg": [-90, 0, 0]},
    "candle": {"prefab": "candle/lit.prefab", "position": [1, 0, 0], "scale": 2}
  },
  "variants": {"dark": {"flame": null, "candle/glow": null}}
})");
    write_src(sandbox, "content/world/lit.cell", R"({
  "coord": [0, 0],
  "entities": [
    {"name": "lamp", "position": [10, 2, 10], "light": {"color": [0.6, 0.7, 1], "intensity": 4, "radius_m": 9}},
    {"name": "plain", "position": [20, 0, 20], "light": {}}
  ]
})");
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    REQUIRE(report.ok());
    Cooked lantern_file(vfs, asset_id(guid_of(vfs, "src:/meta/props/lantern.prefab.json")), AssetType::Prefab);
    const auto& lantern = lantern_file.load<PrefabAsset>();
    const std::span<const PrefabPart> parts = lantern.parts.get(&lantern);
    const std::span<const LightDesc> lights = lantern.lights.get(&lantern);
    REQUIRE(parts.size() == 4); // flame, beam, and the candle's two
    const auto part_named = [&](std::string_view name) -> const PrefabPart& {
        for (const PrefabPart& part : parts) {
            if (lantern.name(&lantern, part) == name) {
                return part;
            }
        }
        FAIL("a part of that name is missing");
        return parts[0];
    };
    // A light with an effect in one part: what's given is read, the rest is a light's own.
    const PrefabPart& flame = part_named("flame");
    CHECK(flame.effect != PrefabPart::kNone);
    REQUIRE(flame.light < lights.size());
    CHECK(lights[flame.light].intensity == 2.0f);
    CHECK(lights[flame.light].radius_m == 6.0f);
    CHECK(lights[flame.light].color[0] == 1.0f);
    CHECK(lights[flame.light].cone_deg == 0.0f);
    // A part that's only a light: a spot.
    const PrefabPart& beam = part_named("beam");
    CHECK(beam.mesh == PrefabPart::kNone);
    CHECK(beam.effect == PrefabPart::kNone);
    REQUIRE(beam.light < lights.size());
    CHECK(lights[beam.light].cone_deg == 25.0f);
    CHECK(lights[beam.light].cone_soft == 0.5f);
    // A nested prefab's light is among its own, placed with what nests it, and a variant leaves it out by its path.
    const PrefabPart& glow = part_named("candle/glow");
    REQUIRE(glow.light < lights.size());
    CHECK(lights[glow.light].flicker == doctest::Approx(0.3f));
    CHECK(lights[glow.light].flicker_hz == 9.0f);
    CHECK(lights[glow.light].shadow == 0.5f); // how much of a shadow it casts: 1 unless said
    CHECK(glow.position[0] == doctest::Approx(1.0f));
    CHECK(glow.position[1] == doctest::Approx(0.44f));
    CHECK(glow.scale[0] == doctest::Approx(2.0f));
    CHECK(glow.absent_in == 1U);
    CHECK(part_named("candle/wax").light == PrefabPart::kNone);
    CHECK(part_named("candle/wax").absent_in == 0U);

    // A cell's entity: its light in the cell's list.
    Cooked cell_file(vfs, asset_id(guid_of(vfs, "src:/meta/world/lit.cell.json")), AssetType::Cell);
    const auto& cell = cell_file.load<CellAsset>();
    const std::span<const CellEntity> entities = cell.entities.get(&cell);
    const std::span<const LightDesc> cell_lights = cell.lights.get(&cell);
    REQUIRE(entities.size() == 2);
    REQUIRE(cell_lights.size() == 2);
    CHECK(cell_lights[entities[0].light].intensity == 4.0f);
    CHECK(cell_lights[entities[0].light].radius_m == 9.0f);
    CHECK(cell_lights[entities[0].light].color[2] == 1.0f);
    CHECK(cell_lights[entities[1].light].intensity == 1.0f); // nothing given: a light's own values

    // What's wrong with a light is said, with where it is.
    write_src(sandbox, "content/props/dim.prefab", R"({
  "parts": {
    "far":    {"light": {"radius_m": 0}},
    "wide":   {"light": {"cone_deg": 120}},
    "dark":   {"light": {"color": [1, -1, 0]}},
    "typo":   {"light": {"radious_m": 3}},
    "list":   {"light": [1, 2, 3]},
    "nested": {"prefab": "candle/lit.prefab", "light": {}}
  }
})");
    write_src(sandbox, "content/world/dim.cell", R"({
  "coord": [1, 0],
  "entities": [{"name": "strobe", "position": [130, 0, 0], "light": {"flicker": 2}}]
})");
    const CookReport bad = run_cook(vfs);
    const auto said = [&](std::string_view text) { return has_message(bad, "content/props/dim.prefab", text); };
    CHECK(said("part 'far': light radius_m should be above 0"));
    CHECK(said("part 'wide': light cone_deg should be from 0"));
    CHECK(said("part 'dark': light color should be 3 numbers of 0 or more"));
    CHECK(said("radious_m")); // not a key of a light
    CHECK(said("part 'list': light should be an object"));
    CHECK(said("part 'nested': is a prefab"));
    CHECK(has_message(bad, "content/world/dim.cell", "strobe: light flicker should be from 0 to 1"));
    CHECK(bad.stats.failed == 2);
}

TEST_CASE("a prefab file with parts nothing could place is refused when loaded") {
    struct alignas(16) Aligned {
        std::byte bytes[1024];
    };
    const auto loads = [](const de::cooker::PrefabBuild& build) {
        const de::core::Array<std::byte> cooked = de::cooker::cook_prefab(build, AssetId::from_value(0xb1));
        Aligned aligned{};
        REQUIRE(cooked.size() <= sizeof(aligned.bytes));
        std::memcpy(aligned.bytes, cooked.data(), cooked.size());
        return load_in_place<PrefabAsset>(std::span(aligned.bytes, cooked.size())).ok();
    };
    // A mesh with its collision, and an effect that one variant leaves out.
    const auto sample = [] {
        de::cooker::PrefabBuild build;
        build.dependencies.push({.id = AssetId::from_value(1), .type = AssetType::Mesh});
        build.dependencies.push({.id = AssetId::from_value(2), .type = AssetType::Collision});
        build.dependencies.push({.id = AssetId::from_value(3), .type = AssetType::Effect});
        for (const char c : std::string_view("waxflameunlit")) {
            build.names.push(c);
        }
        build.parts.push({.mesh = 0, .collision = 1, .name_offset = 0, .name_length = 3, .id = 11});
        build.parts.push({.effect = 2, .absent_in = 1, .name_offset = 3, .name_length = 5, .id = 12});
        build.variants.push({.name_offset = 8, .name_length = 5, .name_hash = prefab_variant_hash("unlit")});
        return build;
    };
    CHECK(loads(sample()));
    const float not_a_number = std::numeric_limits<float>::quiet_NaN();
    const auto with_part = [&](auto change) {
        de::cooker::PrefabBuild build = sample();
        change(build.parts[0]);
        return loads(build);
    };
    CHECK_FALSE(with_part([](PrefabPart& part) { part.mesh = 3; }));      // past the dependencies
    CHECK_FALSE(with_part([](PrefabPart& part) { part.mesh = 2; }));      // an effect isn't a mesh
    CHECK_FALSE(with_part([](PrefabPart& part) { part.collision = 0; })); // nor a mesh its own collision
    CHECK_FALSE(with_part([](PrefabPart& part) { part.effect = 1; }));
    CHECK_FALSE(with_part([](PrefabPart& part) { part.name_length = 0; })); // every part has its path
    CHECK_FALSE(with_part([](PrefabPart& part) { part.name_offset = 12; }));
    CHECK_FALSE(with_part([](PrefabPart& part) { part.absent_in = 2; })); // a variant there isn't
    CHECK_FALSE(with_part([](PrefabPart& part) { part.scale[1] = 0.0f; }));
    CHECK_FALSE(with_part([&](PrefabPart& part) { part.position[2] = not_a_number; }));
    CHECK_FALSE(with_part([](PrefabPart& part) { part.position[0] = 1e9f; }));
    CHECK_FALSE(with_part([&](PrefabPart& part) { part.rotation[0] = not_a_number; }));

    de::cooker::PrefabBuild none = sample();
    none.parts.clear();
    CHECK_FALSE(loads(none));
    de::cooker::PrefabBuild unnamed = sample();
    unnamed.variants[0].name_length = 0;
    CHECK_FALSE(loads(unnamed));
    de::cooker::PrefabBuild far = sample();
    far.reach_m = not_a_number;
    CHECK_FALSE(loads(far));
}
