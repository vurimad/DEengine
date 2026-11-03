#include "render/graph/render_graph.h"
#include "render/rhi/device.h"
#include "support/gpu.h"
#include "support/shader_kit.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

using namespace de::rhi;
using de::render::GraphBuffer;
using de::render::GraphTexture;
using de::render::PassBuilder;
using de::render::PassContext;
using de::render::RenderGraph;
using de::test::check_clean;
using de::test::FillBufferPush;
using de::test::make_device;
using de::test::read_u32;
using de::test::ShaderKit;

namespace {

constexpr Color kClear{.r = 0.25f, .g = 0.5f, .b = 0.75f, .a = 1.0f};
constexpr uint32_t kClearRgba = 0xFFBF8040; // kClear as RGBA8, little endian

const TextureDesc kSmallColor{.width = 4, .height = 4, .format = Format::RGBA8Unorm, .debug_name = "small color"};

// The data of a pass that makes one texture.
struct TextureData {
    GraphTexture texture;
};

// Clears a whole single-mip texture through a render pass.
void clear_texture(PassContext& context, GraphTexture texture) {
    const ColorAttachment attachment{.texture = context.texture(texture), .clear = kClear};
    context.list.begin_rendering({.colors = std::span(&attachment, 1)});
    context.list.end_rendering();
}

// A pass that makes a small texture and clears it.
const TextureData& add_clear(RenderGraph& graph, const char* name, const TextureDesc& desc = kSmallColor,
                             float reference_scale = 0.0f) {
    return graph.add_pass<TextureData>(
        name,
        [&](PassBuilder& pass, TextureData& data) {
            data.texture = pass.create_texture({.desc = desc, .reference_scale = reference_scale});
            pass.use(data.texture, ResourceState::RenderTarget);
        },
        [](const TextureData& data, PassContext& context) { clear_texture(context, data.texture); });
}

// A pass that copies a single-mip texture into an imported buffer.
void add_copy_out(RenderGraph& graph, GraphTexture texture, GraphBuffer out) {
    graph.add_pass(
        "copy out",
        [&](PassBuilder& pass) {
            pass.use(texture, ResourceState::CopySrc);
            pass.use(out, ResourceState::CopyDst);
        },
        [texture, out](PassContext& context) {
            context.list.copy_texture_to_buffer(context.texture(texture), context.buffer(out));
        });
}

BufferHandle make_readback(Device& device, uint64_t size) {
    return *device.create_buffer({.size = size, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});
}

// Every pixel of an RGBA8 readback within one of the expected color.
bool all_pixels(Device& device, BufferHandle readback, uint32_t pixels, uint32_t rgba) {
    device.invalidate_mapped(readback);
    for (uint32_t i = 0; i < pixels; ++i) {
        const uint32_t got = read_u32(device, readback, i);
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            const int a = static_cast<int>((got >> shift) & 0xFFU);
            const int b = static_cast<int>((rgba >> shift) & 0xFFU);
            if (std::abs(a - b) > 1) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TEST_CASE("passes run in order, with the barriers their uses need") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    const BufferHandle readback = make_readback(*device, 64);

    device->begin_frame();
    graph->begin(4, 4);
    const GraphBuffer out =
        graph->import_buffer({.buffer = readback, .name = "readback", .final_state = ResourceState::HostRead});
    const TextureData& clear = add_clear(*graph, "clear");
    add_copy_out(*graph, clear.texture, out);
    graph->execute();
    device->end_frame();
    device->wait_idle();

    CHECK(all_pixels(*device, readback, 16, kClearRgba));
    const auto stats = graph->stats();
    CHECK(stats.passes == 2);
    CHECK(stats.culled_passes == 0);
    CHECK(stats.barriers == 4); // to render target, to copy source, readback to copy dst, then to host read
    CHECK(stats.pooled_textures == 1);
    device->destroy(readback);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("passes nothing needs are culled; never_cull and imported writes keep theirs") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    const TextureHandle target = *device->create_texture(
        {.width = 4, .height = 4, .format = Format::RGBA8Unorm, .usage = TextureUsage::RenderTarget});
    bool ran[5] = {};

    device->begin_frame();
    graph->begin(4, 4);
    const GraphTexture imported =
        graph->import_texture({.texture = target, .name = "target", .final_state = ResourceState::RenderTarget});
    struct Flag {
        bool* ran = nullptr;
        GraphTexture texture;
    };
    const auto flagging = [](const Flag& data, PassContext&) { *data.ran = true; };
    // 0 writes a texture that only the never_cull pass 1 reads: both run. 2 writes an imported
    // texture: it runs. 3 writes what only 4 reads, and 4 writes nothing: both go.
    const Flag& unread = graph->add_pass<Flag>(
        "writer",
        [&](PassBuilder& pass, Flag& data) {
            data.ran = &ran[0];
            data.texture = pass.create_texture({.desc = kSmallColor});
            pass.use(data.texture, ResourceState::RenderTarget);
        },
        flagging);
    graph->add_pass<Flag>(
        "kept",
        [&](PassBuilder& pass, Flag& data) {
            data.ran = &ran[1];
            pass.use(unread.texture, ResourceState::ShaderRead).never_cull();
        },
        flagging);
    graph->add_pass<Flag>(
        "imported",
        [&](PassBuilder& pass, Flag& data) {
            data.ran = &ran[2];
            data.texture = imported;
            pass.use(imported, ResourceState::RenderTarget);
        },
        [](const Flag& data, PassContext& context) {
            clear_texture(context, data.texture);
            *data.ran = true;
        });
    const Flag& chain = graph->add_pass<Flag>(
        "chain writer",
        [&](PassBuilder& pass, Flag& data) {
            data.ran = &ran[3];
            data.texture = pass.create_texture({.desc = kSmallColor});
            pass.use(data.texture, ResourceState::RenderTarget);
        },
        flagging);
    graph->add_pass<Flag>(
        "chain reader",
        [&](PassBuilder& pass, Flag& data) {
            data.ran = &ran[4];
            pass.use(chain.texture, ResourceState::ShaderRead);
        },
        flagging);
    graph->execute();
    device->end_frame();

    CHECK(ran[0]);
    CHECK(ran[1]);
    CHECK(ran[2]);
    CHECK_FALSE(ran[3]);
    CHECK_FALSE(ran[4]);
    CHECK(graph->stats().culled_passes == 2);
    CHECK(graph->stats().pooled_textures == 1); // the culled chain's texture was never made
    CHECK(graph->final_texture_state(imported) == ResourceState::RenderTarget);
    device->destroy(target);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("reads in one layout share their barrier; new stages and writes get their own") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    device->begin_frame();
    graph->begin(4, 4);
    const GraphTexture texture = add_clear(*graph, "write").texture;
    const auto read = [&](const char* name, ShaderStages stages) {
        graph->add_pass(
            name, [&](PassBuilder& pass) { pass.use(texture, ResourceState::ShaderRead, stages).never_cull(); },
            [](PassContext&) {});
    };
    read("fragment read", ShaderStages::Fragment);                      // barrier: render target to read
    read("fragment read again", ShaderStages::Fragment);                // none
    read("compute read", ShaderStages::Compute);                        // barrier: a new stage
    read("both again", ShaderStages::Fragment | ShaderStages::Compute); // none
    graph->add_pass(
        "write again", [&](PassBuilder& pass) { pass.use(texture, ResourceState::RenderTarget); },
        [texture](PassContext& context) { clear_texture(context, texture); });
    read("last read", ShaderStages::Fragment);
    graph->execute();
    device->end_frame();
    // The write, two read barriers, the second write (after both read stages), the last read.
    CHECK(graph->stats().barriers == 5);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("mips are tracked one by one: a chain of blits down a texture") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    constexpr uint64_t kTop = uint64_t{8} * 8 * 4;
    constexpr uint64_t kAll = kTop + 64 + 16 + 4;
    const BufferHandle upload =
        *device->create_buffer({.size = kTop, .usage = BufferUsage::CopySrc, .memory = MemoryLocation::Upload});
    for (uint32_t i = 0; i < kTop / 4; ++i) {
        std::memcpy(device->mapped(upload).data() + (size_t{i} * 4), &kClearRgba, 4);
    }
    device->flush_mapped(upload);
    const BufferHandle readback = make_readback(*device, kAll);

    device->begin_frame();
    graph->begin(8, 8);
    const GraphBuffer source = graph->import_buffer({.buffer = upload, .name = "upload"});
    const GraphBuffer out =
        graph->import_buffer({.buffer = readback, .name = "readback", .final_state = ResourceState::HostRead});
    struct Top {
        GraphTexture texture;
        GraphBuffer source;
    };
    const GraphTexture texture =
        graph
            ->add_pass<Top>(
                "top",
                [&](PassBuilder& pass, Top& data) {
                    data.texture = pass.create_texture({.desc = {.width = 8,
                                                                 .height = 8,
                                                                 .mip_count = 4,
                                                                 .format = Format::RGBA8Unorm,
                                                                 .debug_name = "mips"}});
                    data.source = source;
                    pass.use(source, ResourceState::CopySrc);
                    pass.use(data.texture, ResourceState::CopyDst, ShaderStages::All, {.first_mip = 0, .mip_count = 1});
                },
                [](const Top& data, PassContext& context) {
                    context.list.copy_buffer_to_texture(context.buffer(data.source), context.texture(data.texture));
                })
            .texture;
    for (uint32_t mip = 1; mip < 4; ++mip) {
        graph->add_pass(
            "down",
            [&](PassBuilder& pass) {
                pass.use(texture, ResourceState::CopySrc, ShaderStages::All, {.first_mip = mip - 1, .mip_count = 1});
                pass.use(texture, ResourceState::CopyDst, ShaderStages::All, {.first_mip = mip, .mip_count = 1});
            },
            [texture, mip](PassContext& context) {
                context.list.blit_texture(context.texture(texture), context.texture(texture),
                                          {.src_mip = mip - 1, .dst_mip = mip});
            });
    }
    graph->add_pass(
        "read all",
        [&](PassBuilder& pass) {
            pass.use(texture, ResourceState::CopySrc);
            pass.use(out, ResourceState::CopyDst);
        },
        [texture, out](PassContext& context) {
            uint64_t offset = 0;
            for (uint32_t mip = 0; mip < 4; ++mip) {
                context.list.copy_texture_to_buffer(context.texture(texture), context.buffer(out),
                                                    {.buffer_offset = offset, .mip = mip});
                const uint32_t size = 8U >> mip;
                offset += uint64_t{size} * size * 4;
            }
        });
    graph->execute();
    device->end_frame();
    device->wait_idle();
    CHECK(all_pixels(*device, readback, static_cast<uint32_t>(kAll / 4), kClearRgba)); // solid, every mip
    device->destroy(upload);
    device->destroy(readback);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("transient resources are pooled across frames, and go once unused") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto graph = RenderGraph::create(*device, {.keep_unused_frames = 3});
    REQUIRE(graph.ok());
    struct Scaled {
        GraphTexture texture;
        TextureHandle* physical = nullptr;
    };
    TextureHandle seen[2];
    // The same texture in back-to-back frames, nothing waited for between them: its first barrier
    // each frame must wait for the last frame's use (sync validation checks).
    for (TextureHandle& physical : seen) {
        device->begin_frame();
        graph->begin(64, 64);
        graph->add_pass<Scaled>(
            "scaled",
            [&](PassBuilder& pass, Scaled& data) {
                data.texture = pass.create_texture({.desc = kSmallColor, .reference_scale = 0.5f});
                data.physical = &physical;
                pass.use(data.texture, ResourceState::RenderTarget).never_cull();
            },
            [](const Scaled& data, PassContext& context) {
                *data.physical = context.texture(data.texture);
                CHECK(context.desc(data.texture).width == 32);
                CHECK(context.desc(data.texture).height == 32);
                CHECK(context.desc(data.texture).usage == TextureUsage::RenderTarget);
                clear_texture(context, data.texture);
            });
        graph->execute();
        device->end_frame();
    }
    CHECK(seen[0] == seen[1]);
    CHECK(graph->stats().pool_creations == 1);
    CHECK(graph->stats().pooled_textures == 1);

    // A different size is another texture; unused ones go after keep_unused_frames.
    for (uint32_t frame = 0; frame < 6; ++frame) {
        device->begin_frame();
        graph->begin(16, 16);
        graph->add_pass(
            "other size",
            [&](PassBuilder& pass) {
                pass.use(pass.create_texture({.desc = kSmallColor, .reference_scale = 0.5f}),
                         ResourceState::RenderTarget)
                    .never_cull();
            },
            [](PassContext&) {});
        graph->execute();
        device->end_frame();
    }
    CHECK(graph->stats().pool_creations == 2);
    CHECK(graph->stats().pooled_textures == 1); // the 32x32 one went
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("transient buffers: a compute write, then a copy out") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    ShaderKit kit(*device);
    struct Fill {
        GraphBuffer values;
        PipelineHandle pipeline;
    };
    const PipelineHandle pipeline = kit.pipeline(kit.compute("tests.fill_buffer"));
    const BufferHandle readback = make_readback(*device, 256);

    device->begin_frame();
    graph->begin(1, 1);
    const GraphBuffer out =
        graph->import_buffer({.buffer = readback, .name = "readback", .final_state = ResourceState::HostRead});
    const GraphBuffer values = graph
                                   ->add_pass<Fill>(
                                       "fill",
                                       [&](PassBuilder& pass, Fill& data) {
                                           data.values = pass.create_buffer({.size = 256, .debug_name = "values"});
                                           data.pipeline = pipeline;
                                           pass.use(data.values, ResourceState::ShaderWrite, ShaderStages::Compute);
                                       },
                                       [](const Fill& data, PassContext& context) {
                                           context.list.bind_pipeline(data.pipeline);
                                           context.list.push_constants(FillBufferPush{
                                               .output = context.device.buffer_address(context.buffer(data.values)),
                                               .value = 100,
                                               .count = 64});
                                           context.list.dispatch(1);
                                       })
                                   .values;
    graph->add_pass(
        "copy",
        [&](PassBuilder& pass) {
            pass.use(values, ResourceState::CopySrc);
            pass.use(out, ResourceState::CopyDst);
        },
        [values, out](PassContext& context) {
            context.list.copy_buffer(context.buffer(values), 0, context.buffer(out), 0, 256);
        });
    graph->execute();
    device->end_frame();
    device->wait_idle();
    device->invalidate_mapped(readback);
    int wrong = 0;
    for (uint32_t i = 0; i < 64; ++i) {
        wrong += read_u32(*device, readback, i) == 100 + i ? 0 : 1;
    }
    CHECK(wrong == 0);
    CHECK(graph->stats().pooled_buffers == 1);
    device->destroy(readback);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("immediate mode runs each pass as it is added, culling nothing") {
    if (DE_BUILD_DEV == 0) {
        return;
    }
    auto device = make_device();
    if (!device) {
        return;
    }
    auto graph = RenderGraph::create(*device, {.immediate = true, .log_transitions = true});
    REQUIRE(graph.ok());
    const BufferHandle readback = make_readback(*device, 64);
    device->begin_frame();
    graph->begin(4, 4);
    const GraphBuffer out =
        graph->import_buffer({.buffer = readback, .name = "readback", .final_state = ResourceState::HostRead});
    struct Ran {
        bool* ran = nullptr;
    };
    bool unread_ran = false;
    graph->add_pass<Ran>(
        "unread",
        [&](PassBuilder& pass, Ran& data) {
            data.ran = &unread_ran;
            pass.use(pass.create_texture({.desc = kSmallColor}), ResourceState::RenderTarget);
        },
        [](const Ran& data, PassContext&) { *data.ran = true; });
    CHECK(unread_ran); // already, and never culled
    const GraphTexture color = add_clear(*graph, "clear").texture;
    add_copy_out(*graph, color, out);
    graph->execute();
    graph->dump();
    device->end_frame();
    device->wait_idle();
    CHECK(all_pixels(*device, readback, 16, kClearRgba));
    CHECK(graph->stats().culled_passes == 0);
    device->destroy(readback);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("imported textures come in from their state and leave in their final one") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    const TextureHandle persistent =
        *device->create_texture({.width = 4,
                                 .height = 4,
                                 .format = Format::RGBA8Unorm,
                                 .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc,
                                 .debug_name = "persistent"});
    const BufferHandle readback = make_readback(*device, 64);

    // Frame 1 renders it and leaves it ready to copy from; frame 2 imports it in that state.
    device->begin_frame();
    graph->begin(4, 4);
    const GraphTexture first =
        graph->import_texture({.texture = persistent, .name = "persistent", .final_state = ResourceState::CopySrc});
    graph->add_pass(
        "render", [&](PassBuilder& pass) { pass.use(first, ResourceState::RenderTarget); },
        [first](PassContext& context) { clear_texture(context, first); });
    graph->execute();
    CHECK(graph->final_texture_state(first) == ResourceState::CopySrc);
    device->end_frame();

    device->begin_frame();
    graph->begin(4, 4);
    const GraphTexture again =
        graph->import_texture({.texture = persistent, .name = "persistent", .state = ResourceState::CopySrc});
    const GraphBuffer out =
        graph->import_buffer({.buffer = readback, .name = "readback", .final_state = ResourceState::HostRead});
    add_copy_out(*graph, again, out);
    graph->execute();
    device->end_frame();
    device->wait_idle();
    CHECK(all_pixels(*device, readback, 16, kClearRgba));
    CHECK(graph->stats().barriers == 2); // the texture was already in CopySrc: only the readback's two
    device->destroy(persistent);
    device->destroy(readback);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("only shader states carry shader stages: a non-shader read doesn't cover later shader reads") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    device->begin_frame();
    graph->begin(1, 1);
    struct Made {
        GraphBuffer buffer;
    };
    const GraphBuffer buffer = graph
                                   ->add_pass<Made>(
                                       "write",
                                       [&](PassBuilder& pass, Made& data) {
                                           data.buffer = pass.create_buffer({.size = 64, .debug_name = "args"});
                                           pass.use(data.buffer, ResourceState::ShaderWrite, ShaderStages::Compute);
                                       },
                                       [](const Made&, PassContext&) {})
                                   .buffer;
    const auto read = [&](const char* name, ResourceState state, ShaderStages stages) {
        graph->add_pass(
            name, [&](PassBuilder& pass) { pass.use(buffer, state, stages).never_cull(); }, [](PassContext&) {});
    };
    read("indirect", ResourceState::IndirectArgument, ShaderStages::All);
    read("indirect and compute", ResourceState::IndirectArgument | ResourceState::ShaderRead, ShaderStages::Compute);
    read("fragment", ResourceState::ShaderRead, ShaderStages::Fragment); // a new stage: needs its barrier
    graph->execute();
    device->end_frame();
    CHECK(graph->stats().barriers == 4);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("imports: twice is once, untouched ones stay put, mixed end states settle in General") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    const TextureHandle target = *device->create_texture(
        {.width = 4, .height = 4, .format = Format::RGBA8Unorm, .usage = TextureUsage::RenderTarget});
    const TextureHandle mips = *device->create_texture(
        {.width = 4, .height = 4, .mip_count = 2, .format = Format::RGBA8Unorm, .usage = TextureUsage::CopyDst});
    const BufferHandle upload =
        *device->create_buffer({.size = 64, .usage = BufferUsage::CopySrc, .memory = MemoryLocation::Upload});

    device->begin_frame();
    graph->begin(4, 4);
    // Untouched, and already in its final state: no barrier, discard or not.
    const GraphTexture untouched = graph->import_texture({.texture = target,
                                                          .name = "target",
                                                          .state = ResourceState::RenderTarget,
                                                          .final_state = ResourceState::RenderTarget,
                                                          .discard = true});
    CHECK(graph->import_texture({.texture = target, .final_state = ResourceState::RenderTarget}).index ==
          untouched.index);
    // Only mip 0 written, no final state: mip 1 is still Undefined, so it all ends in General.
    const GraphTexture half =
        graph->import_texture({.texture = mips, .name = "half written", .state = ResourceState::Undefined});
    const GraphBuffer source = graph->import_buffer({.buffer = upload, .name = "upload"});
    graph->add_pass(
        "write mip 0",
        [&](PassBuilder& pass) {
            pass.use(half, ResourceState::CopyDst, ShaderStages::All, {.first_mip = 0, .mip_count = 1});
            pass.use(source, ResourceState::CopySrc);
        },
        [half, source](PassContext& context) {
            context.list.copy_buffer_to_texture(context.buffer(source), context.texture(half));
        });
    graph->execute();
    device->end_frame();
    CHECK(graph->final_texture_state(untouched) == ResourceState::RenderTarget);
    CHECK(graph->final_texture_state(half) == ResourceState::General);
    CHECK(graph->stats().barriers == 2 + 2); // mip 0 to CopyDst, the upload; then both mips to General
    device->destroy(target);
    device->destroy(mips);
    device->destroy(upload);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("a transient the device can't make skips its passes instead of crashing") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    device->begin_frame();
    graph->begin(1, 1);
    struct Ran {
        bool* ran = nullptr;
    };
    bool ran = false;
    graph->add_pass<Ran>(
        "impossible",
        [&](PassBuilder& pass, Ran& data) {
            data.ran = &ran;
            const TextureDesc huge{.width = 1U << 24U, .height = 1U << 24U, .format = Format::RGBA8Unorm};
            pass.use(pass.create_texture({.desc = huge}), ResourceState::RenderTarget).never_cull();
        },
        [](const Ran& data, PassContext&) { *data.ran = true; });
    graph->execute();
    device->end_frame();
    CHECK_FALSE(ran);
    CHECK(graph->stats().skipped_passes == 1);
    CHECK(graph->stats().pooled_textures == 0);
    device->wait_idle();
}

TEST_CASE("immediate mode makes transients with every usage a later pass may need") {
    if (DE_BUILD_DEV == 0) {
        return;
    }
    auto device = make_device();
    if (!device) {
        return;
    }
    auto graph = RenderGraph::create(*device, {.immediate = true});
    REQUIRE(graph.ok());
    device->begin_frame();
    graph->begin(1, 1);
    struct Made {
        GraphBuffer buffer;
    };
    const GraphBuffer buffer = graph
                                   ->add_pass<Made>(
                                       "write",
                                       [&](PassBuilder& pass, Made& data) {
                                           data.buffer = pass.create_buffer({.size = 64});
                                           pass.use(data.buffer, ResourceState::ShaderWrite, ShaderStages::Compute);
                                       },
                                       [](const Made&, PassContext&) {})
                                   .buffer;
    struct Usage {
        GraphBuffer buffer;
        BufferUsage* usage = nullptr;
    };
    BufferUsage usage = BufferUsage::None;
    graph->add_pass<Usage>(
        "indirect",
        [&](PassBuilder& pass, Usage& data) {
            data = {.buffer = buffer, .usage = &usage};
            pass.use(buffer, ResourceState::IndirectArgument);
        },
        [](const Usage& data, PassContext& context) {
            *data.usage = context.device.buffer_desc(context.buffer(data.buffer))->usage;
        });
    graph->execute();
    device->end_frame();
    CHECK(de::core::has_all(usage, BufferUsage::Indirect | BufferUsage::Storage));
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("passes are timed on the GPU, and their times arrive once the frame is done") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    const auto run_frame = [&] {
        device->begin_frame();
        graph->begin(64, 64);
        const GraphTexture first = add_clear(*graph, "first clear").texture;
        graph->add_pass(
            "second clear", [&](PassBuilder& pass) { pass.use(first, ResourceState::RenderTarget).never_cull(); },
            [first](PassContext& context) { clear_texture(context, first); });
        graph->execute();
        device->end_frame();
    };
    run_frame();
    CHECK(graph->stats().gpu_pass_count == 0); // nothing has finished yet
    if (!device->capabilities().timestamps) {
        return;
    }
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        run_frame();
    }
    const de::render::RenderGraphStats stats = graph->stats();
    REQUIRE(stats.gpu_pass_count == 2);
    CHECK((stats.gpu_passes[0].name.view() == "first clear"));
    CHECK((stats.gpu_passes[1].name.view() == "second clear"));
    CHECK(stats.gpu_passes[0].ms >= 0.0f);
    CHECK(stats.gpu_passes[0].ms < 100.0f);
    CHECK(stats.gpu_frame_ms >= stats.gpu_passes[1].ms);
    // Frames later, still the two passes of one frame (each frame's records replace the slot's).
    for (uint32_t frame = 0; frame < 2 * kFramesInFlight; ++frame) {
        run_frame();
    }
    CHECK(graph->stats().gpu_pass_count == 2);
    // Frames without the graph, writing timestamps of their own: when the graph comes back, the
    // finished frame isn't one it timed, so the last times stand (they aren't read from those).
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        device->begin_frame();
        CHECK(device->begin_command_list().write_timestamp() == 0);
        device->end_frame();
    }
    run_frame();
    CHECK(graph->stats().gpu_pass_count == 2);
    CHECK((graph->stats().gpu_passes[1].name.view() == "second clear"));
    device->wait_idle();
    check_clean(*device);
}
