// The GPU cull: every instance against every view (the camera and each shadow cascade), by its mesh's
// bounding sphere: a cascade's view is what can shadow something it lights. What's in a view uses the parts of its mesh's
// coarsest LOD that's good enough there. While a LOD takes over from the one before, it uses the parts of both, in a group of
// their own.
//
// It runs twice around scene.cull_assign, so everything of one part in one group is one instanced draw: first it counts them,
// then that gives each its draw and where its records go, and then it writes those records. Both runs have to find the same
// parts. The order within a draw depends on the threads: nothing here may rely on it.

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

struct CullPush
{
    SceneFrame* frame;
    GpuInstance* instances; // per slot, staying between frames
    uint* instance_meshes;  // per slot, its slot in the mesh table, or kNoMesh
    GpuMeshInfo* meshes;
    GpuMeshLod* lods;
    GpuMeshPart* parts;
    GpuMaterial* materials;
    GpuCullView* views;
    GpuDrawList* lists;
    DrawIndexedArgs* args; // by slot, as a draw's first instance
    GpuDraw* records;      // by slot
    uint* counts;          // as in scene_common
    uint* part_keys;       // per group and part: how many, and then the next of its records to write
    uint instance_count;
    uint part_count;
    uint writing; // 0: count, 1: write the records
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

struct LodPick
{
    GpuMeshLod lod;
    GpuMeshLod finer; // what it's taking over from, drawn too while fade is below 1
    float fade;       // the share of pixels that are lod's. Above 0: a part with no pixels would look like no fade
};

// The coarsest LOD whose error is at most a pixel (or shadow map texel) in this view, from the sphere's nearest depth:
// what's on screen scales with depth, not distance, so a mesh in a corner gets what it would get in the middle.
// The fade comes from the error alone, so it needs no history: a LOD comes in at the limit, where it's still good enough,
// and finishes as its error falls to lod_fade_end. It's complete before the next LOD can start.
LodPick pick_lod(GpuCullView* view, GpuMeshInfo mesh, float3 center, float radius, float scale)
{
    LodPick pick;
    pick.lod = push.lods[mesh.first_lod];
    pick.finer = pick.lod;
    pick.fade = 1.0;
    if (view.lod_scale < 0.0)
    {
        return pick;
    }
    float error_scale = scale * view.lod_scale;
    if (view.lod_perspective != 0)
    {
        // At the sphere itself or inside it, no error is small enough.
        error_scale /= max(dot(view.depth_plane.xyz, center) + view.depth_plane.w - radius, 1e-4);
    }
    float fade_end = view.lod_fade_end;
    bool has_finer = false; // LOD 0 has nothing to fade from, whatever its error says
    for (uint l = 1; l < mesh.lod_count; ++l)
    {
        const GpuMeshLod coarser = push.lods[mesh.first_lod + l];
        if (coarser.error_m * error_scale > 1.0)
        {
            fade_end = max(fade_end, pick.lod.error_m / coarser.error_m);
            break;
        }
        if (coarser.error_m > pick.lod.error_m) // of two with the same error, the first is never seen alone
        {
            pick.finer = pick.lod;
            has_finer = true;
        }
        pick.lod = coarser;
    }
    if (has_finer && fade_end < 1.0)
    {
        pick.fade = clamp((1.0 - pick.lod.error_m * error_scale) / (1.0 - fade_end), 1e-4, 1.0);
    }
    return pick;
}

// A LOD's parts, counted or written, and how many triangles they are. fade is as in GpuDraw.
uint add_parts(uint view_index, GpuMeshLod lod, uint instance, float fade)
{
    uint triangles = 0;
    const uint group = view_index == 0 ? (fade != 0.0 ? 1u : 0u) : 1 + view_index;
    for (uint p = 0; p < lod.part_count; ++p)
    {
        const uint part_index = lod.first_part + p;
        const GpuMeshPart part = push.parts[part_index];
        const uint key = group * push.part_count + part_index;
        if (push.writing == 0)
        {
            uint ignored;
            InterlockedAdd(push.part_keys[key], 1, ignored);
            const uint list = draw_list_of(group, push.materials[part.material].flags);
            InterlockedAdd(push.counts[kCountRecordsPerList + list], 1, ignored);
            triangles += part.index_count / 3;
        }
        else if (push.part_keys[key] != kPartLost)
        {
            uint slot;
            InterlockedAdd(push.part_keys[key], 1, slot);
            GpuDraw record;
            record.instance = instance;
            record.material = part.material;
            record.fade = fade;
            push.records[slot] = record;
        }
    }
    return triangles;
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
    // No mesh, or none ready to draw, is no LODs. Not an early return: llvmpipe, which runs a group's invocations as
    // lanes of one, crashes in what follows when only the first lane has gone (a slot 0 with nothing in it).
    const uint mesh_slot = push.instance_meshes[instance];
    GpuMeshInfo mesh;
    mesh.center = float3(0.0);
    mesh.radius = 0.0;
    mesh.vertex_offset = 0;
    mesh.first_lod = 0;
    mesh.lod_count = 0;
    mesh.padding = 0;
    if (mesh_slot != kNoMesh)
    {
        mesh = push.meshes[mesh_slot];
    }
    const InstanceTransform transform = push.instances[instance].transform;
    const float3 center = instance_point(transform, push.frame.render_origin_steps.xyz, mesh.center);
    // The longest axis: exact for any rotation and scale, a little short only for a transform that shears.
    const float3 row0 = transform.row0.linear;
    const float3 row1 = transform.row1.linear;
    const float3 row2 = transform.row2.linear;
    const float3 axis0 = float3(row0.x, row1.x, row2.x);
    const float3 axis1 = float3(row0.y, row1.y, row2.y);
    const float3 axis2 = float3(row0.z, row1.z, row2.z);
    const float scale = sqrt(max(dot(axis0, axis0), max(dot(axis1, axis1), dot(axis2, axis2))));
    const float radius = mesh.radius * scale;

    for (uint v = 0; v < kCullViewCount; ++v)
    {
        GpuCullView* view = push.views + v;
        if (mesh.lod_count == 0 || radius < view.min_radius_m || !in_view(view, center, radius))
        {
            continue;
        }
        const LodPick pick = pick_lod(view, mesh, center, radius, scale);
        const bool fading = pick.fade < 1.0;
        uint triangles = add_parts(v, pick.lod, instance, fading ? pick.fade : 0.0);
        if (fading)
        {
            triangles += add_parts(v, pick.finer, instance, -pick.fade);
        }
        if (push.writing == 0)
        {
            uint ignored;
            InterlockedAdd(push.counts[kCountTriangles + (v == 0 ? 0u : 1u)], triangles, ignored);
        }
    }
}
