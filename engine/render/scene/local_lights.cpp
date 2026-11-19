#include "render/scene/local_lights.h"

#include "core/allocator.h"
#include "core/log.h"
#include "core/profile.h"
#include "platform/time.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/scene/particles.h"
#include "render/scene/scene_renderer.h"
#include "render/shader/shader_library.h"
#include "world/render_extract.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace de::render {

namespace {

// A light's shadow from one frame to the next.
struct ShadowHistory {
    DVec3 position{0.0};    // in the world: the render origin moves
    uint32_t size = 0;      // the size it's to have, in texels
    float pending_s = 0.0f; // how long another size has been wanted
    float strength = 0.0f;  // 0 to 1: how much of its shadow shows
    // The tiles it holds in the atlas, kept from frame to frame: their size (0: none), how many, and where.
    uint32_t tiles = 0;
    uint32_t faces = 0;
    uint16_t tile_x[6] = {}; // in cells
    uint16_t tile_y[6] = {};
    // What its faces were drawn with: turned, or reaching farther, or a wider cone, what its tiles hold is no longer
    // what it sees.
    float shape[5] = {}; // its direction, the cosine of its cone, its radius
};

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

constexpr uint32_t kMaxWords = kMaxFrameLights / kLightWordBits;
static_assert(kMaxFrameLights % kLightWordBits == 0);
constexpr uint64_t kLightBytes = uint64_t{kMaxFrameLights} * sizeof(GpuLocalLight);
constexpr uint64_t kClusterBytes = uint64_t{kLightClusters} * kMaxWords * sizeof(uint32_t);
// A light fades out over this share of its draw distance, the last of it.
constexpr float kFadeShare = 0.2f;

// How many slices a doubling of depth crosses.
float slices_per_doubling() {
    return static_cast<float>(kLightGridZ) / std::log2(kLightSliceFarM / kLightSliceNearM);
}

// Matches LightCullPush in shaders/scene/light_cull.slang.
struct LightCullPush {
    uint64_t frame = 0;
    uint32_t light_count = 0;
    uint32_t cluster_count = 0;
    float camera_right[4] = {}; // at 16: a block's float4 sits on a multiple of 16
    float camera_up[4] = {};
    float slice_near_m = 0.0f;
    float slices_per_doubling = 0.0f;
    uint32_t reserved[2] = {};
};
static_assert(sizeof(LightCullPush) == 64);

} // namespace

uint32_t light_slice(float depth_m) {
    const float slice = std::log2(std::max(depth_m, 1e-3f) / kLightSliceNearM) * slices_per_doubling();
    return static_cast<uint32_t>(std::clamp(slice, 0.0f, static_cast<float>(kLightGridZ - 1)));
}

struct LocalLightsState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    shader::ProgramHandle cull_program;
    rhi::BufferHandle light_buffers[rhi::kFramesInFlight]; // each frame's lights, written by the CPU
    rhi::BufferHandle clusters;                            // the words, written by the cull each frame
    bool clusters_used = false;                            // by some frame: the graph is told how that left it
    uint32_t lit_now = 0;                                  // lights the last update() kept
    LocalLightStats stats;
    // What each light's shadow was last frame (where it stood: lights have no other name here), so its tiles keep
    // their size and its shadow fades rather than coming and going.
    ShadowHistory history[kMaxFrameLights] = {};
    uint32_t history_count = 0;
    bool cells[kLocalShadowCellsY][kLocalShadowCellsX] = {}; // of the atlas: taken by a light's tile
    uint32_t most_shadowed = kMaxFrameLights;                // set_most_shadowed
    uint64_t clock_ns = 0;                                   // when the last update() ran
};

