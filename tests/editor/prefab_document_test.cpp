#include "core/allocator.h"
#include "core/array.h"
#include "editor/prefab_document.h"

#include <doctest/doctest.h>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <string_view>

using de::Quat;
using de::Vec3;
using de::editor::PrefabDocPart;
using de::editor::PrefabDocument;

namespace {

de::core::Allocator& heap() {
    return de::core::heap_allocator(de::core::MemoryTag::Editor);
}

std::string_view text_of(const de::core::Array<char>& text) {
    return {text.data(), text.size()};
}

// A candle as a person wrote it: a comment, parts lined up by hand, a variant.
constexpr std::string_view kCandle = R"({
  // A candle: its wax, and a flame at its wick. Unlit, it has no flame.
  "parts": {
    "wax":   {"mesh": "candle.glb"},
    "flame": {"effect": "../effects/candle_flame.vfx", "position": [0, 0.196, 0]}
  },
  "variants": {
    "unlit": {"flame": null}
  }
}
)";

bool parses(PrefabDocument& doc, std::string_view text) {
    return static_cast<bool>(doc.parse(text));
}

// Whether two rotations turn a thing the same way (q and -q do).
bool same_turn(const Quat& a, const Quat& b) {
    return std::abs(glm::dot(a, b)) > 0.99999f;
}

} // namespace

TEST_CASE("PrefabDocument reads what the cooker reads") {
    PrefabDocument doc(heap());
    REQUIRE(parses(doc, kCandle));
    REQUIRE(doc.parts.size() == 2);
    CHECK((doc.parts[0].id.view() == "wax"));
    CHECK((doc.parts[0].mesh.view() == "candle.glb"));
    CHECK(doc.parts[0].effect.empty());
    CHECK((doc.parts[1].id.view() == "flame"));
    CHECK((doc.parts[1].effect.view() == "../effects/candle_flame.vfx"));
    CHECK(doc.parts[1].position.y == doctest::Approx(0.196f));
    REQUIRE(doc.variants.size() == 1);
    CHECK((doc.variants[0].name.view() == "unlit"));
    CHECK(doc.leaves_out(0, "flame"));
    CHECK(!doc.leaves_out(0, "wax"));
    de::core::Array<de::core::FixedString<159>> problems(heap());
    doc.check(problems);
    CHECK(problems.empty());
}

TEST_CASE("PrefabDocument writes one layout, and reads it back as it was") {
    PrefabDocument doc(heap());
    REQUIRE(parses(doc, kCandle));
    de::core::Array<char> first(heap());
    doc.write(first);
    // The comment it opened with, a part a line with their objects lined up, the variant.
    CHECK((text_of(first) == R"({
  // A candle: its wax, and a flame at its wick. Unlit, it has no flame.
  "parts": {
    "wax":   {"mesh": "candle.glb"},
    "flame": {"effect": "../effects/candle_flame.vfx", "position": [0, 0.196, 0]}
  },
  "variants": {
    "unlit": {"flame": null}
  }
}
)"));
    // Undo keeps texts: what's written is what's read, over and over.
    PrefabDocument again(heap());
    REQUIRE(parses(again, text_of(first)));
    de::core::Array<char> second(heap());
    again.write(second);
    CHECK((text_of(second) == text_of(first)));
}

TEST_CASE("PrefabDocument keeps a part's every value through its text") {
    PrefabDocument doc(heap());
    REQUIRE(parses(doc, R"({"parts": {
      "table": {"prefab": "table.prefab", "variant": "bare", "position": [1.5, 0, -2], "rotation_deg": [0, 260, 0]},
      "post": {"mesh": "post.glb", "collision": "mesh", "effect": "smoke.vfx", "scale": [1, 2, 1]},
      "lamp": {"mesh": "lamp.glb", "rotation": [0, 0.70710678, 0, 0.70710678], "scale": 0.5}
    }})"));
    REQUIRE(doc.parts.size() == 3);
    const PrefabDocPart& table = doc.parts[0];
    CHECK((table.prefab.view() == "table.prefab"));
    CHECK((table.variant.view() == "bare"));
    CHECK(table.rotation_deg.y == doctest::Approx(260.0f)); // as the file has it, not -100
    CHECK(!table.quaternion);
    const PrefabDocPart& post = doc.parts[1];
    CHECK(post.collision);
    CHECK((post.effect.view() == "smoke.vfx"));
    CHECK(post.scale.y == doctest::Approx(2.0f));
    const PrefabDocPart& lamp = doc.parts[2];
    CHECK(lamp.quaternion);
    CHECK(lamp.scale.x == doctest::Approx(0.5f));
    CHECK(same_turn(lamp.rotation, glm::angleAxis(glm::radians(90.0f), Vec3(0.0f, 1.0f, 0.0f))));

    de::core::Array<char> text(heap());
    doc.write(text);
    CHECK((text_of(text) == R"({
  "parts": {
    "table": {"prefab": "table.prefab", "variant": "bare", "position": [1.5, 0, -2], "rotation_deg": [0, 260, 0]},
    "post":  {"mesh": "post.glb", "collision": "mesh", "effect": "smoke.vfx", "scale": [1, 2, 1]},
    "lamp":  {"mesh": "lamp.glb", "rotation": [0, 0.707107, 0, 0.707107], "scale": 0.5}
  }
}
)"));
}

