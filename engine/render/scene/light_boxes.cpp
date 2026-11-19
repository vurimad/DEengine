#include "render/scene/light_boxes.h"

#include "core/allocator.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/shader/shader_library.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace de::render {

namespace {

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

// Matches LightBoxBakePush in shaders/scene/light_box_bake.slang.
struct LightBoxBakePush {
    uint64_t frame = 0;
    uint64_t sums = 0;
    float origin_voxel[4] = {};
    uint32_t size[4] = {};
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t rays = 0;
    uint32_t light_count = 0;
    uint32_t max_bounces = 0;
    uint32_t seed = 0;
    uint32_t maps[4] = {};
};
static_assert(sizeof(LightBoxBakePush) == 88);

// A dispatch gives each of its voxels this many rays; a box is baked once every voxel has had kLightBoxRays.
constexpr uint32_t kRaysPerPass = 32;
constexpr uint32_t kPassesPerVoxel = kLightBoxRays / kRaysPerPass;
// Bounces a bake's path takes at most: what's left past them is next to nothing, and roulette ends most before.
constexpr uint32_t kBakeBounces = 8;
// Per voxel: four float4 sums.
constexpr uint64_t kSumBytes = uint64_t{4} * 4 * sizeof(float);

struct Box {
    LightBoxDesc desc;
    float voxel_m = 0.0f;
    uint32_t size[3] = {};
    uint32_t voxels = 0;
    rhi::TextureHandle maps[4] = {};
    rhi::BufferHandle sums;
    uint32_t cursor = 0; // the next voxel to add rays to
    uint32_t passes = 0; // whole passes over its voxels done
    bool fresh = true;   // its sums are to be zeroed first
    bool used = false;   // its maps have been in a graph: they're in ShaderRead
};

} // namespace

struct LightBoxesState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    shader::ProgramHandle bake_program;
    Box boxes[kMaxLightBoxes];
    uint32_t count = 0;
    rhi::BufferHandle frame_buffers[rhi::kFramesInFlight] = {};
    float origins[kMaxLightBoxes][3] = {}; // this frame's, relative to the render origin
    bool ready = false;
    GraphTexture imported[kMaxLightBoxes * 4] = {};
    uint32_t seed = 0;

    void release(Box& box) {
        for (rhi::TextureHandle& map : box.maps) {
            if (map.is_valid()) {
                device->destroy(map);
            }
            map = {};
        }
        if (box.sums.is_valid()) {
            device->destroy(box.sums);
        }
        box = {};
    }
};

core::Result<LightBoxes> LightBoxes::create(const LightBoxesDesc& desc) {
    if (desc.device == nullptr || desc.shaders == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "light boxes need a device and a shader library");
    }
    auto* state = ::new (render_heap().allocate(sizeof(LightBoxesState), alignof(LightBoxesState))) LightBoxesState();
    state->device = desc.device;
    state->shaders = desc.shaders;
    LightBoxes boxes(state);
    const core::Result<shader::ProgramHandle> bake =
        desc.shaders->create_compute({.module = "scene.light_box_bake", .debug_name = "light box bake"});
    if (!bake) {
        DE_LOG_WARN("render", "the light box bake's shader didn't build: no light boxes until it does");
    }
    state->bake_program = bake ? *bake : shader::ProgramHandle{};
    for (rhi::BufferHandle& buffer : state->frame_buffers) {
        const core::Result<rhi::BufferHandle> made =
            desc.device->create_buffer({.size = kMaxLightBoxes * sizeof(GpuLightBox),
                                        .usage = rhi::BufferUsage::Storage,
                                        .memory = rhi::MemoryLocation::Upload,
                                        .debug_name = "light boxes"});
        if (!made) {
            return core::fail(made.error());
        }
        buffer = *made;
    }
    return boxes;
}

LightBoxes::~LightBoxes() {
    if (state_ == nullptr) {
        return;
    }
    for (Box& box : state_->boxes) {
        state_->release(box);
    }
    for (const rhi::BufferHandle buffer : state_->frame_buffers) {
        if (buffer.is_valid()) {
            state_->device->destroy(buffer);
        }
    }
    state_->shaders->destroy(state_->bake_program);
    state_->~LightBoxesState();
    render_heap().deallocate(state_, sizeof(LightBoxesState), alignof(LightBoxesState));
}

