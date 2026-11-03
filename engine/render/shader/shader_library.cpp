#include "render/shader/shader_library.h"

#include "core/allocator.h"
#include "core/job_system.h"
#include "core/log.h"
#include "core/profile.h"
#include "platform/time.h"
#include "platform/vfs.h"
#include "render/rhi/device.h"
#include "render/shader/shader_asset.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <new>
#include <utility>

namespace de::shader {

namespace {

constexpr uint32_t kMaxStages = 2;

enum class Build : uint8_t {
    Idle,
    Queued,    // a source changed: waiting for its turn (one rebuild runs at a time)
    Compiling, // a background job owns Program::pending
    Done,      // the job finished: the rendering thread swaps it in or drops it
};

struct StoredDefine {
    ShaderName name;
    ShaderName value;
};

// A compile's outcome, kept until the rendering thread acts on it.
struct PendingBuild {
    bool ok = false;
    CompiledProgram program;
    CompileReport report;
};

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

// The bindless tables of shaders/common/bindless.slang: the only descriptor bindings a pipeline
// has. The numbers come from the RHI; the names from the Slang module (a test checks they agree).
struct BindlessDeclaration {
    const char* name;
    rhi::BindlessTable table;
};

constexpr BindlessDeclaration kBindlessDeclarations[] = {
    {.name = "de_textures", .table = rhi::BindlessTable::Texture2D},
    {.name = "de_texture_arrays", .table = rhi::BindlessTable::Texture2DArray},
    {.name = "de_cube_textures", .table = rhi::BindlessTable::TextureCube},
    {.name = "de_cube_texture_arrays", .table = rhi::BindlessTable::TextureCubeArray},
    {.name = "de_textures_3d", .table = rhi::BindlessTable::Texture3D},
    {.name = "de_storage_textures", .table = rhi::BindlessTable::Storage2D},
    {.name = "de_storage_texture_arrays", .table = rhi::BindlessTable::Storage2DArray},
    {.name = "de_storage_textures_3d", .table = rhi::BindlessTable::Storage3D},
    {.name = "de_storage_textures_uint", .table = rhi::BindlessTable::Storage2DUint},
    {.name = "de_samplers", .table = rhi::BindlessTable::Sampler},
    {.name = "de_comparison_samplers", .table = rhi::BindlessTable::ComparisonSampler},
};
static_assert(std::size(kBindlessDeclarations) == static_cast<size_t>(rhi::BindlessTable::Count),
              "one declaration per bindless table");

bool fits(const char* text) {
    return text != nullptr && text[0] != '\0' && std::strlen(text) <= kMaxShaderNameLength;
}

} // namespace

struct Program {
    LibraryState* library = nullptr;
    bool graphics = true;
    ShaderName module;
    ShaderName debug_name;
    StoredDefine defines[kMaxShaderDefines];
    uint32_t define_count = 0;
    ShaderName entries[kMaxStages];
    rhi::ShaderStage stages[kMaxStages] = {};
    uint32_t stage_count = 0;
    rhi::GraphicsPipelineDesc graphics_state; // shader code and debug name filled in per build

    rhi::PipelineHandle pipeline; // invalid until a build succeeds
    ShaderReflection reflections[kMaxStages];

    // Every source file the program depends on, with the info its last build read, and what the
    // last check saw. Rendering thread only.
    core::Array<ShaderDependency> watched{render_heap()};
    core::Array<platform::FileInfo> seen{render_heap()};

