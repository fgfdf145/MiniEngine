// Cube map directions and the probe's sizes, shared by the capture and prefilter shaders and the
// shading that samples the prefiltered cube.
#ifndef CUBEMAP_COMMON_SLANG
#define CUBEMAP_COMMON_SLANG

// Must match engine/renderer/vulkan/environment_probe.cpp.
static const float RADIANCE_CUBE_SIZE = 256.0;
static const float RADIANCE_CUBE_MIP_COUNT = 9.0;
static const float PREFILTER_MIP_COUNT = 6.0;

// Vulkan's cube face orientation: face 0..5 = +X, -X, +Y, -Y, +Z, -Z; uv with t growing down.
float3 CubeFaceDirection(uint face, float2 uv)
{
    float2 st = uv * 2.0 - 1.0;
    float s = st.x;
    float t = st.y;
    float3 direction;
    if (face == 0u)
    {
        direction = float3(1.0, -t, -s);
    }
    else if (face == 1u)
    {
        direction = float3(-1.0, -t, s);
    }
    else if (face == 2u)
    {
        direction = float3(s, 1.0, t);
    }
    else if (face == 3u)
    {
        direction = float3(s, -1.0, -t);
    }
    else if (face == 4u)
    {
        direction = float3(s, -t, 1.0);
    }
    else
    {
        direction = float3(-s, -t, -1.0);
    }
    return normalize(direction);
}

#endif