core::Result<LocalLights> LocalLights::create(const LocalLightsDesc& desc) {
    if (desc.device == nullptr || desc.shaders == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "local lights need a device and a shader library");
    }
    auto* state =
        ::new (render_heap().allocate(sizeof(LocalLightsState), alignof(LocalLightsState))) LocalLightsState();
    state->device = desc.device;
    state->shaders = desc.shaders;
    LocalLights lights(state); // lets go of what was made if the rest fails
    for (rhi::BufferHandle& buffer : state->light_buffers) {
        const core::Result<rhi::BufferHandle> made = desc.device->create_buffer({.size = kLightBytes,
                                                                                 .usage = rhi::BufferUsage::Storage,
                                                                                 .memory = rhi::MemoryLocation::Upload,
                                                                                 .debug_name = "local lights"});
        if (!made) {
            return core::fail(made.error());
        }
        buffer = *made;
    }
    const core::Result<rhi::BufferHandle> clusters = desc.device->create_buffer(
        {.size = kClusterBytes, .usage = rhi::BufferUsage::Storage, .debug_name = "light clusters"});
    if (!clusters) {
        return core::fail(clusters.error());
    }
    state->clusters = *clusters;
    const core::Result<shader::ProgramHandle> cull =
        desc.shaders->create_compute({.module = "scene.light_cull", .debug_name = "light cull"});
    state->cull_program = cull ? *cull : shader::ProgramHandle{};
    if (!cull) {
        DE_LOG_WARN("render", "the light cull's shader didn't build: no local lights until it does");
    }
    return lights;
}

LocalLights::~LocalLights() {
    if (state_ == nullptr) {
        return;
    }
    state_->shaders->destroy(state_->cull_program);
    state_->device->destroy(state_->clusters);
    for (const rhi::BufferHandle buffer : state_->light_buffers) {
        state_->device->destroy(buffer);
    }
    state_->~LocalLightsState();
    render_heap().deallocate(state_, sizeof(LocalLightsState), alignof(LocalLightsState));
}

