#version 450
#extension GL_GOOGLE_include_directive : require

#include "gbuffer_common.glsl"
#include "normal_map.glsl"

layout(constant_id = 0) const bool kAlphaMask = false;

// Identical to the block in triangle.vert and triangle.frag; ObjectPushConstants' static
// assertions pin the layout on the C++ side.
layout(push_constant) uniform DrawConstants
{
    mat4 model;
    vec4 baseColorFactor;
    vec3 emissiveFactor;
    float alphaCutoff;
    vec4 surfaceFactors;
    vec4 nodeGraphFactors;
}
drawData;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 1) uniform sampler2D normalTexture;
layout(set = 1, binding = 2) uniform sampler2D metallicTexture;
layout(set = 1, binding = 3) uniform sampler2D roughnessTexture;
layout(set = 1, binding = 4) uniform sampler2D occlusionTexture;
layout(set = 1, binding = 5) uniform sampler2D emissiveTexture;
layout(set = 1, binding = 6) uniform sampler2D secondaryBaseColorTexture;
layout(set = 1, binding = 7) uniform sampler2D secondaryNormalTexture;
layout(set = 1, binding = 8) uniform sampler2D secondaryMetallicTexture;
layout(set = 1, binding = 9) uniform sampler2D secondaryRoughnessTexture;
layout(set = 1, binding = 10) uniform sampler2D secondaryOcclusionTexture;
layout(set = 1, binding = 11) uniform sampler2D secondaryEmissiveTexture;
layout(set = 1, binding = 12) uniform sampler2D blendMaskTexture;

// triangle.vert also writes world position at location 4. The G-buffer does not store it; the
// lighting pass reconstructs it from depth, so the input is deliberately not declared here.
layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragWorldNormal;
layout(location = 3) in vec4 fragWorldTangent;
layout(location = 5) in vec4 fragCurrClip;
layout(location = 6) in vec4 fragPrevClip;

// Locations match VulkanGeometryPass::kAttachments. All five are vec4 so no attachment receives
// fewer components than it has; channels the encoding table marks unused are written as stated.
layout(location = 0) out vec4 outAlbedo;   // GB0 R8G8B8A8_SRGB: rgb albedo, a = 1
layout(location = 1) out vec4 outNormal;   // GB1 R16G16B16A16_SFLOAT: rg shading normal, ba geometric normal, both octahedral
layout(location = 2) out vec4 outSurface;  // GB2 R8G8B8A8_UNORM: metallic, roughness, occlusion, a = 0
layout(location = 3) out vec4 outEmissive; // GB3 B10G11R11_UFLOAT: rgb emissive
layout(location = 4) out vec4 outVelocity; // R16G16_SFLOAT: current uv - previous uv

void main()
{
    // ---- Blend mask & blend weight ----------------------------------------
    float blendMask = texture(blendMaskTexture, fragTexCoord).r;
    float blendWeight = clamp(
        mix(0.0, drawData.nodeGraphFactors.y, clamp(drawData.nodeGraphFactors.x, 0.0, 1.0)) * blendMask,
        0.0, 1.0);

    // ---- Albedo -----------------------------------------------------------
    vec4 primaryBaseColor = texture(baseColorTexture, fragTexCoord);
    vec4 secondaryBaseColor = texture(secondaryBaseColorTexture, fragTexCoord);
    vec4 sampledBaseColor = mix(primaryBaseColor, secondaryBaseColor, blendWeight);
    vec4 albedo = sampledBaseColor * vec4(fragColor, 1.0) * drawData.baseColorFactor;

    if (kAlphaMask && albedo.a < drawData.alphaCutoff)
        discard;

    // ---- Normal -----------------------------------------------------------
    // A back face is only rasterized by a double-sided pipeline, and it is seen from the side the
    // vertex normal points away from. Mirroring the whole tangent frame, bitangent included, shades
    // it as the front face would be from the other side, with the normal map's perturbation
    // mirrored along with it. Single-sided pipelines cull back faces, so this is a no-op for them.
    float faceSign = gl_FrontFacing ? 1.0 : -1.0;
    vec3 geoNormal = normalize(fragWorldNormal) * faceSign;
    vec3 faceTangent = fragWorldTangent.xyz * faceSign;
    vec3 tangent = normalize(faceTangent - geoNormal * dot(geoNormal, faceTangent));
    // cross(-N, -T) == cross(N, T), so the flip has to be applied to the bitangent explicitly.
    vec3 bitangent = normalize(cross(geoNormal, tangent) * fragWorldTangent.w) * faceSign;
    mat3 TBN = mat3(tangent, bitangent, geoNormal);

    vec3 nrmPrimary = DecodeNormalMap(texture(normalTexture, fragTexCoord));
    vec3 nrmSecondary = DecodeNormalMap(texture(secondaryNormalTexture, fragTexCoord));
    vec3 nrmSample = normalize(mix(nrmPrimary, nrmSecondary, blendWeight));
    nrmSample.xy *= drawData.surfaceFactors.z; // normal scale
    vec3 N = normalize(TBN * nrmSample);

    // ---- PBR factors ------------------------------------------------------
    float metallicSample = mix(
        texture(metallicTexture, fragTexCoord).b,
        texture(secondaryMetallicTexture, fragTexCoord).b,
        blendWeight);
    float roughnessSample = mix(
        texture(roughnessTexture, fragTexCoord).g,
        texture(secondaryRoughnessTexture, fragTexCoord).g,
        blendWeight);
    float aoSample = mix(
        texture(occlusionTexture, fragTexCoord).r,
        texture(secondaryOcclusionTexture, fragTexCoord).r,
        blendWeight);
    vec3 emissiveSample = mix(
        texture(emissiveTexture, fragTexCoord).rgb,
        texture(secondaryEmissiveTexture, fragTexCoord).rgb,
        blendWeight);

    float metallic = clamp(drawData.surfaceFactors.x * metallicSample, 0.0, 1.0);
    float roughness = clamp(drawData.surfaceFactors.y * roughnessSample, 0.04, 1.0);
    float ao = mix(1.0, aoSample, clamp(drawData.surfaceFactors.w, 0.0, 1.0));

    // ---- Encode -----------------------------------------------------------
    // Albedo is written linear; the _SRGB format encodes it in hardware and the lighting pass's
    // sampler decodes it. Values above 1.0 would clamp here where the forward path kept them, which
    // glTF's [0, 1] base color factor rules out for imported materials.
    outAlbedo = vec4(albedo.rgb, 1.0);
    // The geometric normal rides along for the shadow lookup's normal offset (see ShadeSurface).
    // It is already face-flipped, so the lighting pass uses it as decoded.
    outNormal = vec4(EncodeNormalOctahedral(N), EncodeNormalOctahedral(geoNormal));
    outSurface = vec4(metallic, roughness, ao, 0.0);
    outEmissive = vec4(emissiveSample * drawData.emissiveFactor, 0.0);

    // uv = ndc * 0.5 + 0.5 with the Y flip inside the projection, so half the NDC difference is
    // the motion in UV units. A consumer finds the previous position at uv - velocity.
    vec2 currNdc = fragCurrClip.xy / fragCurrClip.w;
    vec2 prevNdc = fragPrevClip.xy / fragPrevClip.w;
    outVelocity = vec4((currNdc - prevNdc) * 0.5, 0.0, 0.0);
}
