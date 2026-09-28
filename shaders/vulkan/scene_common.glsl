// Declarations shared by every stage that binds the scene's set 0. The C++ side of the camera block
// is CameraUniformData in engine/renderer/vulkan/uniform_buffer.h; the two must stay byte for byte
// identical under std140.

#ifndef SCENE_COMMON_GLSL
#define SCENE_COMMON_GLSL

#include "reverse_depth.glsl"

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
    vec4 positionAndRange;  // xyz = world position, w = range (metres)
    vec4 colorAndIntensity; // xyz = linear RGB color, w = intensity (lumens or lux)
    vec4 directionAndType;  // xyz = world direction the light travels (an area light's emitting normal), w = LightType
    vec4 spotAndArea;       // x = cos(inner), y = cos(outer), z = areaW (area) or source radius (point, spot), w = areaH
    vec4 areaRightAxis;     // xyz = world axis along areaW (area lights only)
};

layout(set = 0, binding = 0) uniform CameraBuffer
{
    mat4 view;
    mat4 proj;
    vec4 cameraWorldPosition;
    // xyz = ambient luminance in cd/m^2: the sum of the scene's Ambient lights and the direction-free
    // half of its Hemisphere lights, or the fallback when it has neither; w = 1 when it is the
    // fallback. Both are folded in here on the CPU and never appear among the lights; the
    // direction-dependent half is ambientGradient.
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
    vec4 groundAlbedo;               // rgb, w 1 when the ground plane is drawn (ground_plane.glsl)
    vec4 atmosphereRadii;            // x bottom km, y top km, z aerial perspective distance scale
    vec4 atmosphereCameraPositionKm; // xyz camera relative to the planet centre
    vec4 hdriParameters;             // x intensity, y rotation in turns
    vec4 hdriIrradianceSh[9];        // the HDRI's radiance SH, rotated and scaled; xyz used
    vec4 heightFogDensity;           // x density per m (0 off), y falloff per m, z fog height m, w start distance m
    vec4 heightFogColor;             // rgb albedo, w max opacity
    vec4 heightFogParams;            // x Henyey-Greenstein g, yzw sun illuminance at the camera times albedo

    // proj * view without the TAA jitter that proj and invViewProj carry; motion vectors use it.
    mat4 viewProjNoJitter;
    // Geometric specular anti-aliasing: x = 1 when on, y = variance, z = threshold (specular_aa.glsl).
    vec4 specularAntiAliasing;
    // x = pre-exposure (physical radiance to HDR target units, see pre_exposure.glsl), y = its
    // inverse. Every writer of the HDR target and GB3 multiplies its final value by x.
    vec4 exposure;
    // The Hemisphere lights' direction-dependent half, one row per colour channel: the ambient
    // luminance seen along a unit direction d is ambientLuminance.rgb + (row_r . d, row_g . d,
    // row_b . d) (see SceneAmbientAlong). xyz used. Appended last.
    vec4 ambientGradient[3];
    // The DDGI volume (DdgiUniformData): x level count (0 off), y the level the probe view draws, z normal bias, w view
    // bias (times the spacing); each level's spacing; each level's grid origin (xyz). Appended last.
    vec4 ddgiParams;
    vec4 ddgiSpacing;
    vec4 ddgiOrigins[4];
}
ubo;

// The scene's ambient luminance (cd/m^2) arriving along the unit direction d: its Ambient lights
// plus its Hemisphere lights, each of which is sky above its up axis and ground below. A hemisphere
// light's irradiance on a surface facing n is exactly pi times this along n, so the diffuse lobe
// reads it at the normal; the specular lobe reads it along the reflection, which is the
// cosine-blurred radiance rather than the hard horizon, exact for a rough lobe.
vec3 SceneAmbientAlong(vec3 d)
{
    return max(ubo.ambientLuminance.rgb +
                   vec3(dot(ubo.ambientGradient[0].xyz, d), dot(ubo.ambientGradient[1].xyz, d), dot(ubo.ambientGradient[2].xyz, d)),
               vec3(0.0));
}

// The same, but zero when it is only the fallback: under a physical sky the sky takes its place.
vec3 SceneLightsAmbientAlong(vec3 d)
{
    return ubo.ambientLuminance.w > 0.5 ? vec3(0.0) : SceneAmbientAlong(d);
}

#endif
