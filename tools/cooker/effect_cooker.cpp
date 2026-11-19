#include "cooker/effect_cooker.h"

#include "cooker/blob_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace de::cooker {

namespace {

using asset::EffectModule;
using asset::EffectRunKind;
using L = asset::EffectLimits;

constexpr double kRadPerDeg = std::numbers::pi / 180.0;
constexpr double kDefaultStretchMost = 8.0;
constexpr double kDefaultSoftM = 0.05;
// How much farther than worked out a particle is taken to reach: the simulation's steps aren't the exact integral.
constexpr double kReachMargin = 1.05;

// The most words one module is: a colour curve of the most keys.
constexpr size_t kMaxModuleWords = 2 + (size_t{L::kMaxKeys} * 4);

struct ModuleName {
    std::string_view name;
    EffectModule type;
};
constexpr ModuleName kModules[] = {
    {.name = "sphere", .type = EffectModule::Sphere},
    {.name = "disc", .type = EffectModule::Disc},
    {.name = "box", .type = EffectModule::Box},
    {.name = "velocity", .type = EffectModule::Velocity},
    {.name = "cone_velocity", .type = EffectModule::ConeVelocity},
    {.name = "random_velocity", .type = EffectModule::RandomVelocity},
    {.name = "acceleration", .type = EffectModule::Acceleration},
    {.name = "drag", .type = EffectModule::Drag},
    {.name = "turbulence", .type = EffectModule::Turbulence},
    {.name = "wind", .type = EffectModule::Wind},
    {.name = "size", .type = EffectModule::Size},
    {.name = "size_over_life", .type = EffectModule::SizeOverLife},
    {.name = "color", .type = EffectModule::Color},
    {.name = "color_over_life", .type = EffectModule::ColorOverLife},
    {.name = "opacity_over_life", .type = EffectModule::OpacityOverLife},
    {.name = "rotation", .type = EffectModule::Rotation},
    {.name = "stretch", .type = EffectModule::Stretch},
};

// What bounds how far an emitter's particles get from it: added up as its modules are read.
struct Reach {
    double extent_m = 0.0;                      // of where they start
    double speed_mps = 0.0;                     // the fastest one starts
    double acceleration = 0.0;                  // the most that pushes one, m/s2
    double drag_per_s = 0.0;                    // what slows one, wind's pull included
    double wind_per_s = 0.0;                    // how fast one takes up the wind
    double size_m = asset::kDefaultEffectSizeM; // the widest one starts
    double size_factor = 1.0;                   // the most its curves multiply that by
    double stretch_most = 1.0;                  // and how many times longer than wide it gets
};

// How far something goes in t seconds, starting at speed and pushed by acceleration, losing its speed at drag per
// second.
double travel_m(double speed, double acceleration, double drag, double t) {
    if (drag <= 1e-6) {
        return (speed * t) + (0.5 * acceleration * t * t);
    }
    const double settle = (1.0 - std::exp(-drag * t)) / drag; // seconds of its first speed it keeps
    return (speed * settle) + ((acceleration / drag) * (t - settle));
}

class EffectImporter {
public:
    EffectImporter(std::string_view path, Diagnostics& diagnostics) : path_(path), diagnostics_(diagnostics) {}

