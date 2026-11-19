// What glows, for the radiance cascades (docs/design.md, Global illumination: radiance cascades), in compute: the
// luminance of the HDR target before the deferred light adds to it, when it holds only what surfaces emit (the sky
// isn't drawn yet: nothing there). The pyramids (scene.rc_pyramid) set it against the lit picture: a thing that
// glows does so from every side, so its unseen sides are taken to glow as its seen ones do, where a thing lit by a
// light is dark on its far side.
//
// Kept at the GI picture's size (scene.rc_common): a pixel holds the mean over its block of the screen, read with as
// few filtered samples as give exactly that mean (rc_block_mean).

import common.bindless;
import scene.rc_common;

struct RcGlowPush
{
    uint source;  // the HDR target, holding what's emitted (sampled)
    uint output;  // R16F, as a storage texture
    uint2 size;   // the GI picture's
    uint2 screen; // the screen's
    uint scale;   // the screen's pixels to one of the GI picture's, each way
    uint reserved;
};

[[vk::push_constant]]
ConstantBuffer<RcGlowPush> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    if (any(thread.xy >= push.size))
    {
        return;
    }
    const float3 emitted = max(rc_block_mean(push.source, thread.xy, push.scale, push.screen).rgb, 0.0);
    storage_2d(push.output)[thread.xy] = float4(dot(emitted, float3(0.2126, 0.7152, 0.0722)), 0.0, 0.0, 0.0);
}
