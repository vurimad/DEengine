// The sky and the ambient light, as the deferred light pass draws them and what reflects them (water) sees them: a
// gradient toward the horizon haze with the sun's disk, and a sky/ground hemisphere, the placeholder for image-based
// lighting.

import common.fog;
import scene.scene_common;

// The ambient from a direction: the ground below, the sky above.
public float3 hemisphere(SceneFrame* frame, float3 direction)
{
    return lerp(frame.ground_color.rgb, frame.sky_color.rgb, saturate(direction.y * 0.5 + 0.5));
}

// How open to the sky a place (relative to the render origin) is: the share of the sky's ambient that reaches it,
// for whatever can't work that out from what it sees. The world's own (its environment's under_roof), and inside a
// fog volume that says so, that volume's, over its soft edge: a hall with a roof in an open world, or a courtyard
// in a roofed one. The radiance cascades fill what the screen can't answer with the sky by it; fog, lit particles,
// water and what's see-through take the sky's ambient by it. 1 everywhere unless the cascades light the picture.
public float open_sky(SceneFrame* frame, float3 position)
{
    float open = frame.sky_color.w;
    for (uint k = 0; k < frame.sky_volume_count; ++k)
    {
        const FogVolumeData volume = frame.fog_volumes[k];
        open = lerp(open, volume.shape.w, fog_volume_weight(volume, position));
    }
    return open;
}

// A surface asks how open to the sky the air this far in front of it is, not the place it stands on: a wall is
// thinner than a volume's edge is soft, and its two faces stand in different places.
public static const float kOpenSkyAskedM = 0.5;

// The fog's ambient reaches the shaders already times the world's own openness, but never less than this of it, so
// that a volume more open than its world can still be given its share (scene.fog_inject).
public static const float kLeastSkyShare = 1.0 / 1024.0;

// The sky looking along a direction, without the sun: a gradient toward the horizon haze; the ground below it.
public float3 sky_gradient(SceneFrame* frame, float3 view_direction)
{
    const float up = saturate(view_direction.y);
    const float3 horizon = lerp(frame.sky_color.rgb, frame.sun_color.rgb * 0.08, 0.35);
    const float3 gradient = lerp(horizon, frame.sky_color.rgb * 0.7, pow(up, 0.45));
    return view_direction.y >= 0.0 ? gradient : frame.ground_color.rgb * 0.5;
}

// The sky looking along a direction: the gradient, and the sun's disk with a soft glow around it.
public float3 sky(SceneFrame* frame, float3 view_direction)
{
    const float to_sun = saturate(dot(view_direction, frame.sun_direction.xyz));
    const float3 sun = frame.sun_color.rgb * (pow(to_sun, 4000.0) * 30.0 + pow(to_sun, 64.0) * 0.08);
    return sky_gradient(frame, view_direction) + (view_direction.y >= 0.0 ? sun : float3(0.0));
}

// The sky as light arriving from a way, for what's traced (a path tracer's ray, a light box's bake's): shaped so that
// over an open hemisphere, gathered by the cosine, it comes to what hemisphere() gives (A + B n.y is what light
// A + 1.5 B w.y comes to). Indoors, then, only what gets in is there.
public float3 sky_light(SceneFrame* frame, float3 direction)
{
    const float3 middle = (frame.sky_color.rgb + frame.ground_color.rgb) * 0.5;
    const float3 slope = (frame.sky_color.rgb - frame.ground_color.rgb) * 0.5;
    return max(middle + slope * (1.5 * direction.y), float3(0.0));
}
