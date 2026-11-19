#pragma once

#include "asset/light_format.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/fixed_string.h"
#include "core/math.h"
#include "core/result.h"

#include <cstdint>
#include <string_view>

namespace de::editor {

// A .prefab source as the prefab editor holds it (docs/design.md, The prefab editor): its parts and its
// variants, read from the text the cooker reads (tools/cooker/prefab_cooker.h has the format) and written back as
// text. The file is the editor's from the first save: it's written whole, in one layout (a part a line, their
// objects lined up), keeping only the comment the file opens with. Undo keeps texts, so what's read and what's
// written have to be the same thing: write(), then parse(), gives the document back.

// One part: something placed from the prefab's origin. A model (with or without a body in its shape), an effect, a
// light, or any of them together; or another prefab, all of it there.
struct PrefabDocPart {
    core::FixedString<31> id;      // a-z, 0-9 and _: its own in the prefab
    core::FixedString<255> mesh;   // a model source as the file writes it, relative to the prefab's; empty: none
    core::FixedString<255> effect; // an effect source, likewise
    core::FixedString<255> prefab; // another prefab, likewise: not with a mesh or an effect
    core::FixedString<31> variant; // of that prefab; empty: all of it
    bool collision = false;        // with a mesh: a static body in its shape
    bool has_light = false;        // a local light there (docs/design.md, Local lights): not with a prefab
    asset::LightDesc light;        // as a light's keys say it; what a point light or a steady one doesn't use, its own
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};
    // The rotation as angles, as shown and written (about Y, then X, then Z, in degrees): kept beside it so a file's
    // 260 stays 260 and a dragged angle doesn't jump to another set for the same turn.
    Vec3 rotation_deg{0.0f};
    bool quaternion = false; // the file wrote "rotation", a quaternion, and is written so again

    // Turns it: the angles follow, each the nearest of its turns to what it was.
    void set_rotation(const Quat& turned);
    // Turns it to these angles.
    void set_rotation_deg(const Vec3& degrees);
};

// The same prefab with some parts left out.
struct PrefabDocVariant {
    core::FixedString<31> name; // a-z, 0-9 and _
};

// One thing a variant leaves out: a part's ID, or a path into a prefab a part nests ("table/candle/flame").
struct PrefabDocLeftOut {
    uint32_t variant = 0;
    core::FixedString<127> path;
};

// The rotation a part's rotation_deg means, as the cooker reads it: about Y, then X, then Z.
Quat rotation_from_deg(const Vec3& degrees);

class PrefabDocument {
public:
    explicit PrefabDocument(core::Allocator& allocator)
        : parts(allocator), variants(allocator), left_out(allocator), header(allocator) {}

    // Takes a .prefab's text in place of what it holds. Fails, saying why, if it isn't JSON, or says something the
    // editor wouldn't write back (a key it doesn't know): such a file is mended in text first, not half-kept.
    core::Result<void> parse(std::string_view text);
    // Its text.
    void write(core::Array<char>& out) const;
    void clear();

    // An ID for a new part, from a file's name or another part's ID: lowercase, anything but a-z, 0-9 and _ made _,
    // cut to fit, and numbered while a part has it.
    core::FixedString<31> free_id(std::string_view wanted) const;
    static bool valid_id(std::string_view id);
    // The part with that ID, or UINT32_MAX.
    uint32_t find_part(std::string_view id) const;
    // Gives a part another ID; what variants say of it follows. False (nothing changed) if the ID won't do or is
    // another part's.
    bool rename_part(uint32_t index, std::string_view id);
    // Takes a part out, with what variants say of it.
    void remove_part(uint32_t index);
    // Whether a variant leaves that out (a part's ID, or a path), and makes it so or not.
    bool leaves_out(uint32_t variant, std::string_view path) const;
    void leave_out(uint32_t variant, std::string_view path, bool out);
    // Takes a variant out, with what it left out.
    void remove_variant(uint32_t index);

    // What the cooker would refuse of it as it stands, each in a line; nothing if it would cook (as far as the text
    // alone can say: whether the files it names are there is the editor's to look up).
    void check(core::Array<core::FixedString<159>>& problems) const;

    core::Array<PrefabDocPart> parts;
    core::Array<PrefabDocVariant> variants;
    core::Array<PrefabDocLeftOut> left_out;
    core::Array<char> header; // the comment lines the file opens with, as they were; no newline at its end
};

} // namespace de::editor
