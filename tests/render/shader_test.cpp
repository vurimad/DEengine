#include "core/job_system.h"
#include "render/rhi/device.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "support/gpu.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

using namespace de::shader;
using de::platform::Vfs;
using de::rhi::ShaderStage;
using de::test::bytes_of;
using de::test::check_clean;
using de::test::FillBufferPush;
using de::test::make_device;
using de::test::read_u32;
using de::test::Sandbox;

namespace {

constexpr uint32_t kSpirvMagic = 0x07230203;

// A Vfs whose src:/ is the repository, for the engine's real shaders.
Vfs make_repo_vfs(const Sandbox& sandbox) {
    const de::platform::PathString game_root = sandbox.game();
    const de::platform::PathString user_root = sandbox.folder("user");
    const de::platform::PathString temp_root = sandbox.folder("temp");
    auto vfs = Vfs::create({.game_root = game_root.c_str(),
                            .user_root = user_root.c_str(),
                            .temp_root = temp_root.c_str(),
                            .src_root = DE_SOURCE_DIR});
    REQUIRE(vfs.ok());
    return std::move(vfs).value();
}

ShaderCompiler make_compiler(Vfs& vfs) {
    auto compiler = ShaderCompiler::create(vfs);
    REQUIRE(compiler.ok());
    return std::move(compiler).value();
}

bool has_dependency(const CompileReport& report, std::string_view path) {
    for (const ShaderDependency& dependency : report.dependencies) {
        if (dependency.path == path) {
            return true;
        }
    }
    return false;
}

const PushConstantField* field(const ShaderReflection& reflection, std::string_view name) {
    for (const PushConstantField& field : reflection.push_constants) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

void write_source(const Vfs& vfs, const char* path, std::string_view text) {
    REQUIRE(vfs.write_file(path, bytes_of(text)).ok());
}

std::string_view text_of(const de::core::Array<char>& diagnostics) {
    return {diagnostics.data(), diagnostics.empty() ? 0 : diagnostics.size() - 1};
}

// Runs a compute program that writes count uint32 values through a pointer, and reads them back.
void run_fill(de::rhi::Device& device, de::rhi::PipelineHandle pipeline, de::rhi::BufferHandle buffer, uint32_t value,
              uint32_t count) {
    device.begin_frame();
    de::rhi::CommandList list = device.begin_command_list();
    list.bind_pipeline(pipeline);
    list.push_constants(FillBufferPush{.output = device.buffer_address(buffer), .value = value, .count = count});
    list.dispatch((count + 63) / 64);
    list.barrier(buffer, de::rhi::ResourceState::ShaderWrite, de::rhi::ResourceState::HostRead);
    device.end_frame();
    device.wait_idle();
    device.invalidate_mapped(buffer);
}

} // namespace

TEST_CASE("the compiler turns Slang into SPIR-V with reflection and dependencies") {
    const Sandbox sandbox;
    Vfs vfs = make_repo_vfs(sandbox);
    ShaderCompiler compiler = make_compiler(vfs);

    // Both stages of the triangle in one compile.
    const ShaderEntry stages[] = {{.name = "vertex_main", .stage = ShaderStage::Vertex},
                                  {.name = "fragment_main", .stage = ShaderStage::Fragment}};
    CompileReport report;
    auto triangle = compiler.compile({.module = "triangle", .entries = stages}, &report);
    REQUIRE(triangle.ok());
    REQUIRE(triangle->stages.size() == 2);
    const CompiledStage& vertex = triangle->stages[0];
    REQUIRE(vertex.code.size() > 5);
    CHECK(vertex.code[0] == kSpirvMagic);
    CHECK(vertex.reflection.stage == ShaderStage::Vertex);
    CHECK(vertex.reflection.push_constant_bytes == 8);
    REQUIRE(field(vertex.reflection, "time_s") != nullptr);
    CHECK(field(vertex.reflection, "time_s")->offset == 0);
    REQUIRE(field(vertex.reflection, "aspect") != nullptr);
    CHECK(field(vertex.reflection, "aspect")->offset == 4);
    CHECK(vertex.reflection.bindings.empty());
    CHECK(triangle->stages[1].code[0] == kSpirvMagic);
    CHECK(triangle->stages[1].reflection.stage == ShaderStage::Fragment);
    CHECK(has_dependency(report, "src:/shaders/triangle.slang"));
    CHECK(has_dependency(report, "src:/shaders/common/color.slang"));

    const ShaderEntry compute_entry[] = {{.name = "compute_main", .stage = ShaderStage::Compute}};
    auto compute = compiler.compile({.module = "tests.fill_buffer", .entries = compute_entry});
    REQUIRE(compute.ok());
    const ShaderReflection& reflection = compute->stages[0].reflection;
    CHECK(reflection.thread_group_size[0] == 64);
    CHECK(reflection.thread_group_size[1] == 1);
    CHECK(reflection.thread_group_size[2] == 1);
    CHECK(reflection.push_constant_bytes == 16);
    REQUIRE(field(reflection, "output") != nullptr);
    CHECK(field(reflection, "output")->size == 8);
    REQUIRE(field(reflection, "count") != nullptr);
    CHECK(field(reflection, "count")->offset == 12);
}

TEST_CASE("compile errors name the file and line, and missing pieces fail cleanly") {
    const Sandbox sandbox;
    Vfs vfs = sandbox.make_vfs();
    ShaderCompiler compiler = make_compiler(vfs);
    write_source(vfs, "src:/shaders/broken.slang",
                 "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\nvoid compute_main() { int x = ; }\n");
    write_source(vfs, "src:/shaders/permutation.slang",
                 "#if !defined(FAST_PATH) || FAST_PATH != 2\n#error FAST_PATH must be 2\n#endif\n"
                 "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\nvoid compute_main() {}\n");
    write_source(vfs, "src:/shaders/slots.slang",
                 "RWStructuredBuffer<uint> output;\n"
                 "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\nvoid compute_main() { output[0] = 1; }\n");
    // An include that climbs out of its folder: shaders/nested/../shared.slang.
    write_source(vfs, "src:/shaders/shared.slang", "static const uint kShared = 7;\n");
    write_source(vfs, "src:/shaders/nested/climb.slang",
                 "#include \"../shared.slang\"\n"
                 "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\nvoid compute_main() { uint x = kShared; }\n");
    const ShaderEntry compute_main[] = {{.name = "compute_main", .stage = ShaderStage::Compute}};

    CompileReport report;
    const auto broken = compiler.compile({.module = "broken", .entries = compute_main}, &report);
    CHECK_FALSE(broken.ok());
    CHECK(text_of(report.diagnostics).find("broken.slang:3:") != std::string_view::npos); // file:line:column
    CHECK(text_of(report.diagnostics).find("error") != std::string_view::npos);
    CHECK(has_dependency(report, "src:/shaders/broken.slang")); // reported even though it failed

    const auto missing_module = compiler.compile({.module = "nowhere", .entries = compute_main});
    CHECK(missing_module.error().code == de::core::ErrorCode::NotFound);
    const ShaderEntry not_there[] = {{.name = "not_there", .stage = ShaderStage::Compute}};
    const auto missing_entry = compiler.compile({.module = "slots", .entries = not_there});
    CHECK(missing_entry.error().code == de::core::ErrorCode::NotFound);

    const ShaderDefine wrong[] = {{.name = "FAST_PATH", .value = "1"}};
    CHECK_FALSE(compiler.compile({.module = "permutation", .entries = compute_main, .defines = wrong}).ok());
    const ShaderDefine right[] = {{.name = "FAST_PATH", .value = "2"}};
    CHECK(compiler.compile({.module = "permutation", .entries = compute_main, .defines = right}).ok());

    CompileReport climb_report;
    CHECK(compiler.compile({.module = "nested.climb", .entries = compute_main}, &climb_report).ok());
    CHECK(has_dependency(climb_report, "src:/shaders/shared.slang"));

    // Names that don't fit are refused, not cut short into a different module.
    const std::string_view long_name = "a_module_name_that_goes_on_and_on_well_past_the_sixty_three_character_limit";
    const std::string long_module(long_name);
    CHECK(compiler.compile({.module = long_module.c_str(), .entries = compute_main}).error().code ==
          de::core::ErrorCode::InvalidArgument);

    // Compiles, but reflection shows the descriptor slot the engine's pipelines don't have.
    const auto slots = compiler.compile({.module = "slots", .entries = compute_main});
    REQUIRE(slots.ok());
    REQUIRE(slots->stages[0].reflection.bindings.size() == 1);
    CHECK((slots->stages[0].reflection.bindings[0].name == std::string_view("output")));
}

TEST_CASE("a graphics program draws through the RHI, culling by winding") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    Vfs vfs = make_repo_vfs(sandbox);
    ShaderCompiler compiler = make_compiler(vfs);
    de::core::JobSystem jobs({.worker_count = 1});
    auto library = ShaderLibrary::create(*device, vfs, jobs, compiler, {.hot_reload = false});
    REQUIRE(library.ok());

