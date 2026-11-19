#include "editor/prefab_document.h"

#include "asset/prefab_format.h"
#include "editor/cell_source.h"

#include <glm/gtc/quaternion.hpp>
#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace de::editor {
namespace {

void* json_malloc(void* /*context*/, size_t size) {
    return core::tagged_malloc(core::MemoryTag::Editor, size);
}

void* json_realloc(void* /*context*/, void* pointer, size_t /*old_size*/, size_t size) {
    return core::tagged_realloc(core::MemoryTag::Editor, pointer, size);
}

void json_free(void* /*context*/, void* pointer) {
    core::tagged_free(core::MemoryTag::Editor, pointer);
}

constexpr yyjson_alc kJsonAllocator{
    .malloc = &json_malloc, .realloc = &json_realloc, .free = &json_free, .ctx = nullptr};

// A parsed document, freed when it goes.
struct Json {
    yyjson_doc* doc = nullptr;

    Json() = default;
    ~Json() { yyjson_doc_free(doc); }
    Json(const Json&) = delete;
    Json& operator=(const Json&) = delete;
    Json(Json&&) = delete;
    Json& operator=(Json&&) = delete;
};

std::string_view text_of(yyjson_val* value) {
    const char* text = yyjson_get_str(value);
    return text != nullptr ? std::string_view(text, yyjson_get_len(value)) : std::string_view();
}

// `count` numbers from a JSON array of exactly that many.
bool read_numbers(yyjson_val* array, float* out, size_t count) {
    if (!yyjson_is_arr(array) || yyjson_arr_size(array) != count) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        yyjson_val* value = yyjson_arr_get(array, i);
        if (!yyjson_is_num(value) || !std::isfinite(yyjson_get_num(value))) {
            return false;
        }
        out[i] = static_cast<float>(yyjson_get_num(value));
    }
    return true;
}

// A part's light: only the keys a light has, each a number (three for its colour). What a point light and a steady
// one don't use is left as a light has it, since what's written doesn't say them.
bool read_light(yyjson_val* object, asset::LightDesc& light) {
    if (!yyjson_is_obj(object)) {
        return false;
    }
    size_t index = 0;
    size_t count = 0;
    yyjson_val* key = nullptr;
    yyjson_val* value = nullptr;
    yyjson_obj_foreach(object, index, count, key, value) {
        const std::string_view what = text_of(key);
        float* number = what == "intensity"    ? &light.intensity
                        : what == "radius_m"   ? &light.radius_m
                        : what == "cone_deg"   ? &light.cone_deg
                        : what == "cone_soft"  ? &light.cone_soft
                        : what == "flicker"    ? &light.flicker
                        : what == "flicker_hz" ? &light.flicker_hz
                        : what == "shadow"     ? &light.shadow
                                               : nullptr;
        if (what == "color") {
            if (!read_numbers(value, light.color, 3)) {
                return false;
            }
        } else if (number != nullptr && yyjson_is_num(value) && std::isfinite(yyjson_get_num(value))) {
            *number = static_cast<float>(yyjson_get_num(value));
        } else {
            return false;
        }
    }
    const asset::LightDesc usual;
    if (light.cone_deg <= 0.0f) {
        light.cone_soft = usual.cone_soft;
    }
    if (light.flicker <= 0.0f) {
        light.flicker_hz = usual.flicker_hz;
    }
    return true;
}

void append(core::Array<char>& out, std::string_view text) {
    for (const char c : text) {
        out.push(c);
    }
}

// A string as JSON writes it.
void append_quoted(core::Array<char>& out, std::string_view text) {
    out.push('"');
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push('\\');
        }
        out.push(c);
    }
    out.push('"');
}

void append_number(core::Array<char>& out, double value, int decimals) {
    char text[64];
    const size_t length = format_number(value, decimals, text);
    append(out, std::string_view(text, length));
}

