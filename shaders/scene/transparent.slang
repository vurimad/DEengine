// The transparent pass (docs/design.md, Rendering: frame passes): what blends (asset::AlphaMode::Blend), drawn over the
// lit, fogged image, the farthest first (scene.blend_sort), tested against the scene's depth and writing none.
//
// Lit here as the deferred pass lights what's solid: the sun through its shadow cascades, and the sky/ground ambient.
// Mixed premultiplied, one blend state for all of it: a surface's own colour counts by its opacity, and what it reflects
// and emits counts in full, so clear glass still shines, and a glow with no opacity at all only adds light. The fog
// pass has run already, so the fog in front of each surface is applied here. Every surface is drawn from both sides.
//
// Its second target adds up how much of each pixel see-through things cover: they write no depth and no motion, so
// temporal anti-aliasing trusts its history less there (scene.taa).

import common.bindless;
import common.fog;
import scene.brdf;
import scene.forward;
import scene.local_lights;
import scene.scene_common;
import scene.sky;

struct TransparentPush
{
    SceneFrame* frame;
    GpuInstance* instances;
    MeshVertex* vertices;
    GpuMaterial* materials;
    GpuDraw* draws;
    uint sampler;      // for the materials' textures
    uint atlas;        // the sun's shadow atlas
    uint integrated;   // the froxel grid, integrated (scene.fog_integrate); unused without one
    uint grid_sampler; // linear clamp
    uint fog;          // 1: what's behind was fogged, so this is too
    uint froxels;      // 1: there is a grid
};

[[vk::push_constant]]
ConstantBuffer<TransparentPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float3 world : WORLD; // relative to the render origin
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD;
    float4 clip : CLIP; // as position: where on the screen, for the fog's grid
    nointerpolation uint material : MATERIAL;
};

struct TransparentOutput
{
    float4 color : SV_Target0;    // premultiplied: rgb, over what's behind times (1 - a)
    float4 coverage : SV_Target1; // r and a alike: how much of the pixel is covered, mixed over what was, as the colour is
};

// Under this opacity, what a surface reflects fades out with it: a glow has no surface to shine.
static const float kSurfaceOpacity = 0.05;

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VulkanVertexID, uint draw_id: SV_VulkanInstanceID)
{
    const MeshVertex vertex = push.vertices[vertex_id];
    const GpuDraw draw = push.draws[draw_id];
    const GpuInstance instance = push.instances[draw.instance];
    const float3x3 linear =
        float3x3(instance.transform.row0.linear, instance.transform.row1.linear, instance.transform.row2.linear);

    VertexOutput output;
    output.world = instance_point(instance.transform, push.frame.render_origin_steps.xyz, vertex.position);
    output.position = mul_rows(push.frame.view_proj, float4(output.world, 1.0));
    output.clip = output.position;
    output.normal = mul(linear, octahedral_decode(snorm16x2(vertex.normal)));
    const float4 tangent = snorm8x4(vertex.tangent);
    output.tangent = float4(mul(linear, tangent.xyz), tangent.w);
    output.uv = vertex.uv;
    output.material = draw.material;
    return output;
}

[shader("fragment")]
TransparentOutput fragment_main(VertexOutput input, bool front_face: SV_IsFrontFace)
{
    SceneFrame* frame = push.frame;
    const GpuMaterial material = push.materials[input.material];
    const float4 base = sample_texture(material.base_color_texture, push.sampler, input.uv) * material.base_color;
    const float opacity = saturate(base.a);

    // The normal, as the G-buffer makes it; seen from behind, a pane is lit as its other face.
    float2 xy = sample_texture(material.normal_texture, push.sampler, input.uv).xy * 2.0 - 1.0;
    xy *= material.normal_scale;
    const float3 tangent_normal = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
    const float3 geometric = normalize(input.normal);
    const float3 t = normalize(input.tangent.xyz - geometric * dot(geometric, input.tangent.xyz));
    const float3 b = cross(geometric, t) * input.tangent.w;
    const float3 n = normalize(t * tangent_normal.x + b * tangent_normal.y +
                               (front_face ? geometric : -geometric) * tangent_normal.z);

    const float3 orm = sample_texture(material.metallic_roughness_texture, push.sampler, input.uv).rgb;
    const float roughness = saturate(orm.g * material.roughness);
    const float metalness = saturate(orm.b * material.metallic);
    const float occlusion =
        lerp(1.0, sample_texture(material.occlusion_texture, push.sampler, input.uv).r, material.occlusion_strength);
    const float3 emissive = sample_texture(material.emissive_texture, push.sampler, input.uv).rgb * material.emissive;

    const float3 to_camera = frame.camera_position.xyz - input.world;
    const float3 v = to_camera / max(length(to_camera), 1e-4);
    const float3 l = frame.sun_direction.xyz;
    const float3 half_vector = l + v;
    const float3 h = dot(half_vector, half_vector) > 1e-8 ? normalize(half_vector) : n;
    const float n_dot_v = max(dot(n, v), 1e-4);
    const float n_dot_l = saturate(dot(n, l));
    const float a = max(roughness * roughness, 2e-3);
    const float3 f0 = lerp(float3(0.04), base.rgb, metalness);
    const float3 albedo = base.rgb * (1.0 - metalness);
    const float3 fresnel = f_schlick(f0, saturate(dot(v, h)));

    const float view_depth = dot(-to_camera, frame.camera_forward.xyz);
    const float seen = sun_visibility(frame, push.atlas, input.world, n, view_depth);
    const float3 sun_light =
        frame.sun_color.rgb * lerp(frame.shadow_color.rgb * open_sky(frame, input.world), float3(1.0), seen);
    const float3 r = reflect(-v, n);
    const float3 diffuse =
        sun_light * albedo * (1.0 - fresnel) * n_dot_l / kPi +
        albedo * hemisphere(frame, n) * (occlusion * open_sky(frame, input.world));
    const float3 specular = sun_light * d_ggx(saturate(dot(n, h)), a) * v_smith(n_dot_v, n_dot_l, a) * fresnel * n_dot_l +
                            env_brdf(f0, roughness, n_dot_v) * hemisphere(frame, r) * (occlusion * open_sky(frame, input.world));
    // The local lights of its cluster, as on anything solid.
    LitSurface surface;
    surface.position = input.world;
    surface.normal = n;
    surface.to_camera = v;
    surface.albedo = albedo;
    surface.f0 = f0;
    surface.a = a;
    surface.specular_share = 1.0;
    surface.translucency = 0.0;
    const LocalLighting local = local_lights(frame, surface);
    float3 color = (diffuse + local.diffuse) * opacity +
                   (specular + local.specular) * saturate(opacity / kSurfaceOpacity) + emissive;

    if (push.fog != 0u)
    {
        float3 transmittance;
        float3 in_scatter;
        fog_in_front(frame, push.integrated, push.grid_sampler, push.froxels != 0u, input.world,
                     input.clip.xy / input.clip.w, transmittance, in_scatter);
        // The fog's own light in front of it, by how much of the pixel it covers: the rest is in what's behind already.
        color = color * transmittance + in_scatter * opacity;
    }

    TransparentOutput output;
    output.color = float4(min(color, kHalfMax), opacity);
    // A glow covers nothing and still changes the pixel: it counts by how bright it is.
    const float covered = saturate(max(opacity, max(emissive.r, max(emissive.g, emissive.b))));
    output.coverage = float4(covered, 0.0, 0.0, covered);
    return output;
}
