// Between the cull's two runs: every part of every group that anything uses gets its draw in its list, and a run of records.
// What's left in part_keys is where each part's records start, for the cull's second run to count up from. It runs over
// only what's in use, so it costs what's on screen, not what's loaded, and again at the end to put those keys back to 0.

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

struct CullAssignPush
{
    GpuMeshPart* parts;
    GpuMaterial* materials;
    GpuDrawList* lists;
    DrawIndexedArgs* args;
    uint* counts; // as in scene_common
    uint* part_keys;
    uint* part_used;
    uint part_count;
    uint record_capacity;
    uint merged;    // 0: a draw per record instead, to compare with
    uint resetting; // 1: put the keys back to 0, and nothing else
};

[[vk::push_constant]]
ConstantBuffer<CullAssignPush> push;

[shader("compute")]
[numthreads(1, 1, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    const uint key = push.part_used[thread.x];
    if (push.resetting != 0)
    {
        push.part_keys[key] = 0;
        return;
    }
    const uint records = push.part_keys[key];
    const uint group = key / push.part_count;
    const GpuMeshPart part = push.parts[key % push.part_count];
    const uint list = draw_list_of(group, push.materials[part.material].flags);
    const GpuDrawList draws = push.lists[list];
    const uint draw_count = push.merged != 0 ? 1 : records;
    uint first_draw;
    InterlockedAdd(push.counts[list], draw_count, first_draw);
    // A material changed kind, or there are more parts in a list than a call can draw: it's left out, and takes no records.
    // The slots it took that a call would still reach are written as drawing nothing, as others' were counted past them.
    uint first_record = 0;
    bool fits = first_draw + draw_count <= draws.capacity;
    if (fits)
    {
        InterlockedAdd(push.counts[kCountRecordsUsed], records, first_record);
        fits = first_record + records <= push.record_capacity;
    }
    for (uint d = 0; d < draw_count && first_draw + d < draws.capacity; ++d)
    {
        DrawIndexedArgs args;
        args.index_count = part.index_count;
        args.instance_count = !fits ? 0 : (push.merged != 0 ? records : 1);
        args.first_index = part.first_index;
        args.vertex_offset = int(part.vertex_offset);
        args.first_instance = first_record + d; // a draw's instances read consecutive records from here
        push.args[draws.first_slot + first_draw + d] = args;
    }
    uint ignored;
    if (fits)
    {
        push.part_keys[key] = first_record;
        const uint view = group < 2 ? 0 : 1;
        InterlockedAdd(push.counts[kCountRecordsPerList + list], records, ignored);
        InterlockedAdd(push.counts[kCountDraws + view], draw_count, ignored);
        InterlockedAdd(push.counts[kCountTriangles + view], records * (part.index_count / 3), ignored);
    }
    else
    {
        push.part_keys[key] = kPartLost;
        InterlockedAdd(push.counts[kCountRecordsLost], records, ignored);
    }
}