    core::Result<EffectBuild> import(yyjson_val* root) {
        const size_t errors_before = diagnostics_.error_count();
        const JsonReader reader{.object = root, .path = path_, .diagnostics = &diagnostics_};
        static constexpr const char* kKeys[] = {"draw_distance_m", "emitters"};
        reader.check_keys(kKeys, "effect");
        std::snprintf(where_, sizeof(where_), "effect");
        build_.draw_distance_m =
            static_cast<float>(optional(reader, "draw_distance_m", L::kMinDrawDistanceM, L::kMaxDrawDistanceM, 100.0));

        yyjson_val* emitters = yyjson_obj_get(root, "emitters");
        const size_t count = yyjson_is_arr(emitters) ? yyjson_arr_size(emitters) : 0;
        if (count == 0 || count > L::kMaxEmitters) {
            diagnostics_.error(path_, "emitters should be an array of 1 to %u emitters", L::kMaxEmitters);
        } else {
            size_t i = 0;
            size_t n = 0;
            yyjson_val* item = nullptr;
            yyjson_arr_foreach(emitters, i, n, item) {
                read_emitter(item, i);
            }
        }
        if (diagnostics_.error_count() != errors_before) {
            return core::fail(core::ErrorCode::InvalidArgument, "the effect has errors");
        }
        return std::move(build_);
    }

private:
    // NOLINTBEGIN(modernize-avoid-variadic-functions): printf-style, as Diagnostics is
    // A problem, with where it is: "emitter 'flame', module 2 (turbulence): size_m should be...".
    void bad(const char* format, ...) DE_PRINTF_FORMAT(2, 3) {
        char message[kMaxMessageLength + 1];
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);
        diagnostics_.error(path_, "%s: %s", where_, message);
    }
    // NOLINTEND(modernize-avoid-variadic-functions)

    // A number from least to most; reported, and the fallback, otherwise.
    double number_of(yyjson_val* value, const char* key, double least, double most, double fallback) {
        if (!yyjson_is_num(value) || !std::isfinite(yyjson_get_num(value)) || yyjson_get_num(value) < least ||
            yyjson_get_num(value) > most) {
            bad("%s should be a number from %g to %g", key, least, most);
            return fallback;
        }
        return yyjson_get_num(value);
    }
    double required(const JsonReader& reader, const char* key, double least, double most) {
        return number_of(yyjson_obj_get(reader.object, key), key, least, most, least);
    }
    double optional(const JsonReader& reader, const char* key, double least, double most, double fallback) {
        return reader.has(key) ? number_of(yyjson_obj_get(reader.object, key), key, least, most, fallback) : fallback;
    }
    // "[least, most]" or one number for both, each from least to most.
    void range(const JsonReader& reader, const char* key, double least, double most, double (&out)[2],
               bool needed = true, double fallback = 0.0) {
        out[0] = out[1] = fallback;
        yyjson_val* value = yyjson_obj_get(reader.object, key);
        if (value == nullptr && !needed) {
            return;
        }
        if (yyjson_is_num(value)) {
            out[0] = out[1] = number_of(value, key, least, most, fallback);
            return;
        }
        double read[2] = {};
        if (!read_numbers(value, read, 2) || !std::isfinite(read[0]) || !std::isfinite(read[1]) || read[0] < least ||
            read[1] > most || read[0] > read[1]) {
            bad("%s should be a number or [least, most], from %g to %g", key, least, most);
            return;
        }
        out[0] = read[0];
        out[1] = read[1];
    }
    // "[x, y, z]", each from least to most.
    void vector(const JsonReader& reader, const char* key, double least, double most, double (&out)[3]) {
        double read[3] = {};
        const bool good = read_numbers(yyjson_obj_get(reader.object, key), read, 3) &&
                          std::ranges::all_of(read, [least, most](double value) {
                              return std::isfinite(value) && value >= least && value <= most;
                          });
        if (!good) {
            bad("%s should be [x, y, z], each from %g to %g", key, least, most);
        }
        for (int axis = 0; axis < 3; ++axis) {
            out[axis] = good ? read[axis] : 0.0;
        }
    }

