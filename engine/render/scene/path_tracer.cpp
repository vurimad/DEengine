#include "render/scene/path_tracer.h"

#include "core/allocator.h"
#include "core/array.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/scene/scene_renderer.h"
#include "render/shader/shader_library.h"
#include "world/render_extract.h"

#include <algorithm>
#include <cstddef>
#include <new>
#include <type_traits>

namespace de::render {

namespace {

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

// Matches PathTracerPush in shaders/scene/path_tracer.slang.
struct PathTracerPush {
    uint64_t frame = 0;
    uint32_t accumulation = 0;
    uint32_t sample = 0;
    uint32_t size[2] = {};
    uint32_t light_count = 0;
    uint32_t max_bounces = 0;
};
static_assert(sizeof(PathTracerPush) == 32);

// Matches LightingComparePush in shaders/scene/lighting_compare.slang.
struct LightingComparePush {
    uint64_t counts = 0;
    uint32_t raster = 0;
    uint32_t traced = 0;
    uint32_t depth = 0;
    uint32_t heat = 0;
    uint32_t size[2] = {};
};
static_assert(sizeof(LightingComparePush) == 32);

// The compare's sums: pixels, |stops| in 1024ths, over 10%, over 25%, darker by 10%, brighter by 10%; and two to
// round it out.
constexpr uint32_t kCompareWords = 8;

// Matches PathTracerShowPush in shaders/scene/path_tracer_show.slang.
struct PathTracerShowPush {
    uint32_t accumulation = 0;
};

constexpr uint32_t kDefaultMaxBounces = 16;

// What the picture depends on, to see when it changes: the camera, the sun and sky, the local lights.
struct Watched {
    float camera[12] = {};
    float fov_rad = 0.0f;
    float light[16] = {}; // sun direction and colour, sky, ground, shadow colour, exposure
    uint64_t lights_hash = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// FNV-1a, over bytes with no padding in them.
uint64_t hash_bytes(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ bytes[i]) * 1099511628211ULL;
    }
    return hash;
}

constexpr uint64_t kHashStart = 14695981039346656037ULL;

uint64_t hash_lights(const world::RenderFrame& frame) {
    static_assert(sizeof(world::RenderLight) == 13 * sizeof(float), "hashed as bytes");
    return hash_bytes(kHashStart, frame.lights.data(), frame.lights.size() * sizeof(world::RenderLight));
}

// Where an instance is and what it is (never 0: that's a slot not seen).
uint64_t hash_instance(const world::RenderInstance& instance) {
    static_assert(sizeof(world::RenderTransform) == size_t{3} * 4 * sizeof(float), "hashed as bytes");
    static_assert(std::is_trivially_copyable_v<asset::AssetHandle>, "hashed as bytes");
    uint64_t hash = hash_bytes(kHashStart, &instance.transform, sizeof(instance.transform));
    hash = hash_bytes(hash, &instance.mesh, sizeof(instance.mesh));
    return hash != 0 ? hash : 1;
}

Watched watch(const PathTracerInput& input) {
    Watched now;
    std::copy_n(input.frame->camera.transform, 12, now.camera);
    now.fov_rad = input.frame->camera.vertical_fov_rad;
    const SceneLighting& l = *input.lighting;
    float* out = now.light;
    for (const float* source : {l.sun_direction, l.sun_color, l.sky_color, l.ground_color, l.shadow_color}) {
        out = std::copy_n(source, 3, out);
    }
    *out = l.exposure_ev;
    now.lights_hash = hash_lights(*input.frame);
    now.width = input.width;
    now.height = input.height;
    return now;
}

uint32_t groups(uint32_t size) {
    return (size + 7) / 8;
}

} // namespace

