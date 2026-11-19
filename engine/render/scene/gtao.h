#pragma once

#include "core/result.h"
#include "render/graph/render_graph.h"
#include "render/rhi/rhi.h"

#include <cstdint>

namespace de::rhi {
class Device;
}
namespace de::shader {
class ShaderLibrary;
}
namespace de::world {
struct RenderFrame;
}

namespace de::render {

// Ambient occlusion in screen space (docs/design.md, Global illumination: GTAO): how open the hemisphere above each
// pixel's surface is, for the near and small things the probes are too coarse to see. Jimenez et al.'s "ground
// truth" ambient occlusion (shaders/scene/gtao.slang): horizons along a few slices of the hemisphere, found in a
// depth pyramid (ao_depth.slang), integrated in closed form, then denoised along surfaces (ao_denoise.slang). The
// deferred light takes it for the light that has bounced, and for its reflection, not for the sun or the local
// lights, whose shadows are their own.

struct GtaoSettings {
    float radius_m = 0.8f;         // what occludes is within this of the surface
    float falloff_share = 0.4f;    // of the radius, where a sample starts to fade out of the horizon
    uint32_t slices = 2;           // a pixel's; its neighbours have others (the noise), and the denoise joins them
    uint32_t steps = 6;            // along each side of a slice
    float max_radius_share = 0.1f; // of the picture's height: the most the radius spans on the screen
};

// Mips of the depth pyramid: the farthest samples read the coarsest.
inline constexpr uint32_t kGtaoDepthMips = 5;

struct GtaoInput {
    uint64_t frame_address = 0;                // the scene's SceneFrame: TAA's offset and sample
    GraphTexture depth;                        // the scene's, reverse Z
    GraphTexture normals;                      // the G-buffer's (gb1)
    const world::RenderFrame* frame = nullptr; // the camera
    uint32_t width = 0;
    uint32_t height = 0;
};

struct GtaoDesc {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
};

struct GtaoState;

// On the thread that renders. The device and shader library must outlive it. Move-only.
class Gtao {
public:
    static core::Result<Gtao> create(const GtaoDesc& desc);

    ~Gtao();
    Gtao(Gtao&& other) noexcept;
    Gtao& operator=(Gtao&& other) noexcept;
    Gtao(const Gtao&) = delete;
    Gtao& operator=(const Gtao&) = delete;

    // Adds "ao depth", "gtao" and "ao denoise": after the G-buffer, before the light. Returns the occlusion (R8, the
    // picture's size; 1 open, 0 shut), which the light reads; invalid while a shader of its is missing, or with no
    // camera.
    GraphTexture add_passes(RenderGraph& graph, const GtaoInput& input);

    void set_settings(const GtaoSettings& settings);
    const GtaoSettings& settings() const;

private:
    explicit Gtao(GtaoState* state) : state_(state) {}

    GtaoState* state_ = nullptr;
};

} // namespace de::render
