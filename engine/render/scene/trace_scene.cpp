#include "render/scene/trace_scene.h"

#include "asset/mesh_format.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/scene/gpu_assets.h"
#include "render/shader/shader_library.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace de::render {

namespace {

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

// Matches TraceInstancesPush in shaders/scene/trace_instances.slang.
struct TraceInstancesPush {
    uint64_t frame = 0;
    uint64_t instances = 0;
    uint64_t instance_meshes = 0;
    uint64_t mesh_trace = 0;
    uint64_t output = 0;
    uint32_t count = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(TraceInstancesPush) == 48);

// Meshes that went or were cooked again are looked for when the registry or the pool says something changed, and
// this often regardless.
constexpr uint32_t kSweepFrames = 120;

// A mesh as this keeps it, by its slot in the mesh table.
struct MeshEntry {
    asset::AssetHandle handle;
    rhi::AccelerationHandle bottom;
    uint32_t revision = 0;      // of the mesh it was built from
    uint32_t first_part = 0;    // where its parts were in the part table then: elsewhere, it's another mesh's
    uint32_t vertex_offset = 0; // and its vertices in the pool
    uint32_t part_count = 0;
    uint32_t triangles = 0;   // in its bottom level
    uint64_t built_frame = 0; // the frame its build was recorded in
    bool wanted = false;      // some instance uses it
    bool waiting = false;     // in the list of those to build
    bool published = false;   // the table says it: instances may name it
};

struct Build {
    rhi::AccelerationHandle bottom;
    uint32_t first_run = 0;
    uint32_t run_count = 0;
};

} // namespace

