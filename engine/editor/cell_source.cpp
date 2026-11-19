#include "editor/cell_source.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <numbers>

namespace de::editor {

namespace {

constexpr int kMaxDepth = 64;

// Reads JSON with comments far enough to know where things are: spans, not values.
class Scanner {
public:
    explicit Scanner(std::string_view text) : text_(text) {}

    uint32_t at() const { return at_; }
    bool done() const { return at_ >= text_.size(); }
    char peek() const { return done() ? '\0' : text_[at_]; }
    void step() { ++at_; }
    void jump(uint32_t to) { at_ = to; }

    // Past spaces and comments; false on a block comment that never ends.
    bool skip() {
        while (!done()) {
            const char c = text_[at_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++at_;
            } else if (c == '/' && at_ + 1 < text_.size() && text_[at_ + 1] == '/') {
                while (!done() && text_[at_] != '\n') {
                    ++at_;
                }
            } else if (c == '/' && at_ + 1 < text_.size() && text_[at_ + 1] == '*') {
                const size_t close = text_.find("*/", at_ + 2);
                if (close == std::string_view::npos) {
                    return false;
                }
                at_ = static_cast<uint32_t>(close + 2);
            } else {
                return true;
            }
        }
        return true;
    }

    // At a quote: the string's contents (escapes left as they are), and past it.
    bool string(uint32_t& begin, uint32_t& end) {
        if (peek() != '"') {
            return false;
        }
        begin = ++at_;
        while (!done()) {
            const char c = text_[at_];
            if (c == '\\') {
                at_ += 2;
            } else if (c == '"') {
                end = at_++;
                return true;
            } else {
                ++at_;
            }
        }
        return false;
    }

    // Any value, from where it starts to past its end.
    bool value(uint32_t& begin, uint32_t& end, int depth = 0) {
        if (depth > kMaxDepth) {
            return false;
        }
        begin = at_;
        const char c = peek();
        if (c == '"') {
            uint32_t inner_begin = 0;
            uint32_t inner_end = 0;
            if (!string(inner_begin, inner_end)) {
                return false;
            }
        } else if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            step();
            while (true) {
                if (!skip()) {
                    return false;
                }
                if (peek() == close) {
                    step();
                    break;
                }
                uint32_t item_begin = 0;
                uint32_t item_end = 0;
                if (c == '{') {
                    if (!string(item_begin, item_end) || !skip() || peek() != ':') {
                        return false;
                    }
                    step();
                    if (!skip()) {
                        return false;
                    }
                }
                if (!value(item_begin, item_end, depth + 1) || !skip()) {
                    return false;
                }
                if (peek() == ',') {
                    step();
                } else if (peek() != close) {
                    return false;
                }
            }
        } else {
            // A number, true, false or null: up to what ends it.
            while (!done()) {
                const char d = text_[at_];
                if (d == ',' || d == '}' || d == ']' || d == ' ' || d == '\t' || d == '\n' || d == '\r' || d == '/') {
                    break;
                }
                ++at_;
            }
            if (at_ == begin) {
                return false;
            }
        }
        end = at_;
        return true;
    }

private:
    std::string_view text_;
    uint32_t at_ = 0;
};

bool parse_numbers(std::string_view text, double* out, size_t count) {
    // "[a, b, c]": the numbers in order.
    char buffer[256];
    if (text.size() >= sizeof(buffer)) {
        return false;
    }
    std::memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    const char* at = buffer;
    for (size_t i = 0; i < count; ++i) {
        while (*at != '\0' && *at != '-' && *at != '.' && (*at < '0' || *at > '9')) {
            ++at;
        }
        char* end = nullptr;
        const double value = std::strtod(at, &end);
        if (end == at) {
            return false;
        }
        out[i] = value;
        at = end;
    }
    return true;
}

// "[a, b, c]" to the decimals given.
size_t format_list(const double* values, size_t count, int decimals, std::span<char> out) {
    size_t length = 0;
    out[length++] = '[';
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            out[length++] = ',';
            out[length++] = ' ';
        }
        length += format_number(values[i], decimals, out.subspan(length));
    }
    out[length++] = ']';
    return length;
}