struct PathTracerState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    shader::ProgramHandle trace_program;
    shader::ProgramHandle show_program;
    shader::ProgramHandle compare_program;
    // The compare's sums, a buffer each frame in flight: the CPU reads one when its frame comes round again.
    rhi::BufferHandle compare_counts[rhi::kFramesInFlight] = {};
    LightingCompareStats compared;
    GraphTexture imported;           // the accumulation as this frame's graph has it (add_passes)
    rhi::TextureHandle accumulation; // at width x height
    uint32_t width = 0;
    uint32_t height = 0;
    bool written = false;    // the accumulation has been used: the graph is told it's in ShaderRead
    uint32_t samples = 0;    // added so far
    bool reset = true;       // the next frame starts again
    uint64_t last_frame = 0; // the frame it last added to: a gap means it wasn't watching
    Watched watched;
    // Per instance slot, what was last sent for it (hash_instance; 0: never), and how many a frame that held them all
    // had: a frame may send instances that didn't change (the editor does), which mustn't start it again.
    core::Array<uint64_t> slot_hashes{render_heap()};
    size_t all_count = 0;
    const char* reason = "asked";
    uint32_t restarts = 0;
    uint32_t max_bounces = kDefaultMaxBounces;
    // A save: asked for, then a readback copy of the mean the GPU writes, handed over after the frames in flight.
    bool save_asked = false;
    rhi::BufferHandle save_buffer;
    uint32_t save_frames_left = 0;
    uint32_t save_width = 0;
    uint32_t save_height = 0;
    uint32_t save_samples = 0;
};

core::Result<PathTracer> PathTracer::create(const PathTracerDesc& desc) {
    if (desc.device == nullptr || desc.shaders == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "the path tracer needs a device and a shader library");
    }
    auto* state = ::new (render_heap().allocate(sizeof(PathTracerState), alignof(PathTracerState))) PathTracerState();
    state->device = desc.device;
    state->shaders = desc.shaders;
    PathTracer tracer(state);
    const core::Result<shader::ProgramHandle> traced =
        desc.shaders->create_compute({.module = "scene.path_tracer", .debug_name = "path tracer"});
    if (!traced) {
        DE_LOG_WARN("render", "the path tracer's shader didn't build: no path tracing until it does");
    }
    state->trace_program = traced ? *traced : shader::ProgramHandle{};
    rhi::GraphicsPipelineDesc show{.color_count = 1, .cull = rhi::CullMode::None, .debug_name = "path tracer show"};
    show.color_formats[0] = desc.hdr_format;
    const core::Result<shader::ProgramHandle> shown =
        desc.shaders->create_graphics({.module = "scene.path_tracer_show", .state = show});
    state->show_program = shown ? *shown : shader::ProgramHandle{};
    const core::Result<shader::ProgramHandle> compared =
        desc.shaders->create_compute({.module = "scene.lighting_compare", .debug_name = "lighting compare"});
    state->compare_program = compared ? *compared : shader::ProgramHandle{};
    for (rhi::BufferHandle& buffer : state->compare_counts) {
        const core::Result<rhi::BufferHandle> made =
            desc.device->create_buffer({.size = kCompareWords * sizeof(uint32_t),
                                        .usage = rhi::BufferUsage::Storage,
                                        .memory = rhi::MemoryLocation::Upload,
                                        .debug_name = "lighting compare"});
        if (!made) {
            return core::fail(made.error());
        }
        buffer = *made;
        std::memset(desc.device->mapped(buffer).data(), 0, kCompareWords * sizeof(uint32_t));
        desc.device->flush_mapped(buffer);
    }
    return tracer;
}

PathTracer::~PathTracer() {
    if (state_ == nullptr) {
        return;
    }
    state_->shaders->destroy(state_->trace_program);
    state_->shaders->destroy(state_->show_program);
    state_->shaders->destroy(state_->compare_program);
    if (state_->save_buffer.is_valid()) {
        state_->device->destroy(state_->save_buffer);
    }
    for (const rhi::BufferHandle buffer : state_->compare_counts) {
        if (buffer.is_valid()) {
            state_->device->destroy(buffer);
        }
    }
    if (state_->accumulation.is_valid()) {
        state_->device->destroy(state_->accumulation);
    }
    state_->~PathTracerState();
    render_heap().deallocate(state_, sizeof(PathTracerState), alignof(PathTracerState));
}

