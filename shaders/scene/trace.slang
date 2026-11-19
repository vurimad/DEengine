// Tracing rays against the scene (docs/design.md, Global illumination): the top-level acceleration structure over
// the frame's instances (render::TraceScene builds it each frame, from a bottom level per mesh), reached by its
// address, with what a hit needs to say what was hit: the instance's transform, its mesh's parts, the geometry
// pool, the material table. Only on GPUs with ray queries, and only where the renderer traces at all.

import common.bindless;
import scene.scene_common;

// Textures are read this many mips down where a ray lands: a ray stands for a wide footprint, and a coarse mip is
// already in the cache of whatever else reads it.
static const float kTraceMip = 3.0;

public struct TraceHit
{
    public bool hit;
    public bool front;          // it met the side its triangle faces
    public float distance;      // along the ray, in its direction's lengths
    public uint instance;       // the instance's slot
    public uint run;            // which part of its mesh, among those of the level of detail rays see
    public uint triangle;       // which triangle of that part
    public float2 barycentrics; // where on the triangle: the weights of its second and third corners
};

// The nearest thing a ray meets between two distances along it. Everything is solid to a ray for now: what's cut
// out or see-through stops it like anything else.
public TraceHit trace_nearest(TraceScene* scene, float3 origin, float3 direction, float from, float to)
{
    TraceHit found;
    found.hit = false;
    found.front = true;
    found.distance = to;
    found.instance = 0;
    found.run = 0;
    found.triangle = 0;
    found.barycentrics = float2(0.0);
    if (scene.top_level == 0)
    {
        return found; // nothing built yet
    }
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = from;
    ray.TMax = to;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(RaytracingAccelerationStructure(scene.top_level), RAY_FLAG_NONE, 0xff, ray);
    query.Proceed();
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        found.hit = true;
        found.front = query.CommittedTriangleFrontFace();
        found.distance = query.CommittedRayT();
        found.instance = query.CommittedInstanceID();
        found.run = query.CommittedGeometryIndex();
        found.triangle = query.CommittedPrimitiveIndex();
        found.barycentrics = query.CommittedTriangleBarycentrics();
    }
    return found;
}

// Whether anything at all stands between two distances along a ray: for shadows, where which thing doesn't matter.
public bool trace_blocked(TraceScene* scene, float3 origin, float3 direction, float from, float to)
{
    if (scene.top_level == 0)
    {
        return false;
    }
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = from;
    ray.TMax = to;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(RaytracingAccelerationStructure(scene.top_level),
                         RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff, ray);
    query.Proceed();
    return query.CommittedStatus() != COMMITTED_NOTHING;
}

// What a ray landed on.
public struct TraceSurface
{
    public float3 position; // relative to the render origin
    public float3 normal;   // the surface's, as its vertices give it, turned to face where the ray came from
    public float3 base_color;
    public float3 emissive;
    public float roughness;
    public float metalness;
    public uint flags; // its material's (kMaterialDoubleSided and the rest)
};

// The surface at a hit: its triangle's corners from the geometry pool, weighed by where it was hit, in the world by
// its instance's transform, with its material's maps read at a coarse mip (mip: another, 0 for the finest).
public TraceSurface trace_surface(TraceScene* scene, TraceHit hit, float3 origin, float3 direction,
                                  float mip = kTraceMip)
{
    const GpuInstance instance = scene.instances[hit.instance];
    const MeshTrace mesh = scene.mesh_trace[scene.instance_meshes[hit.instance]];
    const GpuMeshPart part = scene.parts[mesh.first_part + hit.run];
    const uint first = part.first_index + hit.triangle * 3;
    const MeshVertex a = scene.vertices[part.vertex_offset + scene.indices[first]];
    const MeshVertex b = scene.vertices[part.vertex_offset + scene.indices[first + 1]];
    const MeshVertex c = scene.vertices[part.vertex_offset + scene.indices[first + 2]];
    const float3 weights = float3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics.x, hit.barycentrics.y);
    const float2 uv = a.uv * weights.x + b.uv * weights.y + c.uv * weights.z;
    const float3 local = octahedral_decode(snorm16x2(a.normal)) * weights.x +
                         octahedral_decode(snorm16x2(b.normal)) * weights.y +
                         octahedral_decode(snorm16x2(c.normal)) * weights.z;
    // Turned by the instance's rotation and scale, as its G-buffer normal is.
    float3 normal = float3(dot(instance.transform.row0.linear, local), dot(instance.transform.row1.linear, local),
                           dot(instance.transform.row2.linear, local));
    normal = normalize(dot(normal, normal) > 1e-12 ? normal : -direction);
    if (dot(normal, direction) > 0.0)
    {
        normal = -normal; // the side the ray met
    }
    const GpuMaterial material = scene.materials[part.material];
    const float4 base =
        sample_texture_level(material.base_color_texture, kSamplerLinearWrap, uv, mip) * material.base_color;
    const float3 orm = sample_texture_level(material.metallic_roughness_texture, kSamplerLinearWrap, uv, mip).rgb;
    TraceSurface surface;
    surface.position = origin + direction * hit.distance;
    surface.normal = normal;
    surface.base_color = base.rgb;
    surface.emissive =
        sample_texture_level(material.emissive_texture, kSamplerLinearWrap, uv, mip).rgb * material.emissive;
    surface.roughness = saturate(orm.g * material.roughness);
    surface.metalness = saturate(orm.b * material.metallic);
    surface.flags = material.flags;
    return surface;
}