    constexpr uint32_t kSize = 32;
    de::rhi::GraphicsPipelineDesc state{.color_count = 1, .cull = de::rhi::CullMode::Back, .debug_name = "solid"};
    state.color_formats[0] = de::rhi::Format::RGBA8Unorm;
    const auto front = library->create_graphics({.module = "tests.solid_color", .state = state});
    REQUIRE(front.ok());
    state.front_counter_clockwise = false; // the triangle now faces away: culled
    const auto back = library->create_graphics({.module = "tests.solid_color", .state = state});
    REQUIRE(back.ok());
    CHECK(library->stats().programs == 2);
    const ShaderReflection* reflection = library->reflection(*front, ShaderStage::Fragment);
    REQUIRE(reflection != nullptr);
    CHECK(reflection->push_constant_bytes == 16);

    const auto target =
        device->create_texture({.width = kSize,
                                .height = kSize,
                                .format = de::rhi::Format::RGBA8Unorm,
                                .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc});
    const auto readback = device->create_buffer({.size = uint64_t{kSize} * kSize * 4,
                                                 .usage = de::rhi::BufferUsage::CopyDst,
                                                 .memory = de::rhi::MemoryLocation::Readback});
    REQUIRE((target.ok() && readback.ok()));

    const auto draw = [&](ProgramHandle program) {
        device->begin_frame();
        de::rhi::CommandList list = device->begin_command_list();
        list.barrier(*target, de::rhi::ResourceState::Undefined, de::rhi::ResourceState::RenderTarget);
        const de::rhi::ColorAttachment color{.texture = *target, .clear = {.r = 0, .g = 0, .b = 0, .a = 1}};
        list.begin_rendering({.colors = std::span(&color, 1)});
        list.bind_pipeline(library->pipeline(program));
        const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        list.push_constants(green);
        list.draw(3);
        list.end_rendering();
        list.barrier(*target, de::rhi::ResourceState::RenderTarget, de::rhi::ResourceState::CopySrc);
        list.copy_texture_to_buffer(*target, *readback);
        list.barrier(*readback, de::rhi::ResourceState::CopyDst, de::rhi::ResourceState::HostRead);
        device->end_frame();
        device->wait_idle();
        device->invalidate_mapped(*readback);
    };
    const auto pixel = [&](uint32_t x, uint32_t y) { return read_u32(*device, *readback, (y * kSize) + x); };
    constexpr uint32_t kGreen = 0xFF00FF00; // RGBA8 little endian: A B G R from the top byte
    constexpr uint32_t kBlack = 0xFF000000;