    // A curve's keys into the record after its count: "[[t, value...], ...]", t from 0 to 1 and rising, each value
    // from 0 to most. Returns the most any of its values is.
    double keys(const JsonReader& reader, uint32_t numbers_per_key, double most, uint32_t* record, uint32_t& words) {
        yyjson_val* array = yyjson_obj_get(reader.object, "keys");
        const size_t count = yyjson_is_arr(array) ? yyjson_arr_size(array) : 0;
        double highest = 0.0;
        if (count == 0 || count > L::kMaxKeys) {
            bad("keys should be an array of 1 to %u keys", L::kMaxKeys);
            record[words++] = 1; // one key of nothing, so the record is still whole
            for (uint32_t value = 0; value < numbers_per_key; ++value) {
                record[words++] = asset::effect_float_word(0.0f);
            }
            return highest;
        }
        record[words++] = static_cast<uint32_t>(count);
        double last_t = -1.0;
        size_t i = 0;
        size_t n = 0;
        yyjson_val* item = nullptr;
        yyjson_arr_foreach(array, i, n, item) {
            double key[4] = {};
            bool good = read_numbers(item, key, numbers_per_key) && std::isfinite(key[0]) && key[0] >= 0.0 &&
                        key[0] <= 1.0 && key[0] > last_t;
            for (uint32_t value = 1; good && value < numbers_per_key; ++value) {
                good = std::isfinite(key[value]) && key[value] >= 0.0 && key[value] <= most;
            }
            if (!good) {
                bad("key %zu should be [t, %s]: t from 0 to 1 and past the key before it, the rest from 0 to %g", i,
                    numbers_per_key == 4 ? "r, g, b" : "value", most);
                key[0] = last_t < 0.0 ? 0.0 : std::min(1.0, last_t + 1e-3);
                std::fill(std::begin(key) + 1, std::end(key), 0.0);
            }
            last_t = key[0];
            for (uint32_t value = 0; value < numbers_per_key; ++value) {
                record[words++] = asset::effect_float_word(static_cast<float>(key[value]));
                highest = value > 0 ? std::max(highest, key[value]) : highest;
            }
        }
        return highest;
    }