// Whether a value's text already holds what written says, to its decimals, however it's laid out (one number or a
// list, on one line or many): then it's left alone.
bool same_value(std::string_view old, std::string_view written, int decimals) {
    const bool old_list = old.find('[') != std::string_view::npos;
    const bool new_list = written.find('[') != std::string_view::npos;
    if (old_list != new_list) {
        return false;
    }
    const size_t count = new_list ? static_cast<size_t>(std::count(written.begin(), written.end(), ',')) + 1 : 1;
    if (count > 4 || static_cast<size_t>(std::count(old.begin(), old.end(), ',')) + 1 != count) {
        return false;
    }
    double values[4] = {};
    if (!parse_numbers(old, values, count)) {
        return false;
    }
    char canonical[256];
    size_t length = 0;
    if (new_list) {
        double numbers[4] = {};
        std::copy_n(values, count, numbers);
        length = format_list(numbers, count, decimals, canonical);
    } else {
        length = format_number(values[0], decimals, canonical);
    }
    return std::string_view(canonical, length) == written;
}

} // namespace

size_t format_number(double value, int decimals, std::span<char> out) {
    const double scale = std::pow(10.0, decimals);
    double rounded = std::round(value * scale) / scale;
    if (rounded == 0.0) {
        rounded = 0.0; // never "-0"
    }
    const int written = std::snprintf(out.data(), out.size(), "%.*f", decimals, rounded);
    if (written <= 0) {
        return 0;
    }
    size_t length = std::min(static_cast<size_t>(written), out.size() - 1);
    if (std::memchr(out.data(), '.', length) != nullptr) {
        while (length > 0 && out[length - 1] == '0') {
            --length;
        }
        if (length > 0 && out[length - 1] == '.') {
            --length;
        }
    }
    return length;
}

size_t light_text(const asset::LightDesc& light, std::span<char> out) {
    size_t length = 0;
    const auto text = [&](std::string_view words) {
        const size_t room = out.size() - 1 - length;
        const size_t count = std::min(words.size(), room);
        std::memcpy(out.data() + length, words.data(), count);
        length += count;
    };
    const auto number = [&](std::string_view key, float value, int decimals) {
        text(", \"");
        text(key);
        text("\": ");
        char written[48];
        text(std::string_view(written, format_number(static_cast<double>(value), decimals, written)));
    };
    text("{\"color\": [");
    for (int channel = 0; channel < 3; ++channel) {
        char written[48];
        text(channel > 0 ? ", " : "");
        text(std::string_view(written,
                              format_number(static_cast<double>(light.color[channel]), kScaleDecimals, written)));
    }
    text("]");
    number("intensity", light.intensity, kScaleDecimals);
    number("radius_m", light.radius_m, kPositionDecimals);
    char zero[48];
    const std::string_view none(zero, format_number(0.0, kAngleDecimals, zero));
    char cone[48];
    if (std::string_view(cone, format_number(static_cast<double>(light.cone_deg), kAngleDecimals, cone)) != none) {
        number("cone_deg", light.cone_deg, kAngleDecimals);
        number("cone_soft", light.cone_soft, kScaleDecimals);
    }
    char flicker[48];
    if (std::string_view(flicker, format_number(static_cast<double>(light.flicker), kScaleDecimals, flicker)) != none) {
        number("flicker", light.flicker, kScaleDecimals);
        number("flicker_hz", light.flicker_hz, kAngleDecimals);
    }
    char one[48];
    const std::string_view all(one, format_number(1.0, kScaleDecimals, one));
    char shadow[48];
    if (std::string_view(shadow, format_number(static_cast<double>(light.shadow), kScaleDecimals, shadow)) != all) {
        number("shadow", light.shadow, kScaleDecimals);
    }
    text("}");
    out[length] = '\0';
    return length;
}

Vec3 rotation_deg_of(const Quat& rotation, const Vec3& near_deg) {
    // q = Y(yaw) * X(pitch) * Z(roll): its matrix's third column is (sin y cos x, -sin x, cos y cos x) and its second
    // row is (cos x sin z, cos x cos z, -sin x).
    const Mat3 m = glm::mat3_cast(glm::normalize(rotation));
    const float sin_pitch = std::clamp(-m[2][1], -1.0f, 1.0f);
    const float pitch = std::asin(sin_pitch);
    float yaw = 0.0f;
    float roll = 0.0f;
    if (std::abs(sin_pitch) < 0.99999f) {
        yaw = std::atan2(m[2][0], m[2][2]);
        roll = std::atan2(m[0][1], m[1][1]);
    } else {
        // Straight up or down: yaw and roll turn about one axis; it's all yaw.
        yaw = std::atan2(-m[0][2], m[0][0]);
    }
    constexpr float kDegrees = 180.0f / std::numbers::pi_v<float>;
    Vec3 degrees(pitch * kDegrees, yaw * kDegrees, roll * kDegrees);
    for (int i = 0; i < 3; ++i) {
        degrees[i] += 360.0f * std::round((near_deg[i] - degrees[i]) / 360.0f);
    }
    return degrees;
}