PathTracer::PathTracer(PathTracer&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

PathTracer& PathTracer::operator=(PathTracer&& other) noexcept {
    if (this != &other) {
        std::swap(state_, other.state_);
    }
    return *this;
}

uint32_t PathTracer::samples() const {
    return state_->samples;
}

const char* PathTracer::restart_reason() const {
    return state_->reason;
}

uint32_t PathTracer::restarts() const {
    return state_->restarts;
}

void PathTracer::reset() {
    state_->reset = true;
    state_->reason = "asked";
}

uint32_t PathTracer::max_bounces() const {
    return state_->max_bounces;
}

void PathTracer::set_max_bounces(uint32_t bounces) {
    state_->max_bounces = std::clamp(bounces, 0U, 256U);
    state_->reset = true;
    state_->reason = "asked";
}

rhi::TextureHandle PathTracer::accumulation() const {
    return state_->accumulation;
}

void PathTracer::request_save() {
    state_->save_asked = true;
}

bool PathTracer::take_saved(core::Array<float>& rgba, uint32_t& width, uint32_t& height, uint32_t& samples) {
    PathTracerState& state = *state_;
    if (!state.save_buffer.is_valid()) {
        return false;
    }
    if (state.save_frames_left > 0) {
        --state.save_frames_left;
        return false;
    }
    const size_t floats = size_t{state.save_width} * state.save_height * 4;
    rgba.resize(floats);
    state.device->invalidate_mapped(state.save_buffer);
    std::memcpy(rgba.data(), state.device->mapped(state.save_buffer).data(), floats * sizeof(float));
    state.device->destroy(state.save_buffer);
    state.save_buffer = {};
    width = state.save_width;
    height = state.save_height;
    samples = state.save_samples;
    return true;
}

LightingCompareStats PathTracer::compare_stats() const {
    return state_->compared;
}

GraphTexture PathTracer::add_compare(RenderGraph& graph, const PathTracerCompareInput& input) {
    PathTracerState& state = *state_;
    const rhi::PipelineHandle compare = state.shaders->pipeline(state.compare_program);
    if (!compare.is_valid() || !state.accumulation.is_valid() || input.width != state.width ||
        input.height != state.height) {
        return {};
    }
    // This slot's sums from when its frame last ran (done by now), then none, for the GPU to add to.
    const rhi::BufferHandle counts = state.compare_counts[state.device->frame_index() % rhi::kFramesInFlight];
    uint32_t summed[kCompareWords] = {};
    state.device->invalidate_mapped(counts);
    std::memcpy(summed, state.device->mapped(counts).data(), sizeof(summed));
    if (summed[0] > 0) {
        const auto pixels = static_cast<float>(summed[0]);
        state.compared = {.pixels = summed[0],
                          .mean_stops = static_cast<float>(summed[1]) / 1024.0f / pixels,
                          .over_10 = static_cast<float>(summed[2]) / pixels,
                          .over_25 = static_cast<float>(summed[3]) / pixels,
                          .darker_10 = static_cast<float>(summed[4]) / pixels,
                          .brighter_10 = static_cast<float>(summed[5]) / pixels};
    }
    const uint32_t none[kCompareWords] = {};
    std::memcpy(state.device->mapped(counts).data(), none, sizeof(none));
    state.device->flush_mapped(counts);
    // The mean as the trace pass leaves it this frame (add_passes imported it).
    const GraphTexture traced = state.imported;
    if (!traced.is_valid()) {
        return {};
    }
    const GraphBuffer sums = graph.import_buffer({.buffer = counts, .name = "lighting compare sums"});
    struct ComparePass {
        GraphTexture heat;
    };
    const LightingComparePush push{.counts = state.device->buffer_address(counts), .size = {input.width, input.height}};
    const auto& compared = graph.add_pass<ComparePass>(
        "lighting compare",
        [&](PassBuilder& pass, ComparePass& data) {
            data.heat = pass.create_texture({.desc = {.width = input.width,
                                                      .height = input.height,
                                                      .format = rhi::Format::RGBA16Float,
                                                      .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled,
                                                      .debug_name = "lighting compare heat"}});
            pass.use(input.raster, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(input.depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(traced, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(sums, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(data.heat, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
        },
        [push, compare, raster = input.raster, depth = input.depth, traced](const ComparePass& data,
                                                                            PassContext& context) {
            DE_PROFILE_ZONE("lighting compare pass");
            LightingComparePush filled = push;
            filled.raster = context.device.texture_index(context.texture(raster));
            filled.traced = context.device.texture_index(context.texture(traced));
            filled.depth = context.device.texture_index(context.texture(depth));
            filled.heat = context.device.storage_texture_index(context.texture(data.heat));
            context.list.bind_pipeline(compare);
            context.list.push_constants(filled);
            context.list.dispatch(groups(filled.size[0]), groups(filled.size[1]));
        });
    return compared.heat;
}

bool PathTracer::add_passes(RenderGraph& graph, const PathTracerInput& input, bool show_it) {
    PathTracerState& state = *state_;
    state.imported = {};
    const rhi::PipelineHandle trace = state.shaders->pipeline(state.trace_program);
    const rhi::PipelineHandle show = state.shaders->pipeline(state.show_program);
    if (!trace.is_valid() || !show.is_valid() || input.frame == nullptr || input.lighting == nullptr ||
        !input.frame->camera.valid || input.width == 0 || input.height == 0) {
        return false;
    }
    if (input.width != state.width || input.height != state.height || !state.accumulation.is_valid()) {
        if (state.accumulation.is_valid()) {
            state.device->destroy(state.accumulation);
        }
        const core::Result<rhi::TextureHandle> made =
            state.device->create_texture({.width = input.width,
                                          .height = input.height,
                                          .format = rhi::Format::RGBA32Float,
                                          .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled |
                                                   rhi::TextureUsage::CopySrc, // tests read it
                                          .debug_name = "path tracer"});
        if (!made) {
            return false;
        }
        state.accumulation = *made;
        state.width = input.width;
        state.height = input.height;
        state.written = false;
        state.reset = true;
        state.reason = "size";
    }
    // Starts again when what it sees changed, or it wasn't watching last frame. Instances changed if one sent is
    // otherwise than its slot last had (one with no slot of its own has its place in the list), or one went.
    const Watched now = watch(input);
    bool moved = !input.frame->freed_slots.empty();
    for (size_t i = 0; i < input.frame->instances.size(); ++i) {
        const world::RenderInstance& instance = input.frame->instances[i];
        const size_t slot = instance.slot == world::kAutoRenderSlot ? i : instance.slot;
        if (slot >= state.slot_hashes.size()) {
            state.slot_hashes.resize(slot + 1, 0);
        }
        const uint64_t hash = hash_instance(instance);
        moved = moved || state.slot_hashes[slot] != hash;
        state.slot_hashes[slot] = hash;
    }
    if (input.frame->all_instances) {
        moved = moved || input.frame->instances.size() != state.all_count;
        state.all_count = input.frame->instances.size();
    }
    // The same frame drawn again (no new extraction since) isn't a gap.
    const uint64_t number = input.frame->number;
    const bool gap = number != state.last_frame + 1 && number != state.last_frame;
    const char* why = nullptr;
    if (!std::ranges::equal(now.camera, state.watched.camera) || now.fov_rad != state.watched.fov_rad) {
        why = "camera";
    } else if (!std::ranges::equal(now.light, state.watched.light)) {
        why = "sun and sky";
    } else if (now.lights_hash != state.watched.lights_hash) {
        why = "local lights";
    } else if (now.width != state.watched.width || now.height != state.watched.height) {
        why = "size";
    } else if (moved) {
        why = "instances";
    } else if (gap) {
        why = "a frame not drawn";
    }
    if (why != nullptr) {
        state.reset = true;
        state.reason = why;
    }
    state.watched = now;
    state.last_frame = input.frame->number;
    if (state.reset) {
        state.samples = 0;
        state.reset = false;
        ++state.restarts;
    }
    const bool adds = state.samples < kPathTracerMaxSamples;
    const PathTracerPush push{.frame = input.frame_address,
                              .sample = state.samples,
                              .size = {input.width, input.height},
                              .light_count = input.light_count,
                              .max_bounces = state.max_bounces};
    if (adds) {
        ++state.samples;
    }
    const GraphTexture accumulation =
        graph.import_texture({.texture = state.accumulation,
                              .name = "path tracer",
                              .state = state.written ? rhi::ResourceState::ShaderRead : rhi::ResourceState::Undefined,
                              .final_state = rhi::ResourceState::ShaderRead});
    state.written = true;
    state.imported = accumulation;
    if (adds) {
        graph.add_pass(
            "path tracer",
            [&](PassBuilder& pass) {
                pass.use(accumulation, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
                pass.never_cull();
            },
            [push, trace, accumulation](PassContext& context) {
                DE_PROFILE_ZONE("path tracer pass");
                PathTracerPush filled = push;
                filled.accumulation = context.device.storage_texture_index(context.texture(accumulation));
                context.list.bind_pipeline(trace);
                context.list.push_constants(filled);
                context.list.dispatch(groups(filled.size[0]), groups(filled.size[1]));
            });
    }
    // A save asked for: the mean as this frame leaves it, copied where the CPU can read it.
    if (state.save_asked && !state.save_buffer.is_valid()) {
        const core::Result<rhi::BufferHandle> made =
            state.device->create_buffer({.size = uint64_t{input.width} * input.height * 4 * sizeof(float),
                                         .usage = rhi::BufferUsage::CopyDst,
                                         .memory = rhi::MemoryLocation::Readback,
                                         .debug_name = "path tracer save"});
        if (made) {
            state.save_buffer = *made;
            state.save_frames_left = rhi::kFramesInFlight + 1;
            state.save_width = input.width;
            state.save_height = input.height;
            state.save_samples = state.samples;
            const GraphBuffer saved = graph.import_buffer({.buffer = *made, .name = "path tracer save"});
            graph.add_pass(
                "path tracer save",
                [&](PassBuilder& pass) {
                    pass.use(accumulation, rhi::ResourceState::CopySrc);
                    pass.use(saved, rhi::ResourceState::CopyDst);
                    pass.never_cull();
                },
                [accumulation, saved](PassContext& context) {
                    context.list.copy_texture_to_buffer(context.texture(accumulation), context.buffer(saved));
                });
        }
        state.save_asked = false;
    }
    if (!show_it) {
        return true;
    }
    graph.add_pass(
        "path tracer show",
        [&](PassBuilder& pass) {
            pass.use(accumulation, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            pass.use(input.hdr, rhi::ResourceState::RenderTarget);
        },
        [show, accumulation, hdr = input.hdr](PassContext& context) {
            DE_PROFILE_ZONE("path tracer show pass");
            const rhi::ColorAttachment color{.texture = context.texture(hdr), .load = rhi::LoadOp::DontCare};
            context.list.begin_rendering({.colors = std::span(&color, 1)});
            context.list.bind_pipeline(show);
            context.list.push_constants(
                PathTracerShowPush{.accumulation = context.device.texture_index(context.texture(accumulation))});
            context.list.draw(3);
            context.list.end_rendering();
        });
    return true;
}

} // namespace de::render
