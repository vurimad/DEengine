#include "asset/cell_format.h"
#include "asset/effect_format.h"
#include "cooker/effect_cooker.h"
#include "support/cook_kit.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
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

// A candle's flame and the motes around it: every kind of module, both spaces, both ways of being lit.
constexpr std::string_view kCandle = R"({
  "draw_distance_m": 60,
  "emitters": [
    {
      "name": "flame",
      "rate_per_s": 30,
      "lifetime_s": [0.5, 1.0],
      "modules": [
        {"type": "sphere", "radius_m": 0.5},
        {"type": "cone_velocity", "direction": [0, 2, 0], "angle_deg": 60, "speed_mps": [1, 2]},
        {"type": "drag", "per_s": 1},
        {"type": "size_over_life", "keys": [[0, 0.5], [0.25, 1], [1, 0]]},
        {"type": "color_over_life", "keys": [[0, 1, 0.8, 0.4], [1, 1, 0.2, 0]]},
        {"type": "acceleration", "mps2": [0, 0.5, 0]},
        {"type": "stretch", "seconds": 0.05}
      ],
      "look": {"shape": "spark", "intensity": 8, "soft_m": 0.1}
    },
    {
      "name": "motes",
      "space": "local",
      "rate_per_s": 4,
      "lifetime_s": 6,
      "capacity": 40,
      "prewarm_s": 2,
      "reach_m": 3,
      "modules": [
        {"type": "box", "half_extents_m": [1, 2, 2]},
        {"type": "disc", "radius_m": 0.25},
        {"type": "velocity", "mps": [0, 0, 0.1]},
        {"type": "random_velocity", "speed_mps": 0.02},
        {"type": "turbulence", "strength_mps2": 0.05, "size_m": 0.5, "speed_per_s": 0.25},
        {"type": "wind", "follow_per_s": 2},
        {"type": "size", "m": [0.002, 0.004]},
        {"type": "color", "rgb": [1, 0.9, 0.7]},
        {"type": "opacity_over_life", "keys": [[0, 0], [0.5, 1], [1, 0]]},
        {"type": "rotation", "deg": [0, 90], "spin_dps": 180}
      ],
      "look": {"shape": "disc", "opacity": 0.75, "lit": true}
    }
  ]
})";

// A run's records: each one's type, and where its numbers start.
struct Record {
    EffectModule type;
    std::vector<float> numbers; // a curve's key count comes out as a float's bits: read it from words instead
    std::vector<uint32_t> words;
};
std::vector<Record> records_of(const EffectAsset& effect, const void* file, EffectRun run) {
    const std::span<const uint32_t> words = effect.words.get(file).subspan(run.first, run.words);
    std::vector<Record> records;
    size_t at = 0;
    while (at < words.size()) {
        const uint32_t count = effect_module_words(words[at]);
        REQUIRE(count >= 1);
        REQUIRE(at + count <= words.size());
        Record record{.type = effect_module_type(words[at]), .numbers = {}, .words = {}};
        for (uint32_t i = 1; i < count; ++i) {
            record.words.push_back(words[at + i]);
            record.numbers.push_back(effect_word_float(words[at + i]));
        }
        records.push_back(record);
        at += count;
    }
    return records;
}

} // namespace

