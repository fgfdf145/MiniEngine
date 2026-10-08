// Declarations shared by every stage that binds the scene's set 0. The C++ side of the camera block
// is CameraUniformData in engine/renderer/vulkan/uniform_buffer.h; the two must stay byte for byte
// identical under std140.

#ifndef SCENE_COMMON_SLANG
#define SCENE_COMMON_SLANG

#include "reverse_depth.slang"

// Light type constants: must match the C++ LightType enum.
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2
#define LIGHT_AREA 3
#define LIGHT_AMBIENT 4
#define LIGHT_HEMISPHERE 5

#define SHADOW_CASCADE_COUNT 4

// The light cluster grid: must match kLightClusterTiles* and kLightClusterSlices in
// engine/renderer/light_clusters.h.
#define LIGHT_CLUSTER_TILES_X 16
#define LIGHT_CLUSTER_TILES_Y 9
#define LIGHT_CLUSTER_SLICES 24
#define LIGHT_CLUSTER_COUNT (LIGHT_CLUSTER_TILES_X * LIGHT_CLUSTER_TILES_Y * LIGHT_CLUSTER_SLICES)

struct SceneLightData
{
    float4 positionAndRange;  // xyz = world position, w = range (metres)
    float4 colorAndIntensity; // xyz = linear RGB color, w = intensity (lumens or lux)
    float4 directionAndType;  // xyz = world direction the light travels (an area light's emitting normal), w = LightType
    float4 spotAndArea;       // x = cos(inner), y = cos(outer), z = areaW (area) or source radius (point, spot), w = areaH
    float4 areaRightAxis;     // xyz = world axis along areaW (area lights only)
};

struct CameraBuffer
{
    float4x4 view;
    float4x4 proj;
    float4 cameraWorldPosition;
    // xyz = ambient luminance in cd/m^2: the sum of the scene's Ambient lights and the direction-free
    // half of its Hemisphere lights, or the fallback when it has neither; w = 1 when it is the
    // fallback. Both are folded in here on the CPU and never appear among the lights; the
    // direction-dependent half is ambientGradient.
    float4 ambientLuminance;
    // The lights are in the storage buffer at binding 10 (see pbr_common.slang), directional ones first.
    uint4 lightCounts;       // x = directional count, y = total count, z = 1 to look up through the cluster grid, 0 to loop over all
    float4 lightClusterSlices; // x = slice scale, y = slice bias: slice = floor(log(view depth) * x + y)

    // Cascaded shadow map of the one directional light that casts shadows (see ShadowUniformData).
    float4x4 shadowCascadeViewProjection[SHADOW_CASCADE_COUNT]; // world to light clip space
    float4 shadowCascadeSplits;                               // view distance where each cascade ends
    float4 shadowCascadeTexelSizes;                           // world size of one texel per cascade
    float4 shadowParams;                                      // x = index of the caster among the lights, -1 for none; y = 1 / resolution
    float4x4 invViewProj;                                       // inverse(proj * view), for reconstructing world position from depth
    float4x4 prevViewProj;                                      // last frame's proj * view, for motion vectors

