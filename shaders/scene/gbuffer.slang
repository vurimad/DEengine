// The G-buffer pass: every mesh instance writes its surface (base color, normal,
// roughness, metalness, occlusion, translucency, lighting group) and its emissive light straight
// into the HDR target. Vertices are pulled by address; materials come from the GpuAssets table.

import common.bindless;
import scene.scene_common;
import scene.wind;

struct GBufferPush
{
    SceneFrame* frame;
    GpuInstance* instances;
    MeshVertex* vertices;
    GpuMaterial* materials;
    GpuDraw* draws;
    uint sampler;
    uint depth;      // the editor's selection mask: the scene's depth, to tell what of the selection is hidden
    uint unjittered; // 1: through the view without TAA's jitter (the selection mask: an outline that keeps still)
};

[[vk::push_constant]]
ConstantBuffer<GBufferPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD;
    float4 clip : CLIP;                   // where the vertex is, and was last frame: the motion vector
    float4 previous_clip : PREVIOUS_CLIP;
    nointerpolation uint material : MATERIAL;
    nointerpolation float fade : FADE;
#ifdef EDITOR_IDS
    nointerpolation uint instance : INSTANCE;
#endif
};

struct GBufferOutput
{
    float4 gb0 : SV_Target0;
    float2 gb1 : SV_Target1;
    float4 gb2 : SV_Target2;
    float4 emissive : SV_Target3;
    float2 motion : SV_Target4; // uv now minus uv last frame
#ifdef EDITOR_IDS
    uint id : SV_Target5; // the editor's: the instance's slot plus one, 0 for none
#endif
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VulkanVertexID, uint draw_id: SV_VulkanInstanceID)
{
    const MeshVertex vertex = push.vertices[vertex_id];
    const GpuDraw draw = push.draws[draw_id];
    const GpuInstance instance = push.instances[draw.instance];
    const uint3 origin_steps = push.frame.render_origin_steps.xyz;
    float3 world = instance_point(instance.transform, origin_steps, vertex.position);
    float3 previous_world = instance_point(instance.previous_transform, origin_steps, vertex.position);
    // The inverse transpose would be exact under non-uniform scale; renormalized in the pixel
    // shader, the 3x3 is close for the uniform scales cells use.
    const float3x3 linear =
        float3x3(instance.transform.row0.linear, instance.transform.row1.linear, instance.transform.row2.linear);
    const float3 normal = mul(linear, octahedral_decode(snorm16x2(vertex.normal)));
    // Plants move in the wind, about their own root; the same for every vertex of a draw, so no divergence.
    if ((push.materials[draw.material].flags & kMaterialWind) != 0u)
    {
        const float bend = push.materials[draw.material].wind_bend_m;
        const float flutter = push.materials[draw.material].wind_flutter_m;
        const float4 color = unorm8x4(vertex.color);
        const float3 unit_normal = normalize(normal);
        world += plant_sway(push.frame, instance_point(instance.transform, origin_steps, float3(0.0)), world, unit_normal,
                            color, bend, flutter, false);
        previous_world += plant_sway(push.frame, instance_point(instance.previous_transform, origin_steps, float3(0.0)),
                                     previous_world, unit_normal, color, bend, flutter, true);
    }

    VertexOutput output;
    output.position = mul_rows(push.frame.view_proj, float4(world, 1.0));
    output.clip = mul_rows(push.frame.unjittered_view_proj, float4(world, 1.0)); // TAA's offset is no motion
    if (push.unjittered != 0u)
    {
        output.position = output.clip;
    }
    output.previous_clip = mul_rows(push.frame.previous_view_proj, float4(previous_world, 1.0));
    output.normal = normal;
    const float4 tangent = snorm8x4(vertex.tangent);
    output.tangent = float4(mul(linear, tangent.xyz), tangent.w);
    output.uv = vertex.uv;
    output.material = draw.material;
    output.fade = draw.fade;
#ifdef EDITOR_IDS
    output.instance = draw.instance;
#endif
    return output;
}

// Whether a LOD gives this pixel to the other one it's fading with. Both ask of the same value, so each pixel gets exactly
// one. Interleaved gradient noise spreads any share evenly over the pixels. The golden ratio per TAA sample then spreads it
// evenly over the frames at each pixel, so the history blends it.
bool faded_out(float fade, float2 pixel)
{
    const float dither = frac(gradient_noise(floor(pixel)) + 0.61803399 * push.frame.taa_params.y);
    return fade > 0.0 ? dither >= fade : dither < -fade;
}

