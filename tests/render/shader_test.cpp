#include "core/allocator.h"
#include "core/job_system.h"
#include "platform/time.h"
#include "render/rhi/device.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "support/gpu.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <span>
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

// A Vfs whose engine:/ is the repository, for the engine's real shaders.
Vfs make_repo_vfs(const Sandbox& sandbox) {
    const de::platform::PathString game_root = sandbox.game();
    const de::platform::PathString user_root = sandbox.folder("user");
    const de::platform::PathString temp_root = sandbox.folder("temp");
    auto vfs = Vfs::create({.game_root = game_root.c_str(),
                            .user_root = user_root.c_str(),
                            .temp_root = temp_root.c_str(),
                            .engine_root = DE_SOURCE_DIR});
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

    // Both stages of the scene's G-buffer pass in one compile.
    const ShaderEntry stages[] = {{.name = "vertex_main", .stage = ShaderStage::Vertex},
                                  {.name = "fragment_opaque", .stage = ShaderStage::Fragment}};
    CompileReport report;
    auto basic_lit = compiler.compile({.module = "scene.gbuffer", .entries = stages}, &report);
    REQUIRE(basic_lit.ok());
    REQUIRE(basic_lit->stages.size() == 2);
    const CompiledStage& vertex = basic_lit->stages[0];
    REQUIRE(vertex.code.size() > 5);
    CHECK(vertex.code[0] == kSpirvMagic);
    CHECK(vertex.reflection.stage == ShaderStage::Vertex);
    CHECK(vertex.reflection.push_constant_bytes >= 44);
    REQUIRE(field(vertex.reflection, "frame") != nullptr);
    CHECK(field(vertex.reflection, "frame")->offset == 0);
    CHECK(field(vertex.reflection, "frame")->size == 8); // a pointer
    REQUIRE(field(vertex.reflection, "draws") != nullptr);
    CHECK(field(vertex.reflection, "draws")->offset == 32);
    CHECK(basic_lit->stages[1].code[0] == kSpirvMagic);
    CHECK(basic_lit->stages[1].reflection.stage == ShaderStage::Fragment);
    CHECK(has_dependency(report, "engine:/shaders/scene/gbuffer.slang"));
    CHECK(has_dependency(report, "engine:/shaders/scene/scene_common.slang"));
    CHECK(has_dependency(report, "engine:/shaders/common/bindless.slang"));

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
    write_source(vfs, "engine:/shaders/broken.slang",
                 "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\nvoid compute_main() { int x = ; }\n");
    write_source(vfs, "engine:/shaders/permutation.slang",
                 "#if !defined(FAST_PATH) || FAST_PATH != 2\n#error FAST_PATH must be 2\n#endif\n"
                 "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\nvoid compute_main() {}\n");
    write_source(vfs, "engine:/shaders/slots.slang",
                 "RWStructuredBuffer<uint> output;\n"
                 "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\nvoid compute_main() { output[0] = 1; }\n");
    // An include that climbs out of its folder: shaders/nested/../shared.slang.
    write_source(vfs, "engine:/shaders/shared.slang", "static const uint kShared = 7;\n");
    write_source(vfs, "engine:/shaders/nested/climb.slang",
                 "#include \"../shared.slang\"\n"
                 "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\nvoid compute_main() { uint x = kShared; }\n");
    const ShaderEntry compute_main[] = {{.name = "compute_main", .stage = ShaderStage::Compute}};

    CompileReport report;
    const auto broken = compiler.compile({.module = "broken", .entries = compute_main}, &report);
    CHECK_FALSE(broken.ok());
    CHECK(text_of(report.diagnostics).find("broken.slang:3:") != std::string_view::npos); // file:line:column
    CHECK(text_of(report.diagnostics).find("error") != std::string_view::npos);
    CHECK(has_dependency(report, "engine:/shaders/broken.slang")); // reported even though it failed

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
    CHECK(has_dependency(climb_report, "engine:/shaders/shared.slang"));

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

    const auto draw = [&](ProgramHandle program, bool cull_none = false) {
        device->begin_frame();
        de::rhi::CommandList list = device->begin_command_list();
        list.barrier(*target, de::rhi::ResourceState::Undefined, de::rhi::ResourceState::RenderTarget);
        const de::rhi::ColorAttachment color{.texture = *target, .clear = {.r = 0, .g = 0, .b = 0, .a = 1}};
        list.begin_rendering({.colors = std::span(&color, 1)});
        list.bind_pipeline(library->pipeline(program));
        if (cull_none) {
            list.set_cull_mode(de::rhi::CullMode::None);
        }
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
    draw(*back, true); // culling off for this draw: the same pipeline shows its back face
    CHECK(pixel(kSize / 2, kSize / 2) == kGreen);
    draw(*back); // and binding the pipeline again culls again
    CHECK(pixel(kSize / 2, kSize / 2) == kBlack);

    library->destroy(*front);
    library->destroy(*back);
    CHECK(library->pipeline(*front).is_valid() == false);
    device->destroy(*target);
    device->destroy(*readback);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("asked for the same program twice, the library gives one, kept until both have let it go") {
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
    de::rhi::GraphicsPipelineDesc state{.color_count = 1, .debug_name = "one"};
    state.color_formats[0] = de::rhi::Format::RGBA8Unorm;
    const auto first = library->create_graphics({.module = "tests.solid_color", .state = state});
    REQUIRE(first.ok());
    // The same again, as a second renderer would ask: the same program, whatever it's called.
    state.debug_name = "two";
    const auto second = library->create_graphics({.module = "tests.solid_color", .state = state});
    REQUIRE(second.ok());
    CHECK((*second == *first));
    CHECK(library->stats().programs == 1);
    // Anything different about it is another program: its pipeline state, its defines.
    de::rhi::GraphicsPipelineDesc blended = state;
    blended.blend[0] = de::rhi::BlendMode::Premultiplied;
    const auto third = library->create_graphics({.module = "tests.solid_color", .state = blended});
    REQUIRE(third.ok());
    CHECK_FALSE((*third == *first));
    const de::shader::ShaderDefine defines[] = {{.name = "SOMETHING", .value = "2"}};
    const auto fourth = library->create_graphics({.module = "tests.solid_color", .defines = defines, .state = state});
    REQUIRE(fourth.ok());
    CHECK_FALSE((*fourth == *first));
    CHECK(library->stats().programs == 3);
    // Let go of once, it's still there for the other; let go of twice, it's gone.
    const de::rhi::PipelineHandle pipeline = library->pipeline(*first);
    REQUIRE(pipeline.is_valid());
    library->destroy(*first);
    CHECK((library->pipeline(*second) == pipeline));
    CHECK(library->stats().programs == 3);
    library->destroy(*second);
    CHECK_FALSE(library->pipeline(*second).is_valid());
    CHECK(library->stats().programs == 2);
    library->destroy(*third);
    library->destroy(*fourth);
    CHECK(library->stats().programs == 0);
    device->wait_idle();
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

TEST_CASE("compiled programs are kept between runs, and compiled again when a source they read changes") {
    const Sandbox sandbox;
    Vfs vfs = sandbox.make_vfs();
    // kept.slang writes value.slang's constant: the program depends on both files.
    constexpr std::string_view kKept = "import value;\n"
                                       "struct Push { uint* output; uint value; uint count; };\n"
                                       "[[vk::push_constant]] ConstantBuffer<Push> push;\n"
                                       "[shader(\"compute\")]\n[numthreads(4, 2, 1)]\n"
                                       "void compute_main() { push.output[0] = kValue; }\n";
    write_source(vfs, "engine:/shaders/value.slang", "public static const uint kValue = 1;\n");
    write_source(vfs, "engine:/shaders/kept.slang", kKept);
    const ShaderEntry entry[] = {{.name = "compute_main", .stage = ShaderStage::Compute}};
    const CompileRequest request{.module = "kept", .entries = entry};
    const ShaderCompilerDesc with_cache{.cache_folder = "user:/shader_cache"};
    const auto same_code = [](const CompiledProgram& a, const CompiledProgram& b) {
        return a.stages.size() == 1 && b.stages.size() == 1 && a.stages[0].code.size() == b.stages[0].code.size() &&
               std::memcmp(a.stages[0].code.data(), b.stages[0].code.data(),
                           a.stages[0].code.size() * sizeof(uint32_t)) == 0;
    };

    auto first_run = ShaderCompiler::create(vfs, with_cache);
    REQUIRE(first_run.ok());
    CompileReport compiled_report;
    const auto compiled = first_run->compile(request, &compiled_report);
    REQUIRE(compiled.ok());
    CHECK(first_run->stats().compiled == 1);
    CHECK(first_run->stats().from_cache == 0);

    // Asked again, it's read back: the same code and reflection, and the same sources reported, for hot reload.
    CompileReport kept_report;
    const auto kept = first_run->compile(request, &kept_report);
    REQUIRE(kept.ok());
    CHECK(first_run->stats().compiled == 1);
    CHECK(first_run->stats().from_cache == 1);
    CHECK(same_code(*compiled, *kept));
    const ShaderReflection& reflection = kept->stages[0].reflection;
    CHECK(kept->stages[0].entry == "compute_main");
    CHECK(reflection.stage == ShaderStage::Compute);
    CHECK(reflection.thread_group_size[0] == 4);
    CHECK(reflection.thread_group_size[1] == 2);
    CHECK(reflection.push_constant_bytes == 16);
    REQUIRE(field(reflection, "count") != nullptr);
    CHECK(field(reflection, "count")->offset == 12);
    REQUIRE(kept_report.dependencies.size() == compiled_report.dependencies.size());
    CHECK(has_dependency(kept_report, "engine:/shaders/kept.slang"));
    CHECK(has_dependency(kept_report, "engine:/shaders/value.slang"));
    for (const ShaderDependency& read : compiled_report.dependencies) {
        bool found = false;
        for (const ShaderDependency& reported : kept_report.dependencies) {
            found = found || (reported.path == read.path.view() && reported.info.size == read.info.size &&
                              reported.info.modified_time == read.info.modified_time &&
                              reported.content_hash == read.content_hash && read.content_hash != 0);
        }
        CHECK(found);
    }

    // Another run reads it back too. Another request (a define) is another program.
    auto second_run = ShaderCompiler::create(vfs, with_cache);
    REQUIRE(second_run.ok());
    const auto again = second_run->compile(request);
    REQUIRE(again.ok());
    CHECK(second_run->stats().compiled == 0);
    CHECK(second_run->stats().from_cache == 1);
    CHECK(same_code(*compiled, *again));
    const ShaderDefine defines[] = {{.name = "OTHER"}};
    REQUIRE(second_run->compile({.module = "kept", .entries = entry, .defines = defines}).ok());
    CHECK(second_run->stats().compiled == 1);

    // A source it imports changes, to one of the same size: it's compiled again, and that is what's kept.
    write_source(vfs, "engine:/shaders/value.slang", "public static const uint kValue = 2;\n");
    const auto edited = second_run->compile(request);
    REQUIRE(edited.ok());
    CHECK(second_run->stats().compiled == 2);
    CHECK_FALSE(same_code(*compiled, *edited));
    const auto edited_kept = second_run->compile(request);
    REQUIRE(edited_kept.ok());
    CHECK(second_run->stats().from_cache == 2);
    CHECK(same_code(*edited, *edited_kept));

    // A compile that fails isn't kept, and doesn't take the place of the one that is.
    write_source(vfs, "engine:/shaders/value.slang", "public static const uint kValue = ;\n");
    CHECK_FALSE(second_run->compile(request).ok());
    write_source(vfs, "engine:/shaders/value.slang", "public static const uint kValue = 2;\n");
    REQUIRE(second_run->compile(request).ok());
    CHECK(second_run->stats().from_cache == 3);

    // Damaged files are compiled over: each one cut short here.
    uint32_t files = 0;
    REQUIRE(vfs.list("user:/shader_cache",
                     [&](const char* name, bool is_directory) {
                         if (is_directory) {
                             return;
                         }
                         de::platform::PathString path("user:/shader_cache/");
                         REQUIRE(path.append(name));
                         auto bytes = vfs.read_whole_file(path.c_str(), de::core::heap_allocator());
                         REQUIRE(bytes.ok());
                         REQUIRE(bytes->size() > 16);
                         REQUIRE(vfs.write_file(path.c_str(), bytes->span().first(bytes->size() / 2)).ok());
                         ++files;
                     })
                .ok());
    CHECK(files == 2); // the program, and the one with a define
    auto third_run = ShaderCompiler::create(vfs, with_cache);
    REQUIRE(third_run.ok());
    const auto recompiled = third_run->compile(request);
    REQUIRE(recompiled.ok());
    CHECK(third_run->stats().compiled == 1);
    CHECK(third_run->stats().from_cache == 0);
    CHECK(same_code(*edited, *recompiled));
    REQUIRE(third_run->compile(request).ok());
    CHECK(third_run->stats().from_cache == 1);

    // Without a folder nothing is kept, and nothing read back.
    auto uncached = ShaderCompiler::create(vfs);
    REQUIRE(uncached.ok());
    REQUIRE(uncached->compile(request).ok());
    REQUIRE(uncached->compile(request).ok());
    CHECK(uncached->stats().compiled == 2);
    CHECK(uncached->stats().from_cache == 0);
}

TEST_CASE("compiles run side by side, each to the code it gives alone") {
    const Sandbox sandbox;
    Vfs vfs = make_repo_vfs(sandbox);
    // The engine's own shaders, the larger ones among them: what several sessions at once have to get right.
    struct Module {
        const char* name;
        ShaderEntry entries[2];
        uint32_t entry_count;
    };
    const ShaderEntry vertex{.name = "vertex_main", .stage = ShaderStage::Vertex};
    const ShaderEntry fragment{.name = "fragment_main", .stage = ShaderStage::Fragment};
    const ShaderEntry compute{.name = "compute_main", .stage = ShaderStage::Compute};
    const Module modules[] = {
        {.name = "scene.water", .entries = {vertex, fragment}, .entry_count = 2},
        {.name = "scene.deferred_light", .entries = {vertex, fragment}, .entry_count = 2},
        {.name = "scene.fog", .entries = {vertex, fragment}, .entry_count = 2},
        {.name = "scene.taa", .entries = {vertex, fragment}, .entry_count = 2},
        {.name = "scene.tonemap", .entries = {vertex, fragment}, .entry_count = 2},
        {.name = "scene.water_behind", .entries = {vertex, fragment}, .entry_count = 2},
        {.name = "scene.cull", .entries = {compute}, .entry_count = 1},
        {.name = "scene.fog_inject", .entries = {compute}, .entry_count = 1},
        {.name = "tests.fill_buffer", .entries = {compute}, .entry_count = 1},
    };
    constexpr size_t kCount = std::size(modules);
    const auto request = [&](size_t i) {
        return CompileRequest{.module = modules[i].name,
                              .entries = std::span(modules[i].entries, modules[i].entry_count)};
    };

    // Each alone, one after another.
    auto alone = ShaderCompiler::create(vfs);
    REQUIRE(alone.ok());
    CHECK(alone->parallel_compiles() == 1);
    de::core::Result<CompiledProgram> expected[kCount] = {
        de::core::fail(de::core::ErrorCode::Unknown, ""), de::core::fail(de::core::ErrorCode::Unknown, ""),
        de::core::fail(de::core::ErrorCode::Unknown, ""), de::core::fail(de::core::ErrorCode::Unknown, ""),
        de::core::fail(de::core::ErrorCode::Unknown, ""), de::core::fail(de::core::ErrorCode::Unknown, ""),
        de::core::fail(de::core::ErrorCode::Unknown, ""), de::core::fail(de::core::ErrorCode::Unknown, ""),
        de::core::fail(de::core::ErrorCode::Unknown, "")};
    for (size_t i = 0; i < kCount; ++i) {
        expected[i] = alone->compile(request(i));
        REQUIRE(expected[i].ok());
    }

    // All at once, three times over, four at a time: more compiles than sessions, so sessions are made as compiles
    // first overlap, waited for, and used again.
    auto together = ShaderCompiler::create(vfs, {.parallel_compiles = 4});
    REQUIRE(together.ok());
    CHECK(together->parallel_compiles() == 4);
    de::core::JobSystem jobs({.worker_count = 6});
    struct Run {
        ShaderCompiler* compiler;
        const Module* modules;
        de::core::Result<CompiledProgram>* expected;
        std::atomic<uint32_t> same{0};
        std::atomic<uint32_t> reported{0};
    } run{.compiler = &*together, .modules = modules, .expected = expected};
    constexpr size_t kRounds = 3;
    jobs.parallel_for(
        kCount * kRounds, 1,
        [](size_t begin, size_t end, void* context) {
            auto& shared = *static_cast<Run*>(context);
            for (size_t job = begin; job < end; ++job) {
                const size_t i = job % kCount;
                CompileReport report;
                const auto compiled = shared.compiler->compile(
                    {.module = shared.modules[i].name,
                     .entries = std::span(shared.modules[i].entries, shared.modules[i].entry_count)},
                    &report);
                if (!compiled.ok() || compiled->stages.size() != shared.expected[i]->stages.size()) {
                    continue;
                }
                bool same = true;
                for (size_t stage = 0; stage < compiled->stages.size(); ++stage) {
                    const auto& code = compiled->stages[stage].code;
                    const auto& wanted = (*shared.expected[i]).stages[stage].code;
                    same = same && code.size() == wanted.size() &&
                           std::memcmp(code.data(), wanted.data(), code.size() * sizeof(uint32_t)) == 0 &&
                           compiled->stages[stage].reflection.push_constant_bytes ==
                               (*shared.expected[i]).stages[stage].reflection.push_constant_bytes;
                }
                shared.same.fetch_add(same ? 1 : 0);
                // Each compile's own sources, not another's that ran beside it.
                bool own = !report.dependencies.empty();
                for (const ShaderDependency& dependency : report.dependencies) {
                    own = own && dependency.content_hash != 0;
                }
                shared.reported.fetch_add(own ? 1 : 0);
            }
        },
        &run);
    CHECK(run.same.load() == kCount * kRounds);
    CHECK(run.reported.load() == kCount * kRounds);
    CHECK(together->stats().compiled == kCount * kRounds);

    // Out of range, it's held to what there can be.
    auto many = ShaderCompiler::create(vfs, {.parallel_compiles = 1000});
    REQUIRE(many.ok());
    CHECK(many->parallel_compiles() == kMaxParallelCompiles);
    auto none = ShaderCompiler::create(vfs, {.parallel_compiles = 0});
    REQUIRE(none.ok());
    CHECK(none->parallel_compiles() == 1);
}

TEST_CASE("first builds can run in the background, their pipelines arriving on a later update") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    Vfs vfs = sandbox.make_vfs();
    constexpr std::string_view kFill = "struct Push { uint* output; uint value; uint count; };\n"
                                       "[[vk::push_constant]] ConstantBuffer<Push> push;\n"
                                       "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\n"
                                       "void compute_main() { push.output[0] = push.value; }\n";
    write_source(vfs, "engine:/shaders/slow.slang", kFill);
    write_source(vfs, "engine:/shaders/urgent.slang", kFill);
    write_source(vfs, "engine:/shaders/broken.slang", "this is not a shader\n");
    auto compiler = ShaderCompiler::create(vfs, {.cache_folder = "user:/shader_cache"});
    REQUIRE(compiler.ok());
    de::core::JobSystem jobs({.worker_count = 2});
    const auto buffer = device->create_buffer(
        {.size = 16, .usage = de::rhi::BufferUsage::Storage, .memory = de::rhi::MemoryLocation::Readback});
    REQUIRE(buffer.ok());
    // Frames go by until no more than `left` first builds are still to come.
    const auto frames_until = [](ShaderLibrary& library, uint32_t left) {
        const uint64_t start_ns = de::platform::time_ns();
        while (library.stats().first_builds > left &&
               de::platform::time_ns() - start_ns < 120 * de::platform::kNsPerSecond) {
            library.update();
            de::platform::sleep_ns(de::platform::kNsPerMs);
        }
        return library.stats().first_builds <= left;
    };
    const ShaderLibraryDesc in_background{.hot_reload = false, .background_first_builds = true};
    {
        auto library = ShaderLibrary::create(*device, vfs, jobs, *compiler, in_background);
        REQUIRE(library.ok());
        // Nothing is cached yet: each comes back at once, with no pipeline, and they build one after another.
        const auto slow = library->create_compute({.module = "slow"});
        const auto broken = library->create_compute({.module = "broken"});
        const auto urgent = library->create_compute({.module = "urgent", .build_first = true});
        REQUIRE(slow.ok());
        REQUIRE(broken.ok());
        REQUIRE(urgent.ok());
        CHECK(library->stats().first_builds == 3);
        CHECK(library->stats().compiling == 3);
        // Even if its compile is done by now, a pipeline comes in with update(), on this thread.
        CHECK_FALSE(library->pipeline(*slow).is_valid());

        // The first one created was already compiling; after it, the one that asked to go first.
        REQUIRE(frames_until(*library, 2));
        CHECK(library->pipeline(*slow).is_valid());
        CHECK_FALSE(library->pipeline(*urgent).is_valid());
        REQUIRE(frames_until(*library, 1));
        CHECK(library->pipeline(*urgent).is_valid());
        // A first build that fails leaves the program without a pipeline, as one that blocked would.
        REQUIRE(frames_until(*library, 0));
        CHECK(library->stats().compiling == 0);
        CHECK_FALSE(library->pipeline(*broken).is_valid());
        // None of it was a reload.
        CHECK(library->stats().reloads == 0);
        CHECK(library->stats().failed_reloads == 0);

        run_fill(*device, library->pipeline(*slow), *buffer, 7, 1);
        CHECK(read_u32(*device, *buffer, 0) == 7);

        // One destroyed while it waits is gone from the count. (With a define of its own: asked for as it was, it
        // would be the broken one there already is.)
        const de::shader::ShaderDefine another[] = {{.name = "ANOTHER"}};
        const auto dropped = library->create_compute({.module = "broken", .defines = another});
        REQUIRE(dropped.ok());
        CHECK(library->stats().first_builds == 1);
        library->destroy(*dropped);
        CHECK(library->stats().first_builds == 0);
        CHECK(library->stats().compiling == 0);
    }
    // With a compiler that compiles several at once, as many build at once; they all arrive.
    {
        write_source(vfs, "engine:/shaders/side_a.slang", kFill);
        write_source(vfs, "engine:/shaders/side_b.slang", kFill);
        write_source(vfs, "engine:/shaders/side_c.slang", kFill);
        auto several = ShaderCompiler::create(vfs, {.parallel_compiles = 3});
        REQUIRE(several.ok());
        auto library = ShaderLibrary::create(*device, vfs, jobs, *several, in_background);
        REQUIRE(library.ok());
        const auto a = library->create_compute({.module = "side_a"});
        const auto b = library->create_compute({.module = "side_b"});
        const auto c = library->create_compute({.module = "side_c"});
        const auto broken = library->create_compute({.module = "broken"});
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        REQUIRE(c.ok());
        REQUIRE(broken.ok());
        CHECK(library->stats().first_builds == 4);
        CHECK(library->stats().compiling == 4);
        // One destroyed while its build is in flight leaves the rest to finish.
        library->destroy(*b);
        CHECK(library->stats().first_builds == 3);
        REQUIRE(frames_until(*library, 0));
        CHECK(library->stats().compiling == 0);
        CHECK(library->pipeline(*a).is_valid());
        CHECK(library->pipeline(*c).is_valid());
        CHECK_FALSE(library->pipeline(*broken).is_valid());
        run_fill(*device, library->pipeline(*c), *buffer, 5, 1);
        CHECK(read_u32(*device, *buffer, 0) == 5);
        // reload_now() waits for whatever is in flight, too.
        const auto late = library->create_compute({.module = "side_b"});
        REQUIRE(late.ok());
        library->reload_now();
        CHECK(library->stats().compiling == 0);
        CHECK(library->pipeline(*late).is_valid());
    }
    // What the compiler has cached is there at once, all the same.
    {
        auto library = ShaderLibrary::create(*device, vfs, jobs, *compiler, in_background);
        REQUIRE(library.ok());
        const auto slow = library->create_compute({.module = "slow"});
        REQUIRE(slow.ok());
        CHECK(library->pipeline(*slow).is_valid());
        CHECK(library->stats().first_builds == 0);
        run_fill(*device, library->pipeline(*slow), *buffer, 9, 1);
        CHECK(read_u32(*device, *buffer, 0) == 9);
    }
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
    write_source(vfs, "engine:/shaders/value.slang", "public static const uint kValue = 1;\n");
    write_source(vfs, "engine:/shaders/hot.slang", kHot);
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
    write_source(vfs, "engine:/shaders/value.slang", "public static const uint kValue = 22;\n");
    library->reload_now();
    CHECK(library->stats().reloads == 1);
    run_fill(*device, library->pipeline(*program), *buffer, 0, 1);
    CHECK(read_u32(*device, *buffer, 0) == 22);

    // A broken edit: the rebuild fails and the last good pipeline stays in use.
    const de::rhi::PipelineHandle good = library->pipeline(*program);
    write_source(vfs, "engine:/shaders/hot.slang", "this is not slang\n");
    library->reload_now();
    CHECK(library->stats().failed_reloads == 1);
    CHECK(library->pipeline(*program) == good);
    library->reload_now(); // the failure isn't retried until the file changes again
    CHECK(library->stats().failed_reloads == 1);
    run_fill(*device, library->pipeline(*program), *buffer, 0, 1);
    CHECK(read_u32(*device, *buffer, 0) == 22);

    // The fix goes live through update(), as in the frame loop.
    write_source(vfs, "engine:/shaders/hot.slang",
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

    write_source(vfs, "engine:/shaders/later.slang",
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
