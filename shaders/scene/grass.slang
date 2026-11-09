// Grass into the G-buffer: blades built in the vertex shader from what the scatter wrote. A blade's vertex number says
// where along it and which side: rows 0 to 4, two to a row but for the tip, and the coarser levels of detail use fewer of
// the same vertices. The further off a blade is, the more it turns to face the camera, as a low view would otherwise see
// past thin blades to the ground, and one still near edge-on is thickened: thin things seen edge-on are also what makes
// grass crawl when the camera moves. Up close they keep most of their own direction, so they don't swivel as you pass.

import scene.grass_common;
import scene.scene_common;
import scene.wind;

// Matches GrassDrawPush in engine/render/scene/terrain.cpp.
struct GrassDrawPush
{
    SceneFrame* frame;
    GrassBlade* blades;
    float3 color_base;
    float width_m;
    float3 color_tip;
    float height_low_m;
    float height_high_m;
    float screen_height;
    float padding0;
    float padding1;
    float3 other_base;
    float padding2;
    float3 other_tip;
    float padding3;
};

[[vk::push_constant]]
ConstantBuffer<GrassDrawPush> push;

static const float kRows = 4.0;
static const float kTurnNear = 0.3;
static const float kTurnFar = 0.97;
static const float kTurnFromM = 2.0;
static const float kTurnToM = 25.0;
static const float kEdgeOnThicken = 1.6;
// A blade's own normal gives way to straight up over this range: far off, its lighting is all noise and no shape.
static const float kNormalFadeFromM = 8.0;
static const float kNormalFadeToM = 40.0;
// The most a blade leans over in the wind, as a share of its height, and how fast it gets there per m/s.
static const float kMostLean = 0.8;
static const float kLeanPerMps = 0.1;
// Its quick bob dies out over this range: far off, only the slow lean of gusts rolling across the field shows.
static const float kBobFromM = 20.0;
static const float kBobToM = 55.0;

// The bob is a wave running with the wind, this long: blades in a row move together, as a field does, rather than each
// on its own. With kBobHz it travels at about 4.4 m/s.
static const float kBobWavelengthM = 3.5;

// Where the wind puts a blade's tip, now or a frame ago, as a share of its height: over toward the wind, more the harder
// it blows, never lying flat, and bobbing in it up close. What a blade feels is the wind times the ripple it's in, so
// waves sweep through the field. Neighbours agree: each differs from the next only a little in stiffness and timing
// (from `character`, 0 to 1).
float2 blade_lean(SceneFrame* frame, float3 root, float character, float near, bool previous)
{
    const float2 w = wind_at(frame, root.xz, previous);
    const float speed = length(w);
    if (speed < 1e-3)
    {
        return float2(0.0);
    }
    const float felt = speed * (0.35 + 1.3 * wind_ripple(frame, root.xz, previous));
    const float stiffness = 0.85 + 0.3 * character;
    const float lean = kMostLean * (1.0 - exp(-felt * kLeanPerMps / stiffness));
    const float2 world = root.xz + float2(int2(frame.render_origin_steps.xz)) * kMetersPerRenderStep;
    const float wave = dot(world, frame.wind.xy) / kBobWavelengthM;
    const float bob = sin(6.2831853 * (kBobHz * wind_time(frame, previous) - wave + 0.08 * character)) *
                      (0.1 + 0.2 * saturate(speed / 8.0)) * lean * near;
    return w / speed * min(lean + bob, kMostLean);
}

float grass_hash(int2 cell)
{
    uint h = uint(cell.x) * 0x8da6b343u ^ uint(cell.y) * 0xd8163841u;
    h ^= h >> 13u;
    h *= 0x5bd1e995u;
    h ^= h >> 15u;
    return float(h & 0xffffu) / 65535.0;
}

float grass_noise(float2 at)
{
    const float2 cell = floor(at);
    const float2 f = at - cell;
    const float2 s = f * f * (3.0 - 2.0 * f);
    const int2 c = int2(cell);
    return lerp(lerp(grass_hash(c), grass_hash(c + int2(1, 0)), s.x), lerp(grass_hash(c + int2(0, 1)), grass_hash(c + int2(1, 1)), s.x), s.y);
}

// How well grass grows here, 0 to 1, in patches a few meters across: tall and lush, or short and dry. In the world's
// own coordinates, so it stays put as the render origin moves.
float grass_patch(SceneFrame* frame, float3 root)
{
    const float2 world = root.xz + float2(int2(frame.render_origin_steps.xz)) * kMetersPerRenderStep;
    return smoothstep(0.2, 0.8, grass_noise(world / 9.0) * 0.7 + grass_noise(world / 3.5 + 17.0) * 0.3);
}

// A point on a blade, at a share of the way up, leaning by `lean`: it bends along its length, and doesn't stretch.
float3 blade_point(float3 root, float height_m, float along, float3 own_lean, float2 lean)
{
    const float curve = along * along;
    const float drop = 1.0 - 0.45 * dot(lean, lean) * along;
    return root + float3(0.0, height_m * along * drop, 0.0) + own_lean * height_m * curve + float3(lean.x, 0.0, lean.y) * height_m * curve;
}

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 color : COLOR;
    float along : ALONG;
    float4 clip : CLIP;
    float4 previous_clip : PREVIOUS_CLIP;
};

