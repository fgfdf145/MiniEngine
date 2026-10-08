// Local light shadow helpers shared by pbr_common.slang and tests/local_shadows_tests.cpp, which
// compiles this file as C++. Written, like ssr_common.slang, in the subset Slang and C++/GLM both
// accept: every float literal carries the f suffix, no out parameters.

#ifndef LOCAL_SHADOW_COMMON_SLANG
#define LOCAL_SHADOW_COMMON_SLANG

// Must match kLocalShadowTileSize, kLocalShadowGuardTexels and kLocalShadowNearPlane in
// engine/renderer/local_shadows.h; the test checks that they do.
static const float kLocalShadowTileTexels = 512.0f;
// How far inside its tile, as a fraction of it, a lookup is kept.
static const float kLocalShadowGuardFraction = 2.0f / 512.0f;
static const float kLocalShadowNearPlaneMetres = 0.05f;

// The cube face a direction from the light falls on: its major axis, in the order +X, -X, +Y, -Y,
// +Z, -Z. BuildLocalShadowCubeFace builds the faces in the same order. Ties go to the earlier axis.
int SelectCubeFace(float3 direction)
{
    float3 a = abs(direction);
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
float3 LocalShadowAtlasCoordinates(float4 clip, float4 atlasRect, float guard)
{
    float3 ndc = float3(clip.x, clip.y, clip.z) / clip.w;
    float2 local = clamp(float2(ndc.x, ndc.y) * 0.5f + 0.5f, float2(guard), float2(1.0f - guard));
    float2 uv = float2(atlasRect.x, atlasRect.y) + local * float2(atlasRect.z, atlasRect.w);
    return float3(uv.x, uv.y, ndc.z);
}

#endif
