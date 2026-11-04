// Temporal anti-aliasing: each frame is drawn with a different sub-pixel offset, and each pixel blends
// into its own history, reprojected by motion. What keeps history from ghosting, in layers:
//   - Motion comes from the nearest surface around the pixel, so an object's edges travel with it.
//   - History is clipped to the colours around the pixel now: a colour that left can't stay.
//   - History fades where the object motion at its old place (the camera's part taken out, kept from
//     last frame) differs from the object motion here: it was about something else. It goes where the
//     depth there says another surface was in front, and off the screen. A pixel loses its sharpness
//     as it loses its history: without any, it's a soft blur of this frame.
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
    uint previous_object_motion;
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
// The sky's depth along the view: farther than anything, and the same however the camera turns.
static const float kSkyZ = 1e30;
// Motion may differ by about this share of its speed: a fast thing's frame pacing isn't another thing.
static const float kMotionTolerance = 0.25;
// Object motion under this many pixels counts as none: only the camera moved the point.
static const float kStillPx = 0.05;
// More than this many of the 9 pixels around one share its depth: it isn't a sliver.
static const uint kSliverPixels = 4;
// Room in the colour box beyond its extent, so an axis with none (a neutral or single-hue area has no
// chroma to speak of) doesn't stop history that only rounding moved along it.
static const float kBoxPadding = 1e-3;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD;
};

struct ResolveOutput
{
    float4 color : SV_Target0;
    float2 object_motion : SV_Target1; // uv now minus uv last frame, without the camera's part
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    VertexOutput output;
    output.position = fullscreen_triangle(vertex_id, output.ndc);
    return output;
}

// A bad value counts as black: alone, not with everything that blends with it.
float3 tonemapped(float3 color)
{
    const float3 exposed = select(isfinite(color), max(color, 0.0), 0.0) * push.exposure;
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
    const float3 room = max(select(along > 0.0, box_max - inside, inside - box_min), 0.0) + kBoxPadding;
    const float3 ratios = room / max(abs(along), 1e-6);
    const float share = min(ratios.x, min(ratios.y, ratios.z));
    return share < 1.0 ? inside + along * share : color;
}

// Catmull-Rom over the history, tonemapped, in five bilinear taps: it stays sharp where bilinear would
// blur a little more every frame, and tonemapped, a highlight's negative lobes don't ring.
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
        sum += tonemapped(sample_texture_level(push.history, push.sampler, taps[i], 0.0).rgb) * weights[i];
        total += weights[i];
    }
    return max(sum / total, 0.0);
}

[shader("fragment")]
ResolveOutput fragment_main(VertexOutput input)
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
    float nearest = 0.0; // the sky, unless a surface is around
    int2 nearest_texel = texel;
    float3 here = float3(0.0);
    uint same_depth = 0;
    const float center_depth = load_texture(push.depth, texel).r;
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
            same_depth += abs(depth - center_depth) <= center_depth * kDepthSlack ? 1 : 0;
            if (depth > nearest)
            {
                nearest = depth;
                nearest_texel = at;
            }
        }
    }

    // Where this point was: by the nearest surface's motion. The sky has none written, and moves with
    // the camera alone. What's left without the camera's part is the object's own.
    const float2 nearest_ndc = input.ndc + float2(nearest_texel - texel) * float2(2.0, -2.0) / size;
    const float4 nearest_position = float4(position_from_depth(frame, nearest_ndc, nearest), 1.0);
    const float2 camera_motion =
        motion_between(mul_rows(frame.unjittered_view_proj, nearest_position), mul_rows(frame.previous_view_proj, nearest_position));
    const float2 motion = nearest > 0.0 ? load_texture(push.motion, nearest_texel).rg : camera_motion;
    const float2 object_motion = motion - camera_motion;
    const float2 previous_uv = uv - motion;

    float weight = kFeedback;
    if (push.has_history == 0 || any(previous_uv < 0.0) || any(previous_uv > 1.0))
    {
        weight = 0.0;
    }
    else
    {
        // The object motion at its old place: if it differs, the history there was about something else.
        const int2 previous_texel = clamp(int2(previous_uv * size), int2(0), last);
        const float2 here_px = object_motion * size;
        const float2 there_px = load_texture(push.previous_object_motion, previous_texel).rg * size;
        const float2 difference_px = here_px - there_px;
        const float speed_squared = max(dot(here_px, here_px), dot(there_px, there_px));
        weight /= 1.0 + dot(difference_px, difference_px) / (1.0 + kMotionTolerance * kMotionTolerance * speed_squared);

        // The depth at its old place, where only the camera moved it (a moving surface's own motion
        // along the view isn't known, and its colour is clipped anyway): a surface that was in front
        // there has moved off, and what's here wasn't seen. Over the texels around it, so neither a
        // slope at a grazing angle nor an edge the jitter moves counts against itself. A sliver the
        // jitter hits this frame and missed last isn't something that came in front.
        if (length(object_motion * size) < kStillPx)
        {
            float4 position = nearest_position;
            if (any(nearest_texel != texel))
            {
                position = float4(position_from_depth(frame, input.ndc, center_depth), 1.0);
            }
            const float expected = center_depth > 0.0 ? max(mul_rows(frame.previous_view_proj, position).w, 1e-4) : kSkyZ;
            float near_z = kSkyZ;
            float far_z = 0.0;
            for (int y = -1; y <= 1; ++y)
            {
                for (int x = -1; x <= 1; ++x)
                {
                    const float depth = load_texture(push.previous_depth, clamp(previous_texel + int2(x, y), int2(0), last)).r;
                    const float z = depth > 0.0 ? frame.taa_params.x / depth : kSkyZ;
                    near_z = min(near_z, z);
                    far_z = max(far_z, z);
                }
            }
            const float came = same_depth > kSliverPixels ? near_z * (1.0 - kDepthSlack) - expected : 0.0;
            const float outside = max(came, expected - far_z * (1.0 + kDepthSlack));
            weight *= saturate(1.0 - outside / expected * 20.0);
        }
    }

    // This frame softens as history goes: raw aliasing would show where nothing covers it.
    const float3 soft = lerp(here, blurred, 0.5 * (1.0 - weight / kFeedback));
    float3 kept = soft;
    if (weight > 0.0)
    {
        const float3 history = to_ycocg(sample_history(previous_uv, size));
        kept = from_ycocg(clip_to_box(history, box_min, box_max, sum / 9.0));
    }
    const float3 color = untonemapped(saturate(lerp(soft, kept, weight)));
    ResolveOutput output;
    output.color = float4(any(isnan(color)) ? float3(0.0) : min(color, kHalfMax), 1.0);
    output.object_motion = object_motion;
    return output;
}