    std::atomic<Build> build{Build::Idle};
    uint64_t build_start_ns = 0;
    PendingBuild pending; // written by the build job
    core::JobCounter counter;
};

struct LibraryState {
    rhi::Device* device = nullptr;
    platform::Vfs* vfs = nullptr;
    core::JobSystem* jobs = nullptr;
    ShaderCompiler* compiler = nullptr; // null: programs load from cooked .deshader files
    ShaderLibraryDesc desc;
    core::SlotMap<Program*> programs{render_heap()};
    Program* building = nullptr; // the one rebuild in flight
    uint32_t queued = 0;
    uint64_t next_poll_ns = 0;
    uint32_t reloads = 0;
    uint32_t failed_reloads = 0;
};

namespace {

core::SlotHandle<Program*> to_slot(ProgramHandle handle) {
    return {.index = handle.index, .generation = handle.generation};
}

ProgramHandle from_slot(core::SlotHandle<Program*> handle) {
    return {.index = handle.index, .generation = handle.generation};
}

Program* find(const LibraryState& state, ProgramHandle handle) {
    Program* const* program = state.programs.get(to_slot(handle));
    return program != nullptr ? *program : nullptr;
}

// The cooked file of the program's module (create_cooked).
platform::PathString cooked_file(const Program& program) {
    return platform::PathString(
        asset::cooked_path(asset::shader_asset_id(program.module.view()), asset::AssetType::Shader).view());
}

// A cooked file in memory, 16-byte aligned as load_in_place needs.
class CookedBuffer {
public:
    explicit CookedBuffer(size_t size)
        : size_(size),
          data_(static_cast<std::byte*>(render_heap().allocate(std::max<size_t>(size, 1), asset::kAssetAlignment))) {}
    ~CookedBuffer() { render_heap().deallocate(data_, std::max<size_t>(size_, 1), asset::kAssetAlignment); }
    CookedBuffer(const CookedBuffer&) = delete;
    CookedBuffer& operator=(const CookedBuffer&) = delete;
    CookedBuffer(CookedBuffer&&) = delete;
    CookedBuffer& operator=(CookedBuffer&&) = delete;