core::Result<void> CellSource::parse(std::string_view text) {
    if (text.data() != text_.data()) {
        text_.resize(text.size());
        std::memcpy(text_.data(), text.data(), text.size());
    }
    entities_.clear();
    coord_value_ = {};
    entities_array_ = {};
    const std::string_view source(text_.data(), text_.size());
    Scanner scan(source);
    if (!scan.skip() || scan.peek() != '{') {
        return core::fail(core::ErrorCode::CorruptData, "a cell should be a JSON object");
    }
    scan.step();
    bool found = false;
    while (true) {
        if (!scan.skip()) {
            return core::fail(core::ErrorCode::CorruptData, "a comment in the cell never ends");
        }
        if (scan.peek() == '}') {
            break;
        }
        uint32_t key_begin = 0;
        uint32_t key_end = 0;
        if (!scan.string(key_begin, key_end) || !scan.skip() || scan.peek() != ':') {
            return core::fail(core::ErrorCode::CorruptData, "the cell's keys couldn't be read");
        }
        scan.step();
        if (!scan.skip()) {
            return core::fail(core::ErrorCode::CorruptData, "the cell couldn't be read");
        }
        const std::string_view key = source.substr(key_begin, key_end - key_begin);
        if (key == "entities") {
            if (scan.peek() != '[') {
                return core::fail(core::ErrorCode::CorruptData, "the cell's entities should be an array");
            }
            found = true;
            entities_array_.begin = scan.at();
            scan.step();
            while (true) {
                if (!scan.skip()) {
                    return core::fail(core::ErrorCode::CorruptData, "a comment in the cell never ends");
                }
                if (scan.peek() == ']') {
                    scan.step();
                    entities_array_.end = scan.at();
                    break;
                }
                if (scan.peek() != '{') {
                    return core::fail(core::ErrorCode::CorruptData, "each of the cell's entities should be an object");
                }
                Entry entry;
                entry.object.begin = scan.at();
                scan.step();
                while (true) {
                    if (!scan.skip()) {
                        return core::fail(core::ErrorCode::CorruptData, "a comment in the cell never ends");
                    }
                    if (scan.peek() == '}') {
                        scan.step();
                        break;
                    }
                    uint32_t member_begin = 0;
                    uint32_t member_end = 0;
                    const uint32_t quote = scan.at();
                    if (!scan.string(member_begin, member_end) || !scan.skip() || scan.peek() != ':') {
                        return core::fail(core::ErrorCode::CorruptData, "an entity's keys couldn't be read");
                    }
                    if (entry.first_key == 0) {
                        entry.first_key = quote;
                    }
                    scan.step();
                    Span value;
                    if (!scan.skip() || !scan.value(value.begin, value.end) || !scan.skip()) {
                        return core::fail(core::ErrorCode::CorruptData, "an entity's value couldn't be read");
                    }
                    const std::string_view member = source.substr(member_begin, member_end - member_begin);
                    entry.last_value = value;
                    if (member == "name") {
                        entry.name_value = value;
                        if (value.end - value.begin >= 2 && source[value.begin] == '"') {
                            entry.name = {.begin = value.begin + 1, .end = value.end - 1};
                        }
                    } else if (member == "parent") {
                        entry.parent_value = value;
                    } else if (member == "mesh") {
                        entry.mesh_value = value;
                    } else if (member == "position") {
                        entry.position_value = value;
                    } else if (member == "rotation") {
                        entry.rotation_value = value;
                    } else if (member == "rotation_deg") {
                        entry.rotation_deg_value = value;
                    } else if (member == "scale") {
                        entry.scale_value = value;
                    } else if (member == "fog_volume") {
                        entry.fog_value = value;
                    }
                    if (scan.peek() == ',') {
                        scan.step();
                    } else if (scan.peek() != '}') {
                        return core::fail(core::ErrorCode::CorruptData, "an entity's keys should be apart by commas");
                    }
                }
                entry.object.end = scan.at();
                entities_.push(entry);
                if (!scan.skip()) {
                    return core::fail(core::ErrorCode::CorruptData, "a comment in the cell never ends");
                }
                if (scan.peek() == ',') {
                    scan.step();
                } else if (scan.peek() != ']') {
                    return core::fail(core::ErrorCode::CorruptData, "the cell's entities should be apart by commas");
                }
            }
        } else {
            uint32_t begin = 0;
            uint32_t end = 0;
            if (!scan.value(begin, end)) {
                return core::fail(core::ErrorCode::CorruptData, "a value in the cell couldn't be read");
            }
            if (key == "coord") {
                coord_value_ = {.begin = begin, .end = end};
            }
        }
        if (!scan.skip()) {
            return core::fail(core::ErrorCode::CorruptData, "a comment in the cell never ends");
        }
        if (scan.peek() == ',') {
            scan.step();
        } else if (scan.peek() != '}') {
            return core::fail(core::ErrorCode::CorruptData, "the cell's keys should be apart by commas");
        }
    }
    if (!found) {
        return core::fail(core::ErrorCode::CorruptData, "the cell has no entities array");
    }
    return {};
}

