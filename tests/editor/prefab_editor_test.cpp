#include "app/preview_scene.h"
#include "asset/registry.h"
#include "core/job_system.h"
#include "editor/prefab_editor.h"
#include "editor/preview_pictures.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/scene/gpu_assets.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "support/cook_kit.h"
#include "support/gltf_source.h"
#include "support/gpu.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using de::Vec3;
using de::test::Sandbox;

namespace {

constexpr std::string_view kGlow = R"({
  "emitters": [
    {"name": "glow", "rate_per_s": 6, "lifetime_s": 1, "prewarm_s": 0,
     "modules": [{"type": "size", "m": 0.5}, {"type": "color", "rgb": [1, 0.1, 0.05]}],
     "look": {"shape": "glow", "intensity": 0.25, "soft_m": 0}}
  ]
})";

// A lamp as a person wrote it: a square two metres a side, and a glow above it.
constexpr std::string_view kLamp = R"({
  // A lamp.
  "parts": {
    "body": {"mesh": "square.gltf"},
    "light": {"effect": "glow.vfx", "position": [0, 2, 0]}
  }
})";

bool says(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

} // namespace

TEST_CASE("the prefab editor makes and changes a prefab's source, and shows it as it stands") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    std::vector<de::test::Vertex> vertices;
    std::vector<uint32_t> indices;
    de::test::quad(vertices, indices);
    de::test::write_src(sandbox, "content/props/square.gltf",
                        de::test::gltf(vertices, indices, 0.0f, R"([{"name": "paint"}])", "[]", "[]"));
    de::test::write_src(sandbox, "content/props/glow.vfx", kGlow);
    de::test::write_src(sandbox, "content/props/lamp.prefab", kLamp);
    de::test::write_src(sandbox, "content/town/street.prefab",
                        R"({"parts": {"lamp": {"prefab": "../props/lamp.prefab", "position": [3, 0, 0]}}})");
    {
        de::platform::Vfs cook_vfs = de::test::cooker_vfs(sandbox);
        REQUIRE(de::test::run_cook(cook_vfs).ok());
    }
    // Written after the cook: an effect that isn't cooked, and a prefab that says more than the editor would write.
    de::test::write_src(sandbox, "content/props/raw.vfx", kGlow);
    de::test::write_src(sandbox, "content/props/odd.prefab",
                        R"({"parts": {"a": {"mesh": "square.gltf", "colour": "red"}}})");
    {
        de::core::JobSystem jobs({.worker_count = 2});
        de::platform::Vfs vfs = sandbox.make_vfs({.engine_root = DE_SOURCE_DIR, .jobs = &jobs, .game_writable = true});
        auto compiler = de::shader::ShaderCompiler::create(vfs);
        REQUIRE(compiler.ok());
        auto shaders = de::shader::ShaderLibrary::create(*device, vfs, jobs, *compiler, {.hot_reload = false});
        REQUIRE(shaders.ok());
        auto uploader = de::rhi::Uploader::create(*device);
        REQUIRE(uploader.ok());
        auto assets = de::asset::AssetRegistry::create({.vfs = &vfs, .jobs = &jobs, .hot_reload = false});
        REQUIRE(assets.ok());
        {
            auto gpu_assets =
                de::render::GpuAssets::create({.device = &*device, .uploader = &*uploader, .assets = &*assets});
            REQUIRE(gpu_assets.ok());
            const de::editor::PictureEngine engine{.device = &*device,
                                                   .shaders = &*shaders,
                                                   .assets = &*assets,
                                                   .gpu_assets = &*gpu_assets,
                                                   .jobs = &jobs,
                                                   .vfs = &vfs};
            de::editor::PrefabEditor editor(de::core::heap_allocator(de::core::MemoryTag::Editor));
            char why[200] = {};
            // What isn't a prefab, isn't there, or says what the editor wouldn't write back is refused, saying why.
            CHECK_FALSE(editor.open(engine, "src:/content/props/square.gltf", why));
            CHECK(says(why, "isn't a prefab"));
            CHECK_FALSE(editor.open(engine, "src:/content/props/never.prefab", why));
            CHECK(says(why, "can't be read"));
            CHECK_FALSE(editor.open(engine, "src:/content/props/odd.prefab", why));
            CHECK(says(why, "doesn't know"));
            CHECK_FALSE(editor.is_open());

            REQUIRE(editor.open(engine, "src:/content/props/lamp.prefab", why));
            CHECK(editor.is_open());
            CHECK_FALSE(editor.dirty());
            CHECK_FALSE(editor.can_undo());
            CHECK(editor.document().parts.size() == 2);
            // The editor's frame: the prefab editor moves along, what loaded is taken in, its window shows it, the
            // frame draws.
            const auto frame = [&] {
                editor.update(engine, 1.0f / 60.0f);
                assets->update();
                gpu_assets->update();
                editor.show(96, 96);
                device->begin_frame();
                uploader->record();
                editor.draw();
                device->end_frame();
            };
            const auto emitters_drawn = [&] { return editor.scene()->stats().particles.emitters_drawn; };
            const auto meshes_drawn = [&] { return editor.scene()->stats().visible_draws; };
            // Long enough for a change to have reached the picture and the numbers read back from it.
            const auto settle = [&] {
                for (uint32_t i = 0; i < 40; ++i) {
                    frame();
                }
            };
            // Until what's wanted shows, or too long.
            const auto frames_until = [&](auto&& there) {
                uint32_t frames = 0;
                for (; frames < 600 && !there(); ++frames) {
                    frame();
                }
                return frames < 600;
            };
            // Its scene is the document: the square and the glow, from the cooked model and effect its files cook to.
            CHECK(frames_until([&] { return meshes_drawn() >= 1 && emitters_drawn() == 1; }));
            CHECK(editor.problems().empty());

            // A variant, shown as it's made: what it leaves out goes from the picture, and comes back with all parts.
            REQUIRE(editor.add_variant("dark"));
            CHECK(editor.dirty());
            REQUIRE(editor.leave_out("dark", "light", true));
            settle();
            CHECK(emitters_drawn() == 0);
            CHECK(meshes_drawn() >= 1);
            REQUIRE(editor.show_variant(""));
            CHECK(frames_until([&] { return emitters_drawn() == 1; }));
            CHECK_FALSE(editor.add_variant("dark"));      // there already
            CHECK_FALSE(editor.add_variant("Dark Mode")); // not a name
            CHECK_FALSE(editor.leave_out("dark", "nobody", true));

            // A part from a file: named from it, placed, chosen; one step of undo.
            REQUIRE(editor.add_part("src:/content/props/glow.vfx", Vec3(1.0f, 0.0f, 0.0f)));
            CHECK(editor.document().parts.size() == 3);
            CHECK((editor.selected() == "glow"));
            CHECK((editor.document().parts[2].effect.view() == "glow.vfx"));
            CHECK(frames_until([&] { return emitters_drawn() == 2; }));
            REQUIRE(editor.undo());
            CHECK(editor.document().parts.size() == 2);
            CHECK(editor.selected().empty()); // what was chosen is gone
            CHECK(frames_until([&] { return emitters_drawn() == 1; }));
            REQUIRE(editor.redo());
            CHECK(editor.document().parts.size() == 3);
            CHECK_FALSE(editor.can_redo());
            // What can't be a part says so, and changes nothing.
            CHECK_FALSE(editor.add_part("src:/content/props/lamp.prefab", Vec3(0.0f))); // itself
            CHECK(says(editor.message(), "inside itself"));
            CHECK_FALSE(editor.add_part("src:/content/props/notes.txt", Vec3(0.0f)));
            CHECK(editor.document().parts.size() == 3);

            // Renamed, placed, copied, and the copy taken out again.
            REQUIRE(editor.rename_part("glow", "ember"));
            CHECK_FALSE(editor.rename_part("ember", "body")); // another's
            CHECK_FALSE(editor.rename_part("ember", "Ember!"));
            REQUIRE(editor.select("ember"));
            REQUIRE(editor.place_part("ember", Vec3(1.0f, 0.5f, 0.0f), Vec3(0.0f, 90.0f, 0.0f), Vec3(2.0f)));
            CHECK_FALSE(editor.place_part("ember", Vec3(0.0f), Vec3(0.0f), Vec3(1.0f, 0.0f, 1.0f))); // no scale of zero
            REQUIRE(editor.duplicate_part("ember"));
            CHECK((editor.selected() == "ember_2"));
            CHECK(editor.document().parts.size() == 4);
            REQUIRE(editor.remove_part("ember_2"));
            CHECK(editor.selected().empty());
            CHECK_FALSE(editor.remove_part("ember_2"));

            // A part's light: there the frame it's given, gone when it's taken off; not on a part that's a prefab.
            const de::asset::LightDesc warm{
                .color = {1.0f, 0.6f, 0.3f}, .intensity = 0.8f, .radius_m = 3.0f, .flicker = 0.3f};
            REQUIRE(editor.set_light("ember", &warm));
            CHECK(editor.document().parts[2].has_light);
            CHECK(frames_until([&] { return editor.scene()->stats().lights.lit == 1; }));
            de::asset::LightDesc too_far = warm;
            too_far.radius_m = 0.0f;
            CHECK_FALSE(editor.set_light("ember", &too_far)); // out of range: as it was
            CHECK(editor.document().parts[2].light.radius_m == 3.0f);
            REQUIRE(editor.set_light("ember", nullptr));
            CHECK(frames_until([&] { return editor.scene()->stats().lights.lit == 0; }));
            REQUIRE(editor.undo()); // lit again
            CHECK(editor.document().parts[2].has_light);
            // A file that isn't cooked is a part all the same, and a problem it names.
            REQUIRE(editor.add_part("src:/content/props/raw.vfx", Vec3(0.0f)));
            frame();
            REQUIRE(editor.problems().size() == 1);
            CHECK(says(editor.problems()[0].view(), "raw.vfx"));
            REQUIRE(editor.undo());
            frame();
            CHECK(editor.problems().empty());

            // With changes unsaved, nothing else opens in its place and the editor doesn't close without asking.
            CHECK(editor.dirty());
            CHECK(editor.open(engine, "src:/content/town/street.prefab", why)); // asked: its window's question
            CHECK((editor.path() == "src:/content/props/lamp.prefab"));
            CHECK_FALSE(editor.may_quit());

            // Saved: its file in the one layout, the comment it opened with kept; the editor is told to cook, once.
            REQUIRE(editor.save(engine));
            CHECK_FALSE(editor.dirty());
            CHECK(editor.take_saved());
            CHECK_FALSE(editor.take_saved());
            CHECK(editor.may_quit());
            CHECK((de::test::read_text(vfs, "src:/content/props/lamp.prefab") == R"({
  // A lamp.
  "parts": {
    "body":  {"mesh": "square.gltf"},
    "light": {"effect": "glow.vfx", "position": [0, 2, 0]},
    "ember": {"effect": "glow.vfx", "light": {"color": [1, 0.6, 0.3], "intensity": 0.8, "radius_m": 3, "flicker": 0.3, "flicker_hz": 8}, "position": [1, 0.5, 0], "rotation_deg": [0, 90, 0], "scale": 2}
  },
  "variants": {
    "dark": {"light": null}
  }
}
)"));
            // Undone past the save, it has changes again; back at the saved step, none.
            REQUIRE(editor.undo());
            CHECK(editor.dirty());
            REQUIRE(editor.redo());
            CHECK_FALSE(editor.dirty());

            // A prefab that nests another: the other's parts are there, placed with the part that names it, and a
            // variant can leave out one of them by its path.
            editor.close(engine);
            REQUIRE(editor.open(engine, "src:/content/town/street.prefab", why));
            CHECK((editor.document().parts[0].prefab.view() == "../props/lamp.prefab"));
            CHECK(frames_until([&] { return meshes_drawn() >= 1 && emitters_drawn() == 1; }));
            CHECK(editor.problems().empty());
            REQUIRE(editor.add_variant("unlit"));
            REQUIRE(editor.leave_out("unlit", "lamp/light", true));
            settle();
            CHECK(emitters_drawn() == 0);
            CHECK(meshes_drawn() >= 1);
            editor.close(engine); // as it is: what wasn't saved goes
            CHECK_FALSE(editor.is_open());

            // A new prefab isn't a file until it's saved, and isn't saved with nothing in it.
            CHECK_FALSE(editor.open_new(engine, "src:/content/props/lamp.prefab", why)); // there already
            REQUIRE(editor.open_new(engine, "src:/content/town/shelf.prefab", why));
            CHECK(editor.document().parts.empty());
            CHECK_FALSE(editor.save(engine));
            CHECK(says(editor.message(), "needs a part"));
            CHECK_FALSE(vfs.exists("src:/content/town/shelf.prefab"));
            REQUIRE(editor.add_part("src:/content/props/square.gltf", Vec3(0.0f)));
            CHECK((editor.document().parts[0].mesh.view() == "../props/square.gltf")); // from its own folder
            REQUIRE(editor.save(engine));
            CHECK(vfs.exists("src:/content/town/shelf.prefab"));

            // A new prefab that starts with parts (made from what's chosen in a world): their IDs made fit, and their
            // own.
            de::editor::PrefabDocPart crate;
            static_cast<void>(crate.id.assign("Crate 01"));
            static_cast<void>(crate.mesh.assign("../props/square.gltf"));
            crate.position = Vec3(1.0f, 0.0f, 0.0f);
            const de::editor::PrefabDocPart pile[] = {crate, crate};
            REQUIRE(editor.open_new_with(engine, "src:/content/town/pile.prefab", pile, why));
            CHECK(editor.dirty()); // nothing of it is a file yet
            REQUIRE(editor.document().parts.size() == 2);
            CHECK((editor.document().parts[0].id.view() == "crate_01"));
            CHECK((editor.document().parts[1].id.view() == "crate_01_2"));
            CHECK(editor.document().parts[1].position.x == 1.0f);
            // Not over changes that aren't saved: there's no asking with parts in hand.
            CHECK_FALSE(editor.open_new_with(engine, "src:/content/town/heap.prefab", pile, why));
            CHECK(says(why, "aren't saved"));
            CHECK((editor.path() == "src:/content/town/pile.prefab"));
            editor.shutdown(engine);
        }
    } // the engine's side is gone: the cooker has its own
    // What it wrote is what the cooker reads: the changed prefab and the new one cook (the odd one mended first).
    de::test::write_src(sandbox, "content/props/odd.prefab", R"({"parts": {"a": {"mesh": "square.gltf"}}})");
    de::platform::Vfs cook_vfs = de::test::cooker_vfs(sandbox);
    CHECK(de::test::run_cook(cook_vfs).ok());
}
