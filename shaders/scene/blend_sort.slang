// The transparent pass's order: the draws the cull kept of what blends (kBlendList, a draw each), put farthest first, so
// each is mixed over what's behind it. Two runs: the first measures each draw, by the middle of its instance's bounding
// sphere along the view; the second counts, for each, how many are farther, which is its place. That's every draw
// against every other: nothing for the few hundred see-through things a view holds, and stable (two at one distance
// keep the cull's order, which is why nothing flickers between them). A crowd of particles wants a real sort, and
// brings its own.

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

struct BlendSortPush
{
    SceneFrame* frame;
    GpuInstance* instances;
    uint* instance_meshes;
    GpuMeshInfo* meshes;
    GpuDraw* records;
    DrawIndexedArgs* args;   // the blend list's draws, as the cull wrote them
    DrawIndexedArgs* sorted; // the same, farthest first
    float* keys;             // each draw's distance along the view
    uint* counts;            // the cull's: counts[kBlendList] draws in the list
    uint capacity;           // the most the list holds
    uint placing;            // 0: measure each draw, 1: put each in its place
};

[[vk::push_constant]]
ConstantBuffer<BlendSortPush> push;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    const uint i = thread.x;
    // No early return past a read: llvmpipe runs a group's invocations as lanes of one (see scene.cull).
    const uint count = i < push.capacity ? min(push.counts[kBlendList], push.capacity) : 0;
    if (i < count && push.placing == 0)
    {
        const DrawIndexedArgs draw = push.args[i];
        float key = 0.0;
        // A draw that found no room draws nothing, and its record was never written.
        if (draw.instance_count != 0)
        {
            const GpuDraw record = push.records[draw.first_instance];
            const uint mesh_slot = push.instance_meshes[record.instance];
            float3 center = float3(0.0);
            if (mesh_slot != kNoMesh)
            {
                center = push.meshes[mesh_slot].center;
            }
            const float3 position =
                instance_point(push.instances[record.instance].transform, push.frame.render_origin_steps.xyz, center);
            key = dot(position - push.frame.camera_position.xyz, push.frame.camera_forward.xyz);
        }
        push.keys[i] = key;
    }
    if (i < count && push.placing != 0)
    {
        const float key = push.keys[i];
        uint place = 0;
        for (uint j = 0; j < count; ++j)
        {
            const float other = push.keys[j];
            place += (other > key || (other == key && j < i)) ? 1u : 0u;
        }
        push.sorted[place] = push.args[i];
    }
}