std::string_view CellSource::name(uint32_t index) const {
    return index < entities_.size() ? slice(entities_[index].name) : std::string_view();
}

void CellSource::replace(uint32_t begin, uint32_t end, std::string_view with) {
    core::Array<char> edited(core::heap_allocator(core::MemoryTag::Editor));
    edited.resize(text_.size() - (end - begin) + with.size());
    std::memcpy(edited.data(), text_.data(), begin);
    std::memcpy(edited.data() + begin, with.data(), with.size());
    std::memcpy(edited.data() + begin + with.size(), text_.data() + end, text_.size() - end);
    text_.resize(edited.size());
    std::memcpy(text_.data(), edited.data(), edited.size());
    // The spans after the edit moved: read it again (the text was valid, and the edit keeps it so).
    static_cast<void>(parse({text_.data(), text_.size()}));
}

void CellSource::add_key(const Entry& entry, Span anchor, std::string_view key, std::string_view value) {
    char inserted[256];
    int length = 0;
    if (!anchor.found()) {
        // An entry with no keys: the first one, right after its brace.
        length = std::snprintf(inserted, sizeof(inserted), "\"%.*s\": %.*s", static_cast<int>(key.size()), key.data(),
                               static_cast<int>(value.size()), value.data());
        if (length > 0) {
            replace(entry.object.begin + 1, entry.object.begin + 1, {inserted, static_cast<size_t>(length)});
        }
        return;
    }
    // Keys on lines of their own: the new one on its own line, at the first key's indent.
    const std::string_view head = slice({.begin = entry.object.begin, .end = entry.first_key});
    const size_t newline = head.rfind('\n');
    if (newline != std::string_view::npos) {
        const std::string_view indent = head.substr(newline + 1);
        length = std::snprintf(inserted, sizeof(inserted), ",\n%.*s\"%.*s\": %.*s", static_cast<int>(indent.size()),
                               indent.data(), static_cast<int>(key.size()), key.data(), static_cast<int>(value.size()),
                               value.data());
    } else {
        length = std::snprintf(inserted, sizeof(inserted), ", \"%.*s\": %.*s", static_cast<int>(key.size()), key.data(),
                               static_cast<int>(value.size()), value.data());
    }
    if (length > 0) {
        replace(anchor.end, anchor.end, {inserted, static_cast<size_t>(length)});
    }
}