struct TraceSceneState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    GpuAssets* gpu_assets = nullptr;
    asset::AssetRegistry* assets = nullptr;
    uint32_t max_instances = 0;
    shader::ProgramHandle instances_program;
    rhi::AccelerationHandle top;
    rhi::BufferHandle top_instances;                       // rhi::AccelerationInstance by slot, written by the GPU
    rhi::BufferHandle mesh_trace;                          // GpuMeshTrace by mesh slot, written here
    rhi::BufferHandle scene_buffers[rhi::kFramesInFlight]; // each frame's GpuTraceScene
    uint64_t top_instances_address = 0;
    uint64_t mesh_trace_address = 0;
    bool top_instances_used = false;                  // by some frame: the graph is told how that left it
    bool top_built = false;                           // some frame has built the top level: it can be traced against
    core::Array<MeshEntry> meshes{render_heap()};     // by mesh slot
    core::Array<uint32_t> waiting{render_heap()};     // mesh slots wanted, without a bottom level
    core::Array<uint32_t> built{render_heap()};       // mesh slots with one
    core::Array<uint32_t> unpublished{render_heap()}; // of those, not in the table yet
    core::Array<Build> builds_now{render_heap()};     // this frame's, for its pass
    core::Array<rhi::AccelerationTriangles> runs_now{render_heap()};
    uint32_t reloads_seen = 0;
    uint64_t slots_released_seen = 0;
    uint32_t frames_since_sweep = 0;
    uint64_t triangles = 0; // in every bottom level there is
    bool warned_runs = false;
    TraceStats stats;

    void write_table(uint32_t slot, const GpuMeshTrace& entry) const {
        std::memcpy(device->mapped(mesh_trace).data() + (size_t{slot} * sizeof(GpuMeshTrace)), &entry, sizeof(entry));
        device->flush_mapped(mesh_trace, uint64_t{slot} * sizeof(GpuMeshTrace), sizeof(GpuMeshTrace));
    }

    // Lets a mesh's bottom level go: the table says none from now, and the structure is freed once no frame in
    // flight can still trace it.
    void retire(uint32_t slot) {
        MeshEntry& entry = meshes[slot];
        if (!entry.bottom.is_valid()) {
            return;
        }
        write_table(slot, {});
        device->destroy(entry.bottom);
        triangles -= entry.triangles;
        entry.triangles = 0;
        entry.bottom = {};
        entry.published = false;
        const auto drop = [slot](core::Array<uint32_t>& list) {
            for (size_t i = 0; i < list.size(); ++i) {
                if (list[i] == slot) {
                    list.remove_swap(i);
                    return;
                }
            }
        };
        drop(built);
        drop(unpublished);
    }

    void wait(uint32_t slot) {
        MeshEntry& entry = meshes[slot];
        if (!entry.waiting) {
            entry.waiting = true;
            waiting.push(slot);
        }
    }

    // A bottom level for a mesh that's on the GPU: its build queued for this frame. False if it must wait.
    bool queue_build(uint32_t slot) {
        MeshEntry& entry = meshes[slot];
        const GpuMesh* mesh = gpu_assets->mesh(entry.handle);
        if (mesh == nullptr || !gpu_assets->mesh_on_gpu(entry.handle) || mesh->lods.empty()) {
            return false;
        }
        const size_t lod = trace_lod(*mesh);
        const std::span<const GpuMeshPart> parts = mesh->lod_parts(lod);
        const auto first_run = static_cast<uint32_t>(runs_now.size());
        // A run a part, in the parts' order: a hit's run is its part. More than a bottom level takes are left out.
        const size_t run_count = std::min<size_t>(parts.size(), rhi::kMaxAccelerationGeometry);
        if (parts.size() > rhi::kMaxAccelerationGeometry && !warned_runs) {
            DE_LOG_WARN("render", "a mesh of %zu parts: rays see its first %u", parts.size(),
                        rhi::kMaxAccelerationGeometry);
            warned_runs = true;
        }
        for (size_t p = 0; p < run_count; ++p) {
            const GpuMeshPart& part = parts[p];
            if (part.index_count < 3 || mesh->vertex_count == 0) {
                runs_now.resize(first_run);
                return true; // nothing a ray could meet: not built, and not asked for again
            }
            runs_now.push({.vertices = gpu_assets->pool_vertex_buffer(),
                           .vertex_offset = uint64_t{part.vertex_offset} * sizeof(asset::MeshVertex),
                           .vertex_stride = sizeof(asset::MeshVertex),
                           .vertex_count = mesh->vertex_count,
                           .indices = gpu_assets->pool_index_buffer(),
                           .index_offset = uint64_t{part.first_index} * sizeof(uint32_t),
                           .index_count = part.index_count - (part.index_count % 3)});
        }
        const std::span<const rhi::AccelerationTriangles> runs(runs_now.data() + first_run, run_count);
        const core::Result<rhi::AccelerationHandle> bottom =
            device->create_bottom_level({.geometry = runs, .debug_name = "mesh bottom level"});
        if (!bottom) {
            DE_LOG_ERROR("render", "a mesh gets no bottom level, and no ray meets it: %s", bottom.error().message);
            runs_now.resize(first_run);
            return true;
        }
        entry.bottom = *bottom;
        entry.revision = assets != nullptr ? assets->revision(entry.handle) : 0;
        entry.first_part = mesh->first_table_part + mesh->lods[lod].first_part;
        entry.part_count = static_cast<uint32_t>(run_count);
        entry.triangles = 0;
        for (const rhi::AccelerationTriangles& run : runs) {
            entry.triangles += run.index_count / 3;
        }
        triangles += entry.triangles;
        entry.vertex_offset = mesh->vertex_offset;
        entry.built_frame = device->frame_index();
        entry.published = false;
        built.push(slot);
        unpublished.push(slot);
        builds_now.push({.bottom = *bottom, .first_run = first_run, .run_count = static_cast<uint32_t>(run_count)});
        return true;
    }

    // Bottom levels no instance uses now, or whose mesh went, moved in the pool or was cooked again: let go, and
    // built again if still wanted.
    void sweep(std::span<const uint32_t> mesh_instances) {
        for (size_t i = 0; i < built.size();) {
            const uint32_t slot = built[i];
            MeshEntry& entry = meshes[slot];
            if (slot >= mesh_instances.size() || mesh_instances[slot] == 0) {
                entry.wanted = false;
                retire(slot); // takes it out of built: the same i is the next
                continue;
            }
            const GpuMesh* mesh = gpu_assets->mesh(entry.handle);
            const bool same = mesh != nullptr && mesh->vertex_offset == entry.vertex_offset &&
                              (assets == nullptr || assets->revision(entry.handle) == entry.revision);
            if (same) {
                ++i;
                continue;
            }
            retire(slot); // takes it out of built: the same i is the next
            if (entry.wanted) {
                wait(slot);
            }
        }
    }
};

