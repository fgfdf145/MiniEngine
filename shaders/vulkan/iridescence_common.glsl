// Thin-film iridescence (KHR_materials_iridescence) after Belcour and Barla 2017, "A Practical
// Extension to Microfacet Theory for the Modeling of Varying Iridescence". Shared by
// pbr_common.glsl and tests/iridescence_tests.cpp, which compiles this file as C++. Written, like
// ssr_common.glsl, in the subset GLSL and C++/GLM both accept: every float literal carries the f
// suffix, no out parameters, const only for literal initializers.

#ifndef IRIDESCENCE_COMMON_GLSL
#define IRIDESCENCE_COMMON_GLSL

const float kIridescencePi = 3.14159265f;

float IridescenceSquare(float x)
{
    return x * x;
}

// The reflectance at normal incidence between media of these indices.
float IorToFresnel0(float transmittedIor, float incidentIor)
{
    return IridescenceSquare((transmittedIor - incidentIor) / (transmittedIor + incidentIor));
}

vec3 IorToFresnel0(vec3 transmittedIor, float incidentIor)
{
    vec3 ratio = (transmittedIor - vec3(incidentIor)) / (transmittedIor + vec3(incidentIor));
    return ratio * ratio;
}

// The index of refraction (against air) that gives this F0.
vec3 Fresnel0ToIor(vec3 f0)
{
    vec3 root = sqrt(f0);
    return (vec3(1.0f) + root) / (vec3(1.0f) - root);
}

float SchlickFresnel(float f0, float cosTheta)
{
    return f0 + (1.0f - f0) * pow(clamp(1.0f - cosTheta, 0.0f, 1.0f), 5.0f);
}

vec3 SchlickFresnel(vec3 f0, float cosTheta)
{
    return f0 + (vec3(1.0f) - f0) * pow(clamp(1.0f - cosTheta, 0.0f, 1.0f), 5.0f);
}

// The spectral integral of one interference term, the CIE matching functions fitted by Gaussians
// in Fourier space (the paper's supplemental), for an optical path difference in nanometres and a
// phase shift per channel. Returns linear Rec. 709.
vec3 IridescenceSensitivity(float opd, vec3 shift)
{
    float phase = 2.0f * kIridescencePi * opd * 1.0e-9f;
    vec3 val = vec3(5.4856e-13f, 4.4201e-13f, 5.2481e-13f);
    vec3 pos = vec3(1.6810e+06f, 1.7953e+06f, 2.2084e+06f);
    vec3 var = vec3(4.3278e+09f, 9.3046e+09f, 6.6121e+09f);
    vec3 xyz = val * sqrt(2.0f * kIridescencePi * var) * cos(pos * phase + shift) * exp(-phase * phase * var);
    xyz.x += 9.7470e-14f * sqrt(2.0f * kIridescencePi * 4.5282e+09f) * cos(2.2399e+06f * phase + shift.x) *
             exp(-4.5282e+09f * phase * phase);
    xyz /= 1.0685e-7f;
    return vec3(
        3.2404542f * xyz.x - 1.5371385f * xyz.y - 0.4985314f * xyz.z,
        -0.9692660f * xyz.x + 1.8760108f * xyz.y + 0.0415560f * xyz.z,
        0.0556434f * xyz.x - 0.2040259f * xyz.y + 1.0572252f * xyz.z);
}

// The Fresnel reflectance of a film of index filmIor and thickness (nanometres) over a base of
// reflectance baseF0, seen from air at cosTheta1: the Airy sum truncated to two interference
// terms. A film thinner than 0.03 nm fades into no film, which reflects the base's Schlick Fresnel.
// The fade applies to the result too: Schlick's approximation of the vanishing air-to-film
// interface still reflects 60% at grazing angles, which the Khronos sample viewer lets through.
vec3 EvaluateIridescence(float filmIor, float cosTheta1, float thickness, vec3 baseF0)
{
    float outsideIor = 1.0f;
    float presence = smoothstep(0.0f, 0.03f, thickness);
    float iridescenceIor = mix(outsideIor, filmIor, presence);
    float sinTheta2Sq = IridescenceSquare(outsideIor / iridescenceIor) * (1.0f - IridescenceSquare(cosTheta1));
    float cosTheta2Sq = 1.0f - sinTheta2Sq;
    if (cosTheta2Sq < 0.0f)
    {
        return vec3(1.0f); // total internal reflection
    }
    float cosTheta2 = sqrt(cosTheta2Sq);

    // The first interface, air to film.
    float R12 = SchlickFresnel(IorToFresnel0(iridescenceIor, outsideIor), cosTheta1);
    float T121 = 1.0f - R12;
    float phi12 = iridescenceIor < outsideIor ? kIridescencePi : 0.0f;
    float phi21 = kIridescencePi - phi12;

    // The second interface, film to base.
    vec3 baseIor = Fresnel0ToIor(clamp(baseF0, vec3(0.0f), vec3(0.9999f)));
    vec3 R23 = SchlickFresnel(IorToFresnel0(baseIor, iridescenceIor), cosTheta2);
    vec3 phi23 = vec3(
        baseIor.x < iridescenceIor ? kIridescencePi : 0.0f,
        baseIor.y < iridescenceIor ? kIridescencePi : 0.0f,
        baseIor.z < iridescenceIor ? kIridescencePi : 0.0f);

    float opd = 2.0f * iridescenceIor * thickness * cosTheta2;
    vec3 phi = vec3(phi21) + phi23;

    vec3 R123 = clamp(R12 * R23, vec3(1e-5f), vec3(0.9999f));
    vec3 r123 = sqrt(R123);
    vec3 Rs = IridescenceSquare(T121) * R23 / (vec3(1.0f) - R123);

    vec3 I = vec3(R12) + Rs;
    vec3 Cm = Rs - vec3(T121);
    for (int m = 1; m <= 2; ++m)
    {
        Cm *= r123;
        vec3 Sm = 2.0f * IridescenceSensitivity(float(m) * opd, float(m) * phi);
        I += Cm * Sm;
    }
    // The Gaussian colour-matching fit and the conversion to Rec. 709 can push a saturated
    // interference colour a little past 1 in one channel (1.09 at 75 nm); a reflectance cannot.
    return mix(SchlickFresnel(baseF0, cosTheta1), clamp(I, vec3(0.0f), vec3(1.0f)), presence);
}

#endif