bool CellSource::set_transform(uint32_t index, const SourceTransform& transform, TransformParts parts) {
    if (index >= entities_.size()) {
        return false;
    }
    char value[256];
    const auto pick = [](std::initializer_list<Span> spans) {
        for (const Span span : spans) {
            if (span.found()) {
                return span;
            }
        }
        return Span{};
    };

    // The position.
    if (parts.position) {
        const Entry entry = entities_[index];
        const double position[3] = {transform.position.x, transform.position.y, transform.position.z};
        const size_t length = format_list(position, 3, kPositionDecimals, value);
        const std::string_view written(value, length);
        if (entry.position_value.found()) {
            if (!same_value(slice(entry.position_value), written, kPositionDecimals)) {
                replace(entry.position_value.begin, entry.position_value.end, written);
            }
        } else if (written != "[0, 0, 0]") {
            add_key(entry, pick({entry.mesh_value, entry.parent_value, entry.name_value, entry.last_value}), "position",
                    written);
        }
    }

    // The rotation, as the entry writes it.
    if (parts.rotation) {
        const Entry entry = entities_[index];
        const Quat q = glm::normalize(transform.rotation);
        if (entry.rotation_value.found()) {
            // As the quaternion it was, the sign that's nearer it.
            double old[4] = {0.0, 0.0, 0.0, 1.0};
            const bool read = parse_numbers(slice(entry.rotation_value), old, 4);
            const double dot = (old[0] * q.x) + (old[1] * q.y) + (old[2] * q.z) + (old[3] * q.w);
            const double sign = read && dot < 0.0 ? -1.0 : 1.0;
            const double components[4] = {sign * q.x, sign * q.y, sign * q.z, sign * q.w};
            const size_t length = format_list(components, 4, kQuaternionDecimals, value);
            const std::string_view written(value, length);
            if (!same_value(slice(entry.rotation_value), written, kQuaternionDecimals)) {
                replace(entry.rotation_value.begin, entry.rotation_value.end, written);
            }
        } else {
            double old[3] = {0.0, 0.0, 0.0};
            if (entry.rotation_deg_value.found()) {
                static_cast<void>(parse_numbers(slice(entry.rotation_deg_value), old, 3));
            }
            const Vec3 degrees = rotation_deg_of(
                q, Vec3(static_cast<float>(old[0]), static_cast<float>(old[1]), static_cast<float>(old[2])));
            const double angles[3] = {degrees.x, degrees.y, degrees.z};
            const size_t length = format_list(angles, 3, kAngleDecimals, value);
            const std::string_view written(value, length);
            if (entry.rotation_deg_value.found()) {
                if (!same_value(slice(entry.rotation_deg_value), written, kAngleDecimals)) {
                    replace(entry.rotation_deg_value.begin, entry.rotation_deg_value.end, written);
                }
            } else if (written != "[0, 0, 0]") {
                add_key(entry,
                        pick({entry.position_value, entry.mesh_value, entry.parent_value, entry.name_value,
                              entry.last_value}),
                        "rotation_deg", written);
            }
        }
    }

    // The scale: one number while it's the same along every axis.
    if (parts.scale) {
        const Entry entry = entities_[index];
        const Vec3 s = transform.scale;
        const bool uniform =
            std::abs(s.x - s.y) <= 1e-6f * std::abs(s.x) && std::abs(s.x - s.z) <= 1e-6f * std::abs(s.x);
        size_t length = 0;
        if (uniform) {
            length = format_number(s.x, kScaleDecimals, value);
        } else {
            const double scale[3] = {s.x, s.y, s.z};
            length = format_list(scale, 3, kScaleDecimals, value);
        }
        const std::string_view written(value, length);
        if (entry.scale_value.found()) {
            if (!same_value(slice(entry.scale_value), written, kScaleDecimals)) {
                replace(entry.scale_value.begin, entry.scale_value.end, written);
            }
        } else if (written != "1") {
            add_key(entry,
                    pick({entry.rotation_deg_value, entry.rotation_value, entry.position_value, entry.mesh_value,
                          entry.parent_value, entry.name_value, entry.last_value}),
                    "scale", written);
        }
    }
    return true;
}

bool CellSource::set_fog_member(uint32_t index, std::string_view key, std::string_view value, int decimals) {
    return index < entities_.size() && set_in_object(entities_[index].fog_value, key, value, decimals);
}

bool CellSource::set_entity_member(uint32_t index, std::string_view key, std::string_view value) {
    return index < entities_.size() && set_in_object(entities_[index].object, key, value, -1);
}

bool CellSource::set_in_object(Span object, std::string_view key, std::string_view value, int decimals) {
    const std::string_view source(text_.data(), text_.size());
    if (!object.found() || source[object.begin] != '{') {
        return false;
    }
    // Its members, read again now: the spans of the last edit are gone.
    Scanner scan(source.substr(0, object.end));
    scan.jump(object.begin + 1); // past its brace
    uint32_t first_key = 0;
    Span last;
    while (scan.skip() && !scan.done() && scan.peek() != '}') {
        uint32_t key_begin = 0;
        uint32_t key_end = 0;
        const uint32_t quote = scan.at();
        if (!scan.string(key_begin, key_end) || !scan.skip() || scan.peek() != ':') {
            return false;
        }
        scan.step();
        Span member;
        if (!scan.skip() || !scan.value(member.begin, member.end) || !scan.skip()) {
            return false;
        }
        if (first_key == 0) {
            first_key = quote;
        }
        last = member;
        if (source.substr(key_begin, key_end - key_begin) == key) {
            const std::string_view old = slice(member);
            const bool same = decimals < 0 ? old == value : same_value(old, value, decimals);
            if (!same) {
                replace(member.begin, member.end, value);
            }
            return true;
        }
        if (scan.peek() == ',') {
            scan.step();
        }
    }
    // Not there: after its last key, in its layout.
    Entry inner;
    inner.object = object;
    inner.first_key = first_key;
    add_key(inner, last, key, value);
    return true;
}