size_t trace_lod(const GpuMesh& mesh) {
    // The coarsest that's as good as the finest, to a ray: as near it, with every part of it, and its seams where they
    // were (a ray reads the surface's colour off it, which one with its seams moved gets wrong).
    size_t lod = 0;
    for (size_t l = 1; l < mesh.lods.size(); ++l) {
        const bool seams_kept = l < 32 && (mesh.seams_moved_lods & (1U << static_cast<uint32_t>(l))) == 0;
        if (mesh.lods[l].error_m <= kTraceLodErrorM && seams_kept &&
            mesh.lods[l].part_count == mesh.lods[0].part_count) {
            lod = l;
        }
    }
    return lod;
}

core::Result<TraceScene> TraceScene::create(const TraceSceneDesc& desc) {
    if (desc.device == nullptr || desc.shaders == nullptr || desc.gpu_assets == nullptr || desc.max_instances == 0 ||
        desc.max_meshes == 0) {
        return core::fail(core::ErrorCode::InvalidArgument,
                          "tracing needs a device, a shader library, the GPU's assets and room for instances");
    }
    if (!desc.device->capabilities().ray_query) {
        return core::fail(core::ErrorCode::Unsupported, "this GPU has no ray queries: nothing is traced");
    }
    auto* state = ::new (render_heap().allocate(sizeof(TraceSceneState), alignof(TraceSceneState))) TraceSceneState();
    state->device = desc.device;
    state->shaders = desc.shaders;
    state->gpu_assets = desc.gpu_assets;
    state->assets = desc.assets;
    state->max_instances = std::min(desc.max_instances, kMaxTraceInstances);
    TraceScene scene(state); // lets go of what was made if the rest fails
    state->meshes.resize(desc.max_meshes);
    const core::Result<rhi::AccelerationHandle> top =
        desc.device->create_top_level({.max_instances = state->max_instances, .debug_name = "scene top level"});
    if (!top) {
        return core::fail(top.error());
    }
    state->top = *top;
    const core::Result<rhi::BufferHandle> instances =
        desc.device->create_buffer({.size = uint64_t{state->max_instances} * sizeof(rhi::AccelerationInstance),
                                    .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationInput,
                                    .debug_name = "top level instances"});
    if (!instances) {
        return core::fail(instances.error());
    }
    state->top_instances = *instances;
    state->top_instances_address = desc.device->buffer_address(*instances);
    const core::Result<rhi::BufferHandle> table =
        desc.device->create_buffer({.size = uint64_t{desc.max_meshes} * sizeof(GpuMeshTrace),
                                    .usage = rhi::BufferUsage::Storage,
                                    .memory = rhi::MemoryLocation::Upload,
                                    .debug_name = "mesh trace table"});
    if (!table) {
        return core::fail(table.error());
    }
    state->mesh_trace = *table;
    state->mesh_trace_address = desc.device->buffer_address(*table);
    // No mesh has a bottom level yet.
    const std::span<std::byte> mapped = desc.device->mapped(*table);
    std::memset(mapped.data(), 0, mapped.size());
    desc.device->flush_mapped(*table);
    for (rhi::BufferHandle& buffer : state->scene_buffers) {
        const core::Result<rhi::BufferHandle> made = desc.device->create_buffer({.size = sizeof(GpuTraceScene),
                                                                                 .usage = rhi::BufferUsage::Storage,
                                                                                 .memory = rhi::MemoryLocation::Upload,
                                                                                 .debug_name = "trace scene"});
        if (!made) {
            return core::fail(made.error());
        }
        buffer = *made;
    }
    const core::Result<shader::ProgramHandle> program =
        desc.shaders->create_compute({.module = "scene.trace_instances", .debug_name = "trace instances"});
    state->instances_program = program ? *program : shader::ProgramHandle{};
    if (!program) {
        DE_LOG_WARN("render", "the trace instances' shader didn't build: nothing is traced until it does");
    }
    return scene;
}