TEST_CASE("an effect's emitters cook into runs of modules, each kind in the file's order") {
    const Sandbox sandbox;
    write_src(sandbox, "content/fire/candle.vfx", kCandle);
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    CHECK(report.stats.failed == 0);
    for (const de::cooker::Diagnostic& diagnostic : report.diagnostics) {
        CHECK_MESSAGE(diagnostic.path != "content/fire/candle.vfx", diagnostic.message.c_str());
    }
    const AssetId id = asset_id(guid_of(vfs, "src:/meta/fire/candle.vfx.json"));
    Cooked file(vfs, id, AssetType::Effect);
    const auto& effect = file.load<EffectAsset>();
    CHECK(effect.draw_distance_m == doctest::Approx(60.0f));
    const std::span<const EffectEmitter> emitters = effect.emitters.get(file.data());
    REQUIRE(emitters.size() == 2);

    const EffectEmitter& flame = emitters[0];
    CHECK((effect.name(file.data(), flame) == "flame"));
    CHECK(flame.flags == EffectEmitterFlags::None); // left behind as it moves, its colour its own light
    CHECK(flame.rate_per_s == doctest::Approx(30.0f));
    CHECK(flame.lifetime_s[0] == doctest::Approx(0.5f));
    CHECK(flame.lifetime_s[1] == doctest::Approx(1.0f));
    // What's born while the longest-lived one lives, a step more, and one: ceil(30 * (1 + 1/60)) + 1.
    CHECK(flame.capacity == 32);
    CHECK(flame.prewarm_s == doctest::Approx(1.0f)); // its longest lifetime, unless it says
    CHECK(flame.shape == EffectShape::Spark);
    CHECK(flame.opacity == doctest::Approx(0.0f));
    CHECK(flame.intensity == doctest::Approx(8.0f));
    CHECK(flame.soft_m == doctest::Approx(0.1f));

    const std::vector<Record> birth = records_of(effect, file.data(), flame.birth);
    REQUIRE(birth.size() == 2);
    CHECK(birth[0].type == EffectModule::Sphere);
    CHECK(birth[0].numbers[0] == doctest::Approx(0.5f));
    CHECK(birth[0].numbers[1] == 0.0f); // anywhere inside
    CHECK(birth[1].type == EffectModule::ConeVelocity);
    CHECK(birth[1].numbers[0] == doctest::Approx(0.0f)); // the direction, made a unit
    CHECK(birth[1].numbers[1] == doctest::Approx(1.0f));
    CHECK(birth[1].numbers[3] == doctest::Approx(0.5f)); // cos 60
    CHECK(birth[1].numbers[4] == doctest::Approx(1.0f));
    CHECK(birth[1].numbers[5] == doctest::Approx(2.0f));
    // What moves it keeps the file's order, whatever stood between: the drag, then the lift.
    const std::vector<Record> step = records_of(effect, file.data(), flame.step);
    REQUIRE(step.size() == 2);
    CHECK(step[0].type == EffectModule::Drag);
    CHECK(step[1].type == EffectModule::Acceleration);
    CHECK(step[1].numbers[1] == doctest::Approx(0.5f));
    const std::vector<Record> draw = records_of(effect, file.data(), flame.draw);
    REQUIRE(draw.size() == 3);
    CHECK(draw[0].type == EffectModule::SizeOverLife);
    REQUIRE(draw[0].words.size() == 7);
    CHECK(draw[0].words[0] == 3); // keys
    CHECK(draw[0].numbers[3] == doctest::Approx(0.25f));
    CHECK(draw[0].numbers[4] == doctest::Approx(1.0f));
    CHECK(draw[1].type == EffectModule::ColorOverLife);
    REQUIRE(draw[1].words.size() == 9);
    CHECK(draw[1].words[0] == 2);
    CHECK(draw[1].numbers[6] == doctest::Approx(1.0f)); // the second key: r
    CHECK(draw[1].numbers[7] == doctest::Approx(0.2f));
    CHECK(draw[2].type == EffectModule::Stretch);
    CHECK(draw[2].numbers[1] == doctest::Approx(8.0f)); // at most that many times its width, unless it says

    // How far one gets: from half a meter out, at 2 m/s losing its speed at 1 a second while 0.5 m/s2 lifts it, for a
    // second; a twentieth more, and half the diagonal of the widest it's drawn (0.05 m, 8 times as long).
    const double settle = 1.0 - std::exp(-1.0);
    const double travel = (2.0 * settle) + (0.5 * (1.0 - settle));
    CHECK(flame.reach_m == doctest::Approx(((0.5 + travel) * 1.05) + (0.05 * 8.0 * std::numbers::sqrt2 * 0.5)));
    CHECK(flame.wind_reach_s == 0.0f);

    const EffectEmitter& motes = emitters[1];
    CHECK((effect.name(file.data(), motes) == "motes"));
    CHECK(motes.flags == (EffectEmitterFlags::Local | EffectEmitterFlags::Lit));
    CHECK(motes.lifetime_s[0] == doctest::Approx(6.0f)); // one number is both ends
    CHECK(motes.lifetime_s[1] == doctest::Approx(6.0f));
    CHECK(motes.capacity == 40);
    CHECK(motes.prewarm_s == doctest::Approx(2.0f));
    CHECK(motes.reach_m == doctest::Approx(3.0f)); // as the file says
    // Wind carries one as far as it would in 6 s, less what it takes to take the wind up at 2 a second.
    CHECK(motes.wind_reach_s == doctest::Approx((6.0 - ((1.0 - std::exp(-12.0)) / 2.0)) * 1.05));
    CHECK(motes.shape == EffectShape::Disc);
    CHECK(motes.opacity == doctest::Approx(0.75f));
    CHECK(motes.intensity == doctest::Approx(1.0f));
    const std::vector<Record> motes_birth = records_of(effect, file.data(), motes.birth);
    REQUIRE(motes_birth.size() == 4);
    CHECK(motes_birth[0].type == EffectModule::Box);
    CHECK(motes_birth[1].type == EffectModule::Disc);
    CHECK(motes_birth[2].type == EffectModule::Velocity);
    CHECK(motes_birth[3].type == EffectModule::RandomVelocity);
    CHECK(motes_birth[3].numbers[0] == doctest::Approx(0.02f));
    CHECK(motes_birth[3].numbers[1] == doctest::Approx(0.02f));
    const std::vector<Record> motes_step = records_of(effect, file.data(), motes.step);
    REQUIRE(motes_step.size() == 2);
    CHECK(motes_step[0].type == EffectModule::Turbulence);
    CHECK(motes_step[0].numbers[1] == doctest::Approx(2.0f)); // 1 / its size
    CHECK(motes_step[1].type == EffectModule::Wind);
    const std::vector<Record> motes_draw = records_of(effect, file.data(), motes.draw);
    REQUIRE(motes_draw.size() == 4);
    CHECK(motes_draw[0].type == EffectModule::Size);
    CHECK(motes_draw[3].type == EffectModule::Rotation);
    CHECK(motes_draw[3].numbers[1] == doctest::Approx(std::numbers::pi_v<float> * 0.5f));
    CHECK(motes_draw[3].numbers[2] == doctest::Approx(std::numbers::pi_v<float>)); // one spin for both ends
    CHECK(motes_draw[3].numbers[3] == doctest::Approx(std::numbers::pi_v<float>));

    // Nothing changed: nothing cooks again.
    const CookReport again = run_cook(vfs);
    CHECK(again.stats.cooked == 0);
}

