// The sky and the ambient light, as the deferred light pass draws them and what reflects them (water) sees them: a
// gradient toward the horizon haze with the sun's disk, and a sky/ground hemisphere, the placeholder for image-based
// lighting.

import scene.scene_common;

// The ambient from a direction: the ground below, the sky above.
public float3 hemisphere(SceneFrame* frame, float3 direction)
{
    return lerp(frame.ground_color.rgb, frame.sky_color.rgb, saturate(direction.y * 0.5 + 0.5));
}

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
