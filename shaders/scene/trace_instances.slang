// The top level's instances (docs/design.md, Global illumination), in compute: a thread an instance slot, each
// writing where its instance stands relative to the render origin and which bottom level it is (its mesh's, from
// the table render::TraceScene keeps), as the acceleration structure's build reads them. A slot with no mesh, or
// whose mesh has no bottom level yet, is written as no instance: no ray sees it.

import scene.scene_common;

// Matches rhi::AccelerationInstance.
struct TraceInstance
{
    float4 row0; // rows of the 3 x 4 that places it
    float4 row1;
    float4 row2;
    uint id_and_mask; // low 24 bits: its slot, which a hit reports; high 8: which rays see it
    uint flags;
    uint64_t bottom_level;
};

struct TraceInstancesPush
{
    SceneFrame* frame;
    GpuInstance* instances; // per slot
    uint* instance_meshes;  // per slot, its slot in the mesh table, or kNoMesh
    MeshTrace* mesh_trace;  // per mesh slot
    TraceInstance* output;  // per slot
    uint count;
    uint reserved;
};

[[vk::push_constant]]
ConstantBuffer<TraceInstancesPush> push;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    const uint slot = thread.x;
    if (slot >= push.count)
    {
        return;
    }
    const uint mesh = push.instance_meshes[slot];
    uint64_t bottom_level = 0;
    if (mesh != kNoMesh)
    {
        bottom_level = push.mesh_trace[mesh].bottom_level;
    }
    const InstanceTransform transform = push.instances[slot].transform;
    const float3 position = instance_point(transform, push.frame.render_origin_steps.xyz, float3(0.0));
    TraceInstance placed;
    placed.row0 = float4(transform.row0.linear, position.x);
    placed.row1 = float4(transform.row1.linear, position.y);
    placed.row2 = float4(transform.row2.linear, position.z);
    placed.id_and_mask = (slot & 0xffffffu) | (bottom_level != 0 ? 0xff000000u : 0u);
    placed.flags = 0;
    placed.bottom_level = bottom_level;
    push.output[slot] = placed;
}
