// Local light shadow helpers shared by pbr_common.glsl and tests/local_shadows_tests.cpp, which
// compiles this file as C++. Written, like ssr_common.glsl, in the subset GLSL and C++/GLM both
// accept: every float literal carries the f suffix, no out parameters.

#ifndef LOCAL_SHADOW_COMMON_GLSL
#define LOCAL_SHADOW_COMMON_GLSL

// The cube face a direction from the light falls on: its major axis, in the order +X, -X, +Y, -Y,
// +Z, -Z. BuildLocalShadowCubeFace builds the faces in the same order. Ties go to the earlier axis.
int SelectCubeFace(vec3 direction)
{
    vec3 a = abs(direction);
    if (a.x >= a.y && a.x >= a.z)
    {
        return direction.x >= 0.0f ? 0 : 1;
    }
    if (a.y >= a.z)
    {
        return direction.y >= 0.0f ? 2 : 3;
    }
    return direction.z >= 0.0f ? 4 : 5;
}

// Where a point in a tile's clip space lands in the atlas: xy the uv, z the depth to compare
// against. The uv is kept guard (a fraction of the tile) inside the tile's edges, so no filter tap
// reads a neighbouring tile whatever the point.
vec3 LocalShadowAtlasCoordinates(vec4 clip, vec4 atlasRect, float guard)
{
    vec3 ndc = vec3(clip.x, clip.y, clip.z) / clip.w;
    vec2 local = clamp(vec2(ndc.x, ndc.y) * 0.5f + 0.5f, vec2(guard), vec2(1.0f - guard));
    vec2 uv = vec2(atlasRect.x, atlasRect.y) + local * vec2(atlasRect.z, atlasRect.w);
    return vec3(uv.x, uv.y, ndc.z);
}

#endif
