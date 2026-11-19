// Test shader: shoots rays into a top-level acceleration structure, reached by its address, and writes what each
// one hit: how far along it, the instance, which of its runs of triangles, and which triangle.

struct RayQueryPush
{
    uint64_t scene; // the top level's address (rhi::Device::acceleration_address)
    float4* rays;   // two a ray: where it starts (xyz), then its direction (xyz)
    float4* hits;   // x: the distance, below 0 for a miss; y: the instance's ID; z: the run; w: the triangle
    uint count;
    uint reserved;
};

[[vk::push_constant]]
ConstantBuffer<RayQueryPush> push;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    if (thread.x >= push.count)
    {
        return;
    }
    const RaytracingAccelerationStructure scene = RaytracingAccelerationStructure(push.scene);
    RayDesc ray;
    ray.Origin = push.rays[thread.x * 2].xyz;
    ray.Direction = push.rays[thread.x * 2 + 1].xyz;
    ray.TMin = 0.0;
    ray.TMax = 1000.0;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, ray);
    query.Proceed();
    float4 hit = float4(-1.0, 0.0, 0.0, 0.0);
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        hit = float4(query.CommittedRayT(), float(query.CommittedInstanceID()), float(query.CommittedGeometryIndex()),
                     float(query.CommittedPrimitiveIndex()));
    }
    push.hits[thread.x] = hit;
}
