#include "core/allocator.h"
#include "core/math.h"
#include "editor/cell_source.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string_view>

using de::DVec3;
using de::Quat;
using de::Vec3;
using de::editor::CellSource;

namespace {

de::core::Allocator& heap() {
    return de::core::heap_allocator(de::core::MemoryTag::Editor);
}

// As the cooker reads rotation_deg: about Y, then X, then Z.
Quat from_degrees(Vec3 degrees) {
    return glm::normalize(glm::angleAxis(glm::radians(degrees.y), Vec3(0.0f, 1.0f, 0.0f)) *
                          glm::angleAxis(glm::radians(degrees.x), Vec3(1.0f, 0.0f, 0.0f)) *
                          glm::angleAxis(glm::radians(degrees.z), Vec3(0.0f, 0.0f, 1.0f)));
}

std::string_view number(double value, int decimals, char (&buffer)[64]) {
    return {buffer, de::editor::format_number(value, decimals, buffer)};
}

constexpr std::string_view kCell = R"({
  // A camp: its pieces placed relative to it.
  "coord": [0, 0],
  "entities": [
    {"name": "camp", "position": [64, 0, 64]},
    {"name": "statue", "parent": "camp", "mesh": "statue.gltf", "position": [-3.5, 1.21, -3], "rotation_deg": [0, 260, 0], "scale": 1.2, "collision": "mesh"},
    {"name": "boulder", "mesh": "boulder.gltf", "position": [58, 0, 70]}, // a comment, kept
    {
      "name": "tall",
      "mesh": "boulder.gltf",
      "position": [
        10,
        0,
        20
      ]
    },
    {"name": "turned", "position": [1, 2, 3], "rotation": [0, 0.707107, 0, 0.707107]}
  ]
}
)";

} // namespace

TEST_CASE("cell sources: numbers are written short, to their decimals") {
    char buffer[64];
    CHECK((number(64.0, 4, buffer) == "64"));
    CHECK((number(64.5, 4, buffer) == "64.5"));
    CHECK((number(61.000000001, 4, buffer) == "61"));
    CHECK((number(-0.00001, 4, buffer) == "0"));
    CHECK((number(1.21, 4, buffer) == "1.21"));
    CHECK((number(-3.14159, 3, buffer) == "-3.142"));
    CHECK((number(0.70710678, 6, buffer) == "0.707107"));
}

