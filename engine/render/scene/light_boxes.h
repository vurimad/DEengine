#pragma once

#include "core/math.h"
#include "core/result.h"
#include "render/graph/render_graph.h"
#include "render/rhi/rhi.h"

#include <cstdint>
#include <span>

namespace de::rhi {
class Device;
}
namespace de::shader {
class ShaderLibrary;
}

namespace de::render {

// Light boxes (docs/design.md, Global illumination: the plan from 2026-11-14, step 3): the light that has bounced and
// the sky's, baked into boxes of voxels by the scene's own rays (shaders/scene/light_box_bake.slang: paths of light as
// the path tracer traces them) and read by the light passes in place of the probes where a box holds a point
// (shaders/scene/light_boxes.slang). Each voxel keeps first-order spherical harmonics, so a surface's light is one
// dot product with its normal; a voxel found inside something keeps nothing and isn't read. Direct light (the sun,
// the local lights) is never baked: it stays live, with its shadows.
//
// For now a box is set by the renderer's caller (set_boxes), baked on the GPU over the frames after, in memory only:
// saving bakes, placing boxes in the editor and keys through the day come next.

// A box, in the world.
struct LightBoxDesc {
    DVec3 min_m{0.0};
    DVec3 max_m{0.0};
    float voxel_m = 0.25f; // a voxel's size (the box is rounded up to whole voxels)
    float fade_m = 0.5f;   // over this much of its edge, it fades out
};

// The most boxes at once, and the most voxels a box has each way.
inline constexpr uint32_t kMaxLightBoxes = 8;
inline constexpr uint32_t kMaxLightBoxVoxels = 128;
// Rays a voxel's bake takes in all, and the most paths traced a frame while any box is baking.
inline constexpr uint32_t kLightBoxRays = 1024;
inline constexpr uint32_t kLightBoxPathsPerFrame = 1u << 20;

// Matches GpuLightBox in shaders/scene/scene_common.slang.
struct GpuLightBox {
    float origin_voxel[4] = {}; // xyz: its lowest corner, relative to the render origin; w: a voxel's size
    uint32_t size[4] = {};      // voxels each way; w: all of them
    uint32_t maps[4] = {};      // its four 3D maps (sampled)
    float params[4] = {};       // x: its fade distance
};
static_assert(sizeof(GpuLightBox) == 64);

struct LightBoxStats {
    uint32_t boxes = 0;
    uint32_t voxels = 0;
    float baked = 0.0f; // of all the rays their bakes take, the share taken
};

struct LightBoxesDesc {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
};

struct LightBoxesState;

// On the thread that renders. The device and shader library must outlive it. Move-only.
class LightBoxes {
public:
    static core::Result<LightBoxes> create(const LightBoxesDesc& desc);

    ~LightBoxes();
    LightBoxes(LightBoxes&& other) noexcept;
    LightBoxes& operator=(LightBoxes&& other) noexcept;
    LightBoxes(const LightBoxes&) = delete;
    LightBoxes& operator=(const LightBoxes&) = delete;

    // The boxes from now on (at most kMaxLightBoxes): each bakes again from nothing.
    void set_boxes(std::span<const LightBoxDesc> boxes);
    // Bakes every box again from nothing (the scene changed).
    void rebake();

    // This frame's boxes, relative to the render origin, for SceneFrame::light_boxes: written into a buffer of this
    // frame's, whose address and count it returns. ready: what rays meet is all there (the bake waits for it).
    struct Frame {
        uint64_t boxes = 0;
        uint32_t count = 0;
    };
    Frame update(const DVec3& render_origin, bool ready);

    // Adds "light box bake" (some rays to some voxels, while any box isn't done) after the top level is built. Returns
    // the boxes' maps as this frame's graph has them, for the light passes to read; light_count: the frame's local
    // lights, which the bake's paths are lit by too.
    std::span<const GraphTexture> add_passes(RenderGraph& graph, uint64_t frame_address, uint32_t light_count);

    LightBoxStats stats() const;

private:
    explicit LightBoxes(LightBoxesState* state) : state_(state) {}

    LightBoxesState* state_ = nullptr;
};

} // namespace de::render
