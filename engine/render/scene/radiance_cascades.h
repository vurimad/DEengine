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

// Light that has bounced, by screen-space radiance cascades (docs/design.md, Global illumination: radiance cascades),
// the method Path of Exile 2 ships, after Sannikov's paper ("Radiance Cascades: A Novel Approach to Calculating
// Global Illumination") and his ExileCon talks, in our own words and code (shaders/scene/rc_common.slang has the
// method).
//
// Probes on the picture in cascades. A direction is an azimuth (a direction on the screen) and an elevation (a few
// bins from toward the camera to away from it): every ray along one azimuth crosses the same pixels, so one march
// along the screen serves every elevation, as a horizon that rises. They work on the GI picture: the screen at
// 1 / scale of its size each way. Cascade 0's probes are its pixels, with few azimuths and a reach of a couple of
// pixels; each next cascade has probes twice as far apart each way, twice the azimuths and an interval twice as
// long and as far out, so each costs half the one before. Merged from the topmost down; "rc gather"
// (shaders/scene/rc_gather.slang) marches cascade 0 for each of the GI picture's pixels, merges it with cascade 1
// and keeps the light's moments over directions; the scene's "rc apply" (shaders/scene/rc_apply.slang) takes those
// for each of the screen's pixels, with its own normal, in place of the flat ambient. Everything is marched again
// every frame: nothing is kept, so a light that comes or goes does so at once.
//
// What it doesn't see, by being on the screen: what's off the picture or hidden behind what it shows.

struct RadianceCascadesSettings {
    // The screen's pixels to one of the GI picture's, each way: 1 to 4, or 0 for the most that keeps the GI picture
    // 360 pixels high or more (2 up to 900p, 3 at 1080p, 4 from 1440p). The cost goes with the GI picture's pixels.
    // Measured against the path tracer the light is as right with the GI picture at a quarter of a 2560 x 1440
    // screen as at a third, a half or the whole of it: what tells is how far apart a march's samples are on the
    // screen (max_step_px), not the GI picture's size. What goes with a smaller one is the finest creases, a GI
    // pixel wide.
    uint32_t scale = 0;
    uint32_t azimuths = 4;    // cascade 0's directions on the screen; each cascade has twice the one before's
    float interval_px = 2.0f; // cascade 0's reach, in the GI picture's pixels; each cascade's is twice the one
                              // before's
    // Samples along a march are no farther apart than this, in the screen's pixels (in the GI picture's: the power of
    // two no wider than this over the scale). A march's samples read the pyramids' mip as coarse as they're apart,
    // so a far cascade's march is tens of samples, not hundreds. What matters is this length on the screen, not the
    // GI picture's size: at 16 the light measures the same against the path tracer with the GI picture at a half, a
    // third and a quarter of a 2560 x 1440 screen; at 24 to 32 the sun-lit measuring room is 0.03 stops farther off.
    uint32_t max_step_px = 16;
    // A march takes no more samples than this (0: as many as max_step_px makes it): past it the samples stand
    // farther apart than max_step_px and read a coarser mip still. Only the topmost cascades' marches are that
    // long. At 32 the light measures the same as with no cap; at 16 the sun-lit measuring room is 0.005 to 0.01
    // stops farther off, for 0.03 ms; at 8, up to 0.025 stops.
    uint32_t max_march_steps = 32;
    uint32_t max_cascades = 10; // as many as reach across the picture, at most this
    // How far behind its surface a thing on the screen still stands. Seen from above, what stands on the ground is
    // solid down to it, so the default takes the picture as a height field: smooth, and right for walls and what
    // stands on the floor, but a tall thin thing (a pillar) shadows too wide a strip behind it. A small value (under
    // a meter) lets light pass behind such things, at the cost of patches where pieces of them meet: for a camera
    // that looks along the ground, where a height field is wrong.
    float thickness_m = 100.0f;
    float plane_tolerance =
        1.5f;            // how far off a probe's plane a probe of the cascade above still counts, in its spacings
    float sky = 1.0f;    // what the sky's light is multiplied by
    float bounce = 1.0f; // what the light of what's on the screen is multiplied by
    // Light bounces more than once: what each surface sent on last frame of the light gathered there is met by
    // this frame's marches with the lit picture, a bounce more a frame. Off, a surface lit only by the sky or by
    // what has bounced sends nothing on (to compare with the path tracer held to one bounce).
    bool bounces = true;
    bool white_sky = false;         // the sky white from every way, to see what's open (with the indirect light view)
    bool material_occlusion = true; // times the material's own occlusion; off to compare with the path tracer
};