    draw(*front);
    CHECK(pixel(kSize / 2, kSize / 2) == kGreen);
    CHECK(pixel(0, 0) == kBlack);         // outside the triangle
    CHECK(pixel(kSize / 2, 1) == kBlack); // above its apex: +Y is up, so the apex is near the top
    CHECK(pixel(kSize / 2, kSize - 4) == kGreen);

    draw(*back);
    CHECK(pixel(kSize / 2, kSize / 2) == kBlack);

    library->destroy(*front);
    library->destroy(*back);
    CHECK(library->pipeline(*front).is_valid() == false);
    device->destroy(*target);
    device->destroy(*readback);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("a compute program writes a buffer through its device address") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    Vfs vfs = make_repo_vfs(sandbox);
    ShaderCompiler compiler = make_compiler(vfs);
    de::core::JobSystem jobs({.worker_count = 1});
    auto library = ShaderLibrary::create(*device, vfs, jobs, compiler, {.hot_reload = false});
    REQUIRE(library.ok());
    const auto program = library->create_compute({.module = "tests.fill_buffer", .debug_name = "fill"});
    REQUIRE(program.ok());

    constexpr uint32_t kCount = 200;
    const auto buffer = device->create_buffer({.size = kCount * sizeof(uint32_t),
                                               .usage = de::rhi::BufferUsage::Storage,
                                               .memory = de::rhi::MemoryLocation::Readback});
    REQUIRE(buffer.ok());
    run_fill(*device, library->pipeline(*program), *buffer, 1000, kCount);
    CHECK(read_u32(*device, *buffer, 0) == 1000);
    CHECK(read_u32(*device, *buffer, 63) == 1063);
    CHECK(read_u32(*device, *buffer, kCount - 1) == 1000 + kCount - 1);

    device->destroy(*buffer);
    library->destroy(*program);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("editing a shader rebuilds it live, and a broken edit keeps the last good version") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    Vfs vfs = sandbox.make_vfs();
    ShaderCompiler compiler = make_compiler(vfs);
    de::core::JobSystem jobs({.worker_count = 2});
    auto library = ShaderLibrary::create(*device, vfs, jobs, compiler, {.hot_reload = true, .poll_interval_ms = 0});
    REQUIRE(library.ok());