TEST_CASE("cell sources: a rotation's angles read back as the cooker turns them") {
    const Vec3 cases[] = {{0.0f, 40.0f, 0.0f},    {6.0f, 75.0f, 0.0f},     {0.0f, 150.0f, 4.0f},
                          {-30.0f, 10.0f, 20.0f}, {45.0f, -120.0f, 60.0f}, {89.0f, 30.0f, 0.0f}};
    for (const Vec3& degrees : cases) {
        const Quat turned = from_degrees(degrees);
        const Vec3 read = de::editor::rotation_deg_of(turned, degrees);
        CHECK(read.x == doctest::Approx(degrees.x).epsilon(1e-3));
        CHECK(read.y == doctest::Approx(degrees.y).epsilon(1e-3));
        CHECK(read.z == doctest::Approx(degrees.z).epsilon(1e-3));
        // Whatever angles come back, they turn the same way.
        CHECK(std::abs(glm::dot(from_degrees(read), turned)) == doctest::Approx(1.0f).epsilon(1e-5));
    }
    // Each angle the turn nearest the file's: 260 stays 260.
    CHECK(de::editor::rotation_deg_of(from_degrees({0.0f, 260.0f, 0.0f}), {0.0f, 260.0f, 0.0f}).y ==
          doctest::Approx(260.0f).epsilon(1e-3));
    CHECK(de::editor::rotation_deg_of(from_degrees({0.0f, 260.0f, 0.0f})).y == doctest::Approx(-100.0f).epsilon(1e-3));
    // Straight up: still the same turn.
    const Quat up = from_degrees({90.0f, 30.0f, 0.0f});
    CHECK(std::abs(glm::dot(from_degrees(de::editor::rotation_deg_of(up)), up)) == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("cell sources: an edit rewrites only the values that changed") {
    CellSource source(heap());
    REQUIRE(source.parse(kCell));
    REQUIRE(source.entity_count() == 5);
    CHECK((source.name(1) == "statue"));
    CHECK((source.name(3) == "tall"));

    // As it is: nothing changes, to the byte.
    REQUIRE(source.set_transform(
        1, {.position = DVec3(-3.5, 1.21, -3.0), .rotation = from_degrees({0.0f, 260.0f, 0.0f}), .scale = Vec3(1.2f)}));
    CHECK((source.view() == kCell));

    // Moved: its position, and nothing else in the file.
    REQUIRE(source.set_transform(
        1, {.position = DVec3(-2.0, 1.21, -3.0), .rotation = from_degrees({0.0f, 260.0f, 0.0f}), .scale = Vec3(1.2f)}));
    CHECK(
        source.view().find(
            R"({"name": "statue", "parent": "camp", "mesh": "statue.gltf", "position": [-2, 1.21, -3], "rotation_deg": [0, 260, 0], "scale": 1.2, "collision": "mesh"},)") !=
        std::string_view::npos);
    CHECK(source.view().size() == kCell.size() - 2); // "-3.5" became "-2"
    CHECK(source.view().find("// a comment, kept") != std::string_view::npos);

    // Turned and scaled where it had neither: the keys come after its position, on its line.
    REQUIRE(source.set_transform(2, {.position = DVec3(58.0, 0.0, 70.0),
                                     .rotation = from_degrees({0.0f, 30.0f, 0.0f}),
                                     .scale = Vec3(2.0f, 1.0f, 1.0f)}));
    CHECK(
        source.view().find(
            R"({"name": "boulder", "mesh": "boulder.gltf", "position": [58, 0, 70], "rotation_deg": [0, 30, 0], "scale": [2, 1, 1]}, // a comment, kept)") !=
        std::string_view::npos);

    // An entry with a key on each line: a new key gets a line of its own, at their indent.
    REQUIRE(source.set_transform(
        3, {.position = DVec3(10.0, 0.0, 20.0), .rotation = Quat(1.0f, 0.0f, 0.0f, 0.0f), .scale = Vec3(3.0f)}));
    CHECK(source.view().find("      ],\n      \"scale\": 3\n    },") != std::string_view::npos);

    // A rotation written as a quaternion stays one.
    REQUIRE(source.set_transform(
        4, {.position = DVec3(1.0, 2.0, 3.0), .rotation = from_degrees({0.0f, 180.0f, 0.0f}), .scale = Vec3(1.0f)}));
    CHECK(source.view().find(R"("rotation": [0, 1, 0, 0])") != std::string_view::npos);

    // What it wrote reads again, with every entity where it was.
    CellSource again(heap());
    REQUIRE(again.parse(source.view()));
    CHECK(again.entity_count() == 5);
    CHECK((again.name(4) == "turned"));
    CHECK_FALSE(source.set_transform(5, {}));
}

TEST_CASE("cell sources: what isn't a cell is refused, saying why") {
    CellSource source(heap());
    CHECK_FALSE(source.parse("[]"));
    CHECK_FALSE(source.parse(R"({"coord": [0, 0]})"));
    CHECK_FALSE(source.parse(R"({"entities": [1, 2]})"));
    CHECK_FALSE(source.parse(R"({"entities": [{"name": "a" "position": [0, 0, 0]}]})"));
    CHECK_FALSE(source.parse("{ /* never closed"));
    CHECK(source.parse(R"({"entities": []})"));
    CHECK(source.entity_count() == 0);
}

TEST_CASE("cell sources: entities come out with their line and go in in the file's layout") {
    constexpr std::string_view kLines = R"({
  "coord": [1, -2],
  "entities": [
    {"name": "a"},
    {"name": "b"}, // b's comment
    {"name": "c"}
  ]
}
)";
    CellSource source(heap());
    REQUIRE(source.parse(kLines));
    int32_t x = 0;
    int32_t z = 0;
    REQUIRE(source.coord(x, z));
    CHECK(x == 1);
    CHECK(z == -2);
    // The middle one, its comment with it.
    REQUIRE(source.remove_entity(1));
    CHECK((source.view() ==
           "{\n  \"coord\": [1, -2],\n  \"entities\": [\n    {\"name\": \"a\"},\n    {\"name\": \"c\"}\n  "
           "]\n}\n"));
    // The last: the comma before it goes too.
    REQUIRE(source.remove_entity(1));
    CHECK((source.view() == "{\n  \"coord\": [1, -2],\n  \"entities\": [\n    {\"name\": \"a\"}\n  ]\n}\n"));
    // In again, on a line of its own, the one before taking a comma.
    CHECK(source.add_entity(R"({"name": "d", "parent": "a"})") == 1);
    CHECK((source.view() ==
           "{\n  \"coord\": [1, -2],\n  \"entities\": [\n    {\"name\": \"a\"},\n    {\"name\": \"d\", "
           "\"parent\": \"a\"}\n  ]\n}\n"));
    CHECK((source.parent(1) == "a"));
    REQUIRE(source.set_entity_member(1, "parent", "\"z\""));
    REQUIRE(source.set_entity_member(1, "mesh", "\"m.gltf\""));
    CHECK((source.entity_text(1) == R"({"name": "d", "parent": "z", "mesh": "m.gltf"})"));
    CHECK((source.mesh(1) == "m.gltf"));
    // All out, and one into the empty list.
    REQUIRE(source.remove_entity(0));
    REQUIRE(source.remove_entity(0));
    CHECK(source.entity_count() == 0);
    CHECK(source.add_entity(R"({"name": "e"})") == 0);
    CellSource again(heap());
    REQUIRE(again.parse(source.view()));
    CHECK(again.entity_count() == 1);

    // On one line: apart by commas, kept on it.
    CellSource inline_list(heap());
    REQUIRE(inline_list.parse(R"({"entities": [{"name": "a"}, {"name": "b"}]})"));
    CHECK(inline_list.add_entity(R"({"name": "c"})") == 2);
    CHECK((inline_list.view() == R"({"entities": [{"name": "a"}, {"name": "b"}, {"name": "c"}]})"));
    REQUIRE(inline_list.remove_entity(1));
    CHECK((inline_list.view() == R"({"entities": [{"name": "a"}, {"name": "c"}]})"));
}