    void read_module(yyjson_val* object, size_t index, core::Array<uint32_t> (&runs)[3], uint32_t (&counts)[3],
                     Reach& reach) {
        std::snprintf(where_, sizeof(where_), "%s, module %zu", emitter_where_, index);
        if (!yyjson_is_obj(object)) {
            bad("should be an object with a type");
            return;
        }
        const JsonReader reader{.object = object, .path = path_, .diagnostics = &diagnostics_};
        const std::string_view name = reader.string("type", "");
        const ModuleName* found = std::ranges::find(kModules, name, &ModuleName::name);
        if (found == std::end(kModules)) {
            bad("type '%.*s' isn't a module: sphere, disc, box, velocity, cone_velocity, random_velocity, "
                "acceleration, drag, turbulence, wind, size, size_over_life, color, color_over_life, "
                "opacity_over_life, rotation, stretch",
                static_cast<int>(name.size()), name.data());
            return;
        }
        std::snprintf(where_, sizeof(where_), "%s, module %zu (%.*s)", emitter_where_, index,
                      static_cast<int>(found->name.size()), found->name.data());
        const EffectModule type = found->type;
        uint32_t record[kMaxModuleWords] = {};
        uint32_t words = 1; // the header, written last
        const auto put = [&record, &words](double value) {
            record[words++] = asset::effect_float_word(static_cast<float>(value));
        };
        const auto check = [&reader](std::span<const char* const> known) { reader.check_keys(known, "module"); };
        switch (type) {
        case EffectModule::Sphere: {
            static constexpr const char* kKeys[] = {"type", "radius_m", "surface"};
            check(kKeys);
            const double radius = required(reader, "radius_m", 0.0, L::kMaxExtentM);
            put(radius);
            put(reader.boolean("surface", false) ? 1.0 : 0.0);
            reach.extent_m += radius;
            break;
        }
        case EffectModule::Disc: {
            static constexpr const char* kKeys[] = {"type", "radius_m"};
            check(kKeys);
            const double radius = required(reader, "radius_m", 0.0, L::kMaxExtentM);
            put(radius);
            reach.extent_m += radius;
            break;
        }
        case EffectModule::Box: {
            static constexpr const char* kKeys[] = {"type", "half_extents_m"};
            check(kKeys);
            double half[3] = {};
            vector(reader, "half_extents_m", 0.0, L::kMaxExtentM, half);
            put(half[0]);
            put(half[1]);
            put(half[2]);
            reach.extent_m += std::hypot(half[0], half[1], half[2]);
            break;
        }
        case EffectModule::Velocity: {
            static constexpr const char* kKeys[] = {"type", "mps"};
            check(kKeys);
            double velocity[3] = {};
            vector(reader, "mps", -L::kMaxSpeedMps, L::kMaxSpeedMps, velocity);
            put(velocity[0]);
            put(velocity[1]);
            put(velocity[2]);
            reach.speed_mps += std::hypot(velocity[0], velocity[1], velocity[2]);
            break;
        }
        case EffectModule::ConeVelocity: {
            static constexpr const char* kKeys[] = {"type", "direction", "angle_deg", "speed_mps"};
            check(kKeys);
            double direction[3] = {};
            vector(reader, "direction", -1e6, 1e6, direction);
            const double length = std::hypot(direction[0], direction[1], direction[2]);
            if (length < 1e-6) {
                bad("direction should point somewhere: [0, 1, 0] is up");
                direction[1] = 1.0;
            }
            const double scale = length < 1e-6 ? 1.0 : 1.0 / length;
            double speed[2] = {};
            range(reader, "speed_mps", 0.0, L::kMaxSpeedMps, speed);
            put(direction[0] * scale);
            put(direction[1] * scale);
            put(direction[2] * scale);
            put(std::cos(required(reader, "angle_deg", 0.0, 180.0) * kRadPerDeg));
            put(speed[0]);
            put(speed[1]);
            reach.speed_mps += speed[1];
            break;
        }
        case EffectModule::RandomVelocity: {
            static constexpr const char* kKeys[] = {"type", "speed_mps"};
            check(kKeys);
            double speed[2] = {};
            range(reader, "speed_mps", 0.0, L::kMaxSpeedMps, speed);
            put(speed[0]);
            put(speed[1]);
            reach.speed_mps += speed[1];
            break;
        }
        case EffectModule::Acceleration: {
            static constexpr const char* kKeys[] = {"type", "mps2"};
            check(kKeys);
            double acceleration[3] = {};
            vector(reader, "mps2", -L::kMaxAccelerationMps2, L::kMaxAccelerationMps2, acceleration);
            put(acceleration[0]);
            put(acceleration[1]);
            put(acceleration[2]);
            reach.acceleration += std::hypot(acceleration[0], acceleration[1], acceleration[2]);
            break;
        }
        case EffectModule::Drag: {
            static constexpr const char* kKeys[] = {"type", "per_s"};
            check(kKeys);
            const double drag = required(reader, "per_s", 0.0, L::kMaxPerS);
            put(drag);
            reach.drag_per_s += drag;
            break;
        }
        case EffectModule::Turbulence: {
            static constexpr const char* kKeys[] = {"type", "strength_mps2", "size_m", "speed_per_s"};
            check(kKeys);
            const double strength = required(reader, "strength_mps2", 0.0, L::kMaxAccelerationMps2);
            put(strength);
            put(1.0 / std::max(required(reader, "size_m", L::kMinTurbulenceSizeM, L::kMaxExtentM),
                               double{L::kMinTurbulenceSizeM}));
            put(optional(reader, "speed_per_s", 0.0, L::kMaxPerS, 0.0));
            reach.acceleration += strength;
            break;
        }
        case EffectModule::Wind: {
            static constexpr const char* kKeys[] = {"type", "follow_per_s"};
            check(kKeys);
            const double follow = required(reader, "follow_per_s", 0.0, L::kMaxPerS);
            put(follow);
            reach.drag_per_s += follow; // it pulls a particle's own speed toward the wind's as drag does toward none
            reach.wind_per_s += follow;
            break;
        }
        case EffectModule::Size: {
            static constexpr const char* kKeys[] = {"type", "m"};
            check(kKeys);
            double size[2] = {};
            range(reader, "m", 0.0, L::kMaxSizeM, size);
            put(size[0]);
            put(size[1]);
            reach.size_m = size[1];
            break;
        }
        case EffectModule::SizeOverLife: {
            static constexpr const char* kKeys[] = {"type", "keys"};
            check(kKeys);
            reach.size_factor *= keys(reader, 2, L::kMaxFactor, record, words);
            break;
        }
        case EffectModule::Color: {
            static constexpr const char* kKeys[] = {"type", "rgb"};
            check(kKeys);
            double rgb[3] = {};
            vector(reader, "rgb", 0.0, L::kMaxFactor, rgb);
            put(rgb[0]);
            put(rgb[1]);
            put(rgb[2]);
            break;
        }
        case EffectModule::ColorOverLife: {
            static constexpr const char* kKeys[] = {"type", "keys"};
            check(kKeys);
            keys(reader, 4, L::kMaxFactor, record, words);
            break;
        }
        case EffectModule::OpacityOverLife: {
            static constexpr const char* kKeys[] = {"type", "keys"};
            check(kKeys);
            keys(reader, 2, 1.0, record, words);
            break;
        }
        case EffectModule::Rotation: {
            static constexpr const char* kKeys[] = {"type", "deg", "spin_dps"};
            check(kKeys);
            const double most_deg = L::kMaxAngleRad / kRadPerDeg;
            double angle[2] = {};
            double spin[2] = {};
            range(reader, "deg", -most_deg, most_deg, angle, false);
            range(reader, "spin_dps", -most_deg, most_deg, spin, false);
            put(angle[0] * kRadPerDeg);
            put(angle[1] * kRadPerDeg);
            put(spin[0] * kRadPerDeg);
            put(spin[1] * kRadPerDeg);
            break;
        }
        case EffectModule::Stretch: {
            static constexpr const char* kKeys[] = {"type", "seconds", "most"};
            check(kKeys);
            put(required(reader, "seconds", 0.0, L::kMaxStretchS));
            const double most = optional(reader, "most", 1.0, L::kMaxFactor, kDefaultStretchMost);
            put(most);
            reach.stretch_most = std::max(reach.stretch_most, most);
            break;
        }
        }
        EffectRunKind kind = EffectRunKind::Birth;
        asset::effect_module_run(type, kind);
        const auto run = static_cast<size_t>(kind);
        if (++counts[run] > L::kMaxModules) {
            if (counts[run] == L::kMaxModules + 1) {
                bad("is one too many of its kind: at most %u that place and launch a particle, %u that move it, %u "
                    "that say how it looks",
                    L::kMaxModules, L::kMaxModules, L::kMaxModules);
            }
            return;
        }
        record[0] = asset::effect_module_header(type, words);
        for (uint32_t word = 0; word < words; ++word) {
            runs[run].push(record[word]);
        }
    }

