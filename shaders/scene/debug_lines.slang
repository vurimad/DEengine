// The debug views' lines (dev builds: physics' colliders), over the finished image: where they're in view solid, and
// fainter where the scene stands in front of them (two pipelines, one depth test each against the scene's depth).
// Through the camera without TAA's jitter, so they keep still, and nudged toward it so lines lying on a surface aren't
// lost in it.

import common.color;
import scene.scene_common;

struct DebugVertex
{
    float3 position; // relative to the render origin
    uint color;      // rgba8, red low, as seen (sRGB)
};

struct DebugLinesPush
{
    SceneFrame* frame;
    DebugVertex* vertices;
    float alpha;      // how much of each line's own alpha shows: all where in view, less where hidden
    uint srgb_target; // the target encodes to sRGB: the colors are given as seen, so they're decoded first
};

[[vk::push_constant]]
ConstantBuffer<DebugLinesPush> push;

// Reverse Z: a share nearer in depth, at any distance.
static const float kNearerShare = 0.002;

struct VertexOutput
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    const DebugVertex vertex = push.vertices[vertex_id];
    VertexOutput output;
    output.position = mul_rows(push.frame.unjittered_view_proj, float4(vertex.position, 1.0));
    output.position.z = min(output.position.z * (1.0 + kNearerShare), output.position.w);
    const uint c = vertex.color;
    float4 color = float4(float(c & 0xffu), float((c >> 8u) & 0xffu), float((c >> 16u) & 0xffu), float(c >> 24u)) / 255.0;
    if (push.srgb_target != 0u)
    {
        color.rgb = srgb_to_linear(color.rgb);
    }
    output.color = float4(color.rgb, color.a * push.alpha);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    return input.color;
}