void append_numbers(core::Array<char>& out, const float* values, size_t count, int decimals) {
    out.push('[');
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            append(out, ", ");
        }
        append_number(out, static_cast<double>(values[i]), decimals);
    }
    out.push(']');
}

// Whether two numbers are written the same to that many decimals.
bool same_written(float a, float b, int decimals) {
    char first[64];
    char second[64];
    const size_t first_length = format_number(static_cast<double>(a), decimals, first);
    const size_t second_length = format_number(static_cast<double>(b), decimals, second);
    return std::string_view(first, first_length) == std::string_view(second, second_length);
}

// Whether path is a part's ID, or a path into what that part nests.
bool names_part(std::string_view path, std::string_view id) {
    return path == id || (path.size() > id.size() && path.starts_with(id) && path[id.size()] == '/');
}

} // namespace

Quat rotation_from_deg(const Vec3& degrees) {
    return glm::angleAxis(glm::radians(degrees.y), Vec3(0.0f, 1.0f, 0.0f)) *
           glm::angleAxis(glm::radians(degrees.x), Vec3(1.0f, 0.0f, 0.0f)) *
           glm::angleAxis(glm::radians(degrees.z), Vec3(0.0f, 0.0f, 1.0f));
}

void PrefabDocPart::set_rotation(const Quat& turned) {
    rotation = glm::normalize(turned);
    rotation_deg = rotation_deg_of(rotation, rotation_deg);
}

void PrefabDocPart::set_rotation_deg(const Vec3& degrees) {
    rotation_deg = degrees;
    rotation = rotation_from_deg(degrees);
}

void PrefabDocument::clear() {
    parts.clear();
    variants.clear();
    left_out.clear();
    header.clear();
}