// The pyramids' mips: a march's samples read the mip as coarse as they're apart.
inline constexpr uint32_t kRcPyramidMips = 10;
// Elevation bins a slice: kRcElevations in shaders/scene/rc_common.slang.
inline constexpr uint32_t kRcElevations = 4;

// What the pass that adds the light needs (shaders/scene/rc_apply.slang): the light gathered over the GI picture,
// and where that picture's pixels stand.
struct RadianceCascadesResult {
    GraphTexture depth; // the GI picture's pyramids (shaders/scene/rc_pyramid.slang)
    GraphTexture normal;
    GraphTexture light;    // the light's moments (shaders/scene/rc_gather.slang): three times the GI picture's height
    uint32_t size[2] = {}; // the GI picture's
    uint32_t scale = 1;    // the screen's pixels to one of its, each way
    float tolerance = 0.0f;
    bool material_occlusion = true;

    bool valid() const { return light.is_valid(); }
};

struct RadianceCascadesInput {
    uint64_t frame_address = 0;                // the scene's SceneFrame
    GraphTexture depth;                        // the scene's, reverse Z
    GraphTexture radiance;                     // the lit picture so far: direct light and glow (HDR)
    GraphTexture glow;                         // add_glow_pass's
    GraphTexture base_color;                   // the G-buffer's base colour and material: what a surface sends on
    GraphTexture material;                     // of the light gathered (settings.bounces); without them, nothing
    const world::RenderFrame* frame = nullptr; // the camera
    uint32_t width = 0;
    uint32_t height = 0;
};

struct RadianceCascadesStats {
    uint32_t scale = 0;    // the screen's pixels to one of the GI picture's, each way
    uint32_t cascades = 0; // with cascade 0
    uint32_t marches = 0;  // along the screen, in every cascade (cascade 0's are the gather's)
    uint32_t texels = 0;   // stored, in every cascade
};

struct RadianceCascadesDesc {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
};

struct RadianceCascadesState;

// On the thread that renders. The device and shader library must outlive it. Move-only.
class RadianceCascades {
public:
    static core::Result<RadianceCascades> create(const RadianceCascadesDesc& desc);

    ~RadianceCascades();
    RadianceCascades(RadianceCascades&& other) noexcept;
    RadianceCascades& operator=(RadianceCascades&& other) noexcept;
    RadianceCascades(const RadianceCascades&) = delete;
    RadianceCascades& operator=(const RadianceCascades&) = delete;

    // Adds "rc glow": the luminance of what the HDR target holds, read before the deferred light adds to it, when
    // that's only what surfaces emit, at the GI picture's size (shaders/scene/rc_glow.slang). `width` and `height`
    // are the screen's. Invalid while its shader is missing.
    GraphTexture add_glow_pass(RenderGraph& graph, GraphTexture hdr, uint32_t width, uint32_t height);

    // Adds "rc pyramid", a "radiance cascade" pass per stored cascade, the topmost first, and "rc gather": after
    // the deferred light (whose picture the marches read), before what adds the light. Invalid result while a
    // shader is missing, or with no camera.
    RadianceCascadesResult add_passes(RenderGraph& graph, const RadianceCascadesInput& input);

    void set_settings(const RadianceCascadesSettings& settings);
    const RadianceCascadesSettings& settings() const;
    // The last add_passes'.
    RadianceCascadesStats stats() const;

private:
    explicit RadianceCascades(RadianceCascadesState* state) : state_(state) {}

    RadianceCascadesState* state_ = nullptr;
};

} // namespace de::render
