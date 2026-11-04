// Temporal anti-aliasing: each frame is drawn with a different sub-pixel offset, and each pixel blends
// into its own history, reprojected by motion. What keeps history from ghosting, in layers:
//   - Motion comes from the nearest surface around the pixel, so an object's edges travel with it.
//   - History is clipped to the colours around the pixel now: a colour that left can't stay.
//   - History is dropped where the motion at its old place disagrees with the motion here (it was
//     about something else), where the depth there says another surface was in front (something
//     moved off it), and off the screen. A dropped pixel takes a soft blur of this frame.
// All of it in a tonemapped space, so a bright pixel neither dominates its neighbours nor flickers.

import common.bindless;
import scene.scene_common;

struct TaaPush
{
    SceneFrame* frame;
    uint current;  // fogged, this frame
    uint history;  // last frame's resolved image
    uint depth;
    uint previous_depth;
    uint motion;
    uint previous_motion;
    uint sampler;      // linear clamp
    uint has_history;  // 0: the first frame, after a cut or a gap: no history at all
    float exposure;    // what the tonemap will multiply by: the tonemapped space matches what's seen
};

[[vk::push_constant]]
ConstantBuffer<TaaPush> push;

// How much of the history a pixel keeps, when nothing argues against it.
static const float kFeedback = 0.92;
// Depth may differ by this share before it counts against the history.
static const float kDepthSlack = 0.01;

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

float3 tonemapped(float3 color)
{
    const float3 exposed = max(color, 0.0) * push.exposure;
    return exposed / (1.0 + max(exposed.r, max(exposed.g, exposed.b)));
}

float3 untonemapped(float3 color)
{
    return color / max(1.0 - max(color.r, max(color.g, color.b)), 1e-4) / push.exposure;
}