    void read_look(yyjson_val* object, asset::EffectEmitter& emitter) {
        std::snprintf(where_, sizeof(where_), "%s, look", emitter_where_);
        emitter.soft_m = static_cast<float>(kDefaultSoftM);
        if (object == nullptr) {
            return; // a glow that only adds its light
        }
        if (!yyjson_is_obj(object)) {
            bad("should be an object: shape, opacity, intensity, lit, soft_m");
            return;
        }
        const JsonReader reader{.object = object, .path = path_, .diagnostics = &diagnostics_};
        static constexpr const char* kKeys[] = {"shape", "opacity", "intensity", "lit", "soft_m"};
        reader.check_keys(kKeys, "look");
        const std::string_view shape = reader.string("shape", "glow");
        bool known = false;
        for (uint8_t s = 0; s < static_cast<uint8_t>(asset::EffectShape::Count); ++s) {
            if (shape == asset::effect_shape_name(static_cast<asset::EffectShape>(s))) {
                emitter.shape = static_cast<asset::EffectShape>(s);
                known = true;
            }
        }
        if (!known) {
            bad("shape '%.*s' isn't one: glow, disc, puff, spark", static_cast<int>(shape.size()), shape.data());
        }
        emitter.opacity = static_cast<float>(optional(reader, "opacity", 0.0, 1.0, 0.0));
        emitter.intensity = static_cast<float>(optional(reader, "intensity", 0.0, L::kMaxFactor, 1.0));
        emitter.soft_m = static_cast<float>(optional(reader, "soft_m", 0.0, L::kMaxExtentM, kDefaultSoftM));
        if (reader.boolean("lit", false)) {
            emitter.flags |= asset::EffectEmitterFlags::Lit;
        }
    }

