#version 450

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
    vec4 lightDirectionAndIntensity;
    vec4 lightColorAndAmbient;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D baseColorTexture;
layout(set = 0, binding = 2) uniform sampler2D normalTexture;
layout(set = 0, binding = 3) uniform sampler2D metallicTexture;
layout(set = 0, binding = 4) uniform sampler2D roughnessTexture;
layout(set = 0, binding = 5) uniform sampler2D occlusionTexture;
layout(set = 0, binding = 6) uniform sampler2D emissiveTexture;
layout(set = 0, binding = 7) uniform sampler2D secondaryBaseColorTexture;
layout(set = 0, binding = 8) uniform sampler2D secondaryNormalTexture;
layout(set = 0, binding = 9) uniform sampler2D secondaryMetallicTexture;
layout(set = 0, binding = 10) uniform sampler2D secondaryRoughnessTexture;
layout(set = 0, binding = 11) uniform sampler2D secondaryOcclusionTexture;
layout(set = 0, binding = 12) uniform sampler2D secondaryEmissiveTexture;
layout(set = 0, binding = 13) uniform sampler2D blendMaskTexture;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragWorldNormal;
layout(location = 3) in vec4 fragWorldTangent;
layout(location = 4) in vec3 fragWorldPosition;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

float DistributionGGX(vec3 normal, vec3 halfVector, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float nDotH = max(dot(normal, halfVector), 0.0);
    float nDotHSquared = nDotH * nDotH;

    float denominator = nDotHSquared * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(PI * denominator * denominator, 0.0001);
}

float GeometrySchlickGGX(float nDotV, float roughness)
{
    float remappedRoughness = roughness + 1.0;
    float k = (remappedRoughness * remappedRoughness) / 8.0;
    float denominator = nDotV * (1.0 - k) + k;
    return nDotV / max(denominator, 0.0001);
}

float GeometrySmith(vec3 normal, vec3 viewDirection, vec3 lightDirection, float roughness)
{
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    float ggxView = GeometrySchlickGGX(nDotV, roughness);
    float ggxLight = GeometrySchlickGGX(nDotL, roughness);
    return ggxView * ggxLight;
}

vec3 FresnelSchlick(float cosineTheta, vec3 baseReflectivity)
{
    return baseReflectivity + (1.0 - baseReflectivity) * pow(1.0 - cosineTheta, 5.0);
}

void main()
{
    float blendMask = texture(blendMaskTexture, fragTexCoord).r;
    float blendWeight = clamp(
        mix(0.0, drawData.nodeGraphFactors.y, clamp(drawData.nodeGraphFactors.x, 0.0, 1.0)) * blendMask,
        0.0,
        1.0
    );

    vec4 primaryBaseColor = texture(baseColorTexture, fragTexCoord);
    vec4 secondaryBaseColor = texture(secondaryBaseColorTexture, fragTexCoord);
    vec4 sampledBaseColor = mix(primaryBaseColor, secondaryBaseColor, blendWeight);
    vec4 albedo = sampledBaseColor * vec4(fragColor, 1.0) * drawData.baseColorFactor;

    vec3 geometricNormal = normalize(fragWorldNormal);
    vec3 tangent = normalize(fragWorldTangent.xyz - geometricNormal * dot(geometricNormal, fragWorldTangent.xyz));
    vec3 bitangent = normalize(cross(geometricNormal, tangent) * fragWorldTangent.w);
    mat3 tbn = mat3(tangent, bitangent, geometricNormal);

    vec3 sampledNormalPrimary = texture(normalTexture, fragTexCoord).xyz * 2.0 - 1.0;
    vec3 sampledNormalSecondary = texture(secondaryNormalTexture, fragTexCoord).xyz * 2.0 - 1.0;
    vec3 sampledNormal = normalize(mix(sampledNormalPrimary, sampledNormalSecondary, blendWeight));
    sampledNormal.xy *= drawData.surfaceFactors.z;
    vec3 normal = normalize(tbn * sampledNormal);

    float metallicSample = mix(
        texture(metallicTexture, fragTexCoord).b,
        texture(secondaryMetallicTexture, fragTexCoord).b,
        blendWeight
    );
    float roughnessSample = mix(
        texture(roughnessTexture, fragTexCoord).g,
        texture(secondaryRoughnessTexture, fragTexCoord).g,
        blendWeight
    );
    float ambientOcclusionSample = mix(
        texture(occlusionTexture, fragTexCoord).r,
        texture(secondaryOcclusionTexture, fragTexCoord).r,
        blendWeight
    );
    vec3 emissiveSample = mix(
        texture(emissiveTexture, fragTexCoord).rgb,
        texture(secondaryEmissiveTexture, fragTexCoord).rgb,
        blendWeight
    );

    float metallic = clamp(drawData.surfaceFactors.x * metallicSample, 0.0, 1.0);
    float roughness = clamp(drawData.surfaceFactors.y * roughnessSample, 0.04, 1.0);
    float ambientOcclusion = mix(1.0, ambientOcclusionSample, clamp(drawData.surfaceFactors.w, 0.0, 1.0));

    vec3 viewDirection = normalize(ubo.cameraWorldPosition.xyz - fragWorldPosition);
    vec3 lightDirection = normalize(-ubo.lightDirectionAndIntensity.xyz);
    vec3 halfVector = normalize(viewDirection + lightDirection);
    vec3 radiance = ubo.lightColorAndAmbient.rgb * ubo.lightDirectionAndIntensity.w;

    float nDotL = max(dot(normal, lightDirection), 0.0);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float hDotV = max(dot(halfVector, viewDirection), 0.0);

    vec3 baseReflectivity = mix(vec3(0.04), albedo.rgb, metallic);
    vec3 fresnel = FresnelSchlick(hDotV, baseReflectivity);
    float distribution = DistributionGGX(normal, halfVector, roughness);
    float geometry = GeometrySmith(normal, viewDirection, lightDirection, roughness);

    vec3 numerator = distribution * geometry * fresnel;
    float denominator = max(4.0 * nDotV * nDotL, 0.0001);
    vec3 specular = numerator / denominator;

    vec3 specularRatio = fresnel;
    vec3 diffuseRatio = (vec3(1.0) - specularRatio) * (1.0 - metallic);
    vec3 diffuse = diffuseRatio * albedo.rgb / PI;

    vec3 ambient = albedo.rgb * ubo.lightColorAndAmbient.w * ambientOcclusion;
    vec3 directLighting = (diffuse + specular) * radiance * nDotL;
    vec3 emissive = emissiveSample * drawData.emissiveFactor.rgb;

    vec3 color = ambient + directLighting + emissive;
    color = color / (color + vec3(1.0));

    outColor = vec4(color, albedo.a);
}