TEST_CASE("an effect's mistakes are each reported with the emitter and module they're in") {
    const Sandbox sandbox;
    write_src(sandbox, "content/fx/broken.vfx", R"({
  "draw_distance": 60,
  "emitters": [
    {
      "name": "a",
      "space": "screen",
      "lifetime_s": [2, 1],
      "modules": [
        {"type": "sphere"},
        {"type": "vortex", "strength": 2},
        {"type": "cone_velocity", "direction": [0, 0, 0], "angle_deg": 200, "speed_mps": [1, 2]},
        {"type": "size_over_life", "keys": [[0, 1], [0.5, 2], [0.25, 1]]},
        {"type": "drag", "per_s": 1, "per_second": 2},
        {"type": "color_over_life", "keys": []},
        "smoke"
      ],
      "look": {"shape": "cube", "opacity": 2}
    },
    {"name": "a", "rate_per_s": 100000, "lifetime_s": 60},
    {"name": "c", "rate_per_s": 10, "lifetime_s": 1, "capacity": 4, "modules": {}}
  ]
})");
    write_src(sandbox, "content/fx/empty.vfx", R"({"emitters": []})");
    write_src(sandbox, "content/fx/list.vfx", R"([1, 2])");
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    const auto said = [&](std::string_view text) { return has_message(report, "content/fx/broken.vfx", text); };
    CHECK(said("draw_distance")); // not a key of an effect
    CHECK(said("emitter 'a': space should be"));
    CHECK(said("emitter 'a': rate_per_s should be a number"));
    CHECK(said("emitter 'a': lifetime_s should be a number or [least, most]"));
    CHECK(said("emitter 'a', module 0 (sphere): radius_m should be a number from 0 to"));
    CHECK(said("emitter 'a', module 1: type 'vortex' isn't a module"));
    CHECK(said("module 2 (cone_velocity): direction should point somewhere"));
    CHECK(said("module 2 (cone_velocity): angle_deg should be a number from 0 to 180"));
    CHECK(said("module 3 (size_over_life): key 2 should be [t, value]"));
    CHECK(said("per_second")); // not one of drag's
    CHECK(said("module 5 (color_over_life): keys should be an array of 1 to 8 keys"));
    CHECK(said("emitter 'a', module 6: should be an object with a type"));
    CHECK(said("emitter 'a', look: shape 'cube' isn't one"));
    CHECK(said("emitter 'a', look: opacity should be a number from 0 to 1"));
    CHECK(said("emitter 'a': another emitter has that name"));
    CHECK(said("keep 6001668 particles alive at once"));
    CHECK(said("emitter 'c': capacity 4 is fewer than the 12")); // a warning: it still cooks, were the rest right
    CHECK(said("emitter 'c': modules should be an array"));
    CHECK(has_message(report, "content/fx/empty.vfx", "emitters should be an array of 1 to 16"));
    CHECK(has_message(report, "content/fx/list.vfx", "should be a JSON object"));
    CHECK(report.stats.failed == 3);
}