    void read_emitter(yyjson_val* object, size_t index) {
        std::snprintf(emitter_where_, sizeof(emitter_where_), "emitter %zu", index);
        std::snprintf(where_, sizeof(where_), "%s", emitter_where_);
        if (!yyjson_is_obj(object)) {
            bad("should be an object");
            return;
        }
        const JsonReader reader{.object = object, .path = path_, .diagnostics = &diagnostics_};
        static constexpr const char* kKeys[] = {"name",      "space",   "rate_per_s", "lifetime_s", "capacity",
                                                "prewarm_s", "reach_m", "modules",    "look"};
        reader.check_keys(kKeys, "emitter");
        asset::EffectEmitter emitter;

        const std::string_view name = reader.string("name", "");
        if (name.empty() || name.size() > kMaxNameLength) {
            bad("name should be 1 to %zu characters", kMaxNameLength);
        } else {
            std::snprintf(emitter_where_, sizeof(emitter_where_), "emitter '%.*s'", static_cast<int>(name.size()),
                          name.data());
            std::snprintf(where_, sizeof(where_), "%s", emitter_where_);
            for (const asset::EffectEmitter& other : build_.emitters) {
                if (std::string_view(build_.names.data() + other.name_offset, other.name_length) == name) {
                    bad("another emitter has that name");
                }
            }
        }
        emitter.name_offset = static_cast<uint32_t>(build_.names.size());
        emitter.name_length = static_cast<uint16_t>(std::min(name.size(), kMaxNameLength));
        for (const char c : name.substr(0, kMaxNameLength)) {
            build_.names.push(c);
        }

        const std::string_view space = reader.string("space", "world");
        if (space == "local") {
            emitter.flags |= asset::EffectEmitterFlags::Local;
        } else if (space != "world") {
            bad(R"(space should be "world" (particles are left behind as it moves) or "local" (they move with it))");
        }
        const double rate = required(reader, "rate_per_s", 1e-3, L::kMaxRatePerS);
        emitter.rate_per_s = static_cast<float>(rate);
        double lifetime[2] = {};
        range(reader, "lifetime_s", L::kMinLifetimeS, L::kMaxLifetimeS, lifetime, true, L::kMinLifetimeS);
        emitter.lifetime_s[0] = static_cast<float>(lifetime[0]);
        emitter.lifetime_s[1] = static_cast<float>(lifetime[1]);
        emitter.prewarm_s = static_cast<float>(
            optional(reader, "prewarm_s", 0.0, L::kMaxPrewarmS, std::min(lifetime[1], double{L::kMaxPrewarmS})));

        // The most alive at once: what's born in the steps the longest-lived one lives through (it goes at the first
        // step past its time) and one step more, and one over. In whole steps, so a lifetime that's a whole number of
        // them isn't rounded up to one more.
        const double life_steps = std::ceil((lifetime[1] * asset::kEffectStepsPerS) - 1e-6);
        const double alive = std::ceil((rate * (life_steps + 1.0) / asset::kEffectStepsPerS) - 1e-9) + 1.0;
        if (reader.has("capacity")) {
            const double capacity = required(reader, "capacity", 1.0, L::kMaxCapacity);
            if (capacity != std::floor(capacity)) {
                bad("capacity should be a whole number");
            }
            emitter.capacity = static_cast<uint32_t>(capacity);
            if (capacity < alive) {
                diagnostics_.warning(path_,
                                     "%s: capacity %u is fewer than the %.0f its rate and lifetime keep alive: the "
                                     "oldest are cut short",
                                     where_, emitter.capacity, alive);
            }
        } else if (alive > L::kMaxCapacity) {
            bad("rate_per_s and lifetime_s keep %.0f particles alive at once, and an emitter holds at most %u: lower "
                "one of them",
                alive, L::kMaxCapacity);
            emitter.capacity = L::kMaxCapacity;
        } else {
            emitter.capacity = static_cast<uint32_t>(alive);
        }

        core::Array<uint32_t> runs[3] = {core::Array<uint32_t>(tools_heap()), core::Array<uint32_t>(tools_heap()),
                                         core::Array<uint32_t>(tools_heap())};
        uint32_t counts[3] = {};
        Reach reach;
        yyjson_val* modules = yyjson_obj_get(object, "modules");
        if (modules != nullptr && !yyjson_is_arr(modules)) {
            bad("modules should be an array");
        }
        size_t i = 0;
        size_t n = 0;
        yyjson_val* item = nullptr;
        yyjson_arr_foreach(modules, i, n, item) {
            read_module(item, i, runs, counts, reach);
        }
        std::snprintf(where_, sizeof(where_), "%s", emitter_where_);
        asset::EffectRun* const placed[3] = {&emitter.birth, &emitter.step, &emitter.draw};
        for (size_t run = 0; run < 3; ++run) {
            *placed[run] = {.first = static_cast<uint32_t>(build_.words.size()),
                            .words = static_cast<uint32_t>(runs[run].size())};
            for (const uint32_t word : runs[run]) {
                build_.words.push(word);
            }
        }

        // How far a particle's edge can get from the emitter in still air, and how much farther per m/s of wind.
        const double life = lifetime[1];
        const double widest = reach.size_m * reach.size_factor * reach.stretch_most;
        const double still =
            ((reach.extent_m + travel_m(reach.speed_mps, reach.acceleration, reach.drag_per_s, life)) * kReachMargin) +
            (widest * std::numbers::sqrt2 * 0.5);
        emitter.reach_m = static_cast<float>(std::min(still, double{L::kMaxReachM}));
        if (reader.has("reach_m")) {
            emitter.reach_m = static_cast<float>(required(reader, "reach_m", 0.0, L::kMaxReachM));
        }
        emitter.wind_reach_s = static_cast<float>(
            reach.wind_per_s > 0.0 ? travel_m(0.0, reach.wind_per_s, reach.wind_per_s, life) * kReachMargin : 0.0);

        read_look(yyjson_obj_get(object, "look"), emitter);
        build_.emitters.push(emitter);
    }

