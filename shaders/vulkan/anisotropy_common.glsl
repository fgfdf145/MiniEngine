// Anisotropic specular (KHR_materials_anisotropy) shared by pbr_common.glsl, gbuffer.frag and
// tests/anisotropy_tests.cpp, which compiles this file as C++. Written, like ssr_common.glsl, in the
// subset GLSL and C++/GLM both accept: every float literal carries the f suffix, no out parameters,
// const only for literal initializers.

#ifndef ANISOTROPY_COMMON_GLSL
#define ANISOTROPY_COMMON_GLSL

const float kAnisotropyPi = 3.14159265f;

// An orthonormal frame around a unit normal that depends on the normal alone (Duff et al. 2017,
// "Building an Orthonormal Basis, Revisited"). The G-buffer stores the anisotropy direction as an
// angle in this frame, and the lighting pass rebuilds the frame from the decoded normal.
vec3 OrthonormalTangent(vec3 n)
{
    float s = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (s + n.z);
    float b = n.x * n.y * a;
    return vec3(1.0f + s * n.x * n.x * a, s * b, -s * n.x);
}

vec3 OrthonormalBitangent(vec3 n)
{
    float s = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (s + n.z);
    float b = n.x * n.y * a;
    return vec3(b, s + n.y * n.y * a, -n.y);
}

// The anisotropy direction t (unit, in the surface of n) as an angle in [0, pi) over pi, for GB5.b.
// The lobe is the same for t and -t, so half a turn is enough.
float EncodeAnisotropyAngle(vec3 n, vec3 t)
{
    float angle = atan(dot(t, OrthonormalBitangent(n)), dot(t, OrthonormalTangent(n)));
    if (angle < 0.0f)
    {
        angle += kAnisotropyPi;
    }
    return clamp(angle / kAnisotropyPi, 0.0f, 1.0f);
}

vec3 DecodeAnisotropyTangent(vec3 n, float encoded)
{
    float angle = encoded * kAnisotropyPi;
    return normalize(cos(angle) * OrthonormalTangent(n) + sin(angle) * OrthonormalBitangent(n));
}

// The glTF direction in tangent space: the texture's RG remapped from [0, 1] to [-1, 1], turned
// counter-clockwise by the material's rotation (given as its cosine and sine).
vec2 AnisotropyDirection(vec2 textureRg, float cosRotation, float sinRotation)
{
    vec2 d = textureRg * 2.0f - vec2(1.0f);
    return vec2(cosRotation * d.x - sinRotation * d.y, sinRotation * d.x + cosRotation * d.y);
}

// Anisotropic GGX (Burley 2012), with alphaT along the anisotropy direction and alphaB across it.
// Equal alphas give the isotropic GGX.
float DistributionGgxAnisotropic(float NdH, float TdH, float BdH, float alphaT, float alphaB)
{
    float a2 = alphaT * alphaB;
    vec3 f = vec3(alphaB * TdH, alphaT * BdH, a2 * NdH);
    float w2 = a2 / dot(f, f);
    return a2 * w2 * w2 / kAnisotropyPi;
}

// The height-correlated anisotropic Smith visibility, G / (4 N.L N.V) (Heitz 2014). The Khronos
// sample viewer clamps it to 1, which cuts off the legitimate values above 1 at grazing angles;
// this only keeps the division finite.
float VisibilityGgxAnisotropic(float NdL, float NdV, float TdV, float BdV, float TdL, float BdL, float alphaT, float alphaB)
{
    float ggxV = NdL * length(vec3(alphaT * TdV, alphaB * BdV, NdV));
    float ggxL = NdV * length(vec3(alphaT * TdL, alphaB * BdL, NdL));
    return 0.5f / max(ggxV + ggxL, 1e-7f);
}

// The normal the environment lookup reflects about: bent from n toward the plane across the
// anisotropy direction, as far as the strength and the smoothness say (the Khronos sample viewer
// and Filament). No strength, no bend.
vec3 AnisotropicBentNormal(vec3 n, vec3 v, vec3 t, float strength, float roughness)
{
    vec3 b = cross(n, t);
    vec3 anisotropicNormal = cross(cross(b, v), b);
    float bend = 1.0f - strength * (1.0f - roughness);
    float bend4 = bend * bend * bend * bend;
    return normalize(mix(anisotropicNormal, n, bend4));
}

#endif