    // Environment: EnvironmentUniformData in engine/renderer/atmosphere.h, member for member.
    float4 sunDirectionAndMode;        // xyz toward the sun, w EnvironmentMode
    float4 sunIlluminance;             // rgb lux at the top of the atmosphere, w cos(sun angular radius)
    float4 rayleighScattering;         // rgb per km, w scale height km
    float4 mieParameters;              // x scattering per km, y extinction per km, z scale height km, w g
    float4 ozoneAbsorption;            // rgb per km
    float4 groundAlbedo;               // rgb, w 1 when the ground plane is drawn (ground_plane.slang)
    float4 atmosphereRadii;            // x bottom km, y top km, z aerial perspective distance scale, w 1 for a seamless horizon
    float4 atmosphereCameraPositionKm; // xyz camera relative to the planet centre
    float4 hdriParameters;             // x intensity, y rotation in turns
    float4 hdriIrradianceSh[9];        // the HDRI's radiance SH, rotated and scaled; xyz used
    float4 heightFogDensity;           // x density per m (0 off), y falloff per m, z fog height m, w start distance m
    float4 heightFogColor;             // rgb albedo, w max opacity
    float4 heightFogParams;            // x Henyey-Greenstein g, yzw sun illuminance at the camera times albedo
    float4 cloudLayer;                 // x base altitude km, y thickness km, z coverage offset, w extinction per km (0 off)
    float4 cloudScales;                // frequencies per km: x large billows, y small billows, z plume map; w billow strength
    float4 cloudPhase;                 // x forward g, y back g, z back weight, w single-scattering albedo
    float4 cloudParams;                // x ambient scale, y haze distance km, z deck weight, w frame index
    float4 nightSky;                   // rgb moonless night sky luminance cd/m^2, added above the horizon
    float4 cloudLighting;              // x diffusion, y ambient occlusion, z diffusion decay kappa, w mean cosine
    float4 cloudShapeMotion;           // xyz large billows' offset in tiles, w plume map's u offset
    float4 cloudDetailMotion;          // xyz small billows' offset in tiles, w plume map's v offset
    float4 cloudLife;                  // each plume scale's phase of life (cloud_weather.comp)
    float4 cloudMotionStep;            // xy this frame's wind displacement, world x, z metres; w cloud clock seconds
    float4 wind;                       // xyz world direction the wind blows toward, w speed at 10 m, m/s (engine/scene/wind.h)

    // proj * view without the TAA jitter that proj and invViewProj carry; motion vectors use it.
    float4x4 viewProjNoJitter;
    // Geometric specular anti-aliasing: x = 1 when on, y = variance, z = threshold (specular_aa.slang).
    float4 specularAntiAliasing;
    // x = pre-exposure (physical radiance to HDR target units, see pre_exposure.slang), y = its
    // inverse. Every writer of the HDR target and GB3 multiplies its final value by x.
    float4 exposure;
    // The Hemisphere lights' direction-dependent half, one row per colour channel: the ambient
    // luminance seen along a unit direction d is ambientLuminance.rgb + (row_r . d, row_g . d,
    // row_b . d) (see SceneAmbientAlong). xyz used. Appended last.
    float4 ambientGradient[3];
    // The DDGI volume (DdgiUniformData): x level count (0 off), y the level the probe view draws, z normal bias, w view
    // bias (times the spacing); each level's spacing; each level's grid origin (xyz). Appended last.
    float4 ddgiParams;
    float4 ddgiSpacing;
    float4 ddgiOrigins[4];
    // x = the material textures' mip bias (MATERIAL_MIP_BIAS): negative while DLSS upscales, so they
    // keep the output's detail. Appended last.
    float4 textureParams;
};
[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> ubo;

#define MATERIAL_MIP_BIAS (ubo.textureParams.x)
// A material texture sampled with that bias. Fragment shaders only (a macro, so the compute shaders
// that include this file never compile it).
#define MaterialTexture(textureSampler, uv) textureSampler.SampleBias(uv, MATERIAL_MIP_BIAS)

// The scene's ambient luminance (cd/m^2) arriving along the unit direction d: its Ambient lights
// plus its Hemisphere lights, each of which is sky above its up axis and ground below. A hemisphere
// light's irradiance on a surface facing n is exactly pi times this along n, so the diffuse lobe
// reads it at the normal; the specular lobe reads it along the reflection, which is the
// cosine-blurred radiance rather than the hard horizon, exact for a rough lobe.
float3 SceneAmbientAlong(float3 d)
{
    return max(ubo.ambientLuminance.rgb +
                   float3(dot(ubo.ambientGradient[0].xyz, d), dot(ubo.ambientGradient[1].xyz, d), dot(ubo.ambientGradient[2].xyz, d)),
               float3(0.0));
}

// The same, but zero when it is only the fallback: under a physical sky the sky takes its place.
float3 SceneLightsAmbientAlong(float3 d)
{
    return ubo.ambientLuminance.w > 0.5 ? float3(0.0) : SceneAmbientAlong(d);
}

#endif