bool CellSource::remove_entity_member(uint32_t index, std::string_view key) {
    if (index >= entities_.size()) {
        return false;
    }
    const Span object = entities_[index].object;
    const std::string_view source(text_.data(), text_.size());
    Scanner scan(source.substr(0, object.end));
    scan.jump(object.begin + 1); // past its brace
    uint32_t previous_end = 0;   // past the member before it: where a comma before it would be
    while (scan.skip() && !scan.done() && scan.peek() != '}') {
        const uint32_t quote = scan.at();
        uint32_t key_begin = 0;
        uint32_t key_end = 0;
        if (!scan.string(key_begin, key_end) || !scan.skip() || scan.peek() != ':') {
            return false;
        }
        scan.step();
        Span value;
        if (!scan.skip() || !scan.value(value.begin, value.end)) {
            return false;
        }
        const uint32_t after_value = value.end;
        if (!scan.skip()) {
            return false;
        }
        const bool comma = scan.peek() == ',';
        if (comma) {
            scan.step();
        }
        if (source.substr(key_begin, key_end - key_begin) == key) {
            if (comma) {
                // "key": value, | the next: up to where the next starts.
                const bool more = scan.skip();
                replace(quote, more ? scan.at() : after_value + 1, {});
            } else if (previous_end != 0) {
                // The last: from the end of the one before it, its comma with it.
                replace(previous_end, after_value, {});
            } else {
                replace(quote, after_value, {}); // the only one
            }
            return true;
        }
        previous_end = after_value;
    }
    return false;
}

bool CellSource::coord(int32_t& x, int32_t& z) const {
    double values[2] = {};
    if (!coord_value_.found() || !parse_numbers(slice(coord_value_), values, 2)) {
        return false;
    }
    x = static_cast<int32_t>(values[0]);
    z = static_cast<int32_t>(values[1]);
    return true;
}

std::string_view CellSource::parent(uint32_t index) const {
    if (index >= entities_.size()) {
        return {};
    }
    const std::string_view value = slice(entities_[index].parent_value);
    return value.size() >= 2 && value.front() == '"' ? value.substr(1, value.size() - 2) : std::string_view();
}

std::string_view CellSource::mesh(uint32_t index) const {
    if (index >= entities_.size()) {
        return {};
    }
    const std::string_view value = slice(entities_[index].mesh_value);
    return value.size() >= 2 && value.front() == '"' ? value.substr(1, value.size() - 2) : std::string_view();
}

std::string_view CellSource::string_member(uint32_t index, std::string_view key) const {
    if (index >= entities_.size()) {
        return {};
    }
    const Span object = entities_[index].object;
    const std::string_view source(text_.data(), text_.size());
    Scanner scan(source.substr(0, object.end));
    scan.jump(object.begin + 1);
    while (scan.skip() && !scan.done() && scan.peek() != '}') {
        uint32_t key_begin = 0;
        uint32_t key_end = 0;
        if (!scan.string(key_begin, key_end) || !scan.skip() || scan.peek() != ':') {
            return {};
        }
        scan.step();
        Span value;
        if (!scan.skip() || !scan.value(value.begin, value.end) || !scan.skip()) {
            return {};
        }
        if (source.substr(key_begin, key_end - key_begin) == key) {
            const std::string_view text = slice(value);
            return text.size() >= 2 && text.front() == '"' ? text.substr(1, text.size() - 2) : std::string_view();
        }
        if (scan.peek() == ',') {
            scan.step();
        }
    }
    return {};
}

std::string_view CellSource::entity_text(uint32_t index) const {
    return index < entities_.size() ? slice(entities_[index].object) : std::string_view();
}

bool CellSource::remove_entity(uint32_t index) {
    if (index >= entities_.size()) {
        return false;
    }
    const std::string_view source(text_.data(), text_.size());
    const Span object = entities_[index].object;
    const auto spaces = [&](uint32_t at) {
        while (at < source.size() && (source[at] == ' ' || source[at] == '\t')) {
            ++at;
        }
        return at;
    };
    // What follows it on its line: spaces, its comma, a comment.
    uint32_t after = spaces(object.end);
    const bool comma_after = after < source.size() && source[after] == ',';
    if (comma_after) {
        after = spaces(after + 1);
    }
    if (source.substr(after, 2) == "//") {
        while (after < source.size() && source[after] != '\n' && source[after] != '\r') {
            ++after;
        }
    }
    const bool line_ends = after < source.size() && (source[after] == '\n' || source[after] == '\r');
    const uint32_t line_end = !line_ends ? after : after + (source[after] == '\r' ? 2U : 1U);
    // Before it on its line: only spaces?
    uint32_t line_start = object.begin;
    while (line_start > 0 && (source[line_start - 1] == ' ' || source[line_start - 1] == '\t')) {
        --line_start;
    }
    const bool own_line = line_ends && line_start > 0 && source[line_start - 1] == '\n';
    // The last one takes the comma before it, so none is left hanging after the one before.
    uint32_t comma_before = object.begin;
    if (!comma_after && index > 0) {
        const size_t comma = source.find(',', entities_[index - 1].object.end);
        if (comma != std::string_view::npos && comma < object.begin) {
            comma_before = static_cast<uint32_t>(comma);
        }
    }
    const uint32_t begin = own_line ? line_start : object.begin;
    const uint32_t end = own_line ? line_end : (comma_after ? after : object.end);
    replace(begin, end, {});
    if (comma_before < object.begin) {
        replace(comma_before, comma_before + 1, {}); // before the part removed: its place hasn't moved
    }
    return true;
}

