// The frame debugger's texture view (engine/tools/frame_debugger.h): one texture of the frame, as a pass left it,
// drawn into the view's own target. Any shape, mip and layer; zoomed and panned by texels, never filtered; one
// channel alone or several; a range of values stretched over the display. In probe mode it writes the one texel under
// the mouse as stored, into a pixel the CPU reads back.

import common.bindless;
import common.color;

// Matches ViewPush in engine/tools/frame_debugger.cpp.
struct ViewPush
{
    uint texture;            // bindless index: its table says its shape
    uint mip;
    uint layer;              // an array's layer, a cube's face (of a cube array: cube * 6 + face), a 3D slice
    uint flags;              // kFlag* below
    float origin_x;          // the source texel at the target's top-left pixel
    float origin_y;
    float texels_per_pixel;  // below 1: zoomed in
    float range_min;         // shown as black
    float range_max;         // shown as white
    float size_x;            // the mip's size in texels
    float size_y;
    int probe_x;             // probe mode: the texel to read
    int probe_y;
};

[[vk::push_constant]]
ConstantBuffer<ViewPush> push;

static const uint kFlagRed = 1u << 0;
static const uint kFlagGreen = 1u << 1;
static const uint kFlagBlue = 1u << 2;
static const uint kFlagAlpha = 1u << 3;
static const uint kFlagChannels = 15u;
static const uint kFlagSrgbTarget = 1u << 4; // the target encodes to sRGB
static const uint kFlagColor = 1u << 5;      // linear color, shown as the display would; else data, shown as stored
static const uint kFlagProbe = 1u << 6;
static const uint kFlagMarkBad = 1u << 7;    // NaN and infinity striped in magenta
static const uint kFlagIds = 1u << 8;        // an integer texture's values: each its own color

struct VertexOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    // One triangle over the whole target.
    const float2 corner = float2(float((vertex_id << 1) & 2u), float(vertex_id & 2u));
    VertexOutput output;
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

// Where a cube face's texel looks: faces +X -X +Y -Y +Z -Z, as Vulkan lays them out.
float3 cube_direction(uint face, float2 uv)
{
    const float2 st = uv * 2.0 - 1.0;
    switch (face)
    {
    case 0: return float3(1.0, -st.y, -st.x);
    case 1: return float3(-1.0, -st.y, st.x);
    case 2: return float3(st.x, 1.0, st.y);
    case 3: return float3(st.x, -1.0, -st.y);
    case 4: return float3(st.x, -st.y, 1.0);
    default: return float3(-st.x, -st.y, -1.0);
    }
}

// The texel as stored. An integer texture comes as floats, its value in red (debug.frame_debugger_ids).
float4 load_texel(int2 texel)
{
    const uint table = push.texture >> kEntryBits;
    const float2 uv = (float2(texel) + 0.5) / float2(push.size_x, push.size_y);
    if (table == kTableTexture2DArray)
    {
        return texture_2d_array(push.texture).Load(int4(texel, int(push.layer), int(push.mip)));
    }
    if (table == kTableTexture3D)
    {
        return texture_3d(push.texture).Load(int4(texel, int(push.layer), int(push.mip)));
    }
    if (table == kTableTextureCube)
    {
        return texture_cube(push.texture)
            .SampleLevel(sampler_state(kSamplerPointClamp), cube_direction(push.layer % 6u, uv), float(push.mip));
    }
    if (table == kTableTextureCubeArray)
    {
        return texture_cube_array(push.texture)
            .SampleLevel(sampler_state(kSamplerPointClamp),
                         float4(cube_direction(push.layer % 6u, uv), float(push.layer / 6u)), float(push.mip));
    }
    return texture_2d(push.texture).Load(int3(texel, int(push.mip)));
}

float3 linear_to_srgb(float3 color)
{
    const float3 low = color * 12.92;
    const float3 high = 1.055 * pow(max(color, 0.0), 1.0 / 2.4) - 0.055;
    return lerp(low, high, step(0.0031308, color));
}

// A color for an integer: neighbors far apart, zero black.
float3 id_color(uint id)
{
    if (id == 0u)
    {
        return float3(0.0, 0.0, 0.0);
    }
    uint h = id * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return 0.25 + 0.75 * float3(float(h & 255u), float((h >> 8) & 255u), float((h >> 16) & 255u)) / 255.0;
}

// What the display gets for a value meant to look as it is: decoded first where the target encodes.
float4 to_target(float3 shown, bool color)
{
    const bool srgb_target = (push.flags & kFlagSrgbTarget) != 0u;
    if (color)
    {
        // Linear light: the target's encoding, or ours, makes it look as a display shows it.
        return float4(srgb_target ? shown : linear_to_srgb(shown), 1.0);
    }
    return float4(srgb_target ? srgb_to_linear(shown) : shown, 1.0);
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    if ((push.flags & kFlagProbe) != 0u)
    {
        return load_texel(int2(push.probe_x, push.probe_y));
    }
    const float2 pixel = input.position.xy;
    const float2 at = float2(push.origin_x, push.origin_y) + pixel * push.texels_per_pixel;
    const float2 size = float2(push.size_x, push.size_y);
    if (at.x < 0.0 || at.y < 0.0 || at.x >= size.x || at.y >= size.y)
    {
        // Outside the texture: a quiet checker, so its edges show.
        const int2 square = int2(floor(pixel / 12.0));
        const float shade = ((square.x + square.y) & 1) != 0 ? 0.075 : 0.055;
        return to_target(float3(shade, shade, shade), false);
    }
    const float4 value = load_texel(int2(floor(at)));
    if ((push.flags & kFlagIds) != 0u)
    {
        return to_target(id_color(uint(value.r)), false);
    }
    if ((push.flags & kFlagMarkBad) != 0u && (any(isnan(value)) || any(isinf(value))))
    {
        const float stripe = frac((pixel.x + pixel.y) / 8.0) < 0.5 ? 1.0 : 0.0;
        return to_target(float3(1.0, 0.0, 1.0) * stripe, false);
    }
    const float span = max(push.range_max - push.range_min, 1e-12);
    const float4 scaled = saturate((value - push.range_min) / span);
    const uint channels = push.flags & kFlagChannels;
    float3 shown;
    switch (channels)
    {
    case kFlagRed: shown = scaled.rrr; break;
    case kFlagGreen: shown = scaled.ggg; break;
    case kFlagBlue: shown = scaled.bbb; break;
    case kFlagAlpha: shown = scaled.aaa; break;
    default:
        shown = scaled.rgb * float3((channels & kFlagRed) != 0u ? 1.0 : 0.0, (channels & kFlagGreen) != 0u ? 1.0 : 0.0,
                                    (channels & kFlagBlue) != 0u ? 1.0 : 0.0);
        break;
    }
    // Close enough to tell texels apart: a thin line between them.
    if (push.texels_per_pixel < 1.0 / 10.0)
    {
        const float2 within = frac(at) / push.texels_per_pixel;
        if (within.x < 1.0 || within.y < 1.0)
        {
            shown = lerp(shown, float3(0.5, 0.5, 0.5), 0.35);
        }
    }
    // One channel alone is a number, not a color, whatever the texture holds.
    const bool one = channels == kFlagRed || channels == kFlagGreen || channels == kFlagBlue || channels == kFlagAlpha;
    return to_target(shown, (push.flags & kFlagColor) != 0u && !one);
}