    std::span<std::byte> bytes() { return {data_, size_}; }

private:
    size_t size_;
    std::byte* data_;
};

// The program's stages from its module's .deshader, as if just compiled: the cooker ran the
// compiler with every entry point of the module, once per permutation.
core::Result<CompiledProgram> load_cooked(const LibraryState& state, const Program& program, CompileReport& report,
                                          std::span<const ShaderDefine> defines) {
    const platform::PathString path = cooked_file(program);
    ShaderDependency& dependency = report.dependencies.push(ShaderDependency{.path = path});
    const core::Result<platform::FileInfo> info = state.vfs->file_info(path.c_str());
    if (!info || info->is_directory) {
        DE_LOG_ERROR("shader", "%s: no cooked shader at %s (run the cooker)", program.module.c_str(), path.c_str());
        return core::fail(core::ErrorCode::NotFound, "the shader module isn't cooked");
    }
    dependency.info = *info;
    CookedBuffer buffer(static_cast<size_t>(info->size));
    const core::Result<size_t> read = state.vfs->read_file(path.c_str(), buffer.bytes());
    if (!read || *read != buffer.bytes().size()) {
        DE_LOG_ERROR("shader", "%s: %s could not be read", program.module.c_str(), path.c_str());
        return core::fail(core::ErrorCode::IoError, "the cooked shader could not be read");
    }
    const std::span<const std::byte> file = buffer.bytes();
    const core::Result<const asset::ShaderAsset*> loaded = asset::load_in_place<asset::ShaderAsset>(file);
    if (!loaded) {
        DE_LOG_ERROR("shader", "%s: %s: %s", program.module.c_str(), path.c_str(), loaded.error().message);
        return core::fail(loaded.error());
    }
    const asset::ShaderAsset& shader = **loaded;
    const asset::ShaderPermutation* permutation = shader.find_permutation(file.data(), defines_hash(defines));
    if (permutation == nullptr) {
        DE_LOG_ERROR("shader", "%s: cooked without this program's defines (%u of them)", program.module.c_str(),
                     program.define_count);
        return core::fail(core::ErrorCode::NotFound, "the shader permutation isn't cooked");
    }
    CompiledProgram result{.stages = core::Array<CompiledStage>(render_heap())};
    for (uint32_t i = 0; i < program.stage_count; ++i) {
        const asset::ShaderEntryCode* entry =
            asset::ShaderAsset::find_entry(file.data(), *permutation, program.entries[i].view());
        if (entry == nullptr || from_asset_stage(entry->stage) != program.stages[i]) {
            DE_LOG_ERROR("shader", "%s: the cooked module has no %s entry point '%s'", program.module.c_str(),
                         program.stages[i] == rhi::ShaderStage::Compute    ? "compute"
                         : program.stages[i] == rhi::ShaderStage::Fragment ? "fragment"
                                                                           : "vertex",
                         program.entries[i].c_str());
            return core::fail(core::ErrorCode::NotFound, "a shader entry point is missing");
        }
        CompiledStage& stage = result.stages.emplace(CompiledStage{
            .entry = ShaderName(program.entries[i].view()),
            .code = core::Array<uint32_t>(render_heap()),
            .reflection = ShaderReflection{.stage = program.stages[i],
                                           .push_constant_bytes = entry->push_constant_bytes,
                                           .push_constants = core::Array<PushConstantField>(render_heap()),
                                           .bindings = core::Array<ResourceBinding>(render_heap())},
        });
        stage.code.assign(entry->code.get(file.data()));
        for (uint32_t axis = 0; axis < 3; ++axis) {
            stage.reflection.thread_group_size[axis] = entry->thread_group_size[axis];
        }
        for (const asset::ShaderPushConstant& field : entry->push_constants.get(file.data())) {
            stage.reflection.push_constants.push(
                PushConstantField{.name = ShaderName(field.name), .offset = field.offset, .size = field.size});
        }
    }
    return result;
}

// Compiles every stage of the program in one go, or loads it cooked. Runs on the creating thread
// or as a job.
void compile(Program& program, PendingBuild& out) {
    ShaderDefine defines[kMaxShaderDefines];
    for (uint32_t i = 0; i < program.define_count; ++i) {
        defines[i] = {.name = program.defines[i].name.c_str(), .value = program.defines[i].value.c_str()};
    }
    ShaderEntry entries[kMaxStages];
    for (uint32_t i = 0; i < program.stage_count; ++i) {
        entries[i] = {.name = program.entries[i].c_str(), .stage = program.stages[i]};
    }
    out.report = CompileReport{.dependencies = core::Array<ShaderDependency>(render_heap()),
                               .diagnostics = core::Array<char>(render_heap())};
    const std::span<const ShaderDefine> define_span(defines, program.define_count);
    core::Result<CompiledProgram> compiled =
        program.library->compiler == nullptr
            ? load_cooked(*program.library, program, out.report, define_span)
            : program.library->compiler->compile({.module = program.module.c_str(),
                                                  .entries = std::span(entries, program.stage_count),
                                                  .defines = define_span},
                                                 &out.report);
    out.ok = compiled.ok();
    if (compiled) {
        out.program = std::move(*compiled);
    }
}

void build_job(void* context) {
    DE_PROFILE_ZONE("shader rebuild");
    auto& program = *static_cast<Program*>(context);
    compile(program, program.pending);
    program.build.store(Build::Done, std::memory_order_release);
}

// Builds a pipeline from compiled stages, after checking they fit the engine's one pipeline layout.
core::Result<rhi::PipelineHandle> create_pipeline(LibraryState& state, Program& program,
                                                  const CompiledProgram& compiled) {
    for (uint32_t i = 0; i < program.stage_count; ++i) {
        const ShaderReflection& reflection = compiled.stages[i].reflection;
        for (const ResourceBinding& binding : reflection.bindings) {
            if (is_bindless_binding(binding)) {
                continue;
            }
            DE_LOG_ERROR("shader",
                         "%s:%s binds '%s' to a descriptor slot (set %u, binding %u): reach textures through "
                         "common.bindless and buffers through pointers, with indices and addresses in push constants",
                         program.module.c_str(), program.entries[i].c_str(), binding.name.c_str(), binding.set,
                         binding.binding);
            return core::fail(core::ErrorCode::Unsupported, "the shader binds resources to descriptor slots");
        }
        if (reflection.push_constant_bytes > rhi::kMaxPushConstantBytes) {
            DE_LOG_ERROR("shader", "%s:%s has %u bytes of push constants; the limit is %u", program.module.c_str(),
                         program.entries[i].c_str(), reflection.push_constant_bytes, rhi::kMaxPushConstantBytes);
            return core::fail(core::ErrorCode::Unsupported, "the shader has too many push constant bytes");
        }
    }
    const auto code_of = [&](uint32_t stage) {
        return rhi::ShaderCode{.stage = program.stages[stage],
                               .code = std::as_bytes(compiled.stages[stage].code.span())};
    };
    if (program.graphics) {
        rhi::GraphicsPipelineDesc desc = program.graphics_state;
        desc.vertex = code_of(0);
        desc.fragment = code_of(1);
        desc.debug_name = program.debug_name.c_str();
        return state.device->create_graphics_pipeline(desc);
    }
    return state.device->create_compute_pipeline({.shader = code_of(0), .debug_name = program.debug_name.c_str()});
}

// The file of the program's own module, watched even when a build failed before reading it, so
// creating or fixing it is noticed: the source, or the cooked file.
ShaderDependency module_file(const LibraryState& state, const Program& program) {
    if (state.compiler == nullptr) {
        ShaderDependency cooked{.path = cooked_file(program)};
        const core::Result<platform::FileInfo> info = state.vfs->file_info(cooked.path.c_str());
        cooked.info = info.ok() ? *info : platform::FileInfo{};
        return cooked;
    }
    // Always fits: module names are at most 63 characters.
    platform::PathString relative;
    ShaderDependency file{.path = platform::PathString("src:/")};
    const bool fits_path = shader_module_path(program.module.view(), relative) && file.path.append(relative.view());
    DE_ASSERT(fits_path);
    static_cast<void>(fits_path);
    const core::Result<platform::FileInfo> info = state.vfs->file_info(file.path.c_str());
    file.info = info.ok() ? *info : platform::FileInfo{};
    return file;
}

ShaderDependency* find_dependency(core::Array<ShaderDependency>& dependencies, const platform::PathString& path) {
    for (ShaderDependency& dependency : dependencies) {
        if (dependency.path == path.view()) {
            return &dependency;
        }
    }
    return nullptr;
}

// After a successful build: watch exactly what it read, plus the module's own file.
void watch_build(const LibraryState& state, Program& program, const CompileReport& report) {
    program.watched.clear();
    for (const ShaderDependency& dependency : report.dependencies) {
        program.watched.push(dependency);
    }
    if (const ShaderDependency own = module_file(state, program);
        find_dependency(program.watched, own.path) == nullptr) {
        program.watched.push(own);
    }
    program.seen.clear();
}

// After a failed build: files it read keep the info they had when read (a file still being
// written then shows as changed again); files it didn't read keep what the last check saw, so the
// build isn't retried until something changes again. Files new to this build join the watch.
void watch_failure(const LibraryState& state, Program& program, const CompileReport& report) {
    for (size_t i = 0; i < program.watched.size(); ++i) {
        ShaderDependency& dependency = program.watched[i];
        bool read = false;
        for (const ShaderDependency& file : report.dependencies) {
            if (file.path == dependency.path.view()) {
                dependency.info = file.info;
                read = true;
                break;
            }
        }
        if (!read && i < program.seen.size()) {
            dependency.info = program.seen[i];
        }
    }
    for (const ShaderDependency& file : report.dependencies) {
        if (find_dependency(program.watched, file.path) == nullptr) {
            program.watched.push(file);
        }
    }
    if (const ShaderDependency own = module_file(state, program);
        find_dependency(program.watched, own.path) == nullptr) {
        program.watched.push(own);
    }
    program.seen.clear();
}

// True if a watched file differs from what the last build read. Notes what it saw.
bool sources_changed(const LibraryState& state, Program& program) {
    program.seen.resize(program.watched.size());
    bool changed = false;
    for (size_t i = 0; i < program.watched.size(); ++i) {
        const core::Result<platform::FileInfo> info = state.vfs->file_info(program.watched[i].path.c_str());
        const platform::FileInfo now = info.ok() ? *info : platform::FileInfo{};
        program.seen[i] = now;
        changed = changed || now.size != program.watched[i].info.size ||
                  now.modified_time != program.watched[i].info.modified_time;
    }
    return changed;
}

// Takes a finished build: swaps in the new pipeline, or keeps the last good one. Rendering thread only.
void finish_build(LibraryState& state, Program& program) {
    PendingBuild& build = program.pending;
    core::Result<rhi::PipelineHandle> pipeline =
        build.ok ? create_pipeline(state, program, build.program)
                 : core::Result<rhi::PipelineHandle>(core::fail(core::ErrorCode::InvalidArgument, "compile failed"));
    const double elapsed_ms = static_cast<double>(platform::time_ns() - program.build_start_ns) / 1e6;
    if (pipeline) {
        state.device->destroy(program.pipeline);
        program.pipeline = *pipeline;
        for (uint32_t i = 0; i < program.stage_count; ++i) {
            program.reflections[i] = std::move(build.program.stages[i].reflection);
        }
        watch_build(state, program, build.report);
        ++state.reloads;
        DE_LOG_INFO("shader", "%s reloaded in %.0f ms", program.module.c_str(), elapsed_ms);
    } else {
        watch_failure(state, program, build.report);
        ++state.failed_reloads;
        DE_LOG_ERROR("shader", "%s: rebuild failed, %s", program.module.c_str(),
                     program.pipeline.is_valid() ? "keeping the last good version" : "nothing to draw with yet");
    }
    build = PendingBuild{};
    program.build.store(Build::Idle, std::memory_order_relaxed);
}

void start_next_build(LibraryState& state) {
    if (state.building != nullptr || state.queued == 0) {
        return;
    }
    for (Program* program : state.programs) {
        if (program->build.load(std::memory_order_relaxed) == Build::Queued) {
            --state.queued;
            state.building = program;
            DE_LOG_INFO("shader", "%s changed: rebuilding", program->module.c_str());
            program->build_start_ns = platform::time_ns();
            program->build.store(Build::Compiling, std::memory_order_relaxed);
            // Background: only idle workers run it, so a long compile never stalls a frame.
            state.jobs->submit_background(core::Job{
                .entry = &build_job, .context = program, .counter = &program->counter, .name = "shader rebuild"});
            return;
        }
    }
}

// Finishes the build in flight if it's done (or waits for it), then starts the next queued one.
void advance_builds(LibraryState& state, bool wait) {
    if (state.building != nullptr) {
        if (wait) {
            state.jobs->wait(state.building->counter);
        }
        if (state.building->build.load(std::memory_order_acquire) == Build::Done) {
            finish_build(state, *state.building);
            state.building = nullptr;
        }
    }
    start_next_build(state);
}

void queue_changed(LibraryState& state) {
    for (Program* program : state.programs) {
        if (program->build.load(std::memory_order_relaxed) == Build::Idle && sources_changed(state, *program)) {
            program->build.store(Build::Queued, std::memory_order_relaxed);
            ++state.queued;
        }
    }
}

Program* new_program(LibraryState& state, const char* module, std::span<const ShaderDefine> defines,
                     const char* debug_name) {
    auto* program = ::new (render_heap().allocate(sizeof(Program), alignof(Program))) Program();
    program->library = &state;
    program->module.assign(module);
    program->debug_name.assign(debug_name != nullptr ? debug_name : module);
    for (const ShaderDefine& define : defines) {
        program->defines[program->define_count++] = {.name = ShaderName(define.name),
                                                     .value = ShaderName(define.value != nullptr ? define.value : "1")};
    }
    return program;
}

void free_program(Program* program) {
    program->~Program();
    render_heap().deallocate(program, sizeof(Program), alignof(Program));
}

// The first build runs now, on the calling thread. A program whose first build fails is kept,
// without a pipeline, and watched like any other: fixing the shader brings it to life.
ProgramHandle add_program(LibraryState& state, Program* program) {
    program->build_start_ns = platform::time_ns();
    compile(*program, program->pending);
    PendingBuild& build = program->pending;
    core::Result<rhi::PipelineHandle> pipeline =
        build.ok ? create_pipeline(state, *program, build.program)
                 : core::Result<rhi::PipelineHandle>(core::fail(core::ErrorCode::InvalidArgument, "compile failed"));
    if (pipeline) {
        program->pipeline = *pipeline;
        for (uint32_t i = 0; i < program->stage_count; ++i) {
            program->reflections[i] = std::move(build.program.stages[i].reflection);
        }
        watch_build(state, *program, build.report);
    } else {
        watch_failure(state, *program, build.report);
        DE_LOG_ERROR("shader", "%s: the first build failed; it will be rebuilt when its sources change",
                     program->module.c_str());
    }
    build = PendingBuild{};
    return from_slot(state.programs.insert(program));
}

bool valid_names(const char* module, std::span<const ShaderDefine> defines, const char* const* entries,
                 uint32_t entry_count) {
    bool ok = fits(module) && defines.size() <= kMaxShaderDefines;
    for (uint32_t i = 0; i < entry_count; ++i) {
        ok = ok && fits(entries[i]);
    }
    for (const ShaderDefine& define : defines) {
        ok = ok && fits(define.name) && (define.value == nullptr || std::strlen(define.value) <= kMaxShaderNameLength);
    }
    if (!ok) {
        DE_LOG_ERROR("shader",
                     "program '%s': module, entry, define names and values are 1 to %zu characters, and there are at "
                     "most %zu defines",
                     module != nullptr ? module : "", kMaxShaderNameLength, kMaxShaderDefines);
    }
    return ok;
}

} // namespace

bool is_bindless_binding(const ResourceBinding& binding) {
    for (const BindlessDeclaration& declaration : kBindlessDeclarations) {
        if (binding.set == 0 && binding.binding == static_cast<uint32_t>(declaration.table) &&
            binding.name == declaration.name) {
            return true;
        }
    }
    return false;
}

uint64_t defines_hash(std::span<const ShaderDefine> defines) {
    asset::ShaderDefineText texts[kMaxShaderDefines];
    const size_t count = std::min(defines.size(), kMaxShaderDefines);
    for (size_t i = 0; i < count; ++i) {
        texts[i] = {.name = defines[i].name != nullptr ? defines[i].name : "",
                    .value = defines[i].value != nullptr ? defines[i].value : "1"};
    }
    return asset::shader_defines_hash(std::span(texts, count));
}

bool shader_module_path(std::string_view module, platform::PathString& path) {
    path.assign("shaders/");
    for (const char c : module) {
        if (!path.append(c == '.' ? '/' : c)) {
            return false;
        }
    }
    return path.append(".slang");
}

bool shader_module_name(std::string_view path, ShaderName& module) {
    constexpr std::string_view kFolder = "shaders/";
    constexpr std::string_view kExtension = ".slang";
    if (!path.starts_with(kFolder) || !path.ends_with(kExtension) ||
        path.size() == kFolder.size() + kExtension.size()) {
        return false;
    }
    if (!module.assign(path.substr(kFolder.size(), path.size() - kFolder.size() - kExtension.size()))) {
        return false;
    }
    for (size_t i = 0; i < module.size(); ++i) {
        if (module[i] == '/') {
            module[i] = '.';
        }
    }
    return true;
}

ShaderLibrary ShaderLibrary::make(rhi::Device& device, platform::Vfs& vfs, core::JobSystem& jobs,
                                  ShaderCompiler* compiler, const ShaderLibraryDesc& desc) {
    auto* state = ::new (render_heap().allocate(sizeof(LibraryState), alignof(LibraryState))) LibraryState();
    state->device = &device;
    state->vfs = &vfs;
    state->jobs = &jobs;
    state->compiler = compiler;
    state->desc = desc;
    return ShaderLibrary(state);
}

core::Result<ShaderLibrary> ShaderLibrary::create(rhi::Device& device, platform::Vfs& vfs, core::JobSystem& jobs,
                                                  ShaderCompiler& compiler, const ShaderLibraryDesc& desc) {
    return make(device, vfs, jobs, &compiler, desc);
}

core::Result<ShaderLibrary> ShaderLibrary::create_cooked(rhi::Device& device, platform::Vfs& vfs, core::JobSystem& jobs,
                                                         const ShaderLibraryDesc& desc) {
    return make(device, vfs, jobs, nullptr, desc);
}

ShaderLibrary::~ShaderLibrary() {
    if (state_ == nullptr) {
        return;
    }
    if (state_->building != nullptr) {
        state_->jobs->wait(state_->building->counter);
    }
    for (Program* program : state_->programs) {
        state_->device->destroy(program->pipeline);
        free_program(program);
    }
    state_->~LibraryState();
    render_heap().deallocate(state_, sizeof(LibraryState), alignof(LibraryState));
}

ShaderLibrary::ShaderLibrary(ShaderLibrary&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

ShaderLibrary& ShaderLibrary::operator=(ShaderLibrary&& other) noexcept {
    if (this != &other) {
        const ShaderLibrary discarded(std::exchange(state_, std::exchange(other.state_, nullptr)));
    }
    return *this;
}

core::Result<ProgramHandle> ShaderLibrary::create_graphics(const GraphicsProgramDesc& desc) {
    DE_PROFILE_ZONE("shader create program");
    const char* const entries[] = {desc.vertex_entry, desc.fragment_entry};
    if (!valid_names(desc.module, desc.defines, entries, 2)) {
        return core::fail(core::ErrorCode::InvalidArgument, "a program name, entry or define is missing or too long");
    }
    Program* program = new_program(*state_, desc.module, desc.defines, desc.state.debug_name);
    program->graphics = true;
    program->graphics_state = desc.state;
    program->entries[0].assign(desc.vertex_entry);
    program->entries[1].assign(desc.fragment_entry);
    program->stages[0] = rhi::ShaderStage::Vertex;
    program->stages[1] = rhi::ShaderStage::Fragment;
    program->stage_count = 2;
    return add_program(*state_, program);
}

core::Result<ProgramHandle> ShaderLibrary::create_compute(const ComputeProgramDesc& desc) {
    DE_PROFILE_ZONE("shader create program");
    const char* const entries[] = {desc.entry};
    if (!valid_names(desc.module, desc.defines, entries, 1)) {
        return core::fail(core::ErrorCode::InvalidArgument, "a program name, entry or define is missing or too long");
    }
    Program* program = new_program(*state_, desc.module, desc.defines, desc.debug_name);
    program->graphics = false;
    program->entries[0].assign(desc.entry);
    program->stages[0] = rhi::ShaderStage::Compute;
    program->stage_count = 1;
    return add_program(*state_, program);
}

void ShaderLibrary::destroy(ProgramHandle handle) {
    LibraryState& state = *state_;
    Program* program = find(state, handle);
    if (program == nullptr) {
        return;
    }
    if (program == state.building) {
        state.jobs->wait(program->counter); // its result is dropped
        state.building = nullptr;
    } else if (program->build.load(std::memory_order_relaxed) == Build::Queued) {
        --state.queued;
    }
    state.programs.remove(to_slot(handle));
    state.device->destroy(program->pipeline);
    free_program(program);
}

rhi::PipelineHandle ShaderLibrary::pipeline(ProgramHandle handle) const {
    const Program* program = find(*state_, handle);
    return program != nullptr ? program->pipeline : rhi::PipelineHandle{};
}

const ShaderReflection* ShaderLibrary::reflection(ProgramHandle handle, rhi::ShaderStage stage) const {
    const Program* program = find(*state_, handle);
    if (program == nullptr || !program->pipeline.is_valid()) {
        return nullptr;
    }
    for (uint32_t i = 0; i < program->stage_count; ++i) {
        if (program->stages[i] == stage) {
            return &program->reflections[i];
        }
    }
    return nullptr;
}

void ShaderLibrary::update() {
    DE_PROFILE_ZONE("shader library update");
    LibraryState& state = *state_;
    advance_builds(state, false);
    if (!state.desc.hot_reload) {
        return;
    }
    const uint64_t now = platform::time_ns();
    if (now < state.next_poll_ns) {
        return;
    }
    state.next_poll_ns = now + static_cast<uint64_t>(state.desc.poll_interval_ms) * platform::kNsPerMs;
    {
        DE_PROFILE_ZONE("shader hot reload poll");
        queue_changed(state);
    }
    start_next_build(state);
}

void ShaderLibrary::reload_now() {
    DE_PROFILE_ZONE("shader reload now");
    LibraryState& state = *state_;
    queue_changed(state);
    start_next_build(state);
    while (state.building != nullptr) {
        advance_builds(state, true);
    }
}

ShaderLibraryStats ShaderLibrary::stats() const {
    return {
        .programs = static_cast<uint32_t>(state_->programs.size()),
        .reloads = state_->reloads,
        .failed_reloads = state_->failed_reloads,
        .compiling = state_->queued + (state_->building != nullptr ? 1U : 0U),
    };
}

} // namespace de::shader