    // hot.slang writes value.slang's constant, so editing either file must trigger a rebuild.
    constexpr std::string_view kHot = "import value;\n"
                                      "struct Push { uint* output; uint value; uint count; };\n"
                                      "[[vk::push_constant]] ConstantBuffer<Push> push;\n"
                                      "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\n"
                                      "void compute_main() { push.output[0] = kValue; }\n";
    write_source(vfs, "src:/shaders/value.slang", "public static const uint kValue = 1;\n");
    write_source(vfs, "src:/shaders/hot.slang", kHot);
    const auto program = library->create_compute({.module = "hot"});
    REQUIRE(program.ok());
    const auto buffer = device->create_buffer(
        {.size = 16, .usage = de::rhi::BufferUsage::Storage, .memory = de::rhi::MemoryLocation::Readback});
    REQUIRE(buffer.ok());

    run_fill(*device, library->pipeline(*program), *buffer, 0, 1);
    CHECK(read_u32(*device, *buffer, 0) == 1);

    library->reload_now(); // nothing changed: nothing rebuilds
    CHECK(library->stats().reloads == 0);

    // Edit the imported file (a different size, so the change shows even on coarse file clocks).
    write_source(vfs, "src:/shaders/value.slang", "public static const uint kValue = 22;\n");
    library->reload_now();
    CHECK(library->stats().reloads == 1);
    run_fill(*device, library->pipeline(*program), *buffer, 0, 1);
    CHECK(read_u32(*device, *buffer, 0) == 22);

    // A broken edit: the rebuild fails and the last good pipeline stays in use.
    const de::rhi::PipelineHandle good = library->pipeline(*program);
    write_source(vfs, "src:/shaders/hot.slang", "this is not slang\n");
    library->reload_now();
    CHECK(library->stats().failed_reloads == 1);
    CHECK(library->pipeline(*program) == good);
    library->reload_now(); // the failure isn't retried until the file changes again
    CHECK(library->stats().failed_reloads == 1);
    run_fill(*device, library->pipeline(*program), *buffer, 0, 1);
    CHECK(read_u32(*device, *buffer, 0) == 22);

    // The fix goes live through update(), as in the frame loop.
    write_source(vfs, "src:/shaders/hot.slang",
                 std::string_view("import value;\n"
                                  "struct Push { uint* output; uint value; uint count; };\n"
                                  "[[vk::push_constant]] ConstantBuffer<Push> push;\n"
                                  "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\n"
                                  "void compute_main() { push.output[0] = kValue + 100; }\n"));
    library->update(); // starts the rebuild on a worker; later updates swap it in once it's done
    const uint64_t deadline = de::platform::time_ns() + 30 * de::platform::kNsPerSecond;
    while (library->stats().reloads < 2 && de::platform::time_ns() < deadline) {
        de::platform::sleep_ns(de::platform::kNsPerMs);
        library->update();
    }
    CHECK(library->stats().reloads == 2);
    CHECK(library->stats().compiling == 0);
    run_fill(*device, library->pipeline(*program), *buffer, 0, 1);
    CHECK(read_u32(*device, *buffer, 0) == 122);

    device->destroy(*buffer);
    library->destroy(*program);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("a program whose first build fails is kept, and comes to life when fixed") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    Vfs vfs = sandbox.make_vfs();
    ShaderCompiler compiler = make_compiler(vfs);
    de::core::JobSystem jobs({.worker_count = 1});
    auto library = ShaderLibrary::create(*device, vfs, jobs, compiler, {.hot_reload = false});
    REQUIRE(library.ok());

    // Over-long names are a bad description: no program.
    const std::string long_module(80, 'x');
    CHECK(library->create_compute({.module = long_module.c_str()}).error().code ==
          de::core::ErrorCode::InvalidArgument);

    // The module doesn't exist yet: the program is created without a pipeline.
    const auto program = library->create_compute({.module = "later"});
    REQUIRE(program.ok());
    CHECK_FALSE(library->pipeline(*program).is_valid());
    CHECK(library->reflection(*program, ShaderStage::Compute) == nullptr);
    library->reload_now(); // nothing changed: still nothing
    CHECK_FALSE(library->pipeline(*program).is_valid());

    write_source(vfs, "src:/shaders/later.slang",
                 "struct Push { uint* output; uint value; uint count; };\n"
                 "[[vk::push_constant]] ConstantBuffer<Push> push;\n"
                 "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\n"
                 "void compute_main() { push.output[0] = 5; }\n");
    library->reload_now();
    REQUIRE(library->pipeline(*program).is_valid());
    CHECK(library->reflection(*program, ShaderStage::Compute)->push_constant_bytes == 16);

    const auto buffer = device->create_buffer(
        {.size = 16, .usage = de::rhi::BufferUsage::Storage, .memory = de::rhi::MemoryLocation::Readback});
    REQUIRE(buffer.ok());
    run_fill(*device, library->pipeline(*program), *buffer, 0, 1);
    CHECK(read_u32(*device, *buffer, 0) == 5);

    device->destroy(*buffer);
    library->destroy(*program);
    device->wait_idle();
    check_clean(*device);
}
