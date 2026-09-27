#ifndef SSR_HALF_RES_GLSL
#define SSR_HALF_RES_GLSL

// The reflection trace runs at half resolution (ssr_trace.comp): each invocation traces one
// full-resolution pixel of its 2x2 block, a different one each frame, so the resolve's temporal
// filter sees every pixel over four frames. The resolve (ssr_resolve.comp) calls this with the same
// frame index to find where each half-resolution sample came from.
ivec2 SsrHalfResSourcePixel(ivec2 halfPixel, uint frameIndex, ivec2 extent)
{
    const ivec2 kOffsets[4] = ivec2[4](ivec2(0, 0), ivec2(1, 1), ivec2(1, 0), ivec2(0, 1));
    return min(halfPixel * 2 + kOffsets[frameIndex % 4u], extent - 1);
}

ivec2 SsrHalfResExtent(ivec2 extent)
{
    return (extent + 1) / 2;
}

#endif
