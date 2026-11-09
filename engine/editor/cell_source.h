#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/function_ref.h"
#include "core/math.h"
#include "core/result.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace de::editor {

// A .cell source's text, edited in place for the editor's saving (docs/design.md, Editor & tooling: edits save to the
// text sources). Only the values that change are rewritten: comments, the order of keys, other keys (fog volumes,
// collision) and each entry's layout, one line or many, stay as they were, so a save diffs as the edit it is.
//
//     CellSource source(allocator);
//     if (source.parse(text)) {
//         source.set_transform(3, {.position = moved, .rotation = turned, .scale = scale});
//         write(source.text());
//     }

// A cell entity's transform as its source states it: relative to its parent, or the world without one.
struct SourceTransform {
    DVec3 position{0.0};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};
};

// Which of a transform's parts to write (a part that didn't change is left as the file has it: rotation_deg [180, 0,
// 180] isn't rewritten as [0, 180, 0] just because the entity moved).
struct TransformParts {
    bool position = true;
    bool rotation = true;
    bool scale = true;
};

// How a source's numbers are written: a position to a tenth of a millimetre, an angle to a thousandth of a degree, a
// scale to four places, a quaternion to six; trailing zeros dropped ("64.5", "61", never "-0").
inline constexpr int kPositionDecimals = 4;
inline constexpr int kAngleDecimals = 3;
inline constexpr int kScaleDecimals = 4;
inline constexpr int kQuaternionDecimals = 6;

// rotation_deg's angles for a rotation, as the cooker reads them: about Y, then X, then Z (q = Y * X * Z), in degrees.
// Each angle is the one of its turns nearest near_deg's (a file's 260 stays 260, not -100).
Vec3 rotation_deg_of(const Quat& rotation, const Vec3& near_deg = Vec3(0.0f));

// A new .cell's text: its coord and one entity per line, as a person would lay it out.
void new_cell_text(int32_t x, int32_t z, core::Array<char>& out);
// Each cell file a .world's "cells" lists, as written (relative to the world's folder). False if it has no such list.
bool world_cells(std::string_view world, core::FunctionRef<void(std::string_view cell)> visit);
// A .world's text with one more cell file ("cell_1_0.cell") at the end of its cells list, in the list's layout.
bool add_world_cell(std::string_view world, std::string_view cell, core::Array<char>& out);
// A path relative to a folder: "src:/content/a/b/c.gltf" from "src:/content/a/d" is "../b/c.gltf". Both under one
// mount, folders without a trailing slash. False if it doesn't fit.
bool relative_path(std::string_view folder, std::string_view path, std::span<char> out, size_t& length);
// A path from a folder and a relative path, its ".." steps resolved: "src:/content/a/d" and "../b/c.gltf" make
// "src:/content/a/b/c.gltf". False if it climbs out of the mount or doesn't fit.
bool joined_path(std::string_view folder, std::string_view relative, std::span<char> out, size_t& length);

// A number as a source writes it, to that many decimals: into out (64 chars is plenty), its length returned.
size_t format_number(double value, int decimals, std::span<char> out);

class CellSource {
public:
    explicit CellSource(core::Allocator& allocator) : text_(allocator), entities_(allocator) {}

    // Takes a .cell's text (JSON with comments) and finds its entities. Fails, saying why, if it isn't an object with
    // an entities array of objects.
    core::Result<void> parse(std::string_view text);

    size_t entity_count() const { return entities_.size(); }
    // The name of the entity at that place in the source ("" without one).
    std::string_view name(uint32_t index) const;

    // Writes the entity's transform: each of position, rotation and scale only if its written value changes (a missing
    // key is added only if its value isn't the default: no position at the origin, no turn, a scale of 1). A rotation
    // is written as the entry already writes it: rotation_deg (the default), or rotation as a quaternion. A uniform
    // scale is one number. Only the parts asked for. False if there's no entity at that place.
    bool set_transform(uint32_t index, const SourceTransform& transform, TransformParts parts = {});

    // Writes one key of the entity's fog_volume object: value (already written as the source writes it) replaces what's
    // there unless it states the same, to decimals (-1: compared as text, "box" and "ellipsoid"); a missing key is
    // added after the object's last. False if there's no entity there, or it has no fog_volume object.
    bool set_fog_member(uint32_t index, std::string_view key, std::string_view value, int decimals);
    // Writes one key of the entity's own object the same way: value as the source writes it, with its quotes if it's a
    // string ("\"camp\""), compared as text.
    bool set_entity_member(uint32_t index, std::string_view key, std::string_view value);
    // Takes one key and its value out of the entity's object, with the comma that set it apart. False if it isn't
    // there.
    bool remove_entity_member(uint32_t index, std::string_view key);

    // The cell's place in the grid, from its "coord"; false without a readable one.
    bool coord(int32_t& x, int32_t& z) const;
    // The parent's name the entity names ("" without one), and its mesh's path as written ("" without one).
    std::string_view parent(uint32_t index) const;
    std::string_view mesh(uint32_t index) const;
    // The entity's object, as the file has it ("{...}").
    std::string_view entity_text(uint32_t index) const;

    // Takes an entity out, with its line if it had one to itself (a comment on that line goes with it), and the comma
    // that set it apart. The ones after it move up one place.
    bool remove_entity(uint32_t index);
    // Puts an entity ("{...}") after the last, in the file's layout: on a line of its own at the last one's indent, or
    // after it on its line. Its place.
    uint32_t add_entity(std::string_view object);

    std::span<const char> text() const { return text_.span(); }
    std::string_view view() const { return {text_.data(), text_.size()}; }

private:
    struct Span {
        uint32_t begin = 0;
        uint32_t end = 0;
        bool found() const { return end > begin; }
    };
    struct Entry {
        Span object;
        Span name; // its string, without the quotes
        Span name_value, parent_value, mesh_value;
        Span position_value, rotation_value, rotation_deg_value, scale_value;
        Span fog_value;         // its fog_volume object
        Span last_value;        // the last member's value: where a key goes when nothing better is there
        uint32_t first_key = 0; // where its first key starts (0: it has none)
    };

    // Replaces [begin, end) with text and parses again.
    void replace(uint32_t begin, uint32_t end, std::string_view with);
    // Puts a key and its value after the value at anchor, in the entry's layout (a new line at the entry's indent if
    // its keys are on lines of their own).
    void add_key(const Entry& entry, Span anchor, std::string_view key, std::string_view value);
    std::string_view slice(Span span) const { return {text_.data() + span.begin, span.end - span.begin}; }

    // Sets key in the object at its span (re-read now): replaced unless it states the same (decimals as
    // set_fog_member), or added after its last key.
    bool set_in_object(Span object, std::string_view key, std::string_view value, int decimals);

    core::Array<char> text_;
    core::Array<Entry> entities_;
    Span coord_value_;    // the cell's "coord"
    Span entities_array_; // "[...]"
};

} // namespace de::editor
