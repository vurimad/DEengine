#pragma once

#include "asset/registry.h"
#include "core/array.h"
#include "core/enum_flags.h"
#include "core/hash_map.h"
#include "core/result.h"
#include "render/rhi/rhi.h"

#include <cstdint>
#include <span>

namespace de::rhi {
class Device;
class Uploader;
} // namespace de::rhi

namespace de::render {

enum class GpuMaterialFlags : uint32_t {
    None = 0,
    Masked = 1U << 0U,      // alpha test against alpha_cutoff
    DoubleSided = 1U << 1U, // lit from both sides
};
DE_ENUM_FLAGS(GpuMaterialFlags)

// A material as shaders read it, from GpuAssets::material_table_address() by index (scalar
// layout; shaders/scene/scene_common.slang mirrors it: change both). Texture fields are bindless indices; a
// material without a texture, or whose texture isn't uploaded yet, points at a default (white, or
// a flat normal), so shaders never branch on it.
struct GpuMaterial {
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // linear
    float emissive[3] = {};                         // linear
    float metallic = 0.0f;
    float roughness = 0.5f;
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    float alpha_cutoff = 0.5f;
    uint32_t base_color_texture = 0;         // srgb, rgba
    uint32_t normal_texture = 0;             // rg: tangent-space x, y
    uint32_t metallic_roughness_texture = 0; // g roughness, b metalness
    uint32_t occlusion_texture = 0;          // r
    uint32_t emissive_texture = 0;           // srgb, rgb
    GpuMaterialFlags flags = GpuMaterialFlags::None;
    uint32_t reserved[2] = {};

    constexpr bool operator==(const GpuMaterial&) const = default;
};
static_assert(sizeof(GpuMaterial) == 80);

// A range of the pool's indices drawn with one material.
struct GpuMeshPart {
    uint32_t first_index = 0; // in the pool's index buffer
    uint32_t index_count = 0;
    uint32_t material = 0; // index into the material table
};

// One level of detail: LOD 0 first, coarser after.
struct GpuMeshLod {
    float error_m = 0.0f; // object space: how far this LOD may lie from LOD 0
    uint32_t first_part = 0;
    uint32_t part_count = 0;
};

// A mesh on the GPU: its place in the geometry pool. Its indices count from its own first vertex, which a draw
// passes as its vertex offset.
struct GpuMesh {
    uint32_t vertex_offset = 0;
    float bounds_min[3] = {};
    float bounds_max[3] = {};
    float bounds_center[3] = {}; // of the bounding sphere
    float bounds_radius_m = 0.0f;
    // These are valid until the next update() or the mesh is unloaded.
    std::span<const GpuMeshPart> parts; // of every LOD
    std::span<const GpuMeshLod> lods;

    std::span<const GpuMeshPart> lod_parts(size_t lod) const {
        return parts.subspan(lods[lod].first_part, lods[lod].part_count);
    }
};

struct GpuAssetsStats {
    uint32_t meshes = 0;         // uploaded and drawable
    uint32_t textures = 0;       // on the GPU
    uint32_t materials = 0;      // in the table
    uint32_t pending = 0;        // waiting for their uploads (meshes) or textures (materials)
    uint64_t bytes = 0;          // of vertices, indices and texels
    uint32_t meshes_dropped = 0; // didn't fit in the geometry pool, so aren't drawn (logged)
    uint64_t pool_vertices_free = 0;
    uint64_t pool_indices_free = 0;
};

struct GpuAssetsDesc {
    rhi::Device* device = nullptr;
    rhi::Uploader* uploader = nullptr;
    asset::AssetRegistry* assets = nullptr;
    uint32_t max_materials = 4096;
    // The geometry pool: every mesh's vertices and indices, in two buffers made once, whatever's loaded.
    uint32_t pool_vertices = 8U << 20U; // 32 bytes each: 256 MiB
    uint32_t pool_indices = 32U << 20U; // 4 bytes each: 128 MiB
};

struct GpuAssetsState;

// The GPU copies of what the asset registry has loaded (docs/design.md, Rendering): meshes in one
// shared vertex and index buffer, textures with their bindless index, materials in one table shaders
// index. It listens to the registry: an asset uploads when it's Ready, again when it's Reloaded,
// and goes when it's Unloaded, so the registry stays the only owner of what's loaded and this only
// mirrors the GPU side, keyed by the registry's handles.
//
// A mesh is drawable once its buffers are uploaded (mesh() returns null before); a material whose
// textures are still uploading uses default textures until they're in (update() rewrites it). A
// missing or failed material is drawn with material 0, the default. On the thread that renders, once
// per frame, after the registry's update() and before rendering. The device, uploader and registry
// must outlive it; it stops listening when it goes.
class GpuAssets {
public:
    static core::Result<GpuAssets> create(const GpuAssetsDesc& desc);

    ~GpuAssets();
    GpuAssets(GpuAssets&& other) noexcept;
    GpuAssets& operator=(GpuAssets&& other) noexcept;
    GpuAssets(const GpuAssets&) = delete;
    GpuAssets& operator=(const GpuAssets&) = delete;

    // Finishes what waited on uploads: meshes become drawable, materials get their textures.
    void update();

    // Null unless the mesh is uploaded (or for a stale handle).
    const GpuMesh* mesh(asset::AssetHandle mesh) const;
    uint64_t material_table_address() const;
    // The geometry pool. Vertices are asset::MeshVertex, pulled by address and SV_VulkanVertexID; indices are 32-bit.
    uint64_t pool_vertex_address() const;
    rhi::BufferHandle pool_index_buffer() const;
    // A material table entry's flags (as last written: a reloaded material changes them), which pick
    // how it draws: alpha-tested (Masked) and without back-face culling (DoubleSided). None for an
    // index past the table.
    GpuMaterialFlags material_flags(uint32_t material) const;
    // The fog noise's bindless index (3D, fog_noise.h), or 0 until it's uploaded.
    uint32_t fog_noise_index() const;

    GpuAssetsStats stats() const;

private:
    explicit GpuAssets(GpuAssetsState* state) : state_(state) {}

    GpuAssetsState* state_ = nullptr;
};

} // namespace de::render