// The surface into the G-buffer, past any alpha test.
GBufferOutput write_surface(VertexOutput input, bool front_face, GpuMaterial material, float3 base)
{
    // Tangent-space normal from the map's x, y (z rebuilt), into world space.
    float2 xy = sample_texture(material.normal_texture, push.sampler, input.uv).xy * 2.0 - 1.0;
    xy *= material.normal_scale;
    const float3 tangent_normal = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
    // The tangent frame from the front face's normal; a back face is the mirror image: the same
    // tangent and bitangent, the normal flipped (flipping it first would flip the bitangent too).
    const float3 geometric = normalize(input.normal);
    const float3 t = normalize(input.tangent.xyz - geometric * dot(geometric, input.tangent.xyz));
    const float3 b = cross(geometric, t) * input.tangent.w;
    const bool back = (material.flags & kMaterialDoubleSided) != 0 && !front_face;
    const float3 n = normalize(t * tangent_normal.x + b * tangent_normal.y +
                               (back ? -geometric : geometric) * tangent_normal.z);

    // glTF's packing: occlusion in r, roughness in g, metalness in b.
    const float3 orm = sample_texture(material.metallic_roughness_texture, push.sampler, input.uv).rgb;
    const float occlusion =
        lerp(1.0, sample_texture(material.occlusion_texture, push.sampler, input.uv).r, material.occlusion_strength);
    const float3 emissive = sample_texture(material.emissive_texture, push.sampler, input.uv).rgb * material.emissive;

    GBufferOutput output;
    output.gb0 = float4(base, 0.0); // lighting group 0 until materials carry one
    output.gb1 = octahedral_encode(n) * 0.5 + 0.5;
    output.gb2 = float4(saturate(orm.g * material.roughness), saturate(orm.b * material.metallic), occlusion, 0.0);
    output.emissive = float4(emissive, 1.0);
    output.motion = motion_between(input.clip, input.previous_clip);
#ifdef EDITOR_IDS
    output.id = input.instance + 1u;
#endif
    return output;
}

GBufferOutput opaque_surface(VertexOutput input, bool front_face)
{
    const GpuMaterial material = push.materials[input.material];
    const float3 base = sample_texture(material.base_color_texture, push.sampler, input.uv).rgb * material.base_color.rgb;
    return write_surface(input, front_face, material, base);
}

GBufferOutput masked_surface(VertexOutput input, bool front_face)
{
    const GpuMaterial material = push.materials[input.material];
    const float4 base = sample_texture(material.base_color_texture, push.sampler, input.uv) * material.base_color;
    if (base.a < material.alpha_cutoff)
    {
        discard;
    }
    return write_surface(input, front_face, material, base.rgb);
}

// Opaque materials: no discard anywhere in the shader, so depth is tested before it runs.
[shader("fragment")]
GBufferOutput fragment_opaque(VertexOutput input, bool front_face: SV_IsFrontFace)
{
    return opaque_surface(input, front_face);
}

// Masked materials (the renderer picks this one by their flag): the alpha test.
[shader("fragment")]
GBufferOutput fragment_masked(VertexOutput input, bool front_face: SV_IsFrontFace)
{
    return masked_surface(input, front_face);
}

// What's fading between LODs. The discard costs the early depth test, so only this has it.
[shader("fragment")]
GBufferOutput fragment_opaque_fading(VertexOutput input, bool front_face: SV_IsFrontFace)
{
    if (faded_out(input.fade, input.position.xy))
    {
        discard;
    }
    return opaque_surface(input, front_face);
}

[shader("fragment")]
GBufferOutput fragment_masked_fading(VertexOutput input, bool front_face: SV_IsFrontFace)
{
    if (faded_out(input.fade, input.position.xy))
    {
        discard;
    }
    return masked_surface(input, front_face);
}

// The editor's selection mask: what of the screen the selection covers (x), and where it's seen (y): no nearer surface in
// the scene's depth. That depth has TAA's jitter and this hasn't, and the selection's far LODs may be what the scene
// drew: the slack (1% of the distance, reverse Z) covers both.
[shader("fragment")]
float2 fragment_selection(VertexOutput input) : SV_Target0
{
    const GpuMaterial material = push.materials[input.material];
    if ((material.flags & kMaterialMasked) != 0u)
    {
        const float alpha = sample_texture(material.base_color_texture, push.sampler, input.uv).a * material.base_color.a;
        if (alpha < material.alpha_cutoff)
        {
            discard;
        }
    }
    const float scene = load_texture(push.depth, int2(input.position.xy)).r;
    return float2(1.0, input.position.z >= scene * 0.99 ? 1.0 : 0.0);
}