uint32_t CellSource::add_entity(std::string_view object) {
    const auto count = static_cast<uint32_t>(entities_.size());
    const std::string_view source(text_.data(), text_.size());
    core::Array<char> inserted(core::heap_allocator(core::MemoryTag::Editor));
    const auto append = [&inserted](std::string_view part) {
        const size_t at = inserted.size();
        inserted.resize(at + part.size());
        std::memcpy(inserted.data() + at, part.data(), part.size());
    };
    const uint32_t close = entities_array_.end - 1;
    if (count == 0) {
        // An empty list: the entity on a line of its own, a step in from the line the list starts on.
        const uint32_t open = entities_array_.begin;
        uint32_t line = open;
        while (line > 0 && source[line - 1] != '\n') {
            --line;
        }
        uint32_t indent_end = line;
        while (indent_end < open && (source[indent_end] == ' ' || source[indent_end] == '\t')) {
            ++indent_end;
        }
        const std::string_view indent = source.substr(line, indent_end - line);
        append("\n");
        append(indent);
        append("  ");
        append(object);
        append("\n");
        append(indent);
        replace(open + 1, close, {inserted.data(), inserted.size()});
        return 0;
    }
    const Span last = entities_[count - 1].object;
    uint32_t line_start = last.begin;
    while (line_start > 0 && (source[line_start - 1] == ' ' || source[line_start - 1] == '\t')) {
        --line_start;
    }
    const bool own_line = line_start > 0 && source[line_start - 1] == '\n';
    const size_t newline = source.find('\n', last.end);
    if (own_line && newline != std::string_view::npos && newline < close) {
        // A comma after the last one if it had none; the entity on the next line, at the last one's indent.
        auto at = static_cast<uint32_t>(newline);
        if (at > 0 && source[at - 1] == '\r') {
            --at;
        }
        uint32_t look = last.end;
        while (look < at && (source[look] == ' ' || source[look] == '\t')) {
            ++look;
        }
        const bool has_comma = look < at && source[look] == ',';
        append("\n");
        append(source.substr(line_start, last.begin - line_start));
        append(object);
        replace(at, at, {inserted.data(), inserted.size()});
        if (!has_comma) {
            replace(last.end, last.end, ","); // before the insertion: its place hasn't moved
        }
    } else {
        append(", ");
        append(object);
        replace(last.end, last.end, {inserted.data(), inserted.size()});
    }
    return count;
}

void new_cell_text(int32_t x, int32_t z, core::Array<char>& out) {
    char text[128];
    const int length = std::snprintf(text, sizeof(text), "{\n  \"coord\": [%d, %d],\n  \"entities\": []\n}\n", x, z);
    out.resize(length > 0 ? static_cast<size_t>(length) : 0);
    std::memcpy(out.data(), text, out.size());
}

namespace {

// The "cells" array of a .world: where it opens and closes, and each string in it (quotes excluded).
bool find_world_cells(std::string_view world, uint32_t& open, uint32_t& close,
                      core::FunctionRef<void(uint32_t begin, uint32_t end)> each) {
    Scanner scan(world);
    if (!scan.skip() || scan.peek() != '{') {
        return false;
    }
    scan.step();
    while (scan.skip() && !scan.done() && scan.peek() != '}') {
        uint32_t key_begin = 0;
        uint32_t key_end = 0;
        if (!scan.string(key_begin, key_end) || !scan.skip() || scan.peek() != ':') {
            return false;
        }
        scan.step();
        if (!scan.skip()) {
            return false;
        }
        if (world.substr(key_begin, key_end - key_begin) == "cells") {
            if (scan.peek() != '[') {
                return false;
            }
            open = scan.at();
            scan.step();
            while (scan.skip() && !scan.done() && scan.peek() != ']') {
                uint32_t begin = 0;
                uint32_t end = 0;
                if (!scan.string(begin, end) || !scan.skip()) {
                    return false;
                }
                each(begin, end);
                if (scan.peek() == ',') {
                    scan.step();
                }
            }
            if (scan.peek() != ']') {
                return false;
            }
            close = scan.at();
            return true;
        }
        uint32_t begin = 0;
        uint32_t end = 0;
        if (!scan.value(begin, end) || !scan.skip()) {
            return false;
        }
        if (scan.peek() == ',') {
            scan.step();
        }
    }
    return false;
}

} // namespace

