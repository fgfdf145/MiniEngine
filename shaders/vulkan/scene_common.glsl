// Declarations shared by every stage that binds the scene's set 0. The C++ side of the camera block
// is CameraUniformData in engine/renderer/vulkan/uniform_buffer.h; the two must stay byte for byte
// identical under std140.

#ifndef SCENE_COMMON_GLSL
#define SCENE_COMMON_GLSL

// Light type constants: must match the C++ LightType enum.
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2
#define LIGHT_AREA 3
#define LIGHT_AMBIENT 4

#define SHADOW_CASCADE_COUNT 4

// The light cluster grid: must match kLightClusterTiles* and kLightClusterSlices in
// engine/renderer/light_clusters.h.
#define LIGHT_CLUSTER_TILES_X 16
#define LIGHT_CLUSTER_TILES_Y 9
#define LIGHT_CLUSTER_SLICES 24
#define LIGHT_CLUSTER_COUNT (LIGHT_CLUSTER_TILES_X * LIGHT_CLUSTER_TILES_Y * LIGHT_CLUSTER_SLICES)

struct SceneLightData
{
    vec4 positionAndRange;  // xyz = world position, w = range (metres)
    vec4 colorAndIntensity; // xyz = linear RGB color, w = intensity (lumens or lux)
    vec4 directionAndType;  // xyz = world direction the light travels (an area light's emitting normal), w = LightType
    vec4 spotAndArea;       // x = cos(inner), y = cos(outer), z = areaW, w = areaH
    vec4 areaRightAxis;     // xyz = world axis along areaW (area lights only)
};

layout(set = 0, binding = 0) uniform CameraBuffer
{
    mat4 view;
    mat4 proj;
    vec4 cameraWorldPosition;
    // xyz = ambient luminance in cd/m^2: the sum of the scene's Ambient lights, or the fallback when
    // it has none; w = 1 when it is the fallback. Ambient lights are folded in here on the CPU and never appear among the lights.
    vec4 ambientLuminance;
    // The lights are in the storage buffer at binding 10 (see pbr_common.glsl), directional ones first.
    uvec4 lightCounts;       // x = directional count, y = total count, z = 1 to look up through the cluster grid, 0 to loop over all
    vec4 lightClusterSlices; // x = slice scale, y = slice bias: slice = floor(log(view depth) * x + y)

    // Cascaded shadow map of the one directional light that casts shadows (see ShadowUniformData).
    mat4 shadowCascadeViewProjection[SHADOW_CASCADE_COUNT]; // world to light clip space
    vec4 shadowCascadeSplits;                               // view distance where each cascade ends
    vec4 shadowCascadeTexelSizes;                           // world size of one texel per cascade
    vec4 shadowParams;                                      // x = index of the caster among the lights, -1 for none; y = 1 / resolution
    mat4 invViewProj;                                       // inverse(proj * view), for reconstructing world position from depth
    mat4 prevViewProj;                                      // last frame's proj * view, for motion vectors

    // Environment: EnvironmentUniformData in engine/renderer/atmosphere.h, member for member.
    vec4 sunDirectionAndMode;        // xyz toward the sun, w EnvironmentMode
    vec4 sunIlluminance;             // rgb lux at the top of the atmosphere, w cos(sun angular radius)
    vec4 rayleighScattering;         // rgb per km, w scale height km
    vec4 mieParameters;              // x scattering per km, y extinction per km, z scale height km, w g
    vec4 ozoneAbsorption;            // rgb per km
    vec4 groundAlbedo;               // rgb
    vec4 atmosphereRadii;            // x bottom km, y top km, z aerial perspective distance scale
    vec4 atmosphereCameraPositionKm; // xyz camera relative to the planet centre
    vec4 hdriParameters;             // x intensity, y rotation in turns
    vec4 hdriIrradianceSh[9];        // the HDRI's radiance SH, rotated and scaled; xyz used

    // proj * view without the TAA jitter that proj and invViewProj carry; motion vectors use it.
    mat4 viewProjNoJitter;
}
ubo;

#endif
