#version 450

// The alpha test for Mask materials in the shadow pass. Opaque casters are drawn with no fragment
// shader at all. The coverage here must match triangle.frag's, or cutouts would cast the wrong
// shape: the same two-layer base color blend, and the same cutoff against its alpha.
layout(push_constant) uniform ShadowConstants
{
    mat4 lightModelViewProjection;
    vec4 baseColorFactor;
    vec4 nodeGraphFactors;
    vec4 alphaCutoffAndPadding; // x = alpha cutoff
    // The base colour's texture transform (material_uv.glsl): (a, b, tx, UV set), (c, d, ty, 0).
    vec4 baseColorTransformRow0;
    vec4 baseColorTransformRow1;
}
shadowData;

// The material set, bound at set 0 here: this pass has no camera set.
layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 0, binding = 6) uniform sampler2D secondaryBaseColorTexture;
layout(set = 0, binding = 12) uniform sampler2D blendMaskTexture;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec2 fragTexCoord1;

void main()
{
    // Layer B and the mask sample untransformed, as in the main passes.
    vec3 uv = vec3(shadowData.baseColorTransformRow0.w > 0.5 ? fragTexCoord1 : fragTexCoord, 1.0);
    vec2 baseColorUv = vec2(dot(shadowData.baseColorTransformRow0.xyz, uv), dot(shadowData.baseColorTransformRow1.xyz, uv));
    float blendMask = texture(blendMaskTexture, fragTexCoord).r;
    float blendWeight = clamp(
        mix(0.0, shadowData.nodeGraphFactors.y, clamp(shadowData.nodeGraphFactors.x, 0.0, 1.0)) * blendMask,
        0.0, 1.0);
    float alpha = mix(
                      texture(baseColorTexture, baseColorUv).a,
                      texture(secondaryBaseColorTexture, fragTexCoord).a,
                      blendWeight) *
                  shadowData.baseColorFactor.a;
    if (alpha < shadowData.alphaCutoffAndPadding.x)
        discard;
}