    static constexpr size_t kMaxNameLength = 63;

    std::string_view path_;
    Diagnostics& diagnostics_;
    EffectBuild build_;
    char emitter_where_[96] = {};
    char where_[160] = {};
};

} // namespace

core::Result<EffectBuild> import_effect(const Source& source, std::span<const std::byte> bytes,
                                        Diagnostics& diagnostics) {
    const std::string_view path = source.path.view();
    const core::Result<JsonDoc> doc = JsonDoc::parse(bytes, path, diagnostics);
    if (!doc) {
        return core::fail(doc.error());
    }
    if (!yyjson_is_obj(doc->root())) {
        diagnostics.error(path, "should be a JSON object");
        return core::fail(core::ErrorCode::InvalidArgument, "the effect has errors");
    }
    EffectImporter importer(path, diagnostics);
    return importer.import(doc->root());
}

core::Array<std::byte> cook_effect(const EffectBuild& build, asset::AssetId id) {
    asset::EffectAsset effect;
    effect.draw_distance_m = build.draw_distance_m;
    BlobWriter writer(sizeof(asset::EffectAsset));
    effect.emitters = writer.append(build.emitters.span());
    effect.words = writer.append(build.words.span());
    effect.names = writer.append(build.names.span());
    return writer.finish(effect, id, {});
}

} // namespace de::cooker
