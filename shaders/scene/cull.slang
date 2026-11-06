// The GPU cull: every instance against every view (the camera and each shadow cascade), by its mesh's
// bounding sphere. What's in a view adds the parts of its mesh's coarsest LOD that's good enough there to
// that view's draw lists: a record and draw arguments per part, and a count the draw pass reads
// (scene_renderer.cpp, GpuDrawList). While a LOD takes over from the one before, it adds the parts of both, to
// lists of their own. The order within a list depends on the threads: nothing here may rely on it.

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
static const uint kDrawKinds = 4;
static const uint kDrawListCount = 2 * kDrawKinds + 2 * kCascadeCount;

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

// A LOD's parts into the view's lists, and how many triangles they are. fade is as in GpuDraw.
uint add_parts(GpuCullView* view, GpuMeshInfo mesh, GpuMeshLod lod, uint instance, float fade)
{
    uint triangles = 0;
    for (uint p = 0; p < lod.part_count; ++p)
    {
        const GpuMeshPart part = push.parts[lod.first_part + p];
        const uint flags = push.materials[part.material].flags;
        const uint masked = (flags & kMaterialMasked) != 0 ? 1u : 0u;
        const uint double_sided = (flags & kMaterialDoubleSided) != 0 ? 1u : 0u;
        // A view with every pipeline state has a list per kind ([masked][double-sided]), and again for what's fading; a
        // cascade's casters differ only by the alpha test.
        const uint kind = masked * 2 + double_sided + (fade != 0.0 ? kDrawKinds : 0u);
        const uint list = view.first_list + (view.lists_by_kind != 0 ? kind : masked);
        uint index;
        InterlockedAdd(push.counts[list], 1, index);
        const GpuDrawList draws = push.lists[list];
        if (index >= draws.capacity) // a material changed kind, or a mesh is on its way out
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
        record.fade = fade;
        push.records[slot] = record;
        triangles += part.index_count / 3;
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
        if (mesh.lod_count == 0 || !in_view(view, center, radius))
        {
            continue;
        }
        const LodPick pick = pick_lod(view, mesh, center, radius, scale);
        const bool fading = pick.fade < 1.0;
        uint triangles = add_parts(view, mesh, pick.lod, instance, fading ? pick.fade : 0.0);
        if (fading)
        {
            triangles += add_parts(view, mesh, pick.finer, instance, -pick.fade);
        }
        uint ignored;
        InterlockedAdd(push.counts[kDrawListCount + (v == 0 ? 0u : 1u)], triangles, ignored);
    }
}
