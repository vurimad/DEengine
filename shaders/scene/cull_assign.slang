// Between the cull's two runs: every part of every group that anything uses gets its draw in its list, and a run of records.
// What's left in part_keys is where each part's records start, for the cull's second run to count up from.

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
    uint* counts;
    uint* part_keys;
    uint part_count;
    uint record_capacity;
    uint merged; // 0: a draw per record instead, to compare with
};

[[vk::push_constant]]
ConstantBuffer<CullAssignPush> push;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    const uint key = thread.x;
    if (key >= kCullGroups * push.part_count)
    {
        return;
    }
    const uint records = push.part_keys[key];
    if (records != 0)
    {
        const uint group = key / push.part_count;
        const GpuMeshPart part = push.parts[key % push.part_count];
        const uint list = draw_list_of(group, push.materials[part.material].flags);
        const GpuDrawList draws = push.lists[list];
        const uint draw_count = push.merged != 0 ? 1 : records;
        uint first_draw;
        InterlockedAdd(push.counts[list], draw_count, first_draw);
        uint first_record;
        InterlockedAdd(push.counts[kCountRecordsUsed], records, first_record);
        if (first_draw + draw_count > draws.capacity || first_record + records > push.record_capacity)
        {
            push.part_keys[key] = kPartLost; // a material changed kind, or a mesh is on its way out
        }
        else
        {
            for (uint d = 0; d < draw_count; ++d)
            {
                DrawIndexedArgs args;
                args.index_count = part.index_count;
                args.instance_count = push.merged != 0 ? records : 1;
                args.first_index = part.first_index;
                args.vertex_offset = int(part.vertex_offset);
                args.first_instance = first_record + d; // a draw's instances read consecutive records from here
                push.args[draws.first_slot + first_draw + d] = args;
            }
            push.part_keys[key] = first_record;
        }
    }
}