TEST_CASE("PrefabDocument keeps a part's light: what's given read, what it uses written") {
    PrefabDocument doc(heap());
    REQUIRE(parses(doc, R"({"parts": {
      "glow": {"light": {"color": [1, 0.6, 0.3], "intensity": 0.8, "radius_m": 3, "flicker": 0.3, "flicker_hz": 9},
               "position": [0, 0.22, 0]},
      "beam": {"mesh": "lamp.glb", "light": {"radius_m": 12, "cone_deg": 25, "cone_soft": 0.5}},
      "bare": {"effect": "flame.vfx", "light": {}}
    }})"));
    REQUIRE(doc.parts.size() == 3);
    CHECK(doc.parts[0].has_light);
    CHECK(doc.parts[0].light.color[1] == doctest::Approx(0.6f));
    CHECK(doc.parts[0].light.flicker_hz == 9.0f);
    CHECK(doc.parts[1].light.cone_deg == 25.0f);
    CHECK(doc.parts[1].light.intensity == 1.0f); // not given: a light's own
    CHECK(doc.parts[2].has_light);
    de::core::Array<de::core::FixedString<159>> problems(heap());
    doc.check(problems);
    CHECK(problems.empty());

    de::core::Array<char> text(heap());
    doc.write(text);
    CHECK((text_of(text) == R"({
  "parts": {
    "glow": {"light": {"color": [1, 0.6, 0.3], "intensity": 0.8, "radius_m": 3, "flicker": 0.3, "flicker_hz": 9}, "position": [0, 0.22, 0]},
    "beam": {"mesh": "lamp.glb", "light": {"color": [1, 1, 1], "intensity": 1, "radius_m": 12, "cone_deg": 25, "cone_soft": 0.5}},
    "bare": {"effect": "flame.vfx", "light": {"color": [1, 1, 1], "intensity": 1, "radius_m": 4}}
  }
}
)"));
    PrefabDocument again(heap());
    REQUIRE(parses(again, text_of(text)));
    de::core::Array<char> second(heap());
    again.write(second);
    CHECK((text_of(second) == text_of(text)));

    // A light isn't a number, has no key of its own making, and its values have limits; not with a prefab.
    CHECK(!parses(doc, R"({"parts": {"a": {"light": 3}}})"));
    CHECK(!parses(doc, R"({"parts": {"a": {"light": {"brightness": 3}}}})"));
    REQUIRE(parses(doc, R"({"parts": {
      "far": {"light": {"radius_m": 0}},
      "nested": {"prefab": "a.prefab", "light": {}}
    }})"));
    doc.check(problems);
    CHECK(problems.size() == 2);
}

TEST_CASE("PrefabDocument's angles are the cooker's: about Y, then X, then Z") {
    const Quat turned = de::editor::rotation_from_deg(Vec3(30.0f, 90.0f, 0.0f));
    const Quat expected = glm::angleAxis(glm::radians(90.0f), Vec3(0.0f, 1.0f, 0.0f)) *
                          glm::angleAxis(glm::radians(30.0f), Vec3(1.0f, 0.0f, 0.0f));
    CHECK(same_turn(turned, expected));
    // A part turned by the handles keeps angles near the ones it had: 260 and a little, not -100.
    PrefabDocPart part;
    part.set_rotation_deg(Vec3(0.0f, 260.0f, 0.0f));
    part.set_rotation(de::editor::rotation_from_deg(Vec3(0.0f, 265.0f, 0.0f)));
    CHECK(part.rotation_deg.y == doctest::Approx(265.0f).epsilon(0.001));
    CHECK(same_turn(part.rotation, de::editor::rotation_from_deg(part.rotation_deg)));
}