core::Result<void> PrefabDocument::parse(std::string_view text) {
    clear();
    // The comment lines it opens with, before or just inside its object, as they are.
    {
        size_t at = 0;
        bool inside = false;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            end = end == std::string_view::npos ? text.size() : end;
            std::string_view line = text.substr(at, end - at);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            const size_t first = line.find_first_not_of(" \t");
            const std::string_view words = first == std::string_view::npos ? std::string_view() : line.substr(first);
            if (words.starts_with("//")) {
                if (!header.empty()) {
                    header.push('\n');
                }
                append(header, words);
            } else if (!inside && words == "{") {
                inside = true;
            } else if (!words.empty()) {
                break;
            }
            at = end + 1;
        }
    }
    Json json;
    yyjson_read_err error{};
    // yyjson doesn't write to the text without YYJSON_READ_INSITU.
    json.doc =
        yyjson_read_opts(const_cast<char*>(text.data()), text.size(),
                         YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS, &kJsonAllocator, &error);
    yyjson_val* root = json.doc != nullptr ? yyjson_doc_get_root(json.doc) : nullptr;
    if (!yyjson_is_obj(root)) {
        clear();
        return core::fail(core::ErrorCode::CorruptData,
                          "isn't JSON the prefab editor can read: mend it in a text editor");
    }
    const auto refuse = [this](const char* why) {
        clear();
        return core::fail(core::ErrorCode::CorruptData, why);
    };
    size_t index = 0;
    size_t count = 0;
    yyjson_val* key = nullptr;
    yyjson_val* value = nullptr;
    yyjson_obj_foreach(root, index, count, key, value) {
        if (text_of(key) != "parts" && text_of(key) != "variants") {
            return refuse("has more in it than parts and variants: mend it in a text editor");
        }
    }
    yyjson_val* parts_object = yyjson_obj_get(root, "parts");
    if (parts_object != nullptr && !yyjson_is_obj(parts_object)) {
        return refuse("its parts aren't an object of parts by their IDs");
    }
    yyjson_obj_foreach(parts_object, index, count, key, value) {
        PrefabDocPart part;
        if (!part.id.assign(text_of(key)) || !yyjson_is_obj(value)) {
            return refuse("has a part whose ID is too long, or that isn't an object");
        }
        size_t member = 0;
        size_t members = 0;
        yyjson_val* name = nullptr;
        yyjson_val* held = nullptr;
        bool has_quaternion = false;
        bool has_degrees = false;
        yyjson_obj_foreach(value, member, members, name, held) {
            const std::string_view what = text_of(name);
            bool read = true;
            if (what == "mesh") {
                read = yyjson_is_str(held) && part.mesh.assign(text_of(held));
            } else if (what == "effect") {
                read = yyjson_is_str(held) && part.effect.assign(text_of(held));
            } else if (what == "prefab") {
                read = yyjson_is_str(held) && part.prefab.assign(text_of(held));
            } else if (what == "variant") {
                read = yyjson_is_str(held) && part.variant.assign(text_of(held));
            } else if (what == "collision") {
                read = yyjson_is_str(held) && text_of(held) == "mesh";
                part.collision = read;
            } else if (what == "light") {
                read = read_light(held, part.light);
                part.has_light = read;
            } else if (what == "position") {
                read = read_numbers(held, &part.position.x, 3);
            } else if (what == "rotation") {
                float q[4] = {};
                read = read_numbers(held, q, 4) && (q[0] != 0.0f || q[1] != 0.0f || q[2] != 0.0f || q[3] != 0.0f);
                if (read) {
                    part.rotation = glm::normalize(Quat(q[3], q[0], q[1], q[2]));
                    part.rotation_deg = rotation_deg_of(part.rotation);
                    part.quaternion = true;
                    has_quaternion = true;
                }
            } else if (what == "rotation_deg") {
                Vec3 degrees(0.0f);
                read = read_numbers(held, &degrees.x, 3);
                if (read) {
                    part.set_rotation_deg(degrees);
                    has_degrees = true;
                }
            } else if (what == "scale") {
                if (yyjson_is_num(held)) {
                    part.scale = Vec3(static_cast<float>(yyjson_get_num(held)));
                } else {
                    read = read_numbers(held, &part.scale.x, 3);
                }
            } else {
                return refuse("has a part with a key the prefab editor doesn't know: mend it in a text editor");
            }
            if (!read) {
                return refuse("has a part with a value that isn't what its key takes: mend it in a text editor");
            }
        }
        if (has_quaternion && has_degrees) {
            return refuse("has a part with both rotation and rotation_deg: mend it in a text editor");
        }
        parts.push(part);
    }
    yyjson_val* variants_object = yyjson_obj_get(root, "variants");
    if (variants_object != nullptr && !yyjson_is_obj(variants_object)) {
        return refuse("its variants aren't an object of variants by their names");
    }
    yyjson_obj_foreach(variants_object, index, count, key, value) {
        PrefabDocVariant variant;
        if (!variant.name.assign(text_of(key)) || !yyjson_is_obj(value)) {
            return refuse("has a variant whose name is too long, or that isn't an object of what it leaves out");
        }
        const auto which = static_cast<uint32_t>(variants.size());
        variants.push(variant);
        size_t member = 0;
        size_t members = 0;
        yyjson_val* name = nullptr;
        yyjson_val* held = nullptr;
        yyjson_obj_foreach(value, member, members, name, held) {
            PrefabDocLeftOut gone{.variant = which, .path = {}};
            if (!yyjson_is_null(held) || !gone.path.assign(text_of(name))) {
                return refuse("has a variant that names something other than a part it leaves out, as null");
            }
            left_out.push(gone);
        }
    }
    return {};
}

