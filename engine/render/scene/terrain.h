#pragma once

#include "asset/terrain_format.h"
#include "core/array.h"
#include "core/math.h"
#include "core/result.h"
#include "render/graph/render_graph.h"
#include "render/rhi/rhi.h"
#include "render/shader/shader_library.h"

#include <cstdint>
#include <span>

namespace de::platform {
class Vfs;
}

namespace de::rhi {
class CommandList;
class Device;
class Uploader;
} // namespace de::rhi

namespace de::render {

// A level's patches are drawn this many of its spacings from the camera at most, which keeps about 4 pixels to a square
// at 2560 x 1440, and morphing starts at this share of that (docs/design.md, Terrain & vegetation).
inline constexpr float kTerrainRangeSpacings = 300.0f;
inline constexpr float kTerrainMorphStart = 0.7f;
// A node starts loading this much farther out than it's needed, so it's normally there by the time it is.
inline constexpr float kTerrainStreamMargin = 1.3f;

// How far from the camera a level with this spacing draws.
constexpr float terrain_level_range_m(float spacing_m) {
    return kTerrainRangeSpacings * spacing_m;
}

// A node the camera wants on the GPU.
struct TerrainWant {
    uint32_t node = 0; // into TerrainAsset::nodes
    uint32_t level = 0;
    float distance_m = 0.0f; // to its box
};

// The nodes wanted from where the camera is, coarsest level first and nearest first within one: every root, and every
// other node within its level's range and the margin. It looks only around the camera, so its cost doesn't follow the
// map's size. node_reach holds each node's lowest and highest, in meters. out is cleared, and doesn't allocate once
// it's settled.
void terrain_wanted_nodes(const asset::TerrainAsset& terrain, std::span<const std::byte> file,
                          std::span<const float> node_reach, const DVec3& camera, core::Array<TerrainWant>& out);

struct TerrainDesc {
    rhi::Device* device = nullptr;
    rhi::Uploader* uploader = nullptr;
    platform::Vfs* vfs = nullptr;
    // Blocks on the GPU at once, made once at about 35 KB each (1,024: 36 MiB). Enough for what's near the camera: the
    // roots of the map, and about 64 nodes per other level.
    uint32_t max_blocks = 1024;
};

struct TerrainStats {
    uint32_t wanted = 0;   // nodes wanted right now
    uint32_t resident = 0; // on the GPU
    uint32_t reading = 0;
    uint32_t uploading = 0;
    uint32_t failed = 0;        // couldn't be read: what's coarser stands in for them
    uint32_t over_capacity = 0; // wanted, but no room: max_blocks is too small for this camera
    uint64_t blocks_loaded = 0; // so far
    uint32_t blocks = 0;        // the map's
    // What was drawn, read back a frame or two late.
    uint32_t patches = 0;
    uint32_t shadow_patches = 0;
    uint32_t triangles = 0;
    uint32_t shadow_triangles = 0;
};

// What a frame's terrain passes use, in the graph. Not valid: no terrain to draw.
struct TerrainFrame {
    GraphBuffer args;
    GraphBuffer draws;
    bool valid = false;
};

struct TerrainState;

// The terrain on the GPU, and what keeps it there: heights are read a block at a time as the camera moves, into a pool
// of fixed slots, and a table says which node is where. What's on the GPU is the roots and what's near the camera;
// everything else about the map (where things are, how high they reach) stays loaded. Move-only. The device, uploader
// and vfs must outlive it.
class Terrain {
public:
    static core::Result<Terrain> create(const TerrainDesc& desc);

    ~Terrain();
    Terrain(Terrain&& other) noexcept;
    Terrain& operator=(Terrain&& other) noexcept;
    Terrain(const Terrain&) = delete;
    Terrain& operator=(const Terrain&) = delete;

    // Shows this terrain from now on, starting from nothing; null shows none. file holds it, and must stay valid and
    // unchanged until the next call. Reads in flight are waited for: not for every frame.
    void set(const asset::TerrainAsset* terrain, std::span<const std::byte> file);

    // A frame's streaming: collects what's been read, publishes what's been uploaded, and starts what's wanted most.
    // No allocation per frame once it's settled.
    void update(const DVec3& camera);

    // Picks this frame's patches. frame and views are SceneFrame and GpuCullView[] addresses. Nothing is added while
    // there's nothing to show, or the select pipeline is missing (a shader that failed).
    TerrainFrame add_select_passes(RenderGraph& graph, rhi::PipelineHandle select, uint64_t frame, uint64_t views);

    // Draws into a pass that declared use_draws().
    void use_draws(PassBuilder& pass, const TerrainFrame& frame) const;
    void draw_gbuffer(PassContext& context, const TerrainFrame& frame, rhi::PipelineHandle pipeline,
                      uint64_t frame_address, bool show_levels) const;
    void draw_shadow(PassContext& context, const TerrainFrame& frame, rhi::PipelineHandle pipeline,
                     uint64_t frame_address, uint32_t cascade) const;

    // Copies what was drawn for stats(). After the passes that draw.
    void add_stats_pass(RenderGraph& graph, const TerrainFrame& frame);

    TerrainStats stats() const;

private:
    explicit Terrain(TerrainState* state) : state_(state) {}

    TerrainState* state_ = nullptr;
};

} // namespace de::render
