// Declarations shared by every stage that binds the scene's set 0. The C++ side of the camera block
// is CameraUniformData in engine/renderer/vulkan/uniform_buffer.h; the two must stay byte for byte
// identical under std140.

// Light type constants: must match the C++ LightType enum.
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2
#define LIGHT_AREA 3
#define LIGHT_AMBIENT 4

#define MAX_SCENE_LIGHTS 8

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
    // it has none. Ambient lights are folded in here on the CPU and never appear in lights[].
    vec4 ambientLuminance;
    SceneLightData lights[MAX_SCENE_LIGHTS];
    uvec4 sceneLightCount; // x = active light count
}
ubo;