TEST_CASE("cell sources: a new cell's text, and a world's list of cells") {
    de::core::Array<char> text(heap());
    de::editor::new_cell_text(-1, 3, text);
    CellSource made(heap());
    REQUIRE(made.parse({text.data(), text.size()}));
    int32_t x = 0;
    int32_t z = 0;
    REQUIRE(made.coord(x, z));
    CHECK(x == -1);
    CHECK(z == 3);

    constexpr std::string_view kWorld = R"({"environment": "e.environment", "cells": ["a.cell", "b.cell"]})";
    int seen = 0;
    REQUIRE(de::editor::world_cells(kWorld, [&](std::string_view cell) {
        CHECK((cell == (seen == 0 ? "a.cell" : "b.cell")));
        ++seen;
    }));
    CHECK(seen == 2);
    de::core::Array<char> added(heap());
    REQUIRE(de::editor::add_world_cell(kWorld, "cell_2_0.cell", added));
    CHECK((std::string_view(added.data(), added.size()) ==
           R"({"environment": "e.environment", "cells": ["a.cell", "b.cell", "cell_2_0.cell"]})"));
    REQUIRE(de::editor::add_world_cell(R"({"cells": []})", "a.cell", added));
    CHECK((std::string_view(added.data(), added.size()) == R"({"cells": ["a.cell"]})"));
    CHECK_FALSE(de::editor::add_world_cell(R"({"environment": "e"})", "a.cell", added));
}

TEST_CASE("cell sources: paths from one folder to another") {
    char out[256];
    size_t length = 0;
    const auto path = [&] { return std::string_view(out, length); };
    REQUIRE(de::editor::joined_path("src:/content/a/d", "../b/c.gltf", out, length));
    CHECK((path() == "src:/content/a/b/c.gltf"));
    REQUIRE(de::editor::joined_path("src:/content/a", "./x/y.gltf", out, length));
    CHECK((path() == "src:/content/a/x/y.gltf"));
    CHECK_FALSE(de::editor::joined_path("src:/content", "../../../x", out, length));
    REQUIRE(de::editor::relative_path("src:/content/a/d", "src:/content/a/b/c.gltf", out, length));
    CHECK((path() == "../b/c.gltf"));
    REQUIRE(de::editor::relative_path("src:/content/a", "src:/content/a/x.gltf", out, length));
    CHECK((path() == "x.gltf"));
    REQUIRE(de::editor::relative_path("src:/content/ab", "src:/content/a/x.gltf", out, length));
    CHECK((path() == "../a/x.gltf"));
    REQUIRE(de::editor::relative_path("src:/content/w/deep/er", "src:/content/props/p.gltf", out, length));
    CHECK((path() == "../../../props/p.gltf"));
}