bool world_cells(std::string_view world, core::FunctionRef<void(std::string_view cell)> visit) {
    uint32_t open = 0;
    uint32_t close = 0;
    return find_world_cells(world, open, close,
                            [&](uint32_t begin, uint32_t end) { visit(world.substr(begin, end - begin)); });
}

bool add_world_cell(std::string_view world, std::string_view cell, core::Array<char>& out) {
    uint32_t open = 0;
    uint32_t close = 0;
    uint32_t last_begin = 0; // the last string's opening quote
    uint32_t last_end = 0;   // past its closing quote; 0: there's none
    if (!find_world_cells(world, open, close, [&](uint32_t begin, uint32_t end) {
            last_begin = begin - 1;
            last_end = end + 1;
        })) {
        return false;
    }
    char inserted[512];
    int length = 0;
    uint32_t at = 0;
    if (last_end == 0) {
        at = open + 1;
        length = std::snprintf(inserted, sizeof(inserted), "\"%.*s\"", static_cast<int>(cell.size()), cell.data());
    } else {
        // On a line of its own if the last one was, else after it on its line.
        uint32_t line_start = last_begin;
        while (line_start > 0 && (world[line_start - 1] == ' ' || world[line_start - 1] == '\t')) {
            --line_start;
        }
        at = last_end;
        if (line_start > 0 && world[line_start - 1] == '\n') {
            const std::string_view indent = world.substr(line_start, last_begin - line_start);
            length = std::snprintf(inserted, sizeof(inserted), ",\n%.*s\"%.*s\"", static_cast<int>(indent.size()),
                                   indent.data(), static_cast<int>(cell.size()), cell.data());
        } else {
            length =
                std::snprintf(inserted, sizeof(inserted), ", \"%.*s\"", static_cast<int>(cell.size()), cell.data());
        }
    }
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(inserted)) {
        return false;
    }
    const auto added = static_cast<size_t>(length);
    out.resize(world.size() + added);
    std::memcpy(out.data(), world.data(), at);
    std::memcpy(out.data() + at, inserted, added);
    std::memcpy(out.data() + at + added, world.data() + at, world.size() - at);
    return true;
}

bool joined_path(std::string_view folder, std::string_view relative, std::span<char> out, size_t& length) {
    // The mount ("src:/") stays; the rest is a stack of names.
    const size_t mount = folder.find(":/");
    if (mount == std::string_view::npos || mount + 2 >= out.size()) {
        return false;
    }
    length = mount + 2;
    std::memcpy(out.data(), folder.data(), length);
    const size_t root = length;
    const auto push = [&](std::string_view part) {
        if (part.empty() || part == ".") {
            return true;
        }
        if (part == "..") {
            if (length == root) {
                return false; // out of the mount
            }
            while (length > root && out[length - 1] != '/') {
                --length;
            }
            if (length > root) {
                --length; // its slash
            }
            return true;
        }
        const size_t needed = (length > root ? 1 : 0) + part.size();
        if (length + needed >= out.size()) {
            return false;
        }
        if (length > root) {
            out[length++] = '/';
        }
        std::memcpy(out.data() + length, part.data(), part.size());
        length += part.size();
        return true;
    };
    for (const std::string_view text : {folder.substr(mount + 2), relative}) {
        size_t at = 0;
        while (at <= text.size()) {
            const size_t slash = std::min(text.find('/', at), text.size());
            if (!push(text.substr(at, slash - at))) {
                return false;
            }
            at = slash + 1;
        }
    }
    return true;
}

bool relative_path(std::string_view folder, std::string_view path, std::span<char> out, size_t& length) {
    // Past the folders they share: a ".." for each of the folder's own left, then the rest of the path.
    size_t common = 0; // past the last slash both have at the same place
    size_t at = 0;
    while (at < folder.size() && at < path.size() && folder[at] == path[at]) {
        if (folder[at] == '/') {
            common = at + 1;
        }
        ++at;
    }
    if (at == folder.size() && at < path.size() && path[at] == '/') {
        common = at + 1; // the whole folder is shared
    }
    const std::string_view rest = common >= folder.size() ? std::string_view() : folder.substr(common);
    const size_t ups = rest.empty() ? 0 : 1 + static_cast<size_t>(std::count(rest.begin(), rest.end(), '/'));
    length = 0;
    for (size_t i = 0; i < ups; ++i) {
        if (length + 3 >= out.size()) {
            return false;
        }
        std::memcpy(out.data() + length, "../", 3);
        length += 3;
    }
    const std::string_view tail = path.substr(common);
    if (length + tail.size() >= out.size()) {
        return false;
    }
    std::memcpy(out.data() + length, tail.data(), tail.size());
    length += tail.size();
    return true;
}

} // namespace de::editor