float3 to_ycocg(float3 c)
{
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 from_ycocg(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// The point on the segment from a colour inside the box (the neighbours' mean) to this colour where it
// leaves the box, or the colour if it doesn't: unlike a clamp per channel, it keeps the colour's
// direction, and so its hue.
float3 clip_to_box(float3 color, float3 box_min, float3 box_max, float3 inside)
{
    const float3 along = color - inside;
    const float3 room = max(select(along > 0.0, box_max - inside, inside - box_min), 0.0);
    const float3 ratios = room / max(abs(along), 1e-6);
    const float share = min(ratios.x, min(ratios.y, ratios.z));
    return share < 1.0 ? inside + along * share : color;
}

// Catmull-Rom over the history, in five bilinear taps: it stays sharp where bilinear would blur a
// little more every frame.
float3 sample_history(float2 uv, float2 size)
{
    const float2 position = uv * size;
    const float2 center = floor(position - 0.5) + 0.5;
    const float2 f = position - center;
    const float2 f2 = f * f;
    const float2 f3 = f2 * f;
    const float2 w0 = -0.5 * f3 + f2 - 0.5 * f;
    const float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    const float2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    const float2 w3 = 0.5 * f3 - 0.5 * f2;
    const float2 w12 = w1 + w2;
    const float2 middle = (center + w2 / w12) / size;
    const float2 before = (center - 1.0) / size;
    const float2 after = (center + 2.0) / size;
    const float weights[5] = { w12.x * w0.y, w0.x * w12.y, w12.x * w12.y, w3.x * w12.y, w12.x * w3.y };
    const float2 taps[5] = { float2(middle.x, before.y), float2(before.x, middle.y), middle, float2(after.x, middle.y),
                             float2(middle.x, after.y) };
    float3 sum = float3(0.0);
    float total = 0.0;
    for (uint i = 0; i < 5; ++i)
    {
        sum += sample_texture_level(push.history, push.sampler, taps[i], 0.0).rgb * weights[i];
        total += weights[i];
    }
    return max(sum / total, 0.0);
}

float2 uv_of(float4 clip)
{
    return float2(clip.x, -clip.y) / clip.w * 0.5 + 0.5;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    SceneFrame* frame = push.frame;
    const int2 texel = int2(input.position.xy);
    const float2 size = float2(texture_size(push.current));
    const float2 uv = input.position.xy / size;

    // The pixels around this one: their colours, and the nearest surface (reverse Z: the largest depth).
    float3 box_min = float3(1e30);
    float3 box_max = float3(-1e30);
    float3 sum = float3(0.0);
    float3 blurred = float3(0.0);
    float nearest = -1.0;
    int2 nearest_texel = texel;
    float3 here = float3(0.0);
    const int2 last = int2(size) - 1;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const int2 at = clamp(texel + int2(x, y), int2(0), last);
            const float3 color = tonemapped(load_texture(push.current, at).rgb);
            const float3 ycocg = to_ycocg(color);
            box_min = min(box_min, ycocg);
            box_max = max(box_max, ycocg);
            sum += ycocg;
            blurred += color * (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0) / 16.0;
            if (x == 0 && y == 0)
            {
                here = color;
            }
            const float depth = load_texture(push.depth, at).r;
            if (depth > nearest)
            {
                nearest = depth;
                nearest_texel = at;
            }
        }
    }

    // Where this point was: by its surface's motion; the sky moves with the camera alone.
    const float center_depth = load_texture(push.depth, texel).r;
    const float3 position = position_from_depth(frame, input.ndc, center_depth);
    const float2 camera_motion = uv_of(mul_rows(frame.unjittered_view_proj, float4(position, 1.0))) -
                                 uv_of(mul_rows(frame.previous_view_proj, float4(position, 1.0)));
    const float2 motion = nearest > 0.0 ? load_texture(push.motion, nearest_texel).rg : camera_motion;
    const float2 previous_uv = uv - motion;

    float weight = kFeedback;
    if (push.has_history == 0 || any(previous_uv < 0.0) || any(previous_uv > 1.0))
    {
        weight = 0.0;
    }
    else
    {
        // The motion at its old place: if it differs, the history there was about something else. Not
        // for the sky, whose motion isn't written: it has nothing to compare, and it isn't covering.
        const int2 previous_texel = clamp(int2(previous_uv * size), int2(0), last);
        if (nearest > 0.0)
        {
            const float2 motion_px = motion * size;
            const float2 previous_px = load_texture(push.previous_motion, previous_texel).rg * size;
            const float a = dot(motion_px, motion_px);
            const float b = dot(previous_px, previous_px);
            weight *= (min(a, b) + 1.0) / (max(a, b) + 1.0);
        }

        // The depth at its old place, where only the camera moved it (a moving surface's own motion
        // along the view isn't known, and its colour is clipped anyway): a surface that was in front
        // there has moved off, and what's here wasn't seen. Over the texels around it, so neither a
        // slope at a grazing angle nor an edge the jitter moves counts against itself. The sky counts
        // like any other depth: very far.
        if (length((motion - camera_motion) * size) < 0.5)
        {
            const float expected = mul_rows(frame.previous_view_proj, float4(position, 1.0)).w;
            float near_z = 1e30;
            float far_z = 0.0;
            for (int y = -1; y <= 1; ++y)
            {
                for (int x = -1; x <= 1; ++x)
                {
                    const float depth = load_texture(push.previous_depth, clamp(previous_texel + int2(x, y), int2(0), last)).r;
                    const float z = frame.taa_params.y / max(depth, 1e-7);
                    near_z = min(near_z, z);
                    far_z = max(far_z, z);
                }
            }
            const float outside = max(near_z * (1.0 - kDepthSlack) - expected, expected - far_z * (1.0 + kDepthSlack));
            weight *= saturate(1.0 - outside / expected * 20.0);
        }
    }

    float3 resolved;
    if (weight <= 0.001)
    {
        resolved = lerp(here, blurred, 0.5); // no history: soft, not raw aliasing
    }
    else
    {
        const float3 history = to_ycocg(tonemapped(sample_history(previous_uv, size)));
        const float3 clipped = from_ycocg(clip_to_box(history, box_min, box_max, sum / 9.0));
        resolved = lerp(here, clipped, weight);
    }
    const float3 color = untonemapped(saturate(resolved));
    return float4(any(isnan(color)) ? float3(0.0) : min(color, kHalfMax), 1.0);
}
