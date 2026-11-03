#pragma once

#include "core/result.h"
#include "core/slot_map.h"
#include "render/rhi/rhi.h"
#include "render/shader/shader_compiler.h"

#include <cstdint>
#include <span>

namespace de::core {
class JobSystem;
}
namespace de::platform {
class Vfs;
}
namespace de::rhi {
class Device;
}

namespace de::shader {

struct LibraryState;
struct Program;

// A shader program and the pipeline built from it. Stable across hot reloads: look up the
// current pipeline with ShaderLibrary::pipeline() each time you bind it.
using ProgramHandle = core::SlotHandle<Program>;

struct GraphicsProgramDesc {
    const char* module = nullptr; // Slang module under src:/shaders/: "triangle", "common.color"
    const char* vertex_entry = "vertex_main";
    const char* fragment_entry = "fragment_main";
    std::span<const ShaderDefine> defines{};
    rhi::GraphicsPipelineDesc state{}; // everything but the shader code, which the library fills in
};

struct ComputeProgramDesc {
    const char* module = nullptr;
    const char* entry = "compute_main";
    std::span<const ShaderDefine> defines{};
    const char* debug_name = nullptr;
};

struct ShaderLibraryDesc {
    // Watch the sources of every program and rebuild changed ones while the game runs.
    bool hot_reload = DE_BUILD_DEV != 0;
    uint32_t poll_interval_ms = 250; // how often update() checks the sources
};

struct ShaderLibraryStats {
    uint32_t programs = 0;
    uint32_t reloads = 0;        // programs rebuilt and swapped in after a source change
    uint32_t failed_reloads = 0; // rebuilds that failed: the last good pipeline stayed
    uint32_t compiling = 0;      // rebuilds queued or running in the background
};

// Shader programs with live reload (docs/design.md, Shaders, materials & VFX): edit a .slang file
// and its programs rebuild in the background while the game keeps rendering with the last good
// version; errors are logged with file and line, and the old pipeline stays until a fix compiles.
//
// Builds without src:/ (ship) use create_cooked() instead: programs come from the cooker's
// .deshader files in game:/, compiled ahead of time with the same compiler. With hot reload on,
// a re-cooked file is picked up the same way an edited source is.
//
// Create programs at load time (the first compile blocks). Call update() once per frame on the
// render thread. Pipelines are created and destroyed on that thread only; rebuilds run one at a
// time as background jobs, which only idle workers pick up, so they never stall a frame.
// Move-only; the device, VFS, job system and compiler must outlive it.
class ShaderLibrary {
public:
    static core::Result<ShaderLibrary> create(rhi::Device& device, platform::Vfs& vfs, core::JobSystem& jobs,
                                              ShaderCompiler& compiler, const ShaderLibraryDesc& desc = {});
    // Programs from cooked .deshader files (game:/), with the permutation that matches each
    // program's defines. A module or permutation that wasn't cooked fails like a compile error.
    static core::Result<ShaderLibrary> create_cooked(rhi::Device& device, platform::Vfs& vfs, core::JobSystem& jobs,
                                                     const ShaderLibraryDesc& desc = {});

    ~ShaderLibrary();
    ShaderLibrary(ShaderLibrary&& other) noexcept;
    ShaderLibrary& operator=(ShaderLibrary&& other) noexcept;
    ShaderLibrary(const ShaderLibrary&) = delete;
    ShaderLibrary& operator=(const ShaderLibrary&) = delete;

    // Compiles now and builds the pipeline. Fails only for a bad description (a missing or
    // over-long name). If the shader doesn't compile, the errors are logged and the program is
    // kept without a pipeline: it is watched like any other, and a fix to its sources brings it to
    // life. Check pipeline() before drawing with it.
    core::Result<ProgramHandle> create_graphics(const GraphicsProgramDesc& desc);
    core::Result<ProgramHandle> create_compute(const ComputeProgramDesc& desc);

    // Destroys the program and its pipeline (once the GPU is done with it). Waits for a rebuild in flight.
    void destroy(ProgramHandle program);

    // The program's current pipeline; invalid for a stale handle or a program that has never built.
    rhi::PipelineHandle pipeline(ProgramHandle program) const;

    // The reflection of one stage of the current version; null for a stale handle or a missing stage.
    const ShaderReflection* reflection(ProgramHandle program, rhi::ShaderStage stage) const;

    // Once per frame: every poll interval, checks the sources of each program and starts a
    // background rebuild of the changed ones; swaps in the rebuilds that finished. Without hot
    // reload, or between polls with nothing finished, it does nothing and never allocates.
    void update();

    // Checks every program's sources now, waits for the rebuilds and swaps them in. For tests and
    // tools; works without hot_reload too.
    void reload_now();

    ShaderLibraryStats stats() const;

private:
    explicit ShaderLibrary(LibraryState* state) : state_(state) {}
    static ShaderLibrary make(rhi::Device& device, platform::Vfs& vfs, core::JobSystem& jobs, ShaderCompiler* compiler,
                              const ShaderLibraryDesc& desc);

    LibraryState* state_ = nullptr;
};

// Whether a descriptor binding is one of the bindless tables of shaders/common/bindless.slang: the
// only descriptor bindings the engine's pipelines have (the library refuses programs with others).
bool is_bindless_binding(const ResourceBinding& binding);

} // namespace de::shader
