#ifndef SSR_HALF_RES_SLANG
#define SSR_HALF_RES_SLANG

// The reflection trace runs at half resolution (ssr_trace.comp): each invocation traces one
// full-resolution pixel of its 2x2 block, a different one each frame, so the resolve's temporal
// filter sees every pixel over four frames. The resolve (ssr_resolve.comp) calls this with the same
// frame index to find where each half-resolution sample came from.
int2 SsrHalfResSourcePixel(int2 halfPixel, uint frameIndex, int2 extent)
{
    const int2 kOffsets[4] = {int2(0, 0), int2(1, 1), int2(1, 0), int2(0, 1)};
    return min(halfPixel * 2 + kOffsets[frameIndex % 4u], extent - 1);
}

int2 SsrHalfResExtent(int2 extent)
{
    return (extent + 1) / 2;
}

// Under DLSS ray reconstruction the trace instead runs at full resolution with white noise, and the
// resolve passes its samples through unfiltered: the denoiser wants a fresh, independent sample in
// every pixel every frame. Half-resolution samples spread by the resolve's filters and its slowly
// clipped history reach it as noise that holds still, which it keeps as blotches on glossy paint.
static const uint SSR_TRACE_FLAG_FULL_RES = 1u;

// PCG (Jarzynski and Olano 2020).
uint SsrHash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float2 SsrWhiteNoise(int2 pixel, uint frameIndex)
{
    uint seed = SsrHash(uint(pixel.x) ^ SsrHash(uint(pixel.y) ^ SsrHash(frameIndex)));
    uint second = SsrHash(seed);
    return float2(float(seed >> 8), float(second >> 8)) * (1.0 / 16777216.0);
}

#endif
