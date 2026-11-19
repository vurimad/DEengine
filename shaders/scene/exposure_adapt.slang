// Auto exposure, step 2 (docs/design.md, Auto exposure), in compute, one group: from the frame's histogram
// (scene.exposure_histogram), the mean log luminance of the lit pixels between two shares of them, the darkest and the
// brightest left out (a black corner or the sun's glint shouldn't move it); the exposure that puts that at middle
// grey, only part of the way from the environment's own (an eye doesn't adapt all the way: a dark hall stays darker
// than a sunny yard, and should), within limits; and the eye's way to it from last frame's, faster toward less light than toward more, as a
// pupil closes faster than it opens. Kept on the GPU, where the TAA and the tonemap read it this same frame.

static const uint kBins = 256;

// What's kept between frames.
struct ExposureState
{
    float log2_exposure; // what the picture is multiplied by, as a power of 2
    uint has_value;      // 0: none yet; the first frame takes its target at once
    float exposure;      // 2 to that: what the TAA and the tonemap multiply by
    float target_log2;   // where it's going, for the stats
};

struct ExposureAdaptPush
{
    uint* histogram;
    ExposureState* state;
    float min_log2;       // the histogram's, as scene.exposure_histogram has it
    float log2_per_bin;   // the range over (kBins - 1)
    float low_share;      // of the pixels, the darkest left out
    float high_share;     // and from where the brightest are
    float key_log2;       // where the mean goes: middle grey, and the compensation
    float min_exposure_log2;
    float max_exposure_log2;
    float brighten_rate;  // per second, toward more exposure (a darker scene: the eye opens)
    float darken_rate;    // toward less (a brighter scene: it closes)
    float step_s;         // since last frame; 0 holds it
    uint snap;            // 1: go straight to the target
    float base_log2;      // the environment's exposure
    float share;          // of the way from it to what middle grey asks
};

[[vk::push_constant]]
ConstantBuffer<ExposureAdaptPush> push;

groupshared uint counts[kBins];

[shader("compute")]
[numthreads(256, 1, 1)]
void compute_main(uint index: SV_GroupIndex)
{
    counts[index] = push.histogram[index];
    GroupMemoryBarrierWithGroupSync();
    if (index != 0)
    {
        return;
    }
    float total = 0.0;
    for (uint bin = 0; bin < kBins; ++bin)
    {
        total += float(counts[bin]);
    }
    ExposureState state = *push.state;
    if (total <= 0.0)
    {
        return; // nothing metered: hold
    }
    // The mean log luminance between the two shares: each bin counts for the part of it inside the window.
    const float low = total * push.low_share;
    const float high = total * push.high_share;
    float below = 0.0;
    float sum = 0.0;
    float weight = 0.0;
    for (uint bin = 0; bin < kBins; ++bin)
    {
        const float count = float(counts[bin]);
        const float inside = max(min(below + count, high) - max(below, low), 0.0);
        below += count;
        if (inside <= 0.0)
        {
            continue;
        }
        // Bin 0, black, is never counted (scene.exposure_histogram).
        const float log2_luminance = push.min_log2 + (max(float(bin), 1.0) - 0.5) * push.log2_per_bin;
        sum += log2_luminance * inside;
        weight += inside;
    }
    if (weight <= 0.0)
    {
        return;
    }
    const float metered = push.key_log2 - sum / weight;
    const float target =
        clamp(lerp(push.base_log2, metered, push.share), push.min_exposure_log2, push.max_exposure_log2);
    float now = target;
    if (state.has_value != 0 && push.snap == 0)
    {
        const float rate = target > state.log2_exposure ? push.brighten_rate : push.darken_rate;
        now = lerp(state.log2_exposure, target, 1.0 - exp(-push.step_s * rate));
    }
    state.log2_exposure = now;
    state.has_value = 1;
    state.exposure = exp2(now);
    state.target_log2 = target;
    *push.state = state;
}