TraceScene::~TraceScene() {
    if (state_ == nullptr) {
        return;
    }
    state_->shaders->destroy(state_->instances_program);
    for (const MeshEntry& entry : state_->meshes) {
        state_->device->destroy(entry.bottom);
    }
    state_->device->destroy(state_->top);
    state_->device->destroy(state_->top_instances);
    state_->device->destroy(state_->mesh_trace);
    for (const rhi::BufferHandle buffer : state_->scene_buffers) {
        state_->device->destroy(buffer);
    }
    state_->~TraceSceneState();
    render_heap().deallocate(state_, sizeof(TraceSceneState), alignof(TraceSceneState));
}

TraceScene::TraceScene(TraceScene&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

TraceScene& TraceScene::operator=(TraceScene&& other) noexcept {
    if (this != &other) {
        std::swap(state_, other.state_);
    }
    return *this;
}

TraceStats TraceScene::stats() const {
    return state_->stats;
}

void TraceScene::want(uint32_t mesh_slot, asset::AssetHandle mesh) {
    TraceSceneState& state = *state_;
    if (mesh_slot >= state.meshes.size() || !mesh.is_valid()) {
        return;
    }
    MeshEntry& entry = state.meshes[mesh_slot];
    if (entry.handle != mesh) {
        state.retire(mesh_slot); // the slot is another mesh's now
        entry.handle = mesh;
    }
    entry.wanted = true;
    if (!entry.bottom.is_valid()) {
        state.wait(mesh_slot);
    }
}

uint64_t TraceScene::scene_address() const {
    const TraceSceneState& state = *state_;
    return state.device->buffer_address(state.scene_buffers[state.device->frame_index() % rhi::kFramesInFlight]);
}

void TraceScene::update(uint64_t instance_data_address, uint64_t instance_meshes_address,
                        std::span<const uint32_t> mesh_instances) {
    DE_PROFILE_ZONE("TraceScene::update");
    TraceSceneState& state = *state_;
    state.builds_now.clear();
    state.runs_now.clear();
    // What went or was cooked again.
    const uint32_t reloads = state.assets != nullptr ? state.assets->stats().reloads : 0;
    const uint64_t released = state.gpu_assets->mesh_slots_released();
    if (reloads != state.reloads_seen || released != state.slots_released_seen ||
        ++state.frames_since_sweep >= kSweepFrames) {
        state.reloads_seen = reloads;
        state.slots_released_seen = released;
        state.frames_since_sweep = 0;
        state.sweep(mesh_instances);
    }
    // Those built in an earlier frame go into the table: every frame that could read it from now on runs after the
    // build (a frame recorded while the build's own was still on its way would not).
    const uint64_t frame = state.device->frame_index();
    for (size_t i = 0; i < state.unpublished.size();) {
        const uint32_t slot = state.unpublished[i];
        MeshEntry& entry = state.meshes[slot];
        if (entry.built_frame >= frame) {
            ++i;
            continue;
        }
        state.write_table(slot, {.bottom_level = state.device->acceleration_address(entry.bottom),
                                 .first_part = entry.first_part,
                                 .part_count = entry.part_count});
        entry.published = true;
        state.unpublished.remove_swap(i);
    }
    // A frame's share of those that wait, the rest next frame.
    size_t kept = 0;
    for (size_t i = 0; i < state.waiting.size(); ++i) {
        const uint32_t slot = state.waiting[i];
        MeshEntry& entry = state.meshes[slot];
        const bool used = slot < mesh_instances.size() && mesh_instances[slot] > 0;
        entry.wanted = entry.wanted && used;
        const bool still = entry.wanted && !entry.bottom.is_valid();
        if (still && (state.builds_now.size() >= kTraceBuildsPerFrame || !state.queue_build(slot))) {
            state.waiting[kept++] = slot;
        } else {
            entry.waiting = false;
        }
    }
    state.waiting.resize(kept);
    state.stats.meshes = static_cast<uint32_t>(state.built.size());
    state.stats.waiting = static_cast<uint32_t>(state.waiting.size());
    state.stats.builds = static_cast<uint32_t>(state.builds_now.size());
    state.stats.triangles = state.triangles;

    const rhi::BufferHandle buffer = state.scene_buffers[frame % rhi::kFramesInFlight];
    const GpuTraceScene scene{.top_level = state.top_built ? state.device->acceleration_address(state.top) : 0,
                              .instances = instance_data_address,
                              .instance_meshes = instance_meshes_address,
                              .mesh_trace = state.mesh_trace_address,
                              .parts = state.gpu_assets->part_table_address(),
                              .vertices = state.gpu_assets->pool_vertex_address(),
                              .indices = state.device->buffer_address(state.gpu_assets->pool_index_buffer()),
                              .materials = state.gpu_assets->material_table_address()};
    std::memcpy(state.device->mapped(buffer).data(), &scene, sizeof(scene));
    state.device->flush_mapped(buffer, 0, sizeof(scene));
}

void TraceScene::add_passes(RenderGraph& graph, const TracePassInput& input) {
    TraceSceneState& state = *state_;
    const rhi::PipelineHandle pipeline = state.shaders->pipeline(state.instances_program);
    state.stats.instances = 0;
    // Built whether or not the instances' shader is there: update() has counted them as built.
    if (!state.builds_now.empty()) {
        graph.add_pass(
            "trace bottom levels", [](PassBuilder& pass) { pass.never_cull(); },
            [renderer = &state](PassContext& context) {
                DE_PROFILE_ZONE("trace bottom levels pass");
                for (const Build& build : renderer->builds_now) {
                    context.list.build_bottom_level(
                        build.bottom, std::span(renderer->runs_now.data() + build.first_run, build.run_count));
                }
            });
    }
    if (!pipeline.is_valid()) {
        return;
    }
    const uint32_t slots = std::min(input.slots, state.max_instances);
    // The instances stay between frames, so the graph is told how the last one left them.
    const GraphBuffer top_instances = graph.import_buffer(
        {.buffer = state.top_instances,
         .name = "top level instances",
         .state = state.top_instances_used ? rhi::ResourceState::ShaderRead : rhi::ResourceState::Undefined,
         .final_state = rhi::ResourceState::ShaderRead});
    state.top_instances_used = true;
    if (slots > 0) {
        const TraceInstancesPush push{.frame = input.frame_address,
                                      .instances = input.instance_data_address,
                                      .instance_meshes = input.instance_meshes_address,
                                      .mesh_trace = state.mesh_trace_address,
                                      .output = state.top_instances_address,
                                      .count = slots};
        graph.add_pass(
            "trace instances",
            [&](PassBuilder& pass) {
                pass.use(input.instance_data, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                pass.use(input.instance_meshes, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                pass.use(top_instances, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
                pass.never_cull();
            },
            [push, pipeline](PassContext& context) {
                DE_PROFILE_ZONE("trace instances pass");
                context.list.bind_pipeline(pipeline);
                context.list.push_constants(push);
                context.list.dispatch((push.count + 63) / 64);
            });
    }
    // Built whole each frame, from where the instances are now: with none, an empty one, so nothing is met.
    graph.add_pass(
        "trace top level",
        [&](PassBuilder& pass) {
            pass.use(top_instances, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.never_cull();
        },
        [renderer = &state, top_instances, slots](PassContext& context) {
            DE_PROFILE_ZONE("trace top level pass");
            context.list.build_top_level(renderer->top, context.buffer(top_instances), 0, slots);
        });
    state.top_built = true;
    state.stats.instances = slots;
}

} // namespace de::render