LightBoxes::LightBoxes(LightBoxes&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

LightBoxes& LightBoxes::operator=(LightBoxes&& other) noexcept {
    if (this != &other) {
        std::swap(state_, other.state_);
    }
    return *this;
}

void LightBoxes::set_boxes(std::span<const LightBoxDesc> boxes) {
    LightBoxesState& state = *state_;
    for (Box& box : state.boxes) {
        state.release(box);
    }
    state.count = 0;
    for (const LightBoxDesc& desc : boxes.first(std::min<size_t>(boxes.size(), kMaxLightBoxes))) {
        Box& box = state.boxes[state.count];
        box.desc = desc;
        const DVec3 extent = glm::max(desc.max_m - desc.min_m, DVec3(1e-3));
        // As fine as asked, and no finer than the most voxels a box has each way.
        const double widest = std::max({extent.x, extent.y, extent.z});
        box.voxel_m = static_cast<float>(
            std::max(static_cast<double>(std::max(desc.voxel_m, 0.01f)), widest / kMaxLightBoxVoxels));
        for (int axis = 0; axis < 3; ++axis) {
            box.size[axis] =
                std::clamp(static_cast<uint32_t>(std::ceil(extent[axis] / box.voxel_m)), 2U, kMaxLightBoxVoxels);
        }
        box.voxels = box.size[0] * box.size[1] * box.size[2];
        bool made = true;
        static constexpr const char* kNames[4] = {"light box a", "light box b x", "light box b y", "light box b z"};
        for (uint32_t m = 0; m < 4 && made; ++m) {
            const core::Result<rhi::TextureHandle> map =
                state.device->create_texture({.width = box.size[0],
                                              .height = box.size[1],
                                              .depth = box.size[2],
                                              .format = rhi::Format::RGBA16Float,
                                              .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled,
                                              .dimension = rhi::TextureDimension::Texture3D,
                                              .debug_name = kNames[m]});
            made = map.ok();
            box.maps[m] = made ? *map : rhi::TextureHandle{};
        }
        const core::Result<rhi::BufferHandle> sums =
            state.device->create_buffer({.size = uint64_t{box.voxels} * kSumBytes,
                                         .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                         .debug_name = "light box sums"});
        made = made && sums.ok();
        if (!made) {
            DE_LOG_WARN("render", "a light box of %u x %u x %u voxels couldn't be made", box.size[0], box.size[1],
                        box.size[2]);
            state.release(box);
            continue;
        }
        box.sums = *sums;
        ++state.count;
    }
}

void LightBoxes::rebake() {
    for (uint32_t b = 0; b < state_->count; ++b) {
        Box& box = state_->boxes[b];
        box.cursor = 0;
        box.passes = 0;
        box.fresh = true;
    }
}

LightBoxes::Frame LightBoxes::update(const DVec3& render_origin, bool ready) {
    LightBoxesState& state = *state_;
    state.ready = ready;
    if (state.count == 0) {
        return {};
    }
    const rhi::BufferHandle buffer = state.frame_buffers[state.device->frame_index() % rhi::kFramesInFlight];
    const std::span<std::byte> mapped = state.device->mapped(buffer);
    for (uint32_t b = 0; b < state.count; ++b) {
        const Box& box = state.boxes[b];
        const DVec3 origin = box.desc.min_m - render_origin;
        for (int axis = 0; axis < 3; ++axis) {
            state.origins[b][axis] = static_cast<float>(origin[axis]);
        }
        GpuLightBox gpu{.origin_voxel = {state.origins[b][0], state.origins[b][1], state.origins[b][2], box.voxel_m},
                        .size = {box.size[0], box.size[1], box.size[2], box.voxels},
                        .params = {std::max(box.desc.fade_m, 1e-3f), 0.0f, 0.0f, 0.0f}};
        for (uint32_t m = 0; m < 4; ++m) {
            gpu.maps[m] = state.device->texture_index(box.maps[m]);
        }
        std::memcpy(mapped.data() + (size_t{b} * sizeof(GpuLightBox)), &gpu, sizeof(gpu));
    }
    state.device->flush_mapped(buffer, 0, uint64_t{state.count} * sizeof(GpuLightBox));
    return {.boxes = state.device->buffer_address(buffer), .count = state.count};
}

std::span<const GraphTexture> LightBoxes::add_passes(RenderGraph& graph, uint64_t frame_address, uint32_t light_count) {
    LightBoxesState& state = *state_;
    if (state.count == 0) {
        return {};
    }
    // The maps stay between frames: the graph is told how the last one left them.
    for (uint32_t b = 0; b < state.count; ++b) {
        Box& box = state.boxes[b];
        for (uint32_t m = 0; m < 4; ++m) {
            state.imported[(b * 4) + m] = graph.import_texture(
                {.texture = box.maps[m],
                 .name = "light box",
                 .state = box.used ? rhi::ResourceState::ShaderRead : rhi::ResourceState::Undefined,
                 .final_state = rhi::ResourceState::ShaderRead});
        }
        box.used = true;
    }
    const std::span<const GraphTexture> maps(state.imported, size_t{state.count} * 4);
    const rhi::PipelineHandle bake = state.shaders->pipeline(state.bake_program);
    if (!state.ready || !bake.is_valid()) {
        return maps;
    }
    // This frame's share: the boxes in order, each some voxels further, until the frame's paths are spent.
    struct Dispatch {
        uint32_t box = 0;
        uint32_t first = 0;
        uint32_t count = 0;
        bool zero = false;
    };
    Dispatch dispatches[kMaxLightBoxes * 2];
    uint32_t dispatch_count = 0;
    uint32_t paths_left = kLightBoxPathsPerFrame;
    for (uint32_t b = 0; b < state.count && paths_left >= kRaysPerPass; ++b) {
        Box& box = state.boxes[b];
        bool zero = box.fresh;
        box.fresh = false;
        while (box.passes < kPassesPerVoxel && paths_left >= kRaysPerPass && dispatch_count < std::size(dispatches)) {
            const uint32_t count = std::min(box.voxels - box.cursor, paths_left / kRaysPerPass);
            dispatches[dispatch_count++] = {.box = b, .first = box.cursor, .count = count, .zero = zero};
            zero = false;
            paths_left -= count * kRaysPerPass;
            box.cursor += count;
            if (box.cursor == box.voxels) {
                box.cursor = 0;
                ++box.passes;
            }
        }
    }
    if (dispatch_count == 0) {
        return maps;
    }
    GraphBuffer sums[kMaxLightBoxes] = {};
    for (uint32_t b = 0; b < state.count; ++b) {
        sums[b] = graph.import_buffer({.buffer = state.boxes[b].sums, .name = "light box sums"});
    }
    struct Pass {
        LightBoxesState* state = nullptr;
        rhi::PipelineHandle bake;
        uint64_t frame_address = 0;
        uint32_t light_count = 0;
        uint32_t seed = 0;
        Dispatch dispatches[kMaxLightBoxes * 2];
        uint32_t dispatch_count = 0;
        GraphBuffer sums[kMaxLightBoxes];
        GraphTexture maps[kMaxLightBoxes * 4];
    };
    ++state.seed;
    graph.add_pass<Pass>(
        "light box bake",
        [&](PassBuilder& pass, Pass& data) {
            data.state = &state;
            data.bake = bake;
            data.frame_address = frame_address;
            data.light_count = light_count;
            data.seed = state.seed;
            std::copy_n(dispatches, dispatch_count, data.dispatches);
            data.dispatch_count = dispatch_count;
            for (uint32_t b = 0; b < state.count; ++b) {
                data.sums[b] = sums[b];
                pass.use(sums[b], rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            }
            for (uint32_t m = 0; m < state.count * 4; ++m) {
                data.maps[m] = maps[m];
                pass.use(maps[m], rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            }
            pass.never_cull();
        },
        [](const Pass& data, PassContext& context) {
            DE_PROFILE_ZONE("light box bake pass");
            const LightBoxesState& state = *data.state;
            context.list.bind_pipeline(data.bake);
            for (uint32_t d = 0; d < data.dispatch_count; ++d) {
                const Dispatch& dispatch = data.dispatches[d];
                const Box& box = state.boxes[dispatch.box];
                const rhi::BufferHandle sums = context.buffer(data.sums[dispatch.box]);
                if (dispatch.zero) {
                    context.list.fill_buffer(sums, 0, uint64_t{box.voxels} * kSumBytes, 0);
                    context.list.barrier(rhi::GlobalBarrier{.before = rhi::ResourceState::CopyDst,
                                                            .after = rhi::ResourceState::ShaderWrite,
                                                            .after_stages = rhi::ShaderStages::Compute});
                }
                LightBoxBakePush push{.frame = data.frame_address,
                                      .sums = context.device.buffer_address(sums),
                                      .origin_voxel = {state.origins[dispatch.box][0], state.origins[dispatch.box][1],
                                                       state.origins[dispatch.box][2], box.voxel_m},
                                      .size = {box.size[0], box.size[1], box.size[2], box.voxels},
                                      .first = dispatch.first,
                                      .count = dispatch.count,
                                      .rays = kRaysPerPass,
                                      .light_count = data.light_count,
                                      .max_bounces = kBakeBounces,
                                      .seed = (data.seed * 0x9e3779b9U) + d};
                for (uint32_t m = 0; m < 4; ++m) {
                    push.maps[m] =
                        context.device.storage_texture_index(context.texture(data.maps[(dispatch.box * 4) + m]));
                }
                context.list.push_constants(push);
                context.list.dispatch((dispatch.count + 63) / 64);
                // The next dispatch may add to the same voxels' sums.
                context.list.barrier(rhi::GlobalBarrier{.before = rhi::ResourceState::ShaderWrite,
                                                        .after = rhi::ResourceState::ShaderWrite,
                                                        .before_stages = rhi::ShaderStages::Compute,
                                                        .after_stages = rhi::ShaderStages::Compute});
            }
        });
    return maps;
}

LightBoxStats LightBoxes::stats() const {
    const LightBoxesState& state = *state_;
    LightBoxStats stats{.boxes = state.count};
    uint64_t taken = 0;
    uint64_t all = 0;
    for (uint32_t b = 0; b < state.count; ++b) {
        const Box& box = state.boxes[b];
        stats.voxels += box.voxels;
        all += uint64_t{box.voxels} * kPassesPerVoxel;
        taken += (uint64_t{box.passes} * box.voxels) + (box.passes < kPassesPerVoxel ? box.cursor : 0);
    }
    stats.baked = all > 0 ? static_cast<float>(static_cast<double>(taken) / static_cast<double>(all)) : 0.0f;
    return stats;
}

} // namespace de::render
