#version 450

layout(set = 0, binding = 0) uniform CameraBuffer
{
    mat4 model;
    mat4 view;
    mat4 proj;
    vec4 cameraWorldPosition;
    vec4 lightDirectionAndIntensity;
    vec4 lightColorAndAmbient;
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
    vec4 worldPosition = ubo.model * vec4(inPosition, 1.0);
    mat3 normalMatrix = transpose(inverse(mat3(ubo.model)));
    vec3 worldTangent = normalize(normalMatrix * inTangent.xyz);

    gl_Position = ubo.proj * ubo.view * worldPosition;
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    fragWorldNormal = normalize(normalMatrix * inNormal);
    fragWorldTangent = vec4(worldTangent, inTangent.w);
    fragWorldPosition = worldPosition.xyz;
}
