// The clouds' temporal reconstruction (docs/design/2026-10-06-cumulus-generation-design.md):
// each frame cloud_march.comp marches one pixel of every 2 x 2 block of the scene, a quarter of
// them, and cloud_resolve.comp rebuilds the others from last frame's clouds, reprojected. Over four
// frames every pixel has been marched, so a still view converges to the full extent. Both lay the
// clouds out on the unjittered pixel grid (CloudJitterUv), so the history of a still view lands
// on its own texels and is never resampled. Include after volumetric_clouds.slang, in compute
// shaders bound to the atmosphere's pipeline layout.
#ifndef CLOUD_RECONSTRUCTION_SLANG
#define CLOUD_RECONSTRUCTION_SLANG

// Must match the constants VulkanAtmosphere::RecordClouds pushes.
struct CloudReconstruction
{
    // x which pixel of each 2 x 2 block is marched this frame (0-3), y 1 when last frame's clouds
    // are there to reproject, zw the scene's extent.
    uint4 frame;
};
#include "d3d_register.slang"

[[vk::push_constant]] ConstantBuffer<CloudReconstruction> cloudReconstruction D3D_PUSH_CONSTANTS(1);

// Over four frames every pixel of the block, the diagonal first so two frames already cover it
// evenly.
static const int2 CLOUD_BLOCK_ORDER[4] = {int2(0, 0), int2(1, 1), int2(1, 0), int2(0, 1)};

int2 CloudMarchedPixelInBlock()
{
    return CLOUD_BLOCK_ORDER[cloudReconstruction.frame.x & 3u];
}

int2 CloudSceneExtent()
{
    return int2(cloudReconstruction.frame.zw);
}

bool CloudHistoryValid()
{
    return cloudReconstruction.frame.y != 0u;
}

#endif
