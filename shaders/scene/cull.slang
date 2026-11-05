// The GPU cull: every instance against every view (the camera and each shadow cascade), by its mesh's
// bounding sphere. What's in a view adds the parts of its mesh's coarsest LOD that's good enough there to
// that view's draw lists: a record and draw arguments per part, and a count the draw pass reads
// (scene_renderer.cpp, GpuDrawList). The order within a list depends on the threads: nothing here may
// rely on it.

import scene.scene_common;

// Matches rhi::DrawIndexedIndirectArgs.
struct DrawIndexedArgs
{
    uint index_count;
    uint instance_count;
    uint first_index;
    int vertex_offset;
    uint first_instance;
};

// Lists and views, as in scene_renderer.cpp.
static const uint kDrawListCount = 4 + 2 * kCascadeCount;

struct CullPush
{
    float4* instances;     // kInstanceRows per slot, staying between frames
    uint* instance_meshes; // per slot, its slot in the mesh table, or kNoMesh
    GpuMeshInfo* meshes;
    GpuMeshLod* lods;
    GpuMeshPart* parts;
    GpuMaterial* materials;
    GpuCullView* views;
    GpuDrawList* lists;
    DrawIndexedArgs* args; // by slot, as a draw's first instance
    GpuDraw* records;      // by slot
    uint* counts;          // per list, then the triangles kept for the camera and for the cascades
    uint instance_count;
};

[[vk::push_constant]]
ConstantBuffer<CullPush> push;

bool in_view(GpuCullView* view, float3 center, float radius)
{
    for (uint p = 0; p < view.plane_count; ++p)
    {
        if (dot(view.planes[p].xyz, center) + view.planes[p].w < -radius)
        {
            return false;
        }
    }
    return true;
}

// The coarsest LOD whose error is at most a pixel (or shadow map texel) in this view, from the sphere's nearest depth:
// what's on screen scales with depth, not distance, so a mesh in a corner gets what it would get in the middle.
GpuMeshLod pick_lod(GpuCullView* view, GpuMeshInfo mesh, float3 center, float radius, float scale)
{
    GpuMeshLod picked = push.lods[mesh.first_lod];
    if (view.lod_scale < 0.0)
    {
        return picked;
    }
    float error_scale = scale * view.lod_scale;
    if (view.lod_perspective != 0)
    {
        // At the sphere itself or inside it, no error is small enough.
        error_scale /= max(dot(view.depth_plane.xyz, center) + view.depth_plane.w - radius, 1e-4);
    }
    for (uint l = 1; l < mesh.lod_count; ++l)
    {
        const GpuMeshLod coarser = push.lods[mesh.first_lod + l];
        if (coarser.error_m * error_scale > 1.0)
        {
            break;
        }
        picked = coarser;
    }
    return picked;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    const uint instance = thread.x;
    if (instance >= push.instance_count)
    {
        return;
    }
    const uint mesh_slot = push.instance_meshes[instance];
    if (mesh_slot == kNoMesh)
    {
        return;
    }
    const GpuMeshInfo mesh = push.meshes[mesh_slot];
    if (mesh.lod_count == 0) // not ready to draw
    {
        return;
    }
    const float4 row0 = push.instances[instance * kInstanceRows + 0];
    const float4 row1 = push.instances[instance * kInstanceRows + 1];
    const float4 row2 = push.instances[instance * kInstanceRows + 2];
    const float4 local = float4(mesh.center, 1.0);
    const float3 center = float3(dot(row0, local), dot(row1, local), dot(row2, local));
    // The longest axis: exact for any rotation and scale, a little short only for a transform that shears.
    const float3 axis0 = float3(row0.x, row1.x, row2.x);
    const float3 axis1 = float3(row0.y, row1.y, row2.y);
    const float3 axis2 = float3(row0.z, row1.z, row2.z);
    const float scale = sqrt(max(dot(axis0, axis0), max(dot(axis1, axis1), dot(axis2, axis2))));
    const float radius = mesh.radius * scale;

    for (uint v = 0; v < kCullViewCount; ++v)
    {
        GpuCullView* view = push.views + v;
        if (!in_view(view, center, radius))
        {
            continue;
        }
        const GpuMeshLod lod = pick_lod(view, mesh, center, radius, scale);
        uint triangles = 0;
        for (uint p = 0; p < lod.part_count; ++p)
        {
            const GpuMeshPart part = push.parts[lod.first_part + p];
            const uint flags = push.materials[part.material].flags;
            const uint masked = (flags & kMaterialMasked) != 0 ? 1u : 0u;
            const uint double_sided = (flags & kMaterialDoubleSided) != 0 ? 1u : 0u;
            // A view with every pipeline state has a list per kind ([masked][double-sided]); a cascade's casters
            // differ only by the alpha test.
            const uint list = view.first_list + (view.lists_by_kind != 0 ? masked * 2 + double_sided : masked);
            uint index;
            InterlockedAdd(push.counts[list], 1, index);
            const GpuDrawList draws = push.lists[list];
            if (index >= draws.capacity) // only if a material's flags changed since the lists were sized
            {
                continue;
            }
            const uint slot = draws.first_slot + index;
            DrawIndexedArgs args;
            args.index_count = part.index_count;
            args.instance_count = 1;
            args.first_index = part.first_index;
            args.vertex_offset = int(mesh.vertex_offset);
            args.first_instance = slot;
            push.args[slot] = args;
            GpuDraw record;
            record.instance = instance;
            record.material = part.material;
            push.records[slot] = record;
            triangles += part.index_count / 3;
        }
        uint ignored;
        InterlockedAdd(push.counts[kDrawListCount + (v == 0 ? 0u : 1u)], triangles, ignored);
    }
}
