#include "render/graph/render_graph.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "support/gpu.h"
#include "support/shader_kit.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

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

namespace {

constexpr uint32_t kWidth = 8;

// Matches IndirectColumnsPush in shaders/tests/indirect_columns.slang.
struct ColumnsPush {
    uint32_t width = kWidth;
};

// Matches WriteIndirectPush in shaders/tests/write_indirect.slang.
struct WriteIndirectPush {
    uint64_t output = 0;
    uint32_t count = 0;
    uint32_t mode = 0;
};

constexpr uint32_t kDrawsMode = 0;
constexpr uint32_t kDispatchMode = 1;
constexpr uint64_t kFirstDrawOffset = 16; // tests.write_indirect: the count, then the draws

// What a column says: as tests.indirect_columns writes it.
struct Column {
    uint32_t instance = 0; // all of these + 1: 0 is a column nothing drew
    uint32_t draw = 0;
    uint32_t instance_id = 0;
    uint32_t vertex_sixths = 0;
};

// A row of columns to draw into and read from, and what to draw with.
struct Bench {
    de::rhi::Device& device;
    ShaderKit& kit;
    RenderGraph& graph;
    PipelineHandle columns;
    TextureHandle target;
    BufferHandle readback;

    std::array<Column, kWidth> read() {
        device.invalidate_mapped(readback);
        std::array<Column, kWidth> out{};
        for (uint32_t x = 0; x < kWidth; ++x) {
            const uint32_t texel = read_u32(device, readback, x);
            out[x] = {.instance = texel & 0xFFU,
                      .draw = (texel >> 8U) & 0xFFU,
                      .instance_id = (texel >> 16U) & 0xFFU,
                      .vertex_sixths = texel >> 24U};
        }
        return out;
    }

    // One frame: draw(list) inside a cleared rendering, with the pipeline bound and its push set, after
    // setup(pass) has declared what it reads; then the target is read back.
    template <typename Setup, typename Draw>
    void frame(Setup&& setup, Draw&& draw) {
        graph.add_pass(
            "columns",
            [&](PassBuilder& pass) {
                setup(pass);
                pass.never_cull();
                color =
                    graph.import_texture({.texture = target, .name = "target", .final_state = ResourceState::CopySrc});
                pass.use(color, ResourceState::RenderTarget);
            },
            [this, &draw](PassContext& context) {
                const ColorAttachment attachment{.texture = context.texture(color), .load = LoadOp::Clear, .clear = {}};
                context.list.begin_rendering({.colors = std::span(&attachment, 1)});
                context.list.bind_pipeline(columns);
                context.list.push_constants(ColumnsPush{});
                draw(context, context.list);
                context.list.end_rendering();
            });
        const GraphBuffer out =
            graph.import_buffer({.buffer = readback, .name = "readback", .final_state = ResourceState::HostRead});
        const GraphTexture from = color;
        graph.add_pass(
            "read target",
            [&](PassBuilder& pass) {
                pass.use(from, ResourceState::CopySrc);
                pass.use(out, ResourceState::CopyDst);
            },
            [from, out](PassContext& context) {
                context.list.copy_texture_to_buffer(context.texture(from), context.buffer(out));
            });
        graph.execute();
        device.end_frame();
        device.wait_idle();
    }

    GraphTexture color;
};

} // namespace