void PrefabDocument::write(core::Array<char>& out) const {
    out.clear();
    append(out, "{\n");
    if (!header.empty()) {
        // Each of its lines inside the object, set in as the parts are.
        append(out, "  ");
        for (const char c : header) {
            out.push(c);
            if (c == '\n') {
                append(out, "  ");
            }
        }
        out.push('\n');
    }
    append(out, "  \"parts\": {");
    size_t longest = 0;
    for (const PrefabDocPart& part : parts) {
        longest = std::max(longest, part.id.view().size());
    }
    for (size_t p = 0; p < parts.size(); ++p) {
        const PrefabDocPart& part = parts[p];
        append(out, p == 0 ? "\n    " : ",\n    ");
        append_quoted(out, part.id.view());
        out.push(':');
        // Their objects lined up under each other.
        for (size_t pad = part.id.view().size(); pad <= longest; ++pad) {
            out.push(' ');
        }
        out.push('{');
        bool first = true;
        const auto member = [&](std::string_view name) {
            append(out, first ? "" : ", ");
            first = false;
            append_quoted(out, name);
            append(out, ": ");
        };
        if (!part.mesh.empty()) {
            member("mesh");
            append_quoted(out, part.mesh.view());
        }
        if (part.collision) {
            member("collision");
            append_quoted(out, "mesh");
        }
        if (!part.effect.empty()) {
            member("effect");
            append_quoted(out, part.effect.view());
        }
        if (part.has_light) {
            // As every source writes one (cell_source.h): its colour, how bright and how far, then a spot's and a
            // flicker's numbers only for those.
            member("light");
            char text[kLightTextSize];
            append(out, std::string_view(text, light_text(part.light, text)));
        }
        if (!part.prefab.empty()) {
            member("prefab");
            append_quoted(out, part.prefab.view());
        }
        if (!part.variant.empty()) {
            member("variant");
            append_quoted(out, part.variant.view());
        }
        const bool at_origin = same_written(part.position.x, 0.0f, kPositionDecimals) &&
                               same_written(part.position.y, 0.0f, kPositionDecimals) &&
                               same_written(part.position.z, 0.0f, kPositionDecimals);
        if (!at_origin) {
            member("position");
            append_numbers(out, &part.position.x, 3, kPositionDecimals);
        }
        if (part.quaternion) {
            const float q[4] = {part.rotation.x, part.rotation.y, part.rotation.z, part.rotation.w};
            const bool unturned = same_written(q[0], 0.0f, kQuaternionDecimals) &&
                                  same_written(q[1], 0.0f, kQuaternionDecimals) &&
                                  same_written(q[2], 0.0f, kQuaternionDecimals);
            if (!unturned) {
                member("rotation");
                append_numbers(out, q, 4, kQuaternionDecimals);
            }
        } else {
            const bool unturned = same_written(part.rotation_deg.x, 0.0f, kAngleDecimals) &&
                                  same_written(part.rotation_deg.y, 0.0f, kAngleDecimals) &&
                                  same_written(part.rotation_deg.z, 0.0f, kAngleDecimals);
            if (!unturned) {
                member("rotation_deg");
                append_numbers(out, &part.rotation_deg.x, 3, kAngleDecimals);
            }
        }
        const bool uniform = same_written(part.scale.x, part.scale.y, kScaleDecimals) &&
                             same_written(part.scale.x, part.scale.z, kScaleDecimals);
        if (!uniform) {
            member("scale");
            append_numbers(out, &part.scale.x, 3, kScaleDecimals);
        } else if (!same_written(part.scale.x, 1.0f, kScaleDecimals)) {
            member("scale");
            append_number(out, static_cast<double>(part.scale.x), kScaleDecimals);
        }
        out.push('}');
    }
    append(out, parts.empty() ? "}" : "\n  }");
    if (!variants.empty()) {
        append(out, ",\n  \"variants\": {");
        for (uint32_t v = 0; v < variants.size(); ++v) {
            append(out, v == 0 ? "\n    " : ",\n    ");
            append_quoted(out, variants[v].name.view());
            append(out, ": {");
            bool first = true;
            for (const PrefabDocLeftOut& gone : left_out) {
                if (gone.variant != v) {
                    continue;
                }
                append(out, first ? "" : ", ");
                first = false;
                append_quoted(out, gone.path.view());
                append(out, ": null");
            }
            out.push('}');
        }
        append(out, "\n  }");
    }
    append(out, "\n}\n");
}