TEST_CASE("an effect file whose modules a shader couldn't walk is refused when loaded") {
    struct alignas(16) Aligned {
        std::byte bytes[1024];
    };
    const auto loads = [](const de::cooker::EffectBuild& build) {
        const de::core::Array<std::byte> cooked = de::cooker::cook_effect(build, AssetId::from_value(0x91));
        Aligned aligned{};
        REQUIRE(cooked.size() <= sizeof(aligned.bytes));
        std::memcpy(aligned.bytes, cooked.data(), cooked.size());
        return load_in_place<EffectAsset>(std::span(aligned.bytes, cooked.size())).ok();
    };
    const auto f = [](float value) { return effect_float_word(value); };
    // One emitter: a sphere to start in, a drag, a size curve of two keys.
    const auto sample = [&f] {
        de::cooker::EffectBuild build;
        for (const uint32_t word :
             {effect_module_header(EffectModule::Sphere, 3), f(0.5f), f(0.0f),
              effect_module_header(EffectModule::Drag, 2), f(1.0f), effect_module_header(EffectModule::SizeOverLife, 6),
              uint32_t{2}, f(0.0f), f(1.0f), f(1.0f), f(0.0f)}) {
            build.words.push(word);
        }
        build.names.push('a');
        EffectEmitter emitter;
        emitter.name_length = 1;
        emitter.rate_per_s = 10.0f;
        emitter.capacity = 12;
        emitter.birth = {.first = 0, .words = 3};
        emitter.step = {.first = 3, .words = 2};
        emitter.draw = {.first = 5, .words = 6};
        build.emitters.push(emitter);
        return build;
    };
    CHECK(loads(sample()));

    const float not_a_number = std::numeric_limits<float>::quiet_NaN();
    const auto with_word = [&](size_t at, uint32_t word) {
        de::cooker::EffectBuild build = sample();
        build.words[at] = word;
        return loads(build);
    };
    CHECK_FALSE(with_word(0, effect_module_header(EffectModule::Drag, 3)));   // a step's module in the birth run
    CHECK_FALSE(with_word(0, effect_module_header(EffectModule::Sphere, 4))); // longer than a sphere is
    CHECK_FALSE(with_word(0, effect_module_header(EffectModule::Sphere, 0))); // no length: it would never end
    CHECK_FALSE(with_word(0, effect_module_header(static_cast<EffectModule>(99), 3))); // no such module
    CHECK_FALSE(with_word(3, effect_module_header(EffectModule::Drag, 9)));            // past the end of its run
    CHECK_FALSE(with_word(1, f(not_a_number)));
    CHECK_FALSE(with_word(1, f(-1.0f)));  // a radius
    CHECK_FALSE(with_word(2, f(0.5f)));   // on its surface or not: nothing between
    CHECK_FALSE(with_word(4, f(1e9f)));   // more drag than the limit
    CHECK_FALSE(with_word(6, 0));         // a curve with no keys
    CHECK_FALSE(with_word(6, 9));         // or more than it can hold
    CHECK_FALSE(with_word(6, 3));         // or more than its length has room for
    CHECK_FALSE(with_word(9, f(0.0f)));   // a key not past the one before it
    CHECK_FALSE(with_word(9, f(1.5f)));   // or past the end of a life
    CHECK_FALSE(with_word(10, f(-0.5f))); // a size less than nothing

    const auto with_emitter = [&](auto change) {
        de::cooker::EffectBuild build = sample();
        change(build.emitters[0]);
        return loads(build);
    };
    CHECK_FALSE(with_emitter([](EffectEmitter& e) { e.capacity = 0; }));
    CHECK_FALSE(with_emitter([](EffectEmitter& e) { e.capacity = EffectLimits::kMaxCapacity + 1; }));
    CHECK_FALSE(with_emitter([](EffectEmitter& e) { e.rate_per_s = 0.0f; }));
    CHECK_FALSE(with_emitter([&](EffectEmitter& e) { e.rate_per_s = not_a_number; }));
    CHECK_FALSE(with_emitter([](EffectEmitter& e) { e.lifetime_s[0] = 2.0f; })); // its least past its most
    CHECK_FALSE(with_emitter([](EffectEmitter& e) { e.lifetime_s[0] = 0.0f; })); // shorter than a step
    CHECK_FALSE(with_emitter([](EffectEmitter& e) { e.shape = static_cast<EffectShape>(9); }));
    CHECK_FALSE(with_emitter([](EffectEmitter& e) { e.flags = static_cast<EffectEmitterFlags>(1U << 7U); }));
    CHECK_FALSE(with_emitter([](EffectEmitter& e) { e.opacity = 1.5f; }));
    CHECK_FALSE(with_emitter([&](EffectEmitter& e) { e.reach_m = not_a_number; }));
    CHECK_FALSE(with_emitter([](EffectEmitter& e) { e.name_length = 2; }));         // past the names
    CHECK_FALSE(with_emitter([](EffectEmitter& e) { e.draw.words = 7; }));          // past the words
    CHECK_FALSE(with_emitter([](EffectEmitter& e) { e.step.first = UINT32_MAX; })); // and where adding would wrap
    CHECK_FALSE(with_emitter([](EffectEmitter& e) { e.birth.words = 2; }));         // cutting a record short

    de::cooker::EffectBuild none = sample();
    none.emitters.clear();
    CHECK_FALSE(loads(none));
    de::cooker::EffectBuild far = sample();
    far.draw_distance_m = 0.0f;
    CHECK_FALSE(loads(far));

    // A run may hold only so many modules: a shader's loop over them is bounded by it.
    std::vector<uint32_t> many;
    for (uint32_t i = 0; i < EffectLimits::kMaxModules; ++i) {
        many.push_back(effect_module_header(EffectModule::Drag, 2));
        many.push_back(f(0.0f));
    }
    CHECK(validate_effect_run(many, EffectRunKind::Step).ok());
    many.push_back(effect_module_header(EffectModule::Drag, 2));
    many.push_back(f(0.0f));
    CHECK_FALSE(validate_effect_run(many, EffectRunKind::Step).ok());
    CHECK(validate_effect_run({}, EffectRunKind::Draw).ok()); // no modules is fine
}

