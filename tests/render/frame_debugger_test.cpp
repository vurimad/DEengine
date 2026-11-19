#include "console/console.h"
#include "core/array.h"
#include "render/graph/render_graph.h"
#include "render/rhi/device.h"
#include "support/gpu.h"
#include "support/shader_kit.h"
#include "tools/frame_debugger.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

using namespace de::rhi;
using de::render::GraphBuffer;
using de::render::GraphTexture;
using de::render::PassBuilder;
using de::render::PassContext;
using de::render::RenderGraph;
using de::test::check_clean;
using de::test::make_device;
using de::test::read_u32;
using de::test::ShaderKit;
using de::tools::FrameDebugger;
using de::tools::ProbeValue;

namespace {

constexpr Color kPaint{.r = 0.25f, .g = 0.5f, .b = 0.75f, .a = 1.0f};
constexpr uint32_t kView = 64;      // the view's size in pixels
constexpr uint32_t kViewSide = 256; // its texture's: no smaller than this, and copied out whole

struct Painted {
    GraphTexture color;
    GraphTexture depth;
};

// The frame the tests look at: "paint" clears a small color texture to kPaint and a depth texture to 0.5, "wipe"
// clears the same color texture to black, and "out" needs both, so nothing is culled.
const Painted& add_frame(RenderGraph& graph, GraphBuffer out) {
    const auto& painted = graph.add_pass<Painted>(
        "paint",
        [&](PassBuilder& pass, Painted& data) {
            data.color = pass.create_texture(
                {.desc = {.width = 4, .height = 4, .format = Format::RGBA8Unorm, .debug_name = "painted"}});
            data.depth = pass.create_texture(
                {.desc = {.width = 4, .height = 4, .format = Format::D32Float, .debug_name = "painted depth"}});
            pass.use(data.color, ResourceState::RenderTarget);
            pass.use(data.depth, ResourceState::DepthWrite);
        },
        [](const Painted& data, PassContext& context) {
            const ColorAttachment color{.texture = context.texture(data.color), .clear = kPaint};
            const DepthAttachment depth{.texture = context.texture(data.depth), .clear_depth = 0.5f};
            context.list.begin_rendering({.colors = std::span(&color, 1), .depth = &depth});
            context.list.end_rendering();
        });
    graph.add_pass(
        "wipe", [&](PassBuilder& pass) { pass.use(painted.color, ResourceState::RenderTarget); },
        [color = painted.color](PassContext& context) {
            const ColorAttachment attachment{.texture = context.texture(color), .clear = {.a = 1.0f}};
            context.list.begin_rendering({.colors = std::span(&attachment, 1)});
            context.list.end_rendering();
        });
    graph.add_pass(
        "out",
        [&](PassBuilder& pass) {
            pass.use(painted.color, ResourceState::CopySrc);
            pass.use(painted.depth, ResourceState::DepthRead);
            pass.use(out, ResourceState::CopyDst);
        },
        [color = painted.color, out](PassContext& context) {
            context.list.copy_texture_to_buffer(context.texture(color), context.buffer(out));
        });
    return painted;
}

// Frames until the debugger's probe has a texel (or a good many have gone by). copy_view: the view's texture is
// copied into it in the frames it's drawn.
ProbeValue run_frames(Device& device, ShaderKit& kit, RenderGraph& graph, FrameDebugger& debugger, BufferHandle out,
                      BufferHandle copy_view = {}) {
    bool used = false;
    bool view_used = false;
    for (int frame = 0; frame < 40; ++frame) {
        device.begin_frame();
        graph.begin(4, 4);
        debugger.begin_frame(graph, Format::RGBA8Unorm);
        const ResourceState before = used ? ResourceState::HostRead : ResourceState::Undefined;
        const GraphBuffer end = graph.import_buffer(
            {.buffer = out, .name = "out", .state = before, .final_state = ResourceState::HostRead});
        add_frame(graph, end);
        if (copy_view.is_valid() && debugger.shown().is_valid()) {
            const GraphBuffer copy =
                graph.import_buffer({.buffer = copy_view,
                                     .name = "view copy",
                                     .state = view_used ? ResourceState::HostRead : ResourceState::Undefined,
                                     .final_state = ResourceState::HostRead});
            view_used = true;
            graph.add_pass(
                "copy view",
                [&](PassBuilder& pass) {
                    pass.use(debugger.shown(), ResourceState::CopySrc);
                    pass.use(copy, ResourceState::CopyDst);
                },
                [view = debugger.shown(), copy](PassContext& context) {
                    context.list.copy_texture_to_buffer(context.texture(view), context.buffer(copy));
                });
        }
        used = true;
        graph.execute();
        debugger.end_frame(graph);
        device.end_frame();
        kit.library().update();
        if (debugger.probe().valid) {
            break;
        }
    }
    device.wait_idle();
    return debugger.probe();
}

int channel(uint32_t texel, uint32_t index) {
    return static_cast<int>((texel >> (index * 8U)) & 0xFFU);
}

bool has_line(const de::core::Array<de::console::Line>& lines, std::string_view part) {
    for (const de::console::Line& line : lines) {
        if (line.text.view().find(part) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("the frame debugger shows a texture as a pass left it, and reads its texels back") {
    auto device = make_device();
    if (!device) {
        return;
    }
    ShaderKit kit(*device);
    auto graph = RenderGraph::create(*device, {.gpu_timing = true});
    REQUIRE(graph.ok());
    const BufferHandle out =
        *device->create_buffer({.size = 64, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});
    const BufferHandle view_copy = *device->create_buffer({.size = uint64_t{kViewSide} * kViewSide * 4,
                                                           .usage = BufferUsage::CopyDst,
                                                           .memory = MemoryLocation::Readback});
    {
        FrameDebugger debugger = FrameDebugger::create(*device, kit.library());

        // Closed: it adds nothing to the frame, and takes nothing in.
        debugger.select("paint");
        CHECK_FALSE(run_frames(*device, kit, *graph, debugger, out).valid);
        CHECK(graph->pass_count() == 3);
        CHECK_FALSE(debugger.viewing());

        // Open, looking at "paint": the first texture it writes, as it left it, though "wipe" blacks it out after.
        debugger.set_open(true);
        debugger.set_view_size(kView, kView);
        debugger.probe_at(1, 2);
        const ProbeValue painted = run_frames(*device, kit, *graph, debugger, out, view_copy);
        REQUIRE(painted.valid);
        CHECK(debugger.viewing());
        CHECK(std::string_view(debugger.view_problem()).empty());
        CHECK(painted.x == 1);
        CHECK(painted.y == 2);
        CHECK(painted.value[0] == doctest::Approx(0.25).epsilon(0.03));
        CHECK(painted.value[1] == doctest::Approx(0.5).epsilon(0.03));
        CHECK(painted.value[2] == doctest::Approx(0.75).epsilon(0.03));
        CHECK(painted.value[3] == doctest::Approx(1.0));
        // The frame itself is as it was: black at its end.
        device->invalidate_mapped(out);
        CHECK(read_u32(*device, out, 5) == 0xFF000000);
        // The view's picture: the whole texture fitted into it, its values as stored. In the middle of a texel
        // (texels are 16 pixels wide here, with a line between them).
        device->invalidate_mapped(view_copy);
        const uint32_t middle = read_u32(*device, view_copy, (kViewSide * 24) + 24);
        CHECK(std::abs(channel(middle, 0) - 64) <= 2);
        CHECK(std::abs(channel(middle, 1) - 128) <= 2);
        CHECK(std::abs(channel(middle, 2) - 191) <= 2);

        // Its passes are in the frame, right after the one looked at, and in what it lists.
        REQUIRE(graph->pass_count() >= 6);
        CHECK((std::string_view(graph->pass_info(0).name) == "paint"));
        CHECK((std::string_view(graph->pass_info(1).name) == "frame debugger view"));
        CHECK(graph->pass_info(1).work.draws == 1);
        CHECK(graph->pass_info(1).work.vertices == 3);
        de::core::Array<de::console::Line> lines;
        de::console::Output said(lines);
        debugger.report(said);
        CHECK(has_line(lines, "paint"));
        CHECK(has_line(lines, "wipe"));
        CHECK(has_line(lines, "4 passes"));             // paint, wipe, out, and the copy of the view made here
        CHECK_FALSE(has_line(lines, "frame debugger")); // its own passes aren't among those it lists
        lines.clear();
        debugger.report(said, 0);
        CHECK(has_line(lines, "painted depth"));
        CHECK(has_line(lines, "4x4 rgba8_unorm"));

        // The same texture after "wipe": black.
        debugger.select("wipe");
        debugger.probe_at(1, 2);
        const ProbeValue wiped = run_frames(*device, kit, *graph, debugger, out);
        REQUIRE(wiped.valid);
        CHECK(wiped.value[0] == doctest::Approx(0.0));
        CHECK(wiped.value[3] == doctest::Approx(1.0));

        // A depth texture, by name.
        debugger.select("paint", 0, "painted depth");
        debugger.probe_at(3, 3);
        const ProbeValue depth = run_frames(*device, kit, *graph, debugger, out);
        REQUIRE(depth.valid);
        CHECK(depth.value[0] == doctest::Approx(0.5));

        // A pass, or a texture, that isn't there: nothing drawn, and it says why.
        debugger.select("nowhere");
        CHECK_FALSE(run_frames(*device, kit, *graph, debugger, out).valid);
        CHECK_FALSE(debugger.viewing());
        CHECK_FALSE(std::string_view(debugger.view_problem()).empty());
        debugger.select("paint", 0, "no such texture");
        CHECK_FALSE(run_frames(*device, kit, *graph, debugger, out).valid);
        CHECK_FALSE(std::string_view(debugger.view_problem()).empty());

        // Closed again: the frame is its own three passes.
        debugger.set_open(false);
        static_cast<void>(run_frames(*device, kit, *graph, debugger, out));
        CHECK(graph->pass_count() == 3);
    }
    device->wait_idle();
    device->destroy(out);
    device->destroy(view_copy);
    check_clean(*device);
}

TEST_CASE("the frame debugger reads integer textures, through a float copy of them") {
    auto device = make_device();
    if (!device) {
        return;
    }
    ShaderKit kit(*device);
    const PipelineHandle write = kit.pipeline(kit.compute("tests.write_storage_uint"));
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    {
        FrameDebugger debugger = FrameDebugger::create(*device, kit.library());
        debugger.set_open(true);
        debugger.set_view_size(kView, kView);
        debugger.select("ids");
        debugger.probe_at(2, 1);
        struct Ids {
            GraphTexture texture;
        };
        ProbeValue probe;
        for (int frame = 0; frame < 40 && !probe.valid; ++frame) {
            device->begin_frame();
            graph->begin(4, 4);
            debugger.begin_frame(*graph, Format::RGBA8Unorm);
            // Each texel: x + y * width + 100.
            static_cast<void>(graph->add_pass<Ids>(
                "ids",
                [&](PassBuilder& pass, Ids& data) {
                    data.texture = pass.create_texture(
                        {.desc = {.width = 4, .height = 4, .format = Format::R32Uint, .debug_name = "ids"}});
                    pass.use(data.texture, ResourceState::ShaderWrite, ShaderStages::Compute);
                    pass.never_cull();
                },
                [write](const Ids& data, PassContext& context) {
                    context.list.bind_pipeline(write);
                    context.list.push_constants(de::test::WriteStorageUintPush{
                        .texture = context.device.storage_texture_index(context.texture(data.texture)),
                        .width = 4,
                        .height = 4,
                        .value = 100});
                    context.list.dispatch(1);
                }));
            graph->execute();
            debugger.end_frame(*graph);
            device->end_frame();
            kit.library().update();
            probe = debugger.probe();
        }
        device->wait_idle();
        REQUIRE(probe.valid);
        CHECK(debugger.viewing());
        CHECK(probe.value[0] == doctest::Approx(106.0)); // at 2, 1
        // In the frame: the pass looked at, the copy into floats, the view of that.
        REQUIRE(graph->pass_count() >= 3);
        CHECK((std::string_view(graph->pass_info(1).name) == "frame debugger ids"));
        CHECK(graph->pass_info(1).work.dispatches == 1);
        CHECK((std::string_view(graph->pass_info(2).name) == "frame debugger view"));
    }
    device->wait_idle();
    check_clean(*device);
}