LocalLights::LocalLights(LocalLights&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

LocalLights& LocalLights::operator=(LocalLights&& other) noexcept {
    if (this != &other) {
        std::swap(state_, other.state_);
    }
    return *this;
}

void LocalLights::set_most_shadowed(uint32_t lights) {
    state_->most_shadowed = lights;
}

LocalLightStats LocalLights::stats() const {
    return state_->stats;
}

rhi::BufferHandle LocalLights::clusters() const {
    return state_->clusters;
}

namespace {

// A face's field of view: a cube's, 90 degrees and a few texels more (a look finds its own face: shaders/scene/
// local_lights.slang); a spot's cone, as wide (at most 150 degrees). As the tangent of half of it, for a tile of size
// texels.
float shadow_face_tan(const world::RenderLight& light, uint32_t size) {
    const float margin = 1.0f + (4.0f / static_cast<float>(size));
    if (light.cos_outer <= -1.0f) {
        return margin;
    }
    const float half = std::acos(std::clamp(light.cos_outer, -1.0f, 1.0f));
    return std::tan(std::min(half, glm::radians(75.0f))) * margin;
}

// A light's faces: a camera at the light for each, looking along its axis (a point's six, +x -x +y -y +z -z; a spot's
// one, along its direction), with the engine's camera projection (reverse Z, no far plane).
void add_shadow_faces(const world::RenderLight& light, uint32_t size, LocalShadowFace* faces) {
    const bool spot = light.cos_outer > -1.0f;
    const Vec3 ways[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const uint32_t count = spot ? 1 : 6;
    const float fov_rad = 2.0f * std::atan(shadow_face_tan(light, size));
    for (uint32_t f = 0; f < count; ++f) {
        const Vec3 forward =
            spot ? glm::normalize(Vec3(light.direction[0], light.direction[1], light.direction[2])) : ways[f];
        const Vec3 helper = std::abs(forward.y) > 0.9f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
        const Vec3 back = -forward;
        const Vec3 right = glm::normalize(glm::cross(helper, back));
        const Vec3 up = glm::cross(back, right);
        // Columns: right, up, back, where (a camera's 3x4, as RenderView has it).
        const float camera[12] = {right.x, up.x, back.x, light.position[0], right.y, up.y, back.y, light.position[1],
                                  right.z, up.z, back.z, light.position[2]};
        LocalShadowFace& face = faces[f];
        camera_view_proj(camera, fov_rad, kLocalShadowNearM, 1.0f, face.view_proj);
        std::copy_n(light.position, 3, face.position);
        face.forward[0] = forward.x;
        face.forward[1] = forward.y;
        face.forward[2] = forward.z;
        face.far_m = light.radius_m;
        face.tile[2] = size;
    }
}

// Cells of the atlas, taken or not (LocalLightsState::cells). The first place a tile of size cells fits, on a multiple
// of its size, is taken; false if none.
using AtlasCells = bool[kLocalShadowCellsY][kLocalShadowCellsX];
bool take_cells(AtlasCells& taken, uint32_t size, uint32_t& x, uint32_t& y) {
    for (y = 0; y + size <= kLocalShadowCellsY; y += size) {
        for (x = 0; x + size <= kLocalShadowCellsX; x += size) {
            bool free = true;
            for (uint32_t dy = 0; dy < size && free; ++dy) {
                for (uint32_t dx = 0; dx < size && free; ++dx) {
                    free = !taken[y + dy][x + dx];
                }
            }
            if (free) {
                for (uint32_t dy = 0; dy < size; ++dy) {
                    for (uint32_t dx = 0; dx < size; ++dx) {
                        taken[y + dy][x + dx] = true;
                    }
                }
                return true;
            }
        }
    }
    return false;
}

void give_cells(AtlasCells& taken, uint32_t size, uint32_t x, uint32_t y) {
    for (uint32_t dy = 0; dy < size; ++dy) {
        for (uint32_t dx = 0; dx < size; ++dx) {
            taken[y + dy][x + dx] = false;
        }
    }
}

} // namespace

LocalLightFrame LocalLights::update(const world::RenderFrame& frame, float aspect,
                                    std::span<const LocalShadowChange> changed, bool all_changed) {
    DE_PROFILE_ZONE("LocalLights::update");
    LocalLightsState& state = *state_;
    state.lit_now = 0;
    state.stats = {.lights = static_cast<uint32_t>(frame.lights.size())};
    const rhi::BufferHandle buffer = state.light_buffers[state.device->frame_index() % rhi::kFramesInFlight];
    if (frame.lights.empty() || !frame.camera.valid || !state.shaders->pipeline(state.cull_program).is_valid()) {
        return {};
    }
    // Those whose light reaches something in view, from near enough; the nearest for their size when there are more
    // than a frame lights with (a heap of the kept, its worst on top).
    struct Kept {
        uint32_t index = 0;
        float rank = 0.0f; // how many of its radius away it is: the lower, the more it matters
        float fade = 1.0f;
    };
    const auto worse = [](const Kept& a, const Kept& b) { return a.rank < b.rank; };
    Kept kept[kMaxFrameLights];
    uint32_t count = 0;
    const ParticleView view =
        ParticleView::from_camera(frame.camera.transform, frame.camera.vertical_fov_rad, frame.camera.near_m, aspect);
    for (uint32_t i = 0; i < frame.lights.size(); ++i) {
        const world::RenderLight& light = frame.lights[i];
        const Vec3 center(light.position[0], light.position[1], light.position[2]);
        if (light.radius_m <= 0.0f || !view.sees(center, light.radius_m)) {
            continue;
        }
        const float rank = glm::length(center - view.position) / light.radius_m;
        const float fade =
            std::clamp((kLightDrawDistancePerRadius - rank) / (kLightDrawDistancePerRadius * kFadeShare), 0.0f, 1.0f);
        if (fade <= 0.0f || (light.color[0] + light.color[1] + light.color[2]) <= 0.0f) {
            continue;
        }
        const Kept candidate{.index = i, .rank = rank, .fade = fade};
        if (count < kMaxFrameLights) {
            kept[count++] = candidate;
            if (count == kMaxFrameLights) {
                std::make_heap(kept, kept + kMaxFrameLights, worse);
            }
        } else {
            ++state.stats.dropped;
            if (worse(candidate, kept[0])) {
                std::pop_heap(kept, kept + kMaxFrameLights, worse);
                kept[kMaxFrameLights - 1] = candidate;
                std::push_heap(kept, kept + kMaxFrameLights, worse);
            }
        }
    }
    state.stats.lit = count;
    if (count == 0) {
        return {};
    }
    // Shadows (docs/design.md, Local lights: shadows; the way Godot sizes its shadow atlas): each light's tiles by how
    // much of the picture its reach covers, a power of 2, changed only once another size has been wanted for a while;
    // the most covering first (one that had shadows as if it covered more), smaller while the atlas is short, while
    // there are faces and cells left; then packed, largest first. A shadow fades in when a light gets tiles and out as
    // it grows too small on the screen, never on or off at once.
    const uint64_t now_ns = platform::time_ns();
    const float step_s = state.clock_ns != 0
                             ? std::min(static_cast<float>(static_cast<double>(now_ns - state.clock_ns) * 1e-9), 0.25f)
                             : 0.0f;
    state.clock_ns = now_ns;
    const float tan_y = std::tan(frame.camera.vertical_fov_rad * 0.5f);
    ShadowHistory now[kMaxFrameLights];
    bool matched[kMaxFrameLights] = {};  // of the history: still lit this frame
    bool reshaped[kMaxFrameLights] = {}; // it's turned or changed its reach or its cone since its tiles were drawn
    float priority[kMaxFrameLights];
    float fade_target[kMaxFrameLights];
    for (uint32_t k = 0; k < count; ++k) {
        const world::RenderLight& light = frame.lights[kept[k].index];
        const Vec3 at(light.position[0], light.position[1], light.position[2]);
        const DVec3 at_world = frame.origin + DVec3(at);
        const ShadowHistory* before = nullptr;
        for (uint32_t b = 0; b < state.history_count && before == nullptr; ++b) {
            if (!matched[b] && glm::length(state.history[b].position - at_world) < 0.01) {
                before = &state.history[b];
                matched[b] = true;
            }
        }
        // Its reach's height on the picture, over the picture's (at least as much as from just outside it).
        const float distance_m = glm::length(at - view.position);
        const float coverage = light.radius_m / (std::max(distance_m, light.radius_m * 0.5f) * tan_y);
        const float texels = coverage * kLocalShadowTexelsPerCoverage;
        const uint32_t largest = light.cos_outer > -1.0f ? kLocalShadowLargest : kLocalShadowLargestCube;
        uint32_t wanted = kLocalShadowCell;
        while (wanted < largest && static_cast<float>(wanted) < texels) {
            wanted *= 2;
        }
        ShadowHistory& kept_history = now[k];
        kept_history.position = at_world;
        const float shape[5] = {light.direction[0], light.direction[1], light.direction[2], light.cos_outer,
                                light.radius_m};
        std::copy_n(shape, 5, kept_history.shape);
        reshaped[k] = before == nullptr || !std::equal(shape, shape + 5, before->shape);
        if (before != nullptr) {
            kept_history.tiles = before->tiles;
            kept_history.faces = before->faces;
            std::copy_n(before->tile_x, 6, kept_history.tile_x);
            std::copy_n(before->tile_y, 6, kept_history.tile_y);
        }
        kept_history.size = before != nullptr && before->size != 0 ? std::min(before->size, largest) : wanted;
        // One that wasn't lit last frame (it came into view, or into being) has its shadow as it should be at once: a
        // shadow fading in at the picture's edge each time the camera turns is a pop of its own.
        kept_history.strength = before != nullptr ? before->strength : -1.0f;
        kept_history.pending_s = 0.0f;
        if (wanted != kept_history.size) {
            kept_history.pending_s = (before != nullptr ? before->pending_s : 0.0f) + step_s;
            if (kept_history.pending_s >= kLocalShadowResizeAfterS) {
                kept_history.size = wanted;
                kept_history.pending_s = 0.0f;
            }
        }
        // Too small on the screen for a shadow to show: it fades out over a band of coverage. As much of one as the
        // light casts at all: none, and it wants no tiles.
        fade_target[k] = std::clamp((texels - kLocalShadowFadeTexels) / kLocalShadowFadeTexels, 0.0f, 1.0f) *
                         std::clamp(light.shadow, 0.0f, 1.0f);
        priority[k] = texels * (kept_history.strength != 0.0f ? 1.5f : 1.0f);
    }
    LocalLightFrame result;
    uint32_t order[kMaxFrameLights];
    for (uint32_t k = 0; k < count; ++k) {
        order[k] = k;
    }
    std::sort(order, order + count, [&](uint32_t a, uint32_t b) { return priority[a] > priority[b]; });
    struct Pick {
        uint32_t k = 0;
        uint32_t size = 0; // texels
    };
    Pick picks[kLocalShadowFaces];
    uint32_t pick_count = 0;
    uint32_t faces_left = kLocalShadowFaces;
    uint32_t cells_left = kLocalShadowCellsX * kLocalShadowCellsY;
    bool tiled[kMaxFrameLights] = {};
    const auto face_count = [&](uint32_t k) { return frame.lights[kept[k].index].cos_outer > -1.0f ? 1U : 6U; };
    const auto cells = [](uint32_t faces, uint32_t texels) {
        const uint32_t side = texels / kLocalShadowCell;
        return faces * side * side;
    };
    // First every light that's to have a shadow gets the smallest tiles, the most covering first, while there's room:
    // no light goes without because a nearer one took the atlas. Then, in the same order, each grows to the size it
    // wants, or as near it as the room left allows.
    for (uint32_t o = 0; o < count; ++o) {
        const uint32_t k = order[o];
        const uint32_t faces = face_count(k);
        // One whose shadow has faded out and isn't to come back needs no tiles.
        if (pick_count >= state.most_shadowed || faces > faces_left || cells(faces, kLocalShadowCell) > cells_left ||
            (fade_target[k] <= 0.0f && now[k].strength <= 0.0f)) {
            continue;
        }
        cells_left -= cells(faces, kLocalShadowCell);
        faces_left -= faces;
        picks[pick_count++] = {.k = k, .size = kLocalShadowCell};
        tiled[k] = true;
    }
    for (uint32_t p = 0; p < pick_count; ++p) {
        const uint32_t faces = face_count(picks[p].k);
        uint32_t size = now[picks[p].k].size;
        while (size > kLocalShadowCell && cells(faces, size) - cells(faces, kLocalShadowCell) > cells_left) {
            size /= 2;
        }
        cells_left -= cells(faces, size) - cells(faces, kLocalShadowCell);
        picks[p].size = size;
    }
    // Each shadow toward its strength: up while it has tiles and shows, down as it grows small; none without tiles.
    for (uint32_t k = 0; k < count; ++k) {
        const float target = tiled[k] ? fade_target[k] : 0.0f;
        float& strength = now[k].strength;
        if (strength < 0.0f) {
            strength = target; // new this frame
        }
        strength = !tiled[k]           ? 0.0f
                   : strength < target ? std::min(strength + (step_s * kLocalShadowFadePerS), target)
                                       : std::max(strength - (step_s * kLocalShadowFadePerS), target);
        if (tiled[k] && strength <= 0.0f && target > 0.0f && step_s <= 0.0f) {
            strength = target; // the first frame: as it is, not faded in
        }
    }
    // The tiles. A light keeps the tiles it has while it's lit and its size holds (what they hold is kept too: `draw`
    // below); one that's gone, or has lost its shadow, or changed size, gives its back; then those without take theirs,
    // the largest first. If the free cells are too scattered for one, everything is laid out again.
    uint32_t pick_size[kMaxFrameLights] = {};
    for (uint32_t p = 0; p < pick_count; ++p) {
        pick_size[picks[p].k] = picks[p].size;
    }
    const auto give = [&](ShadowHistory& held) {
        for (uint32_t f = 0; held.tiles != 0 && f < held.faces; ++f) {
            give_cells(state.cells, held.tiles / kLocalShadowCell, held.tile_x[f], held.tile_y[f]);
        }
        held.tiles = 0;
    };
    for (uint32_t b = 0; b < state.history_count; ++b) {
        if (!matched[b]) {
            give(state.history[b]);
        }
    }
    for (uint32_t k = 0; k < count; ++k) {
        if (now[k].tiles != pick_size[k]) {
            give(now[k]);
        }
    }
    std::stable_sort(picks, picks + pick_count, [](const Pick& a, const Pick& b) { return a.size > b.size; });
    bool moved[kMaxFrameLights] = {}; // its tiles are new this frame: nothing is in them yet
    const auto place = [&](uint32_t k, uint32_t size) {
        ShadowHistory& held = now[k];
        held.faces = face_count(k);
        for (uint32_t f = 0; f < held.faces; ++f) {
            uint32_t x = 0;
            uint32_t y = 0;
            if (!take_cells(state.cells, size / kLocalShadowCell, x, y)) {
                for (uint32_t back = 0; back < f; ++back) {
                    give_cells(state.cells, size / kLocalShadowCell, held.tile_x[back], held.tile_y[back]);
                }
                return false;
            }
            held.tile_x[f] = static_cast<uint16_t>(x);
            held.tile_y[f] = static_cast<uint16_t>(y);
        }
        held.tiles = size;
        moved[k] = true;
        return true;
    };
    bool laid_out = true;
    for (uint32_t p = 0; p < pick_count && laid_out; ++p) {
        if (now[picks[p].k].tiles == 0) {
            laid_out = place(picks[p].k, picks[p].size);
        }
    }
    if (!laid_out) {
        std::memset(state.cells, 0, sizeof(state.cells));
        for (uint32_t k = 0; k < count; ++k) {
            now[k].tiles = 0;
        }
        for (uint32_t p = 0; p < pick_count; ++p) {
            place(picks[p].k, picks[p].size); // the cells were counted, and largest first never splits them
        }
    }
    int32_t first_face[kMaxFrameLights];
    std::fill_n(first_face, count, -1);
    for (uint32_t p = 0; p < pick_count; ++p) {
        const uint32_t k = picks[p].k;
        const world::RenderLight& light = frame.lights[kept[k].index];
        const ShadowHistory& held = now[k];
        if (held.tiles == 0) {
            continue;
        }
        // Drawn again if its tiles are new, or something that casts changed within its reach.
        const Vec3 at(light.position[0], light.position[1], light.position[2]);
        bool draw = all_changed || moved[k] || reshaped[k];
        for (size_t c = 0; c < changed.size() && !draw; ++c) {
            const Vec3 where(changed[c].center[0], changed[c].center[1], changed[c].center[2]);
            draw = glm::length(where - at) < light.radius_m + changed[c].radius_m;
        }
        LocalShadowFace* added = result.faces + result.face_count;
        add_shadow_faces(light, held.tiles, added);
        for (uint32_t f = 0; f < held.faces; ++f) {
            added[f].tile[0] = held.tile_x[f] * kLocalShadowCell;
            added[f].tile[1] = held.tile_y[f] * kLocalShadowCell;
            added[f].draw = draw;
        }
        first_face[k] = static_cast<int32_t>(result.face_count);
        result.face_count += held.faces;
        state.stats.shadow_faces_drawn += draw ? held.faces : 0;
        ++state.stats.shadowed;
    }
    std::copy_n(now, count, state.history);
    state.history_count = count;
    state.stats.shadow_faces = result.face_count;
    uint32_t face_size[kMaxFrameLights] = {};
    for (uint32_t p = 0; p < pick_count; ++p) {
        face_size[picks[p].k] = picks[p].size;
    }
    const std::span<std::byte> mapped = state.device->mapped(buffer);
    for (uint32_t k = 0; k < count; ++k) {
        const world::RenderLight& light = frame.lights[kept[k].index];
        const float fade = kept[k].fade;
        const bool spot = light.cos_outer > -1.0f;
        const GpuLocalLight gpu{
            .position_radius = {light.position[0], light.position[1], light.position[2], light.radius_m},
            .color_cone = {light.color[0] * fade, light.color[1] * fade, light.color[2] * fade, light.cone_scale},
            .direction_cone = {light.direction[0], light.direction[1], light.direction[2], light.cos_outer},
            .shadow = {static_cast<float>(first_face[k]), spot ? 1.0f : 6.0f,
                       shadow_face_tan(light, std::max(face_size[k], kLocalShadowCell)), now[k].strength}};
        std::memcpy(mapped.data() + (size_t{k} * sizeof(GpuLocalLight)), &gpu, sizeof(gpu));
    }
    state.device->flush_mapped(buffer, 0, uint64_t{count} * sizeof(GpuLocalLight));
    state.lit_now = count;
    const float per_doubling = slices_per_doubling();
    result.lights = state.device->buffer_address(buffer);
    result.clusters = state.device->buffer_address(state.clusters);
    const uint32_t grid[4] = {kLightGridX, kLightGridY, kLightGridZ, (count + kLightWordBits - 1) / kLightWordBits};
    std::copy_n(grid, 4, result.grid);
    const float slices[4] = {per_doubling, -std::log2(kLightSliceNearM) * per_doubling, 0.0f, 0.0f};
    std::copy_n(slices, 4, result.slices);
    return result;
}

GraphBuffer LocalLights::add_cull_pass(RenderGraph& graph, uint64_t frame_address, const world::RenderFrame& frame,
                                       float aspect) {
    LocalLightsState& state = *state_;
    const rhi::PipelineHandle cull = state.shaders->pipeline(state.cull_program);
    if (state.lit_now == 0 || !cull.is_valid()) {
        return {};
    }
    // The words stay between frames (each frame writes all it reads), so the graph is told how the last one left them.
    const GraphBuffer clusters = graph.import_buffer(
        {.buffer = state.clusters,
         .name = "light clusters",
         .state = state.clusters_used ? rhi::ResourceState::ShaderRead : rhi::ResourceState::Undefined,
         .final_state = rhi::ResourceState::ShaderRead});
    state.clusters_used = true;
    const float* camera = frame.camera.transform;
    const Vec3 right = glm::normalize(Vec3(camera[0], camera[4], camera[8]));
    const Vec3 up = glm::normalize(Vec3(camera[1], camera[5], camera[9]));
    const float tan_y = std::tan(frame.camera.vertical_fov_rad * 0.5f);
    const LightCullPush push{.frame = frame_address,
                             .light_count = state.lit_now,
                             .cluster_count = kLightClusters,
                             .camera_right = {right.x, right.y, right.z, tan_y * aspect},
                             .camera_up = {up.x, up.y, up.z, tan_y},
                             .slice_near_m = kLightSliceNearM,
                             .slices_per_doubling = slices_per_doubling()};
    graph.add_pass(
        "light cull",
        [&](PassBuilder& pass) { pass.use(clusters, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute); },
        [push, cull](PassContext& context) {
            DE_PROFILE_ZONE("light cull pass");
            context.list.bind_pipeline(cull);
            context.list.push_constants(push);
            context.list.dispatch((push.cluster_count + 63) / 64);
        });
    return clusters;
}

} // namespace de::render