TEST_CASE("PrefabDocument refuses what it wouldn't write back") {
    PrefabDocument doc(heap());
    CHECK(!parses(doc, "not json"));
    CHECK(!parses(doc, R"({"parts": {"a": {"mesh": "a.glb", "colour": "red"}}})"));    // a key it doesn't know
    CHECK(!parses(doc, R"({"parts": {"a": {"mesh": "a.glb"}}, "notes": "mine"})"));    // more than parts and variants
    CHECK(!parses(doc, R"({"parts": {"a": {"mesh": "a.glb", "position": [0, 1]}}})")); // two numbers for three
    CHECK(!parses(doc, R"({"parts": {"a": {"mesh": "a.glb"}}, "variants": {"v": {"a": 1}}})")); // not left out: changed
    CHECK(doc.parts.empty());                                                                   // nothing half-kept
    // A new prefab's text: no parts yet.
    REQUIRE(parses(doc, "{}"));
    CHECK(doc.parts.empty());
}

TEST_CASE("PrefabDocument's parts are renamed and removed with what variants say of them") {
    PrefabDocument doc(heap());
    REQUIRE(parses(doc, R"({"parts": {
      "table": {"prefab": "table.prefab"}, "wax": {"mesh": "candle.glb"}, "flame": {"effect": "flame.vfx"}
    }, "variants": {
      "unlit": {"flame": null, "table/candle/flame": null},
      "bare": {"table": null, "wax": null}
    }})"));
    // Another part's ID, or one that won't do, changes nothing.
    CHECK(!doc.rename_part(1, "flame"));
    CHECK(!doc.rename_part(1, "Wax Two"));
    CHECK((doc.parts[1].id.view() == "wax"));
    // A part renamed takes its place in the variants along, and the paths into what it nests.
    REQUIRE(doc.rename_part(0, "desk"));
    CHECK(doc.leaves_out(0, "desk/candle/flame"));
    CHECK(!doc.leaves_out(0, "table/candle/flame"));
    CHECK(doc.leaves_out(1, "desk"));
    // One removed is gone from them.
    doc.remove_part(doc.find_part("flame"));
    CHECK(doc.parts.size() == 2);
    CHECK(!doc.leaves_out(0, "flame"));
    CHECK(doc.leaves_out(0, "desk/candle/flame"));
    // A variant removed takes what it left out; the ones after it keep theirs.
    doc.remove_variant(0);
    REQUIRE(doc.variants.size() == 1);
    CHECK((doc.variants[0].name.view() == "bare"));
    CHECK(doc.leaves_out(0, "desk"));
    CHECK(doc.leaves_out(0, "wax"));
    doc.leave_out(0, "wax", false);
    CHECK(!doc.leaves_out(0, "wax"));
}

TEST_CASE("PrefabDocument names new parts from their files") {
    PrefabDocument doc(heap());
    REQUIRE(parses(doc, R"({"parts": {"candle": {"mesh": "candle.glb"}}})"));
    CHECK((doc.free_id("src:/content/props/Brass Bowl.glb").view() == "brass_bowl"));
    CHECK((doc.free_id("src:/content/props/candle.glb").view() == "candle_2")); // taken: numbered
    CHECK((doc.free_id("candle").view() == "candle_2"));
    CHECK((doc.free_id("").view() == "part"));
    CHECK(PrefabDocument::valid_id("candle_2"));
    CHECK(!PrefabDocument::valid_id("Candle"));
    CHECK(!PrefabDocument::valid_id(""));
    CHECK(!PrefabDocument::valid_id("a_name_that_goes_on_for_far_too_long"));
}

TEST_CASE("PrefabDocument says what the cooker would refuse") {
    PrefabDocument doc(heap());
    de::core::Array<de::core::FixedString<159>> problems(heap());
    REQUIRE(parses(doc, "{}"));
    doc.check(problems);
    CHECK(problems.size() == 1); // nothing in it

    REQUIRE(parses(doc, R"({"parts": {
      "both": {"prefab": "a.prefab", "mesh": "a.glb"},
      "empty": {},
      "ghost": {"effect": "a.vfx", "collision": "mesh"},
      "picky": {"mesh": "a.glb", "variant": "bare"},
      "flat": {"mesh": "a.glb", "scale": [1, 0, 1]}
    }, "variants": {"v": {"nobody": null}}})"));
    doc.check(problems);
    CHECK(problems.size() == 6); // one each, and the variant that leaves out what isn't there
    for (const de::core::FixedString<159>& problem : problems) {
        CHECK(!problem.empty());
    }
}
