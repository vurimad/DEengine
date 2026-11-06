// What changed since last frame, into the instance data that stays on the GPU between frames. The CPU writes
// a record for each change, and this scatters them to their slots.

import scene.scene_common;

// Matches GpuInstanceUpdate in scene_renderer.cpp.
struct InstanceUpdate
{
    GpuInstance instance;
    uint slot;
    uint mesh; // its slot in the mesh table, or kNoMesh
    uint padding[2];
};

struct InstanceUpdatePush
{
    InstanceUpdate* updates;
    GpuInstance* instances; // per slot
    uint* instance_meshes;  // per slot
    uint count;
};

[[vk::push_constant]]
ConstantBuffer<InstanceUpdatePush> push;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    if (thread.x >= push.count)
    {
        return;
    }
    const InstanceUpdate update = push.updates[thread.x];
    push.instance_meshes[update.slot] = update.mesh;
    if (update.mesh != kNoMesh)
    {
        push.instances[update.slot] = update.instance;
    }
}
