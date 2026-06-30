#version 450

struct SceneLightData {
    vec4 positionAndRange;
    vec4 colorAndIntensity;
    vec4 directionAndType;
    vec4 spotAndArea;
};

layout(push_constant) uniform DrawConstants
{
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveFactor;
    vec4 surfaceFactors;
    vec4 nodeGraphFactors;
} drawData;

layout(set = 0, binding = 0) uniform CameraBuffer
{
    mat4 view;
    mat4 proj;
    vec4 cameraWorldPosition;
    vec4 ambientColorAndIntensity;
    SceneLightData lights[8];
    uvec4 sceneLightCount;
} ubo;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec4 inTangent;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec3 fragWorldNormal;
layout(location = 3) out vec4 fragWorldTangent;
layout(location = 4) out vec3 fragWorldPosition;

void main()
{
    vec4 worldPosition = drawData.model * vec4(inPosition, 1.0);

    // Compute the normal matrix analytically from the TRS model columns.
    // For M = R*S, the normal matrix (transpose of inverse) equals R*S^{-1},
    // which is each column divided by its squared length. This avoids inverse().
    vec3 mc0 = drawData.model[0].xyz;
    vec3 mc1 = drawData.model[1].xyz;
    vec3 mc2 = drawData.model[2].xyz;
    mat3 normalMatrix = mat3(
        mc0 / max(dot(mc0, mc0), 1e-6),
        mc1 / max(dot(mc1, mc1), 1e-6),
        mc2 / max(dot(mc2, mc2), 1e-6)
    );

    vec3 worldTangent = normalize(mat3(drawData.model) * inTangent.xyz);

    gl_Position = ubo.proj * ubo.view * worldPosition;
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    fragWorldNormal = normalize(normalMatrix * inNormal);
    fragWorldTangent = vec4(worldTangent, inTangent.w);
    fragWorldPosition = worldPosition.xyz;
}
