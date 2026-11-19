#include "core/job_system.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "support/gpu.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <span>

using de::platform::Vfs;
using de::rhi::BufferUsage;
using de::rhi::MemoryLocation;
using de::test::Sandbox;

namespace {

// Matches RayQueryPush in shaders/tests/ray_query.slang.
struct RayQueryPush {
    uint64_t scene = 0;
    uint64_t rays = 0;
    uint64_t hits = 0;
    uint32_t count = 0;
    uint32_t reserved = 0;
};

struct Hit {
    float distance = 0.0f;
    float instance = 0.0f;
    float run = 0.0f;
    float triangle = 0.0f;
};

template <typename T>
void write(de::rhi::Device& device, de::rhi::BufferHandle buffer, std::span<const T> values) {
    std::memcpy(device.mapped(buffer).data(), values.data(), values.size_bytes());
    device.flush_mapped(buffer);
}

} // namespace

TEST_CASE("ray queries: a ray finds the triangle it's aimed at, in the instance that holds it, where that is now") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    if (!device->capabilities().ray_query) {
        MESSAGE("this GPU has no ray queries: nothing to trace against");
        // Asked for anyway, it says so rather than crash.
        CHECK_FALSE(device->create_top_level({.max_instances = 4}).ok());
        return;
    }
    const Sandbox sandbox;
    const de::platform::PathString game_root = sandbox.game();
    const de::platform::PathString user_root = sandbox.folder("user");
    const de::platform::PathString temp_root = sandbox.folder("temp");
    auto made_vfs = Vfs::create({.game_root = game_root.c_str(),
                                 .user_root = user_root.c_str(),
                                 .temp_root = temp_root.c_str(),
                                 .engine_root = DE_SOURCE_DIR});
    REQUIRE(made_vfs.ok());
    Vfs vfs = std::move(made_vfs).value();
    auto compiler = de::shader::ShaderCompiler::create(vfs);
    REQUIRE(compiler.ok());
    de::core::JobSystem jobs({.worker_count = 1});
    auto library = de::shader::ShaderLibrary::create(*device, vfs, jobs, *compiler, {.hot_reload = false});
    REQUIRE(library.ok());
    const auto program = library->create_compute({.module = "tests.ray_query", .debug_name = "ray query"});
    REQUIRE(program.ok());
    REQUIRE(library->pipeline(*program).is_valid());

    // Two triangles in the plane z = 0, each a run of its own: one about the origin, one a little above it. Their
    // vertices 16 bytes apart: a position, then something that isn't one.
    const float vertices[] = {-1.0f, -1.0f, 0.0f, 99.0f, 1.0f, -1.0f, 0.0f, 99.0f, 0.0f, 1.0f, 0.0f, 99.0f,
                              -1.0f, 2.0f,  0.0f, 99.0f, 1.0f, 2.0f,  0.0f, 99.0f, 0.0f, 4.0f, 0.0f, 99.0f};
    const uint32_t indices[] = {0, 1, 2, 0, 1, 2};
    const auto vertex_buffer = device->create_buffer({.size = sizeof(vertices),
                                                      .usage = BufferUsage::Storage | BufferUsage::AccelerationInput,
                                                      .memory = MemoryLocation::Upload});
    const auto index_buffer = device->create_buffer({.size = sizeof(indices),
                                                     .usage = BufferUsage::Storage | BufferUsage::AccelerationInput,
                                                     .memory = MemoryLocation::Upload});
    REQUIRE(vertex_buffer.ok());
    REQUIRE(index_buffer.ok());
    write<float>(*device, *vertex_buffer, vertices);
    write<uint32_t>(*device, *index_buffer, indices);
    const de::rhi::AccelerationTriangles runs[] = {{.vertices = *vertex_buffer,
                                                    .vertex_stride = 16,
                                                    .vertex_count = 3,
                                                    .indices = *index_buffer,
                                                    .index_count = 3},
                                                   {.vertices = *vertex_buffer,
                                                    .vertex_offset = uint64_t{3} * 16,
                                                    .vertex_stride = 16,
                                                    .vertex_count = 3,
                                                    .indices = *index_buffer,
                                                    .index_offset = 3 * sizeof(uint32_t),
                                                    .index_count = 3}};
    const auto bottom = device->create_bottom_level({.geometry = runs, .debug_name = "two triangles"});
    REQUIRE(bottom.ok());
    const auto top = device->create_top_level({.max_instances = 4, .debug_name = "scene"});
    REQUIRE(top.ok());
    CHECK(device->stats().live_accelerations == 2);
    CHECK(device->acceleration_address(*bottom) != 0);
    CHECK(device->acceleration_address(*top) != 0);

    // Two instances of it: one where it is, known as 7; one ten metres along x, known as 9.
    de::rhi::AccelerationInstance instances[2];
    instances[0].id_and_mask = 0xff000000U | 7U;
    instances[0].bottom_level = device->acceleration_address(*bottom);
    instances[1] = instances[0];
    instances[1].id_and_mask = 0xff000000U | 9U;
    instances[1].transform[3] = 10.0f;
    const auto instance_buffer = device->create_buffer({.size = sizeof(instances),
                                                        .usage = BufferUsage::Storage | BufferUsage::AccelerationInput,
                                                        .memory = MemoryLocation::Upload});
    REQUIRE(instance_buffer.ok());
    write<de::rhi::AccelerationInstance>(*device, *instance_buffer, instances);

    // Rays: where each starts, then the way it goes.
    const float rays[] = {
        0.0f,  0.0f, 5.0f,  0.0f, 0.0f, 0.0f, -1.0f, 0.0f, // down onto the first's lower triangle, from 5 m
        10.0f, 3.0f, 3.0f,  0.0f, 0.0f, 0.0f, -1.0f, 0.0f, // onto the second's upper triangle, from 3 m
        5.0f,  0.0f, 5.0f,  0.0f, 0.0f, 0.0f, -1.0f, 0.0f, // between the two: nothing
        0.0f,  0.0f, -2.0f, 0.0f, 0.0f, 0.0f, 1.0f,  0.0f, // from behind: a triangle has two sides
        0.0f,  0.0f, 5.0f,  0.0f, 0.0f, 0.0f, 1.0f,  0.0f, // away from it: nothing
    };
    constexpr uint32_t kRays = 5;
    const auto ray_buffer =
        device->create_buffer({.size = sizeof(rays), .usage = BufferUsage::Storage, .memory = MemoryLocation::Upload});
    const auto hit_buffer = device->create_buffer(
        {.size = kRays * sizeof(Hit), .usage = BufferUsage::Storage, .memory = MemoryLocation::Readback});
    REQUIRE(ray_buffer.ok());
    REQUIRE(hit_buffer.ok());
    write<float>(*device, *ray_buffer, rays);

    Hit hits[kRays];
    const auto trace = [&](bool build_bottom) {
        device->begin_frame();
        de::rhi::CommandList list = device->begin_command_list();
        if (build_bottom) {
            list.build_bottom_level(*bottom, runs);
        }
        list.build_top_level(*top, *instance_buffer, 0, 2);
        list.bind_pipeline(library->pipeline(*program));
        list.push_constants(RayQueryPush{.scene = device->acceleration_address(*top),
                                         .rays = device->buffer_address(*ray_buffer),
                                         .hits = device->buffer_address(*hit_buffer),
                                         .count = kRays});
        list.dispatch(1);
        list.barrier(*hit_buffer, de::rhi::ResourceState::ShaderWrite, de::rhi::ResourceState::HostRead);
        device->end_frame();
        device->wait_idle();
        device->invalidate_mapped(*hit_buffer);
        std::memcpy(hits, device->mapped(*hit_buffer).data(), sizeof(hits));
    };
    trace(true);
    CHECK(hits[0].distance == doctest::Approx(5.0f));
    CHECK(hits[0].instance == 7.0f);
    CHECK(hits[0].run == 0.0f);
    CHECK(hits[0].triangle == 0.0f);
    CHECK(hits[1].distance == doctest::Approx(3.0f));
    CHECK(hits[1].instance == 9.0f);
    CHECK(hits[1].run == 1.0f); // the run a little above the origin
    CHECK(hits[2].distance < 0.0f);
    CHECK(hits[3].distance == doctest::Approx(2.0f));
    CHECK(hits[3].instance == 7.0f);
    CHECK(hits[4].distance < 0.0f);

    // The second instance moved to between them, and the first no longer seen by any ray: the top level built
    // again finds them as they are now.
    instances[1].transform[3] = 5.0f;
    instances[0].id_and_mask = 7U; // a mask of nothing
    write<de::rhi::AccelerationInstance>(*device, *instance_buffer, instances);
    device->begin_frame(); // the last frame's work is done: its buffer may be written again
    device->end_frame();
    trace(false);
    CHECK(hits[0].distance < 0.0f);
    CHECK(hits[1].distance < 0.0f);
    CHECK(hits[2].distance == doctest::Approx(5.0f));
    CHECK(hits[2].instance == 9.0f);
    CHECK(hits[3].distance < 0.0f);

    device->destroy(*top);
    device->destroy(*bottom);
    device->destroy(*vertex_buffer);
    device->destroy(*index_buffer);
    device->destroy(*instance_buffer);
    device->destroy(*ray_buffer);
    device->destroy(*hit_buffer);
    library->destroy(*program);
    device->wait_idle();
    CHECK(device->stats().live_accelerations == 0);
    de::test::check_clean(*device);
}
