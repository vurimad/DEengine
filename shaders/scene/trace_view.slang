// A debug view of what rays see (docs/design.md, Global illumination: step 1): each pixel shoots one ray from the
// camera into the scene's acceleration structure and shows what it met, as the surface there says it is: its
// colour from its material, its normal from its vertices, lit plainly by the sun (a second ray says whether the sun
// reaches it) and the sky's ambient. Where it differs from the lit view, rays and the picture disagree about the
// scene: a mesh with no bottom level, an instance in the wrong place, a part read as another's.

import common.bindless;
import scene.brdf;
import scene.scene_common;
import scene.sky;
import scene.trace;

struct TraceViewPush
{
    SceneFrame* frame;
};

[[vk::push_constant]]
ConstantBuffer<TraceViewPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    VertexOutput output;
    output.position = fullscreen_triangle(vertex_id, output.ndc);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    SceneFrame* frame = push.frame;
    // Through the pixel: from the camera toward its point on the near plane.
    const float3 origin = frame.camera_position.xyz;
    const float3 direction = normalize(position_from_depth(frame, input.ndc, 1.0) - origin);
    const TraceHit hit = trace_nearest(frame.trace, origin, direction, 0.0, 100000.0);
    if (!hit.hit)
    {
        return float4(min(sky(frame, direction), kHalfMax), 1.0);
    }
    const TraceSurface surface = trace_surface(frame.trace, hit, origin, direction);
    const float3 l = frame.sun_direction.xyz;
    const float facing = saturate(dot(surface.normal, l));
    // Off the surface by a little, so the ray doesn't meet where it starts.
    const bool shadowed =
        facing > 0.0 && trace_blocked(frame.trace, surface.position + surface.normal * 0.02, l, 0.0, 100000.0);
    // As the deferred light has them: the ambient as it is, the sun over pi.
    const float3 falling = hemisphere(frame, surface.normal) + frame.sun_color.rgb * (shadowed ? 0.0 : facing / kPi);
    return float4(min(surface.base_color * falling + surface.emissive, kHalfMax), 1.0);
}