TEST_CASE("a cell's entity names the effect it plays, which becomes one of the cell's dependencies") {
    const Sandbox sandbox;
    write_src(sandbox, "content/fire/candle.vfx", kCandle);
    write_src(sandbox, "content/world/hall/cell_0_0.cell", R"({
  "coord": [0, 0],
  "entities": [
    {"name": "candle_1", "position": [1, 1, 1], "effect": "../../fire/candle.vfx"},
    {"name": "candle_2", "position": [2, 1, 1], "effect": "../../fire/candle.vfx"},
    {"name": "post", "position": [3, 0, 1]}
  ]
})");
    write_src(sandbox, "content/world/hall/cell_1_0.cell", R"({
  "coord": [1, 0],
  "entities": [
    {"name": "nothing", "position": [130, 0, 1], "effect": "../../fire/torch.vfx"},
    {"name": "not_one", "position": [131, 0, 1], "effect": "cell_0_0.cell"}
  ]
})");
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    CHECK(has_message(report, "content/world/hall/cell_1_0.cell",
                      "effect '../../fire/torch.vfx' should name an effect source (.vfx)"));
    CHECK(has_message(report, "content/world/hall/cell_1_0.cell",
                      "effect 'cell_0_0.cell' should name an effect source (.vfx)"));
    CHECK(report.stats.failed == 1);

    const AssetId effect_id = asset_id(guid_of(vfs, "src:/meta/fire/candle.vfx.json"));
    Cooked file(vfs, asset_id(guid_of(vfs, "src:/meta/world/hall/cell_0_0.cell.json")), AssetType::Cell);
    const auto& cell = file.load<CellAsset>();
    const std::span<const AssetRef> dependencies = cell.header.dependencies.get(file.data());
    REQUIRE(dependencies.size() == 1); // named twice, held once
    CHECK((dependencies[0] == AssetRef{.id = effect_id, .type = AssetType::Effect}));
    const std::span<const CellEntity> entities = cell.entities.get(file.data());
    REQUIRE(entities.size() == 3);
    CHECK(entities[0].effect == 0);
    CHECK(entities[1].effect == 0);
    CHECK(entities[2].effect == CellEntity::kNone);

    // The effect gone: the cell that named it cooks again, and says so.
    de::test::remove_src(sandbox, "content/fire/candle.vfx");
    de::test::remove_src(sandbox, "meta/fire/candle.vfx.json");
    const CookReport gone = run_cook(vfs);
    CHECK(has_message(gone, "content/world/hall/cell_0_0.cell", "effect '../../fire/candle.vfx' should name"));
}