bool PrefabDocument::valid_id(std::string_view id) {
    if (id.empty() || id.size() > asset::PrefabLimits::kMaxIdLength) {
        return false;
    }
    return std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

uint32_t PrefabDocument::find_part(std::string_view id) const {
    for (uint32_t p = 0; p < parts.size(); ++p) {
        if (parts[p].id.view() == id) {
            return p;
        }
    }
    return UINT32_MAX;
}

core::FixedString<31> PrefabDocument::free_id(std::string_view wanted) const {
    // A file's name without its folders and its extension.
    if (const size_t slash = wanted.rfind('/'); slash != std::string_view::npos) {
        wanted.remove_prefix(slash + 1);
    }
    if (const size_t dot = wanted.find('.'); dot != std::string_view::npos) {
        wanted = wanted.substr(0, dot);
    }
    char base[32] = {};
    size_t length = 0;
    for (const char c : wanted) {
        if (length >= asset::PrefabLimits::kMaxIdLength) {
            break;
        }
        const char lower = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        base[length++] = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') ? lower : '_';
    }
    if (length == 0) {
        length = std::string_view("part").copy(base, sizeof(base) - 1);
    }
    core::FixedString<31> id{std::string_view(base, length)};
    for (int n = 2; find_part(id.view()) != UINT32_MAX && n < 100000; ++n) {
        char number[16];
        const int digits = std::snprintf(number, sizeof(number), "_%d", n);
        const size_t keep = std::min(length, size_t{asset::PrefabLimits::kMaxIdLength} - static_cast<size_t>(digits));
        static_cast<void>(id.assign(std::string_view(base, keep)));
        static_cast<void>(id.append(std::string_view(number, static_cast<size_t>(digits))));
    }
    return id;
}

bool PrefabDocument::rename_part(uint32_t index, std::string_view id) {
    if (index >= parts.size() || !valid_id(id)) {
        return false;
    }
    const uint32_t taken = find_part(id);
    if (taken != UINT32_MAX) {
        return taken == index; // its own already
    }
    const core::FixedString<31> was = parts[index].id;
    for (PrefabDocLeftOut& gone : left_out) {
        if (names_part(gone.path.view(), was.view())) {
            core::FixedString<127> renamed{id};
            if (renamed.append(gone.path.view().substr(was.view().size()))) {
                gone.path = renamed;
            }
        }
    }
    static_cast<void>(parts[index].id.assign(id));
    return true;
}

void PrefabDocument::remove_part(uint32_t index) {
    if (index >= parts.size()) {
        return;
    }
    const core::FixedString<31> id = parts[index].id;
    // What's kept moves down over what goes: each is read before the place it's written to is reached.
    size_t kept = 0;
    for (const PrefabDocLeftOut& gone : left_out) {
        if (!names_part(gone.path.view(), id.view())) {
            left_out[kept++] = gone;
        }
    }
    left_out.resize(kept);
    parts.remove_ordered(index);
}

bool PrefabDocument::leaves_out(uint32_t variant, std::string_view path) const {
    return std::ranges::any_of(
        left_out, [&](const PrefabDocLeftOut& gone) { return gone.variant == variant && gone.path.view() == path; });
}

void PrefabDocument::leave_out(uint32_t variant, std::string_view path, bool out) {
    if (variant >= variants.size() || leaves_out(variant, path) == out) {
        return;
    }
    if (out) {
        PrefabDocLeftOut gone{.variant = variant, .path = {}};
        if (gone.path.assign(path)) {
            left_out.push(gone);
        }
        return;
    }
    size_t kept = 0;
    for (const PrefabDocLeftOut& gone : left_out) {
        if (gone.variant != variant || gone.path.view() != path) {
            left_out[kept++] = gone;
        }
    }
    left_out.resize(kept);
}

void PrefabDocument::remove_variant(uint32_t index) {
    if (index >= variants.size()) {
        return;
    }
    size_t kept = 0;
    for (const PrefabDocLeftOut& gone : left_out) {
        if (gone.variant == index) {
            continue;
        }
        // The variants after it are one earlier now.
        PrefabDocLeftOut moved = gone;
        moved.variant -= moved.variant > index ? 1U : 0U;
        left_out[kept++] = moved;
    }
    left_out.resize(kept);
    variants.remove_ordered(index);
}

void PrefabDocument::check(core::Array<core::FixedString<159>>& problems) const {
    problems.clear();
    const auto say = [&problems](const char* format, std::string_view about = {}, std::string_view more = {}) {
        char text[160];
        static_cast<void>(std::snprintf(text, sizeof(text), format, static_cast<int>(about.size()), about.data(),
                                        static_cast<int>(more.size()), more.data()));
        problems.push(core::FixedString<159>(std::string_view(text).substr(0, 159)));
    };
    if (parts.empty()) {
        say("It has no parts yet: drag a model, an effect or a prefab in from Resources.");
    }
    for (uint32_t p = 0; p < parts.size(); ++p) {
        const PrefabDocPart& part = parts[p];
        const std::string_view id = part.id.view();
        if (!valid_id(id)) {
            say("Part '%.*s' needs an ID of a-z, 0-9 and _, at most 31 characters.", id);
        } else if (find_part(id) != p) {
            say("Two parts are called '%.*s': each needs an ID of its own.", id);
        }
        if (!part.prefab.empty() && (!part.mesh.empty() || !part.effect.empty() || part.collision || part.has_light)) {
            say("Part '%.*s' is a prefab: a model, an effect or a light goes in a part of its own.", id);
        } else if (part.prefab.empty() && part.mesh.empty() && part.effect.empty() && !part.has_light) {
            say("Part '%.*s' has nothing in it: give it a model, an effect, a light or a prefab.", id);
        }
        if (part.has_light && !part.light.valid()) {
            say("Part '%.*s' has a light with a value out of range.", id);
        }
        if (part.collision && part.mesh.empty()) {
            say("Part '%.*s' has a body but no model to take its shape from.", id);
        }
        if (!part.variant.empty() && part.prefab.empty()) {
            say("Part '%.*s' names a variant, which only a part that's a prefab has.", id);
        }
        if (part.scale.x == 0.0f || part.scale.y == 0.0f || part.scale.z == 0.0f) {
            say("Part '%.*s' has a scale of zero.", id);
        }
        if (!std::isfinite(glm::length(part.position)) ||
            glm::length(part.position) > asset::PrefabLimits::kMaxReachM) {
            say("Part '%.*s' lies too far from the prefab's origin.", id);
        }
    }
    if (variants.size() > asset::PrefabLimits::kMaxVariants) {
        say("It has more than 32 variants.");
    }
    for (uint32_t v = 0; v < variants.size(); ++v) {
        const std::string_view name = variants[v].name.view();
        if (!valid_id(name)) {
            say("Variant '%.*s' needs a name of a-z, 0-9 and _, at most 31 characters.", name);
        }
        for (uint32_t other = 0; other < v; ++other) {
            if (variants[other].name.view() == name) {
                say("Two variants are called '%.*s'.", name);
            }
        }
    }
    for (const PrefabDocLeftOut& gone : left_out) {
        const std::string_view path = gone.path.view();
        const std::string_view first = path.substr(0, path.find('/'));
        if (gone.variant < variants.size() && find_part(first) == UINT32_MAX) {
            say("Variant '%.*s' leaves out '%.*s', which isn't a part.", variants[gone.variant].name.view(), path);
        }
    }
}

} // namespace de::editor