TEST_CASE("indirect draws read their arguments, and the shaders see what they say") {
    auto device = make_device();
    if (!device) {
        return;
    }
    ShaderKit kit(*device);
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    GraphicsPipelineDesc state{.color_count = 1, .cull = CullMode::None, .debug_name = "columns"};
    state.color_formats[0] = Format::RGBA8Unorm;
    const auto program = kit.library().create_graphics({.module = "tests.indirect_columns", .state = state});
    REQUIRE(program.ok());
    const PipelineHandle columns = kit.pipeline(*program);
    REQUIRE(columns.is_valid());
    const TextureHandle target = *device->create_texture({.width = kWidth,
                                                          .height = 1,
                                                          .format = Format::RGBA8Unorm,
                                                          .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc,
                                                          .debug_name = "target"});
    const BufferHandle readback = *device->create_buffer({.size = uint64_t{kWidth} * 4,
                                                          .usage = BufferUsage::CopyDst,
                                                          .memory = MemoryLocation::Readback,
                                                          .debug_name = "readback"});
    Bench bench{.device = *device,
                .kit = kit,
                .graph = *graph,
                .columns = columns,
                .target = target,
                .readback = readback,
                .color = {}};

    const BufferHandle args = *device->create_buffer(
        {.size = 256, .usage = BufferUsage::Indirect, .memory = MemoryLocation::Upload, .debug_name = "args"});
    const BufferHandle indices = *device->create_buffer(
        {.size = 64, .usage = BufferUsage::Index, .memory = MemoryLocation::Upload, .debug_name = "indices"});
    const auto write_args = [&](const auto& values, uint64_t offset = 0) {
        std::memcpy(device->mapped(args).data() + offset, &values, sizeof(values));
        device->flush_mapped(args);
    };
    const auto begin = [&] {
        device->begin_frame();
        graph->begin(kWidth, 1);
    };
    const auto import_args = [&] {
        return graph->import_buffer({.buffer = args, .name = "args", .state = ResourceState::Undefined});
    };

    SUBCASE("a draw's own numbers, and several in one call") {
        write_args(std::array{
            DrawIndirectArgs{.vertex_count = 6, .instance_count = 2, .first_instance = 1},
            DrawIndirectArgs{.vertex_count = 6, .instance_count = 1, .first_vertex = 12, .first_instance = 5}});
        begin();
        const GraphBuffer used = import_args();
        bench.frame([&](PassBuilder& pass) { pass.use(used, ResourceState::IndirectArgument); },
                    [&](PassContext& context, CommandList& list) { list.draw_indirect(context.buffer(used), 0, 2); });
        const auto seen = bench.read();
        CHECK(seen[0].instance == 0); // nothing drew instance 0
        CHECK(seen[1].instance == 2);
        CHECK(seen[2].instance == 3);
        CHECK(seen[3].instance == 0);
        CHECK(seen[5].instance == 6);
        CHECK(seen[1].draw == 1);
        CHECK(seen[5].draw == 2); // SV_DrawIndex tells them apart
    }

    SUBCASE("the shader-visible IDs leave out the draw's own first vertex and first instance") {
        // What a shared buffer has to work around: these two don't say where in it a draw is.
        write_args(std::array{
            DrawIndirectArgs{.vertex_count = 6, .instance_count = 1, .first_vertex = 12, .first_instance = 3}});
        begin();
        const GraphBuffer used = import_args();
        bench.frame([&](PassBuilder& pass) { pass.use(used, ResourceState::IndirectArgument); },
                    [&](PassContext& context, CommandList& list) { list.draw_indirect(context.buffer(used), 0, 1); });
        const auto seen = bench.read();
        CHECK(seen[3].instance == 4);
        CHECK(seen[3].instance_id == 1);   // SV_InstanceID: 0, for the first instance of the draw
        CHECK(seen[3].vertex_sixths == 1); // SV_VertexID: 0 to 5, not 12 to 17
    }

    SUBCASE("indexed, from a shared index buffer: first index and vertex offset") {
        const std::array<uint32_t, 8> shared = {99, 99, 0, 1, 2, 3, 4, 5};
        std::memcpy(device->mapped(indices).data(), shared.data(), sizeof(shared));
        device->flush_mapped(indices);
        write_args(std::array{DrawIndexedIndirectArgs{
            .index_count = 6, .instance_count = 1, .first_index = 2, .vertex_offset = 24, .first_instance = 4}});
        begin();
        const GraphBuffer used = import_args();
        const GraphBuffer index_buffer = graph->import_buffer({.buffer = indices, .name = "indices"});
        bench.frame(
            [&](PassBuilder& pass) {
                pass.use(used, ResourceState::IndirectArgument);
                pass.use(index_buffer, ResourceState::IndexBuffer);
            },
            [&](PassContext& context, CommandList& list) {
                list.bind_index_buffer(context.buffer(index_buffer), 0, IndexType::Uint32);
                list.draw_indexed_indirect(context.buffer(used), 0, 1);
            });
        const auto seen = bench.read();
        CHECK(seen[4].instance == 5);
        CHECK(seen[4].vertex_sixths == 1); // SV_VertexID leaves out the vertex offset too
        CHECK(seen[0].instance == 0);
    }

    SUBCASE("a count from a buffer, indexed: up to the most asked for") {
        const std::array<uint32_t, 6> shared = {0, 1, 2, 3, 4, 5};
        std::memcpy(device->mapped(indices).data(), shared.data(), sizeof(shared));
        device->flush_mapped(indices);
        std::array<DrawIndexedIndirectArgs, 4> draws{};
        for (uint32_t i = 0; i < draws.size(); ++i) {
            draws[i] = {.index_count = 6, .instance_count = 1, .first_instance = i};
        }
        write_args(uint32_t{4});
        write_args(draws, kFirstDrawOffset);
        begin();
        const GraphBuffer used = import_args();
        const GraphBuffer index_buffer = graph->import_buffer({.buffer = indices, .name = "indices"});
        bench.frame(
            [&](PassBuilder& pass) {
                pass.use(used, ResourceState::IndirectArgument);
                pass.use(index_buffer, ResourceState::IndexBuffer);
            },
            [&](PassContext& context, CommandList& list) {
                list.bind_index_buffer(context.buffer(index_buffer), 0, IndexType::Uint32);
                list.draw_indexed_indirect_count(context.buffer(used), kFirstDrawOffset, context.buffer(used), 0, 3);
            });
        const auto seen = bench.read();
        CHECK(seen[0].instance == 1);
        CHECK(seen[2].instance == 3);
        CHECK(seen[3].instance == 0); // the count says 4; only 3 were allowed
    }

    SUBCASE("arguments apart by a stride") {
        std::array<uint32_t, 16> spaced{};
        spaced[0] = 6;
        spaced[1] = 1;
        spaced[3] = 2; // first instance
        spaced[8] = 6;
        spaced[9] = 1;
        spaced[11] = 6;
        write_args(spaced);
        begin();
        const GraphBuffer used = import_args();
        bench.frame(
            [&](PassBuilder& pass) { pass.use(used, ResourceState::IndirectArgument); },
            [&](PassContext& context, CommandList& list) { list.draw_indirect(context.buffer(used), 0, 2, 32); });
        const auto seen = bench.read();
        CHECK(seen[2].instance == 3);
        CHECK(seen[6].instance == 7);
    }

    SUBCASE("written by compute in the same frame, with a count: the whole path") {
        const auto write = kit.compute("tests.write_indirect");
        const PipelineHandle writer = kit.pipeline(write);
        struct Written {
            GraphBuffer draws;
        };
        begin();
        const GraphBuffer written = graph
                                        ->add_pass<Written>(
                                            "write draws",
                                            [&](PassBuilder& pass, Written& data) {
                                                data.draws = pass.create_buffer({.size = 512, .debug_name = "draws"});
                                                pass.use(data.draws, ResourceState::ShaderWrite, ShaderStages::Compute);
                                            },
                                            [writer](const Written& data, PassContext& context) {
                                                context.list.bind_pipeline(writer);
                                                context.list.push_constants(WriteIndirectPush{
                                                    .output = context.device.buffer_address(context.buffer(data.draws)),
                                                    .count = 5,
                                                    .mode = kDrawsMode});
                                                context.list.dispatch(1);
                                            })
                                        .draws;
        bench.frame([&](PassBuilder& pass) { pass.use(written, ResourceState::IndirectArgument); },
                    [&](PassContext& context, CommandList& list) {
                        list.draw_indirect_count(context.buffer(written), kFirstDrawOffset, context.buffer(written), 0,
                                                 8);
                    });
        const auto seen = bench.read();
        for (uint32_t x = 0; x < 5; ++x) {
            CAPTURE(x);
            CHECK(seen[x].instance == x + 1);
            CHECK(seen[x].draw == x + 1);
        }
        CHECK(seen[5].instance == 0);
        CHECK(seen[7].instance == 0);
    }

    device->wait_idle();
    device->destroy(args);
    device->destroy(indices);
    device->destroy(target);
    device->destroy(readback);
    check_clean(*device);
}

