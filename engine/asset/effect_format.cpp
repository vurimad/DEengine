#include "asset/effect_format.h"

#include <cmath>

namespace de::asset {

namespace {

using L = EffectLimits;

// A curve's record: its header, a key count, then that many keys of this many numbers each.
constexpr uint32_t curve_size(uint32_t keys, uint32_t numbers_per_key) {
    return 2 + (keys * numbers_per_key);
}

bool within(float value, float least, float most) {
    return std::isfinite(value) && value >= least && value <= most;
}

// A least and a most, in that order, both within the range.
bool range_within(float least_value, float most_value, float least, float most) {
    return within(least_value, least, most) && within(most_value, least, most) && least_value <= most_value;
}

// A curve's keys: t from 0 to 1 and rising, each value from 0 to most.
bool keys_good(std::span<const uint32_t> numbers, uint32_t numbers_per_key, float most) {
    float last_t = -1.0f;
    for (size_t at = 0; at < numbers.size(); at += numbers_per_key) {
        const float t = effect_word_float(numbers[at]);
        if (!within(t, 0.0f, 1.0f) || t <= last_t) {
            return false;
        }
        last_t = t;
        for (uint32_t value = 1; value < numbers_per_key; ++value) {
            if (!within(effect_word_float(numbers[at + value]), 0.0f, most)) {
                return false;
            }
        }
    }
    return true;
}

// The numbers of one record, after its header: finite and in range for its type. Its length is its type's already.
bool module_good(EffectModule type, std::span<const uint32_t> numbers) {
    const auto f = [numbers](size_t at) { return effect_word_float(numbers[at]); };
    switch (type) {
    case EffectModule::Sphere:
        return within(f(0), 0.0f, L::kMaxExtentM) && (f(1) == 0.0f || f(1) == 1.0f);
    case EffectModule::Disc:
        return within(f(0), 0.0f, L::kMaxExtentM);
    case EffectModule::Box:
        return within(f(0), 0.0f, L::kMaxExtentM) && within(f(1), 0.0f, L::kMaxExtentM) &&
               within(f(2), 0.0f, L::kMaxExtentM);
    case EffectModule::Velocity:
        return within(f(0), -L::kMaxSpeedMps, L::kMaxSpeedMps) && within(f(1), -L::kMaxSpeedMps, L::kMaxSpeedMps) &&
               within(f(2), -L::kMaxSpeedMps, L::kMaxSpeedMps);
    case EffectModule::ConeVelocity: {
        if (!within(f(0), -1.0f, 1.0f) || !within(f(1), -1.0f, 1.0f) || !within(f(2), -1.0f, 1.0f)) {
            return false;
        }
        const float length = std::sqrt((f(0) * f(0)) + (f(1) * f(1)) + (f(2) * f(2)));
        return std::abs(length - 1.0f) <= 1e-3f && within(f(3), -1.0f, 1.0f) &&
               range_within(f(4), f(5), 0.0f, L::kMaxSpeedMps);
    }
    case EffectModule::RandomVelocity:
        return range_within(f(0), f(1), 0.0f, L::kMaxSpeedMps);
    case EffectModule::Acceleration:
        return within(f(0), -L::kMaxAccelerationMps2, L::kMaxAccelerationMps2) &&
               within(f(1), -L::kMaxAccelerationMps2, L::kMaxAccelerationMps2) &&
               within(f(2), -L::kMaxAccelerationMps2, L::kMaxAccelerationMps2);
    case EffectModule::Drag:
    case EffectModule::Wind:
        return within(f(0), 0.0f, L::kMaxPerS);
    case EffectModule::Turbulence:
        return within(f(0), 0.0f, L::kMaxAccelerationMps2) && within(f(1), 0.0f, 1.0f / L::kMinTurbulenceSizeM) &&
               f(1) > 0.0f && within(f(2), 0.0f, L::kMaxPerS);
    case EffectModule::Size:
        return range_within(f(0), f(1), 0.0f, L::kMaxSizeM);
    case EffectModule::SizeOverLife:
        return keys_good(numbers.subspan(1), 2, L::kMaxFactor);
    case EffectModule::Color:
        return within(f(0), 0.0f, L::kMaxFactor) && within(f(1), 0.0f, L::kMaxFactor) &&
               within(f(2), 0.0f, L::kMaxFactor);
    case EffectModule::ColorOverLife:
        return keys_good(numbers.subspan(1), 4, L::kMaxFactor);
    case EffectModule::OpacityOverLife:
        return keys_good(numbers.subspan(1), 2, 1.0f);
    case EffectModule::Rotation:
        return range_within(f(0), f(1), -L::kMaxAngleRad, L::kMaxAngleRad) &&
               range_within(f(2), f(3), -L::kMaxAngleRad, L::kMaxAngleRad);
    case EffectModule::Stretch:
        return within(f(0), 0.0f, L::kMaxStretchS) && within(f(1), 1.0f, L::kMaxFactor);
    }
    return false;
}

// How many numbers a curve's key holds, t included; 0 for a module that isn't a curve.
uint32_t numbers_per_key(EffectModule type) {
    switch (type) {
    case EffectModule::SizeOverLife:
    case EffectModule::OpacityOverLife:
        return 2;
    case EffectModule::ColorOverLife:
        return 4;
    default:
        return 0;
    }
}

} // namespace

bool effect_module_run(EffectModule type, EffectRunKind& run) {
    switch (type) {
    case EffectModule::Sphere:
    case EffectModule::Disc:
    case EffectModule::Box:
    case EffectModule::Velocity:
    case EffectModule::ConeVelocity:
    case EffectModule::RandomVelocity:
        run = EffectRunKind::Birth;
        return true;
    case EffectModule::Acceleration:
    case EffectModule::Drag:
    case EffectModule::Turbulence:
    case EffectModule::Wind:
        run = EffectRunKind::Step;
        return true;
    case EffectModule::Size:
    case EffectModule::SizeOverLife:
    case EffectModule::Color:
    case EffectModule::ColorOverLife:
    case EffectModule::OpacityOverLife:
    case EffectModule::Rotation:
    case EffectModule::Stretch:
        run = EffectRunKind::Draw;
        return true;
    }
    return false;
}

uint32_t effect_module_size(EffectModule type, uint32_t keys) {
    switch (type) {
    case EffectModule::Sphere:
    case EffectModule::RandomVelocity:
    case EffectModule::Size:
    case EffectModule::Stretch:
        return 3;
    case EffectModule::Disc:
    case EffectModule::Drag:
    case EffectModule::Wind:
        return 2;
    case EffectModule::Box:
    case EffectModule::Velocity:
    case EffectModule::Acceleration:
    case EffectModule::Turbulence:
    case EffectModule::Color:
        return 4;
    case EffectModule::ConeVelocity:
        return 7;
    case EffectModule::Rotation:
        return 5;
    case EffectModule::SizeOverLife:
    case EffectModule::ColorOverLife:
    case EffectModule::OpacityOverLife:
        return curve_size(keys, numbers_per_key(type));
    }
    return 0;
}

const char* effect_module_name(EffectModule type) {
    switch (type) {
    case EffectModule::Sphere:
        return "sphere";
    case EffectModule::Disc:
        return "disc";
    case EffectModule::Box:
        return "box";
    case EffectModule::Velocity:
        return "velocity";
    case EffectModule::ConeVelocity:
        return "cone_velocity";
    case EffectModule::RandomVelocity:
        return "random_velocity";
    case EffectModule::Acceleration:
        return "acceleration";
    case EffectModule::Drag:
        return "drag";
    case EffectModule::Turbulence:
        return "turbulence";
    case EffectModule::Wind:
        return "wind";
    case EffectModule::Size:
        return "size";
    case EffectModule::SizeOverLife:
        return "size_over_life";
    case EffectModule::Color:
        return "color";
    case EffectModule::ColorOverLife:
        return "color_over_life";
    case EffectModule::OpacityOverLife:
        return "opacity_over_life";
    case EffectModule::Rotation:
        return "rotation";
    case EffectModule::Stretch:
        return "stretch";
    }
    return "";
}

const char* effect_shape_name(EffectShape shape) {
    switch (shape) {
    case EffectShape::Glow:
        return "glow";
    case EffectShape::Disc:
        return "disc";
    case EffectShape::Puff:
        return "puff";
    case EffectShape::Spark:
        return "spark";
    case EffectShape::Count:
        break;
    }
    return "";
}

core::Result<void> validate_effect_run(std::span<const uint32_t> run, EffectRunKind kind) {
    using core::ErrorCode;
    uint32_t modules = 0;
    size_t at = 0;
    while (at < run.size()) {
        const uint32_t header = run[at];
        const EffectModule type = effect_module_type(header);
        const uint32_t words = effect_module_words(header);
        EffectRunKind belongs = EffectRunKind::Birth;
        if (!effect_module_run(type, belongs) || belongs != kind) {
            return core::fail(ErrorCode::CorruptData, "effect module isn't one of its run's kind");
        }
        if (words == 0 || words > run.size() - at) {
            return core::fail(ErrorCode::CorruptData, "effect module runs past the end of its run");
        }
        uint32_t keys = 0;
        if (numbers_per_key(type) != 0) {
            // A curve says how many keys it has in the word after its header.
            if (words < 2) {
                return core::fail(ErrorCode::CorruptData, "effect curve has no key count");
            }
            keys = run[at + 1];
            if (keys == 0 || keys > L::kMaxKeys) {
                return core::fail(ErrorCode::CorruptData, "effect curve has too few or too many keys");
            }
        }
        if (words != effect_module_size(type, keys)) {
            return core::fail(ErrorCode::CorruptData, "effect module isn't its type's length");
        }
        if (!module_good(type, run.subspan(at + 1, words - 1))) {
            return core::fail(ErrorCode::CorruptData, "effect module has bad values");
        }
        if (++modules > L::kMaxModules) {
            return core::fail(ErrorCode::CorruptData, "effect run has too many modules");
        }
        at += words;
    }
    return {};
}

core::Result<void> EffectAsset::validate(std::span<const std::byte> file) const {
    using core::ErrorCode;
    if (!span_in_file(emitters, file.size()) || !span_in_file(words, file.size()) ||
        !span_in_file(names, file.size())) {
        return core::fail(ErrorCode::CorruptData, "effect array lies outside the file");
    }
    if (emitters.count == 0 || emitters.count > L::kMaxEmitters) {
        return core::fail(ErrorCode::CorruptData, "effect has no emitters, or too many");
    }
    if (!within(draw_distance_m, L::kMinDrawDistanceM, L::kMaxDrawDistanceM)) {
        return core::fail(ErrorCode::CorruptData, "effect's draw distance is out of range");
    }
    const std::span<const uint32_t> all_words = words.get(file.data());
    constexpr EffectEmitterFlags kKnownFlags = EffectEmitterFlags::Local | EffectEmitterFlags::Lit;
    for (const EffectEmitter& emitter : emitters.get(file.data())) {
        if (static_cast<uint64_t>(emitter.name_offset) + emitter.name_length > names.count) {
            return core::fail(ErrorCode::CorruptData, "effect emitter's name lies outside the names");
        }
        const bool good =
            (emitter.flags & ~kKnownFlags) == EffectEmitterFlags::None &&
            within(emitter.rate_per_s, 0.0f, L::kMaxRatePerS) && emitter.rate_per_s > 0.0f &&
            range_within(emitter.lifetime_s[0], emitter.lifetime_s[1], L::kMinLifetimeS, L::kMaxLifetimeS) &&
            emitter.capacity >= 1 && emitter.capacity <= L::kMaxCapacity &&
            within(emitter.prewarm_s, 0.0f, L::kMaxPrewarmS) && within(emitter.reach_m, 0.0f, L::kMaxReachM) &&
            within(emitter.wind_reach_s, 0.0f, L::kMaxLifetimeS) && emitter.shape < EffectShape::Count &&
            within(emitter.opacity, 0.0f, 1.0f) && within(emitter.intensity, 0.0f, L::kMaxFactor) &&
            within(emitter.soft_m, 0.0f, L::kMaxExtentM);
        if (!good) {
            return core::fail(ErrorCode::CorruptData, "effect emitter has bad values");
        }
        const EffectRun runs[3] = {emitter.birth, emitter.step, emitter.draw};
        constexpr EffectRunKind kKinds[3] = {EffectRunKind::Birth, EffectRunKind::Step, EffectRunKind::Draw};
        for (size_t r = 0; r < 3; ++r) {
            if (static_cast<uint64_t>(runs[r].first) + runs[r].words > all_words.size()) {
                return core::fail(ErrorCode::CorruptData, "effect emitter's modules lie outside the words");
            }
            if (const core::Result<void> run =
                    validate_effect_run(all_words.subspan(runs[r].first, runs[r].words), kKinds[r]);
                !run) {
                return core::fail(run.error());
            }
        }
    }
    return {};
}

} // namespace de::asset