struct GBufferOutput
{
    float4 gb0 : SV_Target0;
    float2 gb1 : SV_Target1;
    float4 gb2 : SV_Target2;
    float4 emissive : SV_Target3;
    float2 motion : SV_Target4;
#ifdef EDITOR_IDS
    uint id : SV_Target5; // the editor's: the instance's slot plus one, 0 for none
#endif
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VulkanVertexID, uint instance: SV_VulkanInstanceID)
{
    const GrassBlade blade = push.blades[instance];
    const float yaw = float(blade.packed & 0xffu) / 255.0 * 6.2831853;
    const float height_share = float((blade.packed >> 8u) & 0xffu) / 255.0;
    const float tone = float((blade.packed >> 16u) & 0xffu) / 255.0;
    const float lean_share = float((blade.packed >> 24u) & 0xfu) / 15.0;
    const float other_share = float((blade.packed >> 28u) & 0xfu) / 15.0;

    const float along = float(vertex_id / 2u) / kRows;
    const float side = (vertex_id & 1u) != 0u ? 1.0 : -1.0;
    // The blade's own: a phase from its yaw, and a share for whatever else differs between neighbours.
    const float phase = yaw / 6.2831853;
    const float character = frac(phase * 7.31 + height_share * 3.17 + tone * 1.93);
    // Taller where the patch grows well, shorter where it doesn't, and now and then one standing above the rest.
    const float patch = grass_patch(push.frame, blade.root);
    const float standout = frac(character * 13.7) > 0.94 ? 1.4 : 1.0;
    const float height_m = lerp(push.height_low_m, push.height_high_m, height_share) * saturate(height_share * 4.0) *
                           lerp(0.55, 1.45, patch) * standout;

    const float3 camera = push.frame.camera_position.xyz;
    const float2 to_camera_flat = camera.xz - blade.root.xz;
    const float distance_m = max(length(to_camera_flat), 0.1);
    const float2 to_camera = to_camera_flat / distance_m;
    const float2 own = float2(cos(yaw), sin(yaw));
    const float2 facing_camera = float2(-to_camera.y, to_camera.x) * (dot(own, float2(-to_camera.y, to_camera.x)) >= 0.0 ? 1.0 : -1.0);
    const float turn = lerp(kTurnNear, kTurnFar, smoothstep(kTurnFromM, kTurnToM, distance_m));
    const float2 across_flat = normalize(lerp(own, facing_camera, turn));
    const float edge_on = 1.0 - abs(dot(across_flat, facing_camera));
    const float3 across = float3(across_flat.x, 0.0, across_flat.y);
    const float3 lean = float3(-own.y, 0.0, own.x) * (0.15 + 0.4 * lean_share);

    // Wider where fewer are left, and never much under a pixel. Row 1 of the projection is f times the view's up axis.
    const float pixel_m = distance_m * 2.0 / (length(push.frame.unjittered_view_proj[1].xyz) * push.screen_height);
    const float kept = grass_kept(distance_m);
    const float taper = 1.0 - along * along;
    const float width_m = max(push.width_m * pow(kept, -0.55) * (1.0 + kEdgeOnThicken * edge_on), pixel_m * 1.1) * taper;

    // In the wind, now and a frame ago, for the motion vector: TAA would smear motion it isn't told of.
    const float near = 1.0 - smoothstep(kBobFromM, kBobToM, distance_m);
    const float2 wind_lean = blade_lean(push.frame, blade.root, character, near, false);
    const float2 previous_wind_lean = blade_lean(push.frame, blade.root, character, near, true);
    const float3 sideways = across * side * width_m * 0.5;
    const float3 at = blade_point(blade.root, height_m, along, lean, wind_lean) + sideways;
    const float3 previous_at = blade_point(blade.root, height_m, along, lean, previous_wind_lean) + sideways;

    VertexOutput output;
    output.position = mul_rows(push.frame.view_proj, float4(at, 1.0));
    output.clip = mul_rows(push.frame.unjittered_view_proj, float4(at, 1.0));
    output.previous_clip = mul_rows(push.frame.previous_view_proj, float4(previous_at, 1.0));
    // Mostly up, a little toward the camera and rounded across: lit about as the ground is, so a blade seen against the
    // sun doesn't go black, and the field keeps one light.
    const float3 facing = normalize(float3(to_camera.x * 0.35, 1.0, to_camera.y * 0.35) + across * side * 0.3);
    const float fade = smoothstep(kNormalFadeFromM, kNormalFadeToM, distance_m);
    output.normal = normalize(lerp(facing, float3(0.0, 1.0, 0.0), fade * 0.85));
    const float toward_tip = pow(along, 1.3);
    float3 color = lerp(lerp(push.color_base, push.color_tip, toward_tip), lerp(push.other_base, push.other_tip, toward_tip), other_share);
    // Along the blade: its tip drying out warmer, more on some blades than others.
    const float dry = smoothstep(0.6, 1.0, along) * (0.12 + 0.4 * frac(character * 5.3));
    color = lerp(color, color * float3(1.35, 1.15, 0.6), dry);
    // Each blade a slightly different green, and each patch: lusher where it grows tall, drier where short.
    const float hue = frac(character * 11.9) - 0.5;
    color *= float3(1.0 + 0.1 * hue, 1.0, 1.0 - 0.16 * hue) * lerp(float3(1.08, 1.0, 0.82), float3(0.93, 1.03, 1.05), patch);
    output.color = color * (0.7 + 0.6 * tone);
    output.along = along;
    return output;
}

[shader("fragment")]
GBufferOutput fragment_main(VertexOutput input)
{
    GBufferOutput output;
    output.gb0 = float4(input.color, float(kLightingGroupGround) / 255.0);
    output.gb1 = octahedral_encode(normalize(input.normal)) * 0.5 + 0.5;
    // Rough; dark at the root, as the blades around it shade it; and light comes through.
    output.gb2 = float4(0.8, 0.0, lerp(0.4, 1.0, input.along), 0.7);
    output.emissive = float4(0.0, 0.0, 0.0, 1.0);
    output.motion = motion_between(input.clip, input.previous_clip);
#ifdef EDITOR_IDS
    output.id = 0u; // the ground and its grass aren't picked
#endif
    return output;
}