TEST_CASE("an indirect dispatch runs the groups its arguments say, written by compute the same frame") {
    auto device = make_device();
    if (!device) {
        return;
    }
    ShaderKit kit(*device);
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    const PipelineHandle writer = kit.pipeline(kit.compute("tests.write_indirect"));
    const auto groups_program =
        kit.library().create_compute({.module = "tests.write_indirect", .entry = "groups_main"});
    REQUIRE(groups_program.ok());
    const PipelineHandle groups = kit.pipeline(*groups_program);
    REQUIRE(groups.is_valid());
    const BufferHandle readback = *device->create_buffer(
        {.size = 64, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback, .debug_name = "readback"});

    struct Made {
        GraphBuffer buffer;
    };
    device->begin_frame();
    graph->begin(1, 1);
    const GraphBuffer arguments = graph
                                      ->add_pass<Made>(
                                          "write arguments",
                                          [&](PassBuilder& pass, Made& data) {
                                              data.buffer = pass.create_buffer({.size = 64, .debug_name = "arguments"});
                                              pass.use(data.buffer, ResourceState::ShaderWrite, ShaderStages::Compute);
                                          },
                                          [writer](const Made& data, PassContext& context) {
                                              context.list.bind_pipeline(writer);
                                              context.list.push_constants(WriteIndirectPush{
                                                  .output = context.device.buffer_address(context.buffer(data.buffer)),
                                                  .count = 3,
                                                  .mode = kDispatchMode});
                                              context.list.dispatch(1);
                                          })
                                      .buffer;
    const GraphBuffer values = graph
                                   ->add_pass<Made>(
                                       "clear values",
                                       [&](PassBuilder& pass, Made& data) {
                                           data.buffer = pass.create_buffer({.size = 64, .debug_name = "values"});
                                           pass.use(data.buffer, ResourceState::CopyDst);
                                       },
                                       [](const Made& data, PassContext& context) {
                                           context.list.fill_buffer(context.buffer(data.buffer), 0, 64, 0);
                                       })
                                   .buffer;
    graph->add_pass(
        "run groups",
        [&](PassBuilder& pass) {
            pass.use(values, ResourceState::ShaderWrite, ShaderStages::Compute);
            pass.use(arguments, ResourceState::IndirectArgument);
        },
        [groups, arguments, values](PassContext& context) {
            context.list.bind_pipeline(groups);
            context.list.push_constants(
                WriteIndirectPush{.output = context.device.buffer_address(context.buffer(values))});
            context.list.dispatch_indirect(context.buffer(arguments), 0);
        });
    const GraphBuffer out =
        graph->import_buffer({.buffer = readback, .name = "readback", .final_state = ResourceState::HostRead});
    graph->add_pass(
        "copy",
        [&](PassBuilder& pass) {
            pass.use(values, ResourceState::CopySrc);
            pass.use(out, ResourceState::CopyDst);
        },
        [values, out](PassContext& context) {
            context.list.copy_buffer(context.buffer(values), 0, context.buffer(out), 0, 64);
        });
    graph->execute();
    device->end_frame();
    device->wait_idle();
    device->invalidate_mapped(readback);
    CHECK(read_u32(*device, readback, 0) == 1);
    CHECK(read_u32(*device, readback, 1) == 2);
    CHECK(read_u32(*device, readback, 2) == 3);
    CHECK(read_u32(*device, readback, 3) == 0);
    device->destroy(readback);
    check_clean(*device);
}
