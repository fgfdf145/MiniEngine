// Cube map directions and the probe's sizes, shared by the capture and prefilter shaders and the
// shading that samples the prefiltered cube.
#ifndef CUBEMAP_COMMON_GLSL
#define CUBEMAP_COMMON_GLSL

// Must match engine/renderer/vulkan/environment_probe.cpp.
const float RADIANCE_CUBE_SIZE = 128.0;
const float RADIANCE_CUBE_MIP_COUNT = 8.0;
const float PREFILTER_MIP_COUNT = 6.0;

// Vulkan's cube face orientation: face 0..5 = +X, -X, +Y, -Y, +Z, -Z; uv with t growing down.
vec3 CubeFaceDirection(uint face, vec2 uv)
{
    vec2 st = uv * 2.0 - 1.0;
    float s = st.x;
    float t = st.y;
    vec3 direction;
    if (face == 0u)
    {
        direction = vec3(1.0, -t, -s);
    }
    else if (face == 1u)
    {
        direction = vec3(-1.0, -t, s);
    }
    else if (face == 2u)
    {
        direction = vec3(s, 1.0, t);
    }
    else if (face == 3u)
    {
        direction = vec3(s, -1.0, -t);
    }
    else if (face == 4u)
    {
        direction = vec3(s, -t, 1.0);
    }
    else
    {
        direction = vec3(-s, -t, -1.0);
    }
    return normalize(direction);
}

#endif
