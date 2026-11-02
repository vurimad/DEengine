#include "render/shader/shader_library.h"

#include "core/allocator.h"
#include "core/job_system.h"
#include "core/log.h"
#include "core/profile.h"
#include "platform/time.h"
#include "platform/vfs.h"
#include "render/rhi/device.h"

#include <atomic>
#include <new>
#include <utility>

namespace de::shader {

namespace {

constexpr uint32_t kMaxStages = 2;

enum class Rebuild : uint8_t {
    Idle,
    Compiling, // a job owns Program::pending
    Done,      // the job finished: the render thread swaps it in or drops it
};

struct StoredDefine {
    ShaderName name;
    ShaderName value;
};

// One stage of one build: the compile's outcome and output.
struct StageBuild {
    bool ok = false;
    CompiledShader shader;
};

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
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

    rhi::PipelineHandle pipeline;
    StageBuild current[kMaxStages]; // the live version (its code is dropped once the pipeline exists)
    // Every source file the program depends on, with the size and time last seen. Render thread only.
    core::Array<ShaderDependency> watched{render_heap()};

    std::atomic<Rebuild> rebuild{Rebuild::Idle};
    uint64_t rebuild_start_ns = 0;
    StageBuild pending[kMaxStages]; // written by the rebuild job
    core::JobCounter counter;
};

struct LibraryState {
    rhi::Device* device = nullptr;
    platform::Vfs* vfs = nullptr;
    core::JobSystem* jobs = nullptr;
    ShaderCompiler* compiler = nullptr;
    ShaderLibraryDesc desc;
    core::SlotMap<Program*> programs{render_heap()};
    uint64_t next_poll_ns = 0;
    std::atomic<uint32_t> compiling{0};
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

// Compiles every stage of the program into builds. Runs on the creating thread or as a job.
void compile_stages(Program& program, StageBuild (&builds)[kMaxStages]) {
    ShaderDefine defines[kMaxShaderDefines];
    for (uint32_t i = 0; i < program.define_count; ++i) {
        defines[i] = {.name = program.defines[i].name.c_str(), .value = program.defines[i].value.c_str()};
    }
    for (uint32_t i = 0; i < program.stage_count; ++i) {
        core::Result<CompiledShader> compiled = program.library->compiler->compile({
            .module = program.module.c_str(),
            .entry = program.entries[i].c_str(),
            .stage = program.stages[i],
            .defines = std::span(defines, program.define_count),
        });
        builds[i].ok = compiled.ok();
        if (compiled) {
            builds[i].shader = std::move(*compiled);
        }
    }
}

void rebuild_job(void* context) {
    DE_PROFILE_ZONE("shader rebuild");
    auto& program = *static_cast<Program*>(context);
    compile_stages(program, program.pending);
    program.rebuild.store(Rebuild::Done, std::memory_order_release);
}

// Builds a pipeline from compiled stages, after checking they fit the engine's one pipeline layout.
core::Result<rhi::PipelineHandle> create_pipeline(LibraryState& state, Program& program,
                                                  const StageBuild (&builds)[kMaxStages]) {
    for (uint32_t i = 0; i < program.stage_count; ++i) {
        const ShaderReflection& reflection = builds[i].shader.reflection;
        if (!reflection.bindings.empty()) {
            DE_LOG_ERROR("shader",
                         "%s:%s binds '%s' to a descriptor slot (set %u, binding %u): reach resources "
                         "bindlessly, by index or pointer, through push constants",
                         program.module.c_str(), program.entries[i].c_str(), reflection.bindings[0].name.c_str(),
                         reflection.bindings[0].set, reflection.bindings[0].binding);
            return core::fail(core::ErrorCode::Unsupported, "the shader binds resources to descriptor slots");
        }
        if (reflection.push_constant_bytes > rhi::kMaxPushConstantBytes) {
            DE_LOG_ERROR("shader", "%s:%s has %u bytes of push constants; the limit is %u", program.module.c_str(),
                         program.entries[i].c_str(), reflection.push_constant_bytes, rhi::kMaxPushConstantBytes);
            return core::fail(core::ErrorCode::Unsupported, "the shader has too many push constant bytes");
        }
    }
    const auto code_of = [&](uint32_t stage) {
        return rhi::ShaderCode{.stage = program.stages[stage], .code = std::as_bytes(builds[stage].shader.code.span())};
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

// The dependencies of builds, merged into watched (each path once, the newest info winning).
void watch(core::Array<ShaderDependency>& watched, const StageBuild (&builds)[kMaxStages], uint32_t stage_count) {
    for (uint32_t stage = 0; stage < stage_count; ++stage) {
        for (const ShaderDependency& dependency : builds[stage].shader.dependencies) {
            bool merged = false;
            for (ShaderDependency& known : watched) {
                if (known.path == dependency.path.view()) {
                    known.info = dependency.info;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                watched.push(dependency);
            }
        }
    }
}

// The live version keeps reflection and dependencies only: the pipeline holds the code now.
void keep_reflection(StageBuild (&builds)[kMaxStages]) {
    for (StageBuild& build : builds) {
        build.shader.code.clear();
        build.shader.code.shrink_to_fit();
    }
}

// True if a watched file changed since it was last seen. Records what it sees now, so a failed
// rebuild isn't retried until the file changes again.
bool sources_changed(const LibraryState& state, Program& program) {
    bool changed = false;
    for (ShaderDependency& dependency : program.watched) {
        const core::Result<platform::FileInfo> info = state.vfs->file_info(dependency.path.c_str());
        const platform::FileInfo now = info.ok() ? *info : platform::FileInfo{};
        if (now.size != dependency.info.size || now.modified_time != dependency.info.modified_time) {
            dependency.info = now;
            changed = true;
        }
    }
    return changed;
}

void start_rebuild(LibraryState& state, Program& program) {
    DE_LOG_INFO("shader", "%s changed: rebuilding", program.module.c_str());
    program.rebuild_start_ns = platform::time_ns();
    program.rebuild.store(Rebuild::Compiling, std::memory_order_relaxed);
    state.compiling.fetch_add(1, std::memory_order_relaxed);
    state.jobs->submit(
        core::Job{.entry = &rebuild_job, .context = &program, .counter = &program.counter, .name = "shader rebuild"});
}

// Swaps in a finished rebuild, or keeps the last good version if it failed. Render thread only.
void finish_rebuild(LibraryState& state, Program& program) {
    if (program.rebuild.load(std::memory_order_acquire) != Rebuild::Done) {
        return;
    }
    bool ok = true;
    for (uint32_t i = 0; i < program.stage_count; ++i) {
        ok = ok && program.pending[i].ok;
    }
    core::Result<rhi::PipelineHandle> pipeline =
        ok ? create_pipeline(state, program, program.pending)
           : core::Result<rhi::PipelineHandle>(core::fail(core::ErrorCode::InvalidArgument, "compile failed"));
    watch(program.watched, program.pending, program.stage_count);
    const double elapsed_ms = static_cast<double>(platform::time_ns() - program.rebuild_start_ns) / 1e6;
    if (pipeline) {
        state.device->destroy(program.pipeline);
        program.pipeline = *pipeline;
        for (uint32_t i = 0; i < program.stage_count; ++i) {
            program.current[i] = std::move(program.pending[i]);
        }
        keep_reflection(program.current);
        ++state.reloads;
        DE_LOG_INFO("shader", "%s reloaded in %.0f ms", program.module.c_str(), elapsed_ms);
    } else {
        ++state.failed_reloads;
        DE_LOG_ERROR("shader", "%s: rebuild failed, keeping the last good version", program.module.c_str());
    }
    for (StageBuild& build : program.pending) {
        build = StageBuild{};
    }
    program.rebuild.store(Rebuild::Idle, std::memory_order_relaxed);
    state.compiling.fetch_sub(1, std::memory_order_relaxed);
}

core::Result<ProgramHandle> add_program(LibraryState& state, Program* program) {
    compile_stages(*program, program->current);
    bool ok = true;
    for (uint32_t i = 0; i < program->stage_count; ++i) {
        ok = ok && program->current[i].ok;
    }
    core::Result<rhi::PipelineHandle> pipeline =
        ok ? create_pipeline(state, *program, program->current)
           : core::Result<rhi::PipelineHandle>(
                 core::fail(core::ErrorCode::InvalidArgument, "the shader failed to compile"));
    if (!pipeline) {
        program->~Program();
        render_heap().deallocate(program, sizeof(Program), alignof(Program));
        return core::fail(pipeline.error());
    }
    program->pipeline = *pipeline;
    watch(program->watched, program->current, program->stage_count);
    keep_reflection(program->current);
    return from_slot(state.programs.insert(program));
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

void destroy_program(LibraryState& state, Program* program) {
    state.jobs->wait(program->counter);
    if (program->rebuild.load(std::memory_order_acquire) == Rebuild::Done) {
        state.compiling.fetch_sub(1, std::memory_order_relaxed);
    }
    state.device->destroy(program->pipeline);
    program->~Program();
    render_heap().deallocate(program, sizeof(Program), alignof(Program));
}

} // namespace

core::Result<ShaderLibrary> ShaderLibrary::create(rhi::Device& device, platform::Vfs& vfs, core::JobSystem& jobs,
                                                  ShaderCompiler& compiler, const ShaderLibraryDesc& desc) {
    auto* state = ::new (render_heap().allocate(sizeof(LibraryState), alignof(LibraryState))) LibraryState();
    state->device = &device;
    state->vfs = &vfs;
    state->jobs = &jobs;
    state->compiler = &compiler;
    state->desc = desc;
    return ShaderLibrary(state);
}

ShaderLibrary::~ShaderLibrary() {
    if (state_ == nullptr) {
        return;
    }
    for (Program* program : state_->programs) {
        destroy_program(*state_, program);
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
    if (desc.module == nullptr || desc.vertex_entry == nullptr || desc.fragment_entry == nullptr ||
        desc.defines.size() > kMaxShaderDefines) {
        return core::fail(core::ErrorCode::InvalidArgument,
                          "a program needs a module, entry points and at most 16 defines");
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
    if (desc.module == nullptr || desc.entry == nullptr || desc.defines.size() > kMaxShaderDefines) {
        return core::fail(core::ErrorCode::InvalidArgument,
                          "a program needs a module, an entry point and at most 16 defines");
    }
    Program* program = new_program(*state_, desc.module, desc.defines, desc.debug_name);
    program->graphics = false;
    program->entries[0].assign(desc.entry);
    program->stages[0] = rhi::ShaderStage::Compute;
    program->stage_count = 1;
    return add_program(*state_, program);
}

void ShaderLibrary::destroy(ProgramHandle handle) {
    Program* program = find(*state_, handle);
    if (program == nullptr) {
        return;
    }
    state_->programs.remove(to_slot(handle));
    destroy_program(*state_, program);
}

rhi::PipelineHandle ShaderLibrary::pipeline(ProgramHandle handle) const {
    const Program* program = find(*state_, handle);
    return program != nullptr ? program->pipeline : rhi::PipelineHandle{};
}

const ShaderReflection* ShaderLibrary::reflection(ProgramHandle handle, rhi::ShaderStage stage) const {
    const Program* program = find(*state_, handle);
    if (program == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; i < program->stage_count; ++i) {
        if (program->stages[i] == stage) {
            return &program->current[i].shader.reflection;
        }
    }
    return nullptr;
}

void ShaderLibrary::update() {
    LibraryState& state = *state_;
    if (state.compiling.load(std::memory_order_relaxed) != 0) {
        for (Program* program : state.programs) {
            finish_rebuild(state, *program);
        }
    }
    if (!state.desc.hot_reload) {
        return;
    }
    const uint64_t now = platform::time_ns();
    if (now < state.next_poll_ns) {
        return;
    }
    DE_PROFILE_ZONE("shader hot reload poll");
    state.next_poll_ns = now + static_cast<uint64_t>(state.desc.poll_interval_ms) * platform::kNsPerMs;
    for (Program* program : state.programs) {
        if (program->rebuild.load(std::memory_order_relaxed) == Rebuild::Idle && sources_changed(state, *program)) {
            start_rebuild(state, *program);
        }
    }
}

void ShaderLibrary::reload_now() {
    DE_PROFILE_ZONE("shader reload now");
    LibraryState& state = *state_;
    for (Program* program : state.programs) {
        if (program->rebuild.load(std::memory_order_relaxed) == Rebuild::Idle && sources_changed(state, *program)) {
            start_rebuild(state, *program);
        }
    }
    for (Program* program : state.programs) {
        state.jobs->wait(program->counter);
        finish_rebuild(state, *program);
    }
}

ShaderLibraryStats ShaderLibrary::stats() const {
    return {
        .programs = static_cast<uint32_t>(state_->programs.size()),
        .reloads = state_->reloads,
        .failed_reloads = state_->failed_reloads,
        .compiling = state_->compiling.load(std::memory_order_relaxed),
    };
}

} // namespace de::shader
