// ReSTIR PT Enhanced (Lin, Kettunen and Wyman 2026; docs/design/2026-10-07-restir-pt-enhanced-design.md):
// what the passes of VulkanRestirPtPass share. The reservoir and its packing, the primary surface
// records, the counter-based random numbers that random replay depends on, the lobe-indexed BSDF, the
// analytic lights' next event estimation, the reconnection criteria and the hybrid shift.
//
// Path space. A path is x1 ... x_d: x1 the pixel's G-buffer surface (x0 the camera), every later vertex
// found by a BSDF-sampled ray, and the last either an emitter that ray hit (an emissive surface or the
// sky) or a point on an analytic light reached by next event estimation (NEE). The two sets of emitters
// do not overlap (analytic lights are not in the ray scene; NEE never draws an emissive triangle or
// the sky), so neither technique needs a MIS weight. Every vertex but the last samples one BSDF lobe
// (PT_LOBE_*), stored per vertex; NEE evaluates every lobe (PT_LOBE_NEE).
//
// Primary sample space. The random numbers of vertex i are PtRandom(seed, i, dimension): dimension 0
// picks the lobe, 1 and 2 the direction, 3 Russian roulette (initial paths only), 4 the path
// selection, 8 + 4c the NEE candidates. A shift replays them from the other pixel's surface. Russian
// roulette and NEE's resampling stay out of the space: their effect is folded into the initial sample's
// unbiased contribution weight (the paper's supplemental sections 5 and 6).
//
// Includes scene_common.glsl, gbuffer_common.glsl, pbr_common.glsl, ray_tracing_common.glsl,
// material_common.glsl, material_uv.glsl and ray_hit_common.glsl before this file.

#ifndef RESTIR_PT_COMMON_GLSL
#define RESTIR_PT_COMMON_GLSL

// The pass's own set (VulkanRestirPtPass), set 2 of every ReSTIR PT pipeline; each shader uses a part.
layout(set = 2, binding = 0) uniform sampler2D ptDepth;    // nearest
layout(set = 2, binding = 1) uniform sampler2D ptNormal;   // GB1, nearest
layout(set = 2, binding = 2) uniform sampler2D ptAlbedo;   // GB0, nearest
layout(set = 2, binding = 3) uniform sampler2D ptSurface;  // GB2, nearest
layout(set = 2, binding = 4) uniform sampler2D ptCoat;     // GB6, nearest
layout(set = 2, binding = 5) uniform sampler2D ptVelocity; // rg uv motion, ba a mapped coat's normal
layout(set = 2, binding = 6, rgba16f) uniform writeonly image2D ptOutput;
// Reservoirs, four uvec4 each: the frame's working reservoirs (initial, then temporal in place) and the
// history (last frame's final reservoirs, read by the temporal reuse, then this frame's final ones).
layout(std430, set = 2, binding = 7) buffer PtWorkReservoirs
{
    uvec4 workReservoirs[];
};
layout(std430, set = 2, binding = 8) buffer PtHistoryReservoirs
{
    uvec4 historyReservoirs[];
};
// Primary surface records, two uvec4 each: this frame's and last frame's.
layout(std430, set = 2, binding = 9) buffer PtCurrentSurfaces
{
    uvec4 currentSurfaces[];
};
layout(std430, set = 2, binding = 10) readonly buffer PtPreviousSurfaces
{
    uvec4 previousSurfaces[];
};
// The paired spatial reuse's shifts, one uvec4 per pixel and pairing texture: this pixel's sample
// shifted onto its partner's surface (PtPackShift).
layout(std430, set = 2, binding = 11) buffer PtShifts
{
    uvec4 spatialShifts[];
};
// The duplication score of each pixel's final reservoir (restir_pt_duplication.comp).
layout(std430, set = 2, binding = 12) buffer PtDuplication
{
    float duplication[];
};
// The pairing textures (engine/renderer/restir_pairing.h), two signed bytes a texel, two texels a word.
layout(std430, set = 2, binding = 13) readonly buffer PtPairing
{
    uint pairing[];
};
// The running average of the accumulate mode.
layout(std430, set = 2, binding = 14) buffer PtAccumulation
{
    vec4 accumulation[];
};

// RestirPtPushConstants in engine/renderer/vulkan/restir_pt_pass.cpp.
layout(push_constant) uniform RestirPtConstants
{
    uvec2 extent;
    uint frameIndex;
    uint flags;
    uint maxBounces;
    uint neeCandidates;
    uint debugView;
    uint accumulatedFrames;
    float footprintScale;
    float roughnessThreshold;
    float legacyDistance;
    float cap;
    float capMin;
    float capGamma;
    uint unused0;
    uint unused1;
    // xyz last frame's camera position, the camera the history's surfaces were seen from.
    vec4 previousCamera;
}
pt;

#define PT_FLAG_TEMPORAL 1u
#define PT_FLAG_SPATIAL 2u
#define PT_FLAG_FOOTPRINT 4u
#define PT_FLAG_DECORRELATION 8u
#define PT_FLAG_COLOR_NOISE 16u
#define PT_FLAG_DUAL_MOTION 32u
#define PT_FLAG_RUSSIAN_ROULETTE 64u
#define PT_FLAG_ACCUMULATE 128u
#define PT_FLAG_HISTORY_VALID 256u

#define PT_DEBUG_DUPLICATION 1u
#define PT_DEBUG_RECONNECTION 2u
#define PT_DEBUG_CONFIDENCE 3u
#define PT_DEBUG_LENGTH 4u
#define PT_DEBUG_PAIRING 5u

#define PT_LOBE_NEE 0u
#define PT_LOBE_DIFFUSE 1u
#define PT_LOBE_SPECULAR 2u
#define PT_LOBE_COAT 3u

#define PT_RC_NONE 0u
#define PT_RC_SURFACE 1u
#define PT_RC_SKY 2u
#define PT_RC_LIGHT 3u

// The pairing textures' sizes and where each starts in pairing[], in texels (RestirPtPairingSizes).
const uint PT_PAIRING_SIZES[3] = uint[3](254u, 230u, 210u);
const uint PT_PAIRING_OFFSETS[3] = uint[3](0u, 64516u, 117416u);
#define PT_SPATIAL_NEIGHBOURS 3u

// How far an open ray, a sky reconnection and the sun's shadow ray look.
const float PT_RAY_DISTANCE = 50000.0;
const float PT_SUN_DISTANCE = 20000.0;
const float PT_PI = 3.14159265359;
// The suffix radiance is stored scaled by 2^-10 so the sun's light fits RGB9E5.
const float PT_SUFFIX_SCALE = 1.0 / 1024.0;

bool PtFlag(uint flag)
{
    return (pt.flags & flag) != 0u;
}

uint PtPixelIndex(ivec2 pixel)
{
    return uint(pixel.y) * pt.extent.x + uint(pixel.x);
}

bool PtInside(ivec2 pixel)
{
    return all(greaterThanEqual(pixel, ivec2(0))) && all(lessThan(pixel, ivec2(pt.extent)));
}

float PtLuminance(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

float PtMax3(vec3 c)
{
    return max(c.r, max(c.g, c.b));
}

// ---------------------------------------------------------------------------
// Random numbers
// ---------------------------------------------------------------------------

uint PtHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// The path's random number for one vertex and dimension, in [0, 1): a hash of the three, so replaying a
// path from another pixel draws the same numbers wherever it is.
float PtRandom(uint seed, uint vertex, uint dimension)
{
    return float(PtHash(seed ^ PtHash(vertex * 512u + dimension + 0x68e31da4u)) >> 8) * (1.0 / 16777216.0);
}

// Each pixel's path seed this frame, never 0 (an empty reservoir's).
uint PtPathSeed(ivec2 pixel)
{
    return PtHash(PtPixelIndex(pixel) * 0x9E3779B1u ^ PtHash(pt.frameIndex + 0x3c6ef372u)) | 1u;
}

// The coverage hash of the rays a path traces from a vertex: the same for the base path and its replay.
uint PtRayId(uint seed, uint vertex)
{
    return PtHash(seed + vertex * 0x85ebca6bu);
}

// ---------------------------------------------------------------------------
// Packing
// ---------------------------------------------------------------------------

uint PtPackNormal(vec3 n)
{
    return packSnorm2x16(EncodeNormalOctahedral(n));
}

vec3 PtUnpackNormal(uint packed)
{
    return DecodeNormalOctahedral(unpackSnorm2x16(packed));
}

uint PtPackNormal12(vec3 n)
{
    vec2 e = clamp(EncodeNormalOctahedral(n) * 0.5 + 0.5, 0.0, 1.0);
    uvec2 q = uvec2(round(e * 4095.0));
    return q.x | (q.y << 12);
}

vec3 PtUnpackNormal12(uint packed)
{
    vec2 e = vec2(float(packed & 4095u), float((packed >> 12) & 4095u)) / 4095.0;
    return DecodeNormalOctahedral(e * 2.0 - 1.0);
}

// RGB9E5 with a shared exponent, for non-negative colours below 65408.
uint PtPackRgb9e5(vec3 c)
{
    const float maxValue = 65408.0;
    c = clamp(c, vec3(0.0), vec3(maxValue));
    float maxChannel = max(PtMax3(c), 1e-30);
    int exponent = max(-16, int(floor(log2(maxChannel)))) + 1;
    float scale = exp2(float(exponent - 9));
    uvec3 m = uvec3(min(round(c / scale), vec3(511.0)));
    if (any(equal(m, uvec3(512u))))
    {
        exponent += 1;
        scale *= 2.0;
        m = uvec3(min(round(c / scale), vec3(511.0)));
    }
    uint biased = uint(clamp(exponent + 15, 0, 31));
    return m.r | (m.g << 9) | (m.b << 18) | (biased << 27);
}

vec3 PtUnpackRgb9e5(uint packed)
{
    float scale = exp2(float(int(packed >> 27) - 15 - 9));
    return vec3(float(packed & 511u), float((packed >> 9) & 511u), float((packed >> 18) & 511u)) * scale;
}

// ---------------------------------------------------------------------------
// Reservoirs
// ---------------------------------------------------------------------------

// 64 bytes (the paper's Algorithm 1 in spirit): the sample's integrand at its own surface F and unbiased
// contribution weight W, the confidence M, the path's seed and shape, and the reconnection vertex x_k
// with what the shift needs to reconnect to it: its position (a direction for the sky, a point or
// direction on the light for a light vertex), normals, material, the next direction omega_k, the light
// carried into x_k along it (the suffix) and the base path's Jacobian terms
// p_{k-1}(omega_{k-1}) G(x_{k-1} -> x_k) p_k(omega_k).
struct PtReservoir
{
    vec3 F;
    float W;
    uint seed;
    // bits 0-2 the vertex count d (0 empty), 3-5 the reconnection vertex k (0 none: pure replay),
    // 6-7 its type (PT_RC_*), 8-21 the lobe of vertices 1-7, two bits each, 22-31 the reconnection
    // vertex's roughness (log-encoded, PtSetRcSurface).
    uint flags;
    float M;
    float rcJacobian;
    vec3 rcPosition;
    uint rcFaceNormal;
    uint rcWi;
    uint rcSuffix;
    // Albedo rgb8 and metallic8, or a light vertex's light index.
    uint rcMaterial;
    // The shading normal, octahedral 16 + 16 bits.
    uint rcExtra;
};

PtReservoir PtEmptyReservoir()
{
    PtReservoir r;
    r.F = vec3(0.0);
    r.W = 0.0;
    r.seed = 0u;
    r.flags = 0u;
    r.M = 0.0;
    r.rcJacobian = 1.0;
    r.rcPosition = vec3(0.0);
    r.rcFaceNormal = 0u;
    r.rcWi = 0u;
    r.rcSuffix = 0u;
    r.rcMaterial = 0u;
    r.rcExtra = 0u;
    return r;
}

uint PtPathLength(uint flags)
{
    return flags & 7u;
}

uint PtRcIndex(uint flags)
{
    return (flags >> 3) & 7u;
}

uint PtRcType(uint flags)
{
    return (flags >> 6) & 3u;
}

uint PtLobeAt(uint flags, uint vertex)
{
    return (flags >> (8u + 2u * (vertex - 1u))) & 3u;
}

// The shape bits, keeping the reconnection vertex's roughness that flags already hold.
uint PtShapeFlags(uint d, uint k, uint type, uint lobes, uint flags)
{
    return d | (k << 3) | (type << 6) | (lobes << 8) | (flags & 0xFFC00000u);
}

void PtStoreReservoir(uint index, PtReservoir r, bool history)
{
    uvec4 q0 = uvec4(floatBitsToUint(r.F), floatBitsToUint(r.W));
    uvec4 q1 = uvec4(r.seed, r.flags, floatBitsToUint(r.M), floatBitsToUint(r.rcJacobian));
    uvec4 q2 = uvec4(floatBitsToUint(r.rcPosition), r.rcFaceNormal);
    uvec4 q3 = uvec4(r.rcWi, r.rcSuffix, r.rcMaterial, r.rcExtra);
    if (history)
    {
        historyReservoirs[index * 4u + 0u] = q0;
        historyReservoirs[index * 4u + 1u] = q1;
        historyReservoirs[index * 4u + 2u] = q2;
        historyReservoirs[index * 4u + 3u] = q3;
    }
    else
    {
        workReservoirs[index * 4u + 0u] = q0;
        workReservoirs[index * 4u + 1u] = q1;
        workReservoirs[index * 4u + 2u] = q2;
        workReservoirs[index * 4u + 3u] = q3;
    }
}

PtReservoir PtUnpackReservoir(uvec4 q0, uvec4 q1, uvec4 q2, uvec4 q3)
{
    PtReservoir r;
    r.F = uintBitsToFloat(q0.xyz);
    r.W = uintBitsToFloat(q0.w);
    r.seed = q1.x;
    r.flags = q1.y;
    r.M = uintBitsToFloat(q1.z);
    r.rcJacobian = uintBitsToFloat(q1.w);
    r.rcPosition = uintBitsToFloat(q2.xyz);
    r.rcFaceNormal = q2.w;
    r.rcWi = q3.x;
    r.rcSuffix = q3.y;
    r.rcMaterial = q3.z;
    r.rcExtra = q3.w;
    return r;
}

PtReservoir PtLoadReservoir(uint index, bool history)
{
    if (history)
    {
        return PtUnpackReservoir(historyReservoirs[index * 4u], historyReservoirs[index * 4u + 1u], historyReservoirs[index * 4u + 2u], historyReservoirs[index * 4u + 3u]);
    }
    return PtUnpackReservoir(workReservoirs[index * 4u], workReservoirs[index * 4u + 1u], workReservoirs[index * 4u + 2u], workReservoirs[index * 4u + 3u]);
}

// The first half of a working reservoir: what resampling weighs (F, W, M, the seed and shape), without
// the reconnection vertex, which only the chosen sample needs.
PtReservoir PtLoadWorkReservoirHead(uint index)
{
    return PtUnpackReservoir(workReservoirs[index * 4u], workReservoirs[index * 4u + 1u], uvec4(0u), uvec4(0u));
}

bool PtReservoirEmpty(PtReservoir r)
{
    return PtPathLength(r.flags) == 0u;
}

// ---------------------------------------------------------------------------
// Surfaces and the BSDF
// ---------------------------------------------------------------------------

// A path vertex's surface. Both normals face the side the path arrives from.
struct PtSurface
{
    vec3 position;
    vec3 normal;
    vec3 faceNormal;
    vec3 albedo;
    float metallic;
    float roughness;
    // KHR_materials_clearcoat at the primary surface only; the ray scene's materials have none.
    float coatFactor;
    float coatRoughness;
    vec3 coatNormal;
    // Physical radiance the surface emits (cd/m^2).
    vec3 emissive;
    // A surface that reflects nothing (unlit); the path ends at it.
    bool black;
    bool valid;
    // How far rays leave it along the face normal before the ulp offset: a G-buffer surface's position
    // carries the depth buffer's error, which grows with distance (the traced effects' offset).
    float rayOffset;
};

PtSurface PtInvalidSurface()
{
    PtSurface s;
    s.position = vec3(0.0);
    s.normal = vec3(0.0, 0.0, 1.0);
    s.faceNormal = vec3(0.0, 0.0, 1.0);
    s.albedo = vec3(0.0);
    s.metallic = 0.0;
    s.roughness = 1.0;
    s.coatFactor = 0.0;
    s.coatRoughness = 1.0;
    s.coatNormal = vec3(0.0, 0.0, 1.0);
    s.emissive = vec3(0.0);
    s.black = true;
    s.valid = false;
    s.rayOffset = 0.0;
    return s;
}

// The traced effects' offset for a surface reconstructed from the depth buffer.
float PtPrimaryRayOffset(vec3 position, vec3 camera)
{
    return 0.002 + 0.0004 * length(position - camera);
}

// The primary surface record (32 bytes): position, normals, material, coat and the uv motion, which the
// dual motion vectors read as the occluder's motion a frame later.
void PtStoreSurfaceRecord(uint index, PtSurface s, vec2 velocity)
{
    uvec4 a = uvec4(floatBitsToUint(s.position), PtPackNormal(s.normal));
    uvec4 b = uvec4(
        PtPackNormal(s.faceNormal),
        packUnorm4x8(vec4(s.albedo, s.roughness)),
        packUnorm4x8(vec4(s.metallic, s.coatFactor, s.coatRoughness, s.valid ? 1.0 : 0.0)),
        packHalf2x16(velocity));
    currentSurfaces[index * 2u] = a;
    currentSurfaces[index * 2u + 1u] = b;
}

PtSurface PtUnpackSurfaceRecord(uvec4 a, uvec4 b, out vec2 velocity)
{
    PtSurface s = PtInvalidSurface();
    vec4 material = unpackUnorm4x8(b.z);
    velocity = unpackHalf2x16(b.w);
    if (material.w < 0.5)
    {
        return s;
    }
    vec4 albedoRoughness = unpackUnorm4x8(b.y);
    s.position = uintBitsToFloat(a.xyz);
    s.normal = PtUnpackNormal(a.w);
    s.faceNormal = PtUnpackNormal(b.x);
    s.albedo = albedoRoughness.rgb;
    s.roughness = clamp(albedoRoughness.a, 0.04, 1.0);
    s.metallic = material.x;
    s.coatFactor = material.y;
    s.coatRoughness = clamp(material.z, 0.04, 1.0);
    s.coatNormal = s.faceNormal;
    s.black = false;
    s.valid = true;
    s.rayOffset = PtPrimaryRayOffset(s.position, ubo.cameraWorldPosition.xyz);
    return s;
}

bool PtSurfaceValid(uint index)
{
    return unpackUnorm4x8(currentSurfaces[index * 2u + 1u].z).w > 0.5;
}

PtSurface PtLoadSurface(uint index, bool previous, out vec2 velocity)
{
    if (previous)
    {
        return PtUnpackSurfaceRecord(previousSurfaces[index * 2u], previousSurfaces[index * 2u + 1u], velocity);
    }
    return PtUnpackSurfaceRecord(currentSurfaces[index * 2u], currentSurfaces[index * 2u + 1u], velocity);
}

float PtSchlick(float cosTheta, float f0)
{
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (1.0 - f0) * m2 * m2 * m;
}

vec3 PtSchlick3(float cosTheta, vec3 f0)
{
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (vec3(1.0) - f0) * (m2 * m2 * m);
}

// The probability each lobe is picked with for an outgoing direction V: its albedo's estimate there.
struct PtLobes
{
    float diffuse;
    float specular;
    float coat;
};

float PtCoatWeight(PtSurface s, vec3 V)
{
    return s.coatFactor > 0.0 ? s.coatFactor * PtSchlick(max(dot(s.coatNormal, V), 0.0), 0.04) : 0.0;
}

PtLobes PtLobeProbabilities(PtSurface s, vec3 V)
{
    float NdV = max(dot(s.normal, V), 1e-4);
    float coat = PtCoatWeight(s, V);
    float base = 1.0 - coat;
    vec3 F0 = mix(vec3(0.04), s.albedo, s.metallic);
    float specular = PtLuminance(PtSchlick3(NdV, F0)) * base;
    float diffuse = PtLuminance(s.albedo) * (1.0 - s.metallic) * (1.0 - PtSchlick(NdV, 0.04)) * base;
    float total = diffuse + specular + coat;
    PtLobes lobes;
    if (total < 1e-6)
    {
        lobes.diffuse = 1.0;
        lobes.specular = 0.0;
        lobes.coat = 0.0;
        return lobes;
    }
    lobes.diffuse = diffuse / total;
    lobes.specular = specular / total;
    lobes.coat = coat / total;
    return lobes;
}

float PtLobeProbability(PtLobes lobes, uint lobe)
{
    return lobe == PT_LOBE_DIFFUSE ? lobes.diffuse : (lobe == PT_LOBE_SPECULAR ? lobes.specular : lobes.coat);
}

uint PtChooseLobe(PtLobes lobes, float u)
{
    if (u < lobes.diffuse)
    {
        return PT_LOBE_DIFFUSE;
    }
    return u < lobes.diffuse + lobes.specular || lobes.coat <= 0.0 ? PT_LOBE_SPECULAR : PT_LOBE_COAT;
}

float PtLobeRoughness(PtSurface s, uint lobe)
{
    return lobe == PT_LOBE_SPECULAR ? s.roughness : (lobe == PT_LOBE_COAT ? s.coatRoughness : 1.0);
}

float PtSmithG1(float NdV, float alpha)
{
    float a2 = alpha * alpha;
    return 2.0 * NdV / (NdV + sqrt(a2 + (1.0 - a2) * NdV * NdV));
}

// The GGX lobe around n: D G2 / (4 N.V N.L) F N.L, and the visible normals' pdf of L.
vec3 PtGgxEval(vec3 n, float roughness, vec3 F0, vec3 V, vec3 L)
{
    float NdL = dot(n, L);
    if (NdL <= 0.0)
    {
        return vec3(0.0);
    }
    float NdV = max(dot(n, V), 1e-4);
    vec3 H = normalize(V + L);
    float alpha = roughness * roughness;
    float term = DistributionGGX(n, H, roughness) * VisibilitySmithGgxCorrelated(NdV, NdL, alpha);
    return PtSchlick3(max(dot(V, H), 0.0), F0) * (term * NdL);
}

float PtGgxPdf(vec3 n, float roughness, vec3 V, vec3 L)
{
    if (dot(n, L) <= 0.0)
    {
        return 0.0;
    }
    float NdV = max(dot(n, V), 1e-4);
    vec3 H = normalize(V + L);
    float alpha = roughness * roughness;
    return PtSmithG1(NdV, alpha) * DistributionGGX(n, H, roughness) / (4.0 * NdV);
}

// One lobe's f cos. Directions below the face normal are zero: no lobe transmits.
vec3 PtLobeEval(PtSurface s, uint lobe, vec3 V, vec3 L)
{
    if (dot(s.faceNormal, L) <= 0.0)
    {
        return vec3(0.0);
    }
    float base = 1.0 - PtCoatWeight(s, V);
    if (lobe == PT_LOBE_DIFFUSE)
    {
        float NdL = dot(s.normal, L);
        float NdV = max(dot(s.normal, V), 1e-4);
        return NdL <= 0.0 ? vec3(0.0) : s.albedo * ((1.0 - s.metallic) * (1.0 - PtSchlick(NdV, 0.04)) * base * NdL / PT_PI);
    }
    if (lobe == PT_LOBE_SPECULAR)
    {
        return PtGgxEval(s.normal, s.roughness, mix(vec3(0.04), s.albedo, s.metallic), V, L) * base;
    }
    return s.coatFactor > 0.0 ? PtGgxEval(s.coatNormal, s.coatRoughness, vec3(0.04), V, L) * s.coatFactor : vec3(0.0);
}

float PtLobePdf(PtSurface s, uint lobe, vec3 V, vec3 L)
{
    if (dot(s.faceNormal, L) <= 0.0)
    {
        return 0.0;
    }
    if (lobe == PT_LOBE_DIFFUSE)
    {
        return max(dot(s.normal, L), 0.0) / PT_PI;
    }
    return lobe == PT_LOBE_SPECULAR ? PtGgxPdf(s.normal, s.roughness, V, L) : PtGgxPdf(s.coatNormal, s.coatRoughness, V, L);
}

vec3 PtBsdfEval(PtSurface s, vec3 V, vec3 L)
{
    vec3 result = PtLobeEval(s, PT_LOBE_DIFFUSE, V, L) + PtLobeEval(s, PT_LOBE_SPECULAR, V, L);
    if (s.coatFactor > 0.0)
    {
        result += PtLobeEval(s, PT_LOBE_COAT, V, L);
    }
    return result;
}

// The pdf of drawing L over all lobes (the footprint test's p, the supplemental's section 3).
float PtMarginalPdf(PtSurface s, PtLobes lobes, vec3 V, vec3 L)
{
    float pdf = lobes.diffuse * PtLobePdf(s, PT_LOBE_DIFFUSE, V, L) + lobes.specular * PtLobePdf(s, PT_LOBE_SPECULAR, V, L);
    if (lobes.coat > 0.0)
    {
        pdf += lobes.coat * PtLobePdf(s, PT_LOBE_COAT, V, L);
    }
    return pdf;
}

vec3 PtCosineDirection(vec3 n, vec2 u)
{
    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(up, n));
    vec3 b = cross(n, t);
    float r = sqrt(u.x);
    float phi = 2.0 * PT_PI * u.y;
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(1.0 - u.x, 0.0)));
}

bool PtSampleLobe(PtSurface s, uint lobe, vec3 V, vec2 u, out vec3 L)
{
    if (lobe == PT_LOBE_DIFFUSE)
    {
        L = PtCosineDirection(s.normal, u);
    }
    else
    {
        vec3 n = lobe == PT_LOBE_SPECULAR ? s.normal : s.coatNormal;
        float roughness = lobe == PT_LOBE_SPECULAR ? s.roughness : s.coatRoughness;
        L = reflect(-V, SampleGgxVisibleNormal(n, V, roughness, u));
    }
    return dot(L, s.faceNormal) > 0.0 && dot(L, lobe == PT_LOBE_COAT ? s.coatNormal : s.normal) > 0.0;
}

// ---------------------------------------------------------------------------
// The scene seen by the paths
// ---------------------------------------------------------------------------

vec3 PtSkyRadiance(vec3 direction)
{
    if (EnvironmentMode() == ENVIRONMENT_NONE)
    {
        return SceneAmbientAlong(direction);
    }
    return textureLod(prefilteredEnvironment, direction, 0.0).rgb + SceneLightsAmbientAlong(direction);
}

vec3 PtRayOrigin(PtSurface s, vec3 direction)
{
    vec3 n = dot(s.faceNormal, direction) >= 0.0 ? s.faceNormal : -s.faceNormal;
    return OffsetRayOrigin(s.position + n * s.rayOffset, n);
}

// What a ray from a surface finds: the surface it hits (its material at a texture level that depends on
// the hit point alone, so a base path and its replay agree), invalid for a single-sided back face, or
// the sky (miss).
PtSurface PtTrace(PtSurface from, vec3 direction, uint rayId, out bool miss)
{
    vec3 origin = PtRayOrigin(from, direction);
    RayHit hit;
    miss = !TraceSceneRayMasked(origin, direction, 0.0, PT_RAY_DISTANCE, false, rayId, RAY_MASK_VISIBILITY, hit);
    if (miss)
    {
        return PtInvalidSurface();
    }
    RayMaterial rayMaterial = rayMaterials[rayInstances[hit.instance].data.z];
    bool doubleSided = (floatBitsToUint(rayMaterial.emissionFlags.w) & RAY_MATERIAL_DOUBLE_SIDED) != 0u;
    if (!hit.frontFace && !doubleSided)
    {
        return PtInvalidSurface();
    }
    RayHitSurface surface = RayHitSurfaceOf(hit, origin, direction);
    float pixelAngle = 2.0 / (abs(ubo.proj[1][1]) * float(pt.extent.y));
    float footprint = pixelAngle * length(surface.position - ubo.cameraWorldPosition.xyz);
    RayHitShading material = RayHitShadingOf(surface, footprint, 1.0);
    PtSurface s = PtInvalidSurface();
    s.position = surface.position;
    s.normal = surface.normal;
    s.faceNormal = surface.faceNormal;
    s.coatNormal = surface.faceNormal;
    s.albedo = material.albedo;
    s.metallic = material.metallic;
    s.roughness = material.roughness;
    s.emissive = material.emissive;
    s.black = false;
    s.valid = true;
    if ((material.flags & SHADING_FLAG_UNLIT) != 0u)
    {
        s.emissive = material.albedo * kFrameBufferUnitsPerExposed * ubo.exposure.y;
        s.black = true;
    }
    return s;
}

bool PtVisible(PtSurface from, vec3 direction, float distance, uint rayId)
{
    if (distance <= 0.0)
    {
        return true;
    }
    RayHit hit;
    return !TraceSceneRayMasked(PtRayOrigin(from, direction), direction, 0.0, distance, true, rayId, RAY_MASK_VISIBILITY, hit);
}

// ---------------------------------------------------------------------------
// Analytic lights (next event estimation only)
// ---------------------------------------------------------------------------

uint PtLightCount()
{
    return ubo.lightCounts.y;
}

// A point on a light as a surface sees it: the direction and distance to it, and the irradiance it
// sends to a surface facing it there (lux, visibility aside). For the sun the point is a direction in
// its disk; for point and spot lights a point on the source's sphere, which only the visibility sees
// (the light falls off from the centre, as the lighting pass has it); for an area light a point on the
// rectangle, Lambertian. None depends on the shading point, so a reconnection to one has Jacobian 1.
struct PtLightSample
{
    vec3 point;
    vec3 L;
    float distance;
    vec3 irradiance;
    bool valid;
};

PtLightSample PtLightFromPoint(uint index, vec3 point, vec3 x)
{
    PtLightSample result;
    result.point = point;
    result.L = vec3(0.0, 0.0, 1.0);
    result.distance = 0.0;
    result.irradiance = vec3(0.0);
    result.valid = false;
    if (index >= PtLightCount())
    {
        return result;
    }
    SceneLightData light = sceneLights.lights[index];
    int type = int(light.directionAndType.w);
    vec3 emitted = light.colorAndIntensity.rgb * light.colorAndIntensity.w;
    if (type == LIGHT_DIRECTIONAL)
    {
        vec3 toSun = normalize(-light.directionAndType.xyz);
        if (dot(point, toSun) < min(ubo.sunIlluminance.w, 0.99999) - 1e-4)
        {
            return result;
        }
        result.L = normalize(point);
        result.distance = PT_SUN_DISTANCE;
        result.irradiance = emitted * CloudShadow(x);
        result.valid = true;
        return result;
    }
    vec3 centre = light.positionAndRange.xyz;
    if (type == LIGHT_POINT || type == LIGHT_SPOT)
    {
        if (length(point - centre) > light.spotAndArea.z * 1.01 + 1e-3)
        {
            return result;
        }
        vec3 toCentre = centre - x;
        float centreDistance = length(toCentre);
        vec3 radiance = emitted / (4.0 * PT_PI) * SmoothDistanceAttenuation(centreDistance, light.positionAndRange.w);
        if (type == LIGHT_SPOT)
        {
            float cosAngle = dot(-toCentre / max(centreDistance, 1e-4), normalize(light.directionAndType.xyz));
            float spot = clamp((cosAngle - light.spotAndArea.y) / max(light.spotAndArea.x - light.spotAndArea.y, 0.0001), 0.0, 1.0);
            float coneOmega = max(2.0 * PT_PI * (1.0 - light.spotAndArea.y), 0.0001);
            radiance = emitted / coneOmega * SmoothDistanceAttenuation(centreDistance, light.positionAndRange.w) * spot * spot;
        }
        vec3 toPoint = point - x;
        result.distance = length(toPoint);
        result.L = toPoint / max(result.distance, 1e-6);
        // The housing in front of the lamp does not shadow it (the shadow atlas's near plane).
        result.distance = max(result.distance - kLocalShadowNearPlaneMetres, 0.0);
        result.irradiance = radiance;
        result.valid = PtMax3(radiance) > 0.0;
        return result;
    }
    if (type == LIGHT_AREA)
    {
        vec3 lightNormal = normalize(light.directionAndType.xyz);
        vec3 rightAxis = normalize(light.areaRightAxis.xyz);
        vec3 upAxis = cross(lightNormal, rightAxis);
        vec2 halfSize = 0.5 * max(light.spotAndArea.zw, vec2(0.001));
        vec3 local = point - centre;
        if (abs(dot(local, lightNormal)) > 1e-3 || abs(dot(local, rightAxis)) > halfSize.x * 1.001 + 1e-4 ||
            abs(dot(local, upAxis)) > halfSize.y * 1.001 + 1e-4 || dot(x - centre, lightNormal) <= 0.0)
        {
            return result;
        }
        vec3 toPoint = point - x;
        float distance2 = max(dot(toPoint, toPoint), kMinLightDistanceSquared);
        result.distance = sqrt(distance2);
        result.L = toPoint / result.distance;
        float cosLight = dot(lightNormal, -result.L);
        if (cosLight <= 0.0)
        {
            return result;
        }
        float area = 4.0 * halfSize.x * halfSize.y;
        vec3 luminance = emitted / (PT_PI * area) * RangeWindow(distance(x, centre), light.positionAndRange.w);
        result.irradiance = luminance * (cosLight * area / distance2);
        result.distance = max(result.distance - kLocalShadowNearPlaneMetres, 0.0);
        result.valid = PtMax3(result.irradiance) > 0.0;
        return result;
    }
    return result;
}

vec3 PtUniformSphere(vec2 u)
{
    float z = 1.0 - 2.0 * u.x;
    float r = sqrt(max(1.0 - z * z, 0.0));
    float phi = 2.0 * PT_PI * u.y;
    return vec3(r * cos(phi), r * sin(phi), z);
}

vec3 PtLightPoint(uint index, vec2 u)
{
    SceneLightData light = sceneLights.lights[index];
    int type = int(light.directionAndType.w);
    if (type == LIGHT_DIRECTIONAL)
    {
        vec3 axis = normalize(-light.directionAndType.xyz);
        float cosMax = min(ubo.sunIlluminance.w, 0.99999);
        float cosTheta = 1.0 - u.x * (1.0 - cosMax);
        float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
        vec3 up = abs(axis.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
        vec3 t = normalize(cross(up, axis));
        vec3 b = cross(axis, t);
        float phi = 2.0 * PT_PI * u.y;
        return normalize(t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + axis * cosTheta);
    }
    if (type == LIGHT_AREA)
    {
        vec3 lightNormal = normalize(light.directionAndType.xyz);
        vec3 rightAxis = normalize(light.areaRightAxis.xyz);
        vec3 upAxis = cross(lightNormal, rightAxis);
        vec2 size = max(light.spotAndArea.zw, vec2(0.001));
        return light.positionAndRange.xyz + rightAxis * ((u.x - 0.5) * size.x) + upAxis * ((u.y - 0.5) * size.y);
    }
    return light.positionAndRange.xyz + PtUniformSphere(u) * light.spotAndArea.z;
}

// Next event estimation at a vertex: candidates lights drawn uniformly (p1 = 1 / N), resampled by
// their unshadowed contribution, the chosen one's visibility traced. ucw is the chosen sample's
// unbiased contribution weight in primary sample space, W_RIS p1 (the supplemental's section 5).
struct PtNee
{
    bool valid;
    uint light;
    vec3 point;
    vec3 L;
    vec3 fcos;
    vec3 irradiance;
    float ucw;
};

PtNee PtSampleNee(PtSurface s, vec3 V, uint seed, uint vertex, uint candidates)
{
    PtNee nee;
    nee.valid = false;
    nee.light = 0u;
    nee.point = vec3(0.0);
    nee.L = vec3(0.0, 0.0, 1.0);
    nee.fcos = vec3(0.0);
    nee.irradiance = vec3(0.0);
    nee.ucw = 0.0;
    uint count = PtLightCount();
    if (count == 0u || candidates == 0u)
    {
        return nee;
    }
    float weightSum = 0.0;
    float chosenTarget = 0.0;
    for (uint c = 0u; c < candidates; ++c)
    {
        uint light = min(uint(PtRandom(seed, vertex, 8u + 4u * c) * float(count)), count - 1u);
        vec3 point = PtLightPoint(light, vec2(PtRandom(seed, vertex, 9u + 4u * c), PtRandom(seed, vertex, 10u + 4u * c)));
        PtLightSample sampled = PtLightFromPoint(light, point, s.position);
        if (!sampled.valid)
        {
            continue;
        }
        vec3 fcos = PtBsdfEval(s, V, sampled.L);
        float target = PtLuminance(fcos * sampled.irradiance);
        if (target <= 0.0)
        {
            continue;
        }
        float weight = target * float(count);
        weightSum += weight;
        if (PtRandom(seed, vertex, 11u + 4u * c) * weightSum < weight)
        {
            nee.valid = true;
            nee.light = light;
            nee.point = point;
            nee.L = sampled.L;
            nee.fcos = fcos;
            nee.irradiance = sampled.irradiance;
            chosenTarget = target;
        }
    }
    if (!nee.valid)
    {
        return nee;
    }
    nee.ucw = weightSum / (float(candidates) * chosenTarget) / float(count);
    PtLightSample chosen = PtLightFromPoint(nee.light, nee.point, s.position);
    nee.valid = PtVisible(s, nee.L, chosen.distance, PtRayId(seed, vertex) ^ 0x5bd1e995u);
    return nee;
}

uint PtNeeCandidates(uint vertex)
{
    return max(1u, pt.neeCandidates / (vertex * vertex));
}

// ---------------------------------------------------------------------------
// Reconnection criteria (the paper's section 4)
// ---------------------------------------------------------------------------

// c / 100 times the primary ray's footprint, |x0 - x1|^2 / (cos theta_1 / 4 pi): the right side of
// equation 5 for a path whose first surface is s, seen from camera.
float PtFootprintThreshold(vec3 camera, PtSurface s)
{
    vec3 toCamera = camera - s.position;
    float distance2 = max(dot(toCamera, toCamera), 1e-8);
    float cosTheta = abs(dot(s.normal, toCamera)) * inversesqrt(distance2);
    return pt.footprintScale * 0.01 * distance2 * 4.0 * PT_PI / max(cosTheta, 0.01);
}

// Whether the path may reconnect from a to b. a is the vertex the reconnection leaves: its sampled
// lobe's roughness, the pdf over all its lobes of the direction toward b, its normal. b is at
// distance^2 dist2 along dir with normal bNormal (a sky vertex is at infinity). testB says the inverse
// footprint applies: b continues along a direction whose pdf over b's lobes, seen from a, is pdfB (it
// does not for an end vertex or a diffuse continuation); bRoughness is that lobe's (1 for none), which
// only the legacy criterion reads.
bool PtConnectable(float aRoughness, float pdfA, vec3 aNormal, vec3 dir, float dist2, bool bAtInfinity, vec3 bNormal,
                   bool testB, float pdfB, float bRoughness, float threshold)
{
    if (!PtFlag(PT_FLAG_FOOTPRINT))
    {
        // Lin et al. 2022: both vertices rough, and not too close.
        return min(aRoughness, bRoughness) >= pt.roughnessThreshold && (bAtInfinity || dist2 >= pt.legacyDistance * pt.legacyDistance);
    }
    if (aRoughness < pt.roughnessThreshold)
    {
        return false;
    }
    if (bAtInfinity)
    {
        return true;
    }
    float cosB = abs(dot(bNormal, dir));
    if (dist2 < threshold * pdfA * cosB)
    {
        return false;
    }
    if (testB)
    {
        float cosA = abs(dot(aNormal, dir));
        if (dist2 < threshold * pdfB * cosA)
        {
            return false;
        }
    }
    return true;
}

// Whether a continuation lobe gets the inverse footprint test: not a diffuse one, whose pdf the
// reconnection hardly changes (the paper's footnote 6).
bool PtTestsInverseFootprint(uint lobe)
{
    return lobe == PT_LOBE_SPECULAR || lobe == PT_LOBE_COAT;
}

// NEE's direction from b gets it unless b reflects almost only diffusely.
bool PtNeeTestsInverseFootprint(PtLobes lobes)
{
    return lobes.specular + lobes.coat > 0.01;
}

// The reconnection vertex as a surface (the ray scene's materials: no coat).
// Roughness in [0.04, 1] as 10 bits of its logarithm: a glossy lobe's peak moves with alpha^-2, so the
// steps must be relative (about 0.3% of the roughness each).
const float PT_LOG2_MIN_ROUGHNESS = -4.643856; // log2(0.04)

PtSurface PtRcSurface(PtReservoir r)
{
    PtSurface s = PtInvalidSurface();
    vec4 albedoMetallic = unpackUnorm4x8(r.rcMaterial);
    s.position = r.rcPosition;
    s.faceNormal = PtUnpackNormal(r.rcFaceNormal);
    s.normal = PtUnpackNormal(r.rcExtra);
    s.coatNormal = s.faceNormal;
    s.albedo = albedoMetallic.rgb;
    s.metallic = albedoMetallic.a;
    s.roughness = clamp(exp2(PT_LOG2_MIN_ROUGHNESS * (1.0 - float(r.flags >> 22) / 1023.0)), 0.04, 1.0);
    s.black = false;
    s.valid = true;
    return s;
}

void PtSetRcSurface(inout PtReservoir r, PtSurface s)
{
    r.rcPosition = s.position;
    r.rcFaceNormal = PtPackNormal(s.faceNormal);
    r.rcMaterial = packUnorm4x8(vec4(s.albedo, s.metallic));
    r.rcExtra = PtPackNormal(s.normal);
    float encoded = 1.0 - log2(clamp(s.roughness, 0.04, 1.0)) / PT_LOG2_MIN_ROUGHNESS;
    r.flags = (r.flags & 0x003FFFFFu) | (uint(round(clamp(encoded, 0.0, 1.0) * 1023.0)) << 22);
}

// ---------------------------------------------------------------------------
// The hybrid shift
// ---------------------------------------------------------------------------

// A path shifted onto another surface: its integrand there, the Jacobian determinant of the shift in
// primary sample space (equation 2), and the shifted path's own Jacobian terms, which it carries if
// it is chosen. valid is false where the shift has no image (a lobe that does not replay, a
// reconnection the criteria refuse or that would make the shift not invertible, an occluded one).
struct PtShift
{
    bool valid;
    vec3 F;
    float jacobian;
    float rcJacobian;
};

// The pair test the replay must fail at every vertex before the reconnection: else the shifted path's
// own reconnection vertex would come earlier and the shift would not be invertible.
bool PtReplayPairConnects(bool hasPrevious, float previousRoughness, float previousPdf, vec3 previousPosition, vec3 previousNormal,
                          PtSurface y, bool testY, float pdfY, float yRoughness, float threshold)
{
    if (!hasPrevious)
    {
        return false;
    }
    vec3 toY = y.position - previousPosition;
    float dist2 = max(dot(toY, toY), 1e-12);
    vec3 dir = toY * inversesqrt(dist2);
    return PtConnectable(previousRoughness, previousPdf, previousNormal, dir, dist2, false, y.faceNormal, testY, pdfY, yRoughness, threshold);
}

// Shifts r's path onto the surface y1 seen from camera: random replay up to x_{k-1}'s counterpart, then
// reconnection to x_k (or the light vertex, or the sky).
PtShift PtShiftPath(PtReservoir r, PtSurface y1, vec3 camera)
{
    PtShift result;
    result.valid = false;
    result.F = vec3(0.0);
    result.jacobian = 0.0;
    result.rcJacobian = r.rcJacobian;
    uint d = PtPathLength(r.flags);
    if (d < 2u || !y1.valid || r.W <= 0.0)
    {
        return result;
    }
    uint k = PtRcIndex(r.flags);
    uint type = PtRcType(r.flags);
    float threshold = PtFootprintThreshold(camera, y1);
    uint last = k == 0u ? d : k - 1u;

    PtSurface y = y1;
    vec3 V = normalize(camera - y1.position);
    vec3 T = vec3(1.0);
    bool hasPrevious = false;
    float previousRoughness = 1.0;
    float previousPdf = 0.0;
    vec3 previousPosition = vec3(0.0);
    vec3 previousNormal = vec3(0.0, 0.0, 1.0);

    for (uint i = 1u; i < last; ++i)
    {
        if (y.black)
        {
            return result;
        }
        uint lobe = PtLobeAt(r.flags, i);
        PtLobes lobes = PtLobeProbabilities(y, V);
        if (PtChooseLobe(lobes, PtRandom(r.seed, i, 0u)) != lobe)
        {
            return result;
        }
        vec3 L;
        if (!PtSampleLobe(y, lobe, V, vec2(PtRandom(r.seed, i, 1u), PtRandom(r.seed, i, 2u)), L))
        {
            return result;
        }
        float pdf = PtLobeProbability(lobes, lobe) * PtLobePdf(y, lobe, V, L);
        vec3 fcos = PtLobeEval(y, lobe, V, L);
        if (pdf <= 0.0 || PtMax3(fcos) <= 0.0)
        {
            return result;
        }
        float marginal = PtMarginalPdf(y, lobes, V, L);
        float roughness = PtLobeRoughness(y, lobe);
        if (PtReplayPairConnects(hasPrevious, previousRoughness, previousPdf, previousPosition, previousNormal, y,
                                 PtTestsInverseFootprint(lobe), marginal, PtTestsInverseFootprint(lobe) ? roughness : 1.0, threshold))
        {
            return result;
        }
        T *= fcos / pdf;
        bool miss;
        PtSurface next = PtTrace(y, L, PtRayId(r.seed, i), miss);
        if (miss)
        {
            // Only the end of a pure replay may be the sky, and only where no reconnection to it was
            // allowed.
            if (k != 0u || i + 1u != d || PtConnectable(roughness, marginal, y.faceNormal, L, 1.0, true, y.faceNormal, false, 0.0, 1.0, threshold))
            {
                return result;
            }
            result.F = T * PtSkyRadiance(L);
            result.jacobian = 1.0;
            result.valid = true;
            return result;
        }
        if (!next.valid)
        {
            return result;
        }
        hasPrevious = true;
        previousRoughness = roughness;
        previousPdf = marginal;
        previousPosition = y.position;
        previousNormal = y.faceNormal;
        y = next;
        V = -L;
    }

    if (k == 0u)
    {
        // y is the end: an emitter the replay must reach without a reconnectable last segment.
        if (PtReplayPairConnects(hasPrevious, previousRoughness, previousPdf, previousPosition, previousNormal, y, false, 0.0, 1.0, threshold))
        {
            return result;
        }
        result.F = T * y.emissive;
        result.jacobian = 1.0;
        result.valid = true;
        return result;
    }
    if (y.black)
    {
        return result;
    }

    if (type == PT_RC_LIGHT)
    {
        // NEE from y_{d-1} to the same point on the same light (forced reconnection, section 6.2.3).
        uint light = r.rcMaterial;
        PtLightSample sampled = PtLightFromPoint(light, r.rcPosition, y.position);
        if (!sampled.valid)
        {
            return result;
        }
        vec3 fcos = PtBsdfEval(y, V, sampled.L);
        if (PtMax3(fcos) <= 0.0)
        {
            return result;
        }
        PtLobes lobes = PtLobeProbabilities(y, V);
        bool testY = PtNeeTestsInverseFootprint(lobes);
        if (PtReplayPairConnects(hasPrevious, previousRoughness, previousPdf, previousPosition, previousNormal, y,
                                 testY, testY ? PtMarginalPdf(y, lobes, V, sampled.L) : 0.0, 1.0, threshold))
        {
            return result;
        }
        if (!PtVisible(y, sampled.L, sampled.distance, PtRayId(r.seed, d - 1u) ^ 0x5bd1e995u))
        {
            return result;
        }
        result.F = T * fcos * sampled.irradiance * float(PtLightCount());
        result.jacobian = 1.0;
        result.valid = true;
        return result;
    }

    // Reconnect y_{k-1} to x_k (a surface, or the sky at infinity) with x_{k-1}'s lobe.
    uint lobe = PtLobeAt(r.flags, k - 1u);
    PtLobes lobes = PtLobeProbabilities(y, V);
    float lobeProbability = PtLobeProbability(lobes, lobe);
    if (lobeProbability <= 0.0)
    {
        return result;
    }
    bool sky = type == PT_RC_SKY;
    vec3 dir;
    float dist2 = 1.0;
    if (sky)
    {
        dir = normalize(r.rcPosition);
    }
    else
    {
        vec3 toX = r.rcPosition - y.position;
        dist2 = max(dot(toX, toX), 1e-12);
        dir = toX * inversesqrt(dist2);
    }
    vec3 fcos = PtLobeEval(y, lobe, V, dir);
    float pdf = lobeProbability * PtLobePdf(y, lobe, V, dir);
    if (pdf <= 0.0 || PtMax3(fcos) <= 0.0)
    {
        return result;
    }
    float marginal = PtMarginalPdf(y, lobes, V, dir);
    float roughness = PtLobeRoughness(y, lobe);
    if (PtReplayPairConnects(hasPrevious, previousRoughness, previousPdf, previousPosition, previousNormal, y,
                             PtTestsInverseFootprint(lobe), marginal, PtTestsInverseFootprint(lobe) ? roughness : 1.0, threshold))
    {
        return result;
    }

    vec3 fcosK = vec3(1.0);
    float pdfK = 1.0;
    float geometry = 1.0;
    if (!sky)
    {
        PtSurface x = PtRcSurface(r);
        float cosK = dot(x.faceNormal, -dir);
        if (cosK <= 0.0)
        {
            return result;
        }
        geometry = cosK / dist2;
        bool testX = false;
        float pdfX = 0.0;
        float xRoughness = 1.0;
        if (k < d)
        {
            uint lobeK = PtLobeAt(r.flags, k);
            vec3 wi = PtUnpackNormal(r.rcWi);
            if (lobeK == PT_LOBE_NEE)
            {
                fcosK = PtBsdfEval(x, -dir, wi);
                PtLobes lobesK = PtLobeProbabilities(x, -dir);
                testX = PtNeeTestsInverseFootprint(lobesK);
                pdfX = testX ? PtMarginalPdf(x, lobesK, -dir, wi) : 0.0;
            }
            else
            {
                PtLobes lobesK = PtLobeProbabilities(x, -dir);
                fcosK = PtLobeEval(x, lobeK, -dir, wi);
                pdfK = PtLobeProbability(lobesK, lobeK) * PtLobePdf(x, lobeK, -dir, wi);
                testX = PtTestsInverseFootprint(lobeK);
                pdfX = testX ? PtMarginalPdf(x, lobesK, -dir, wi) : 0.0;
                xRoughness = testX ? PtLobeRoughness(x, lobeK) : 1.0;
            }
            if (pdfK <= 0.0 || PtMax3(fcosK) <= 0.0)
            {
                return result;
            }
        }
        if (!PtConnectable(roughness, marginal, y.faceNormal, dir, dist2, false, x.faceNormal, testX, pdfX, xRoughness, threshold))
        {
            return result;
        }
        if (!PtVisible(y, dir, sqrt(dist2) * 0.999 - 1e-3, PtRayId(r.seed, k - 1u)))
        {
            return result;
        }
    }
    else
    {
        if (!PtConnectable(roughness, marginal, y.faceNormal, dir, 1.0, true, y.faceNormal, false, 0.0, 1.0, threshold) ||
            !PtVisible(y, dir, PT_RAY_DISTANCE, PtRayId(r.seed, k - 1u)))
        {
            return result;
        }
    }

    vec3 suffix = PtUnpackRgb9e5(r.rcSuffix) / PT_SUFFIX_SCALE;
    result.F = T * (fcos / pdf) * (fcosK / pdfK) * suffix;
    result.rcJacobian = pdf * geometry * pdfK;
    result.jacobian = result.rcJacobian / max(r.rcJacobian, 1e-30);
    result.valid = true;
    return result;
}

// Spatial shift results: F and the Jacobian as full floats; the Jacobian is 0 for a partner the pair is
// not reused with (off screen or unlike), -1 for an accepted partner the shift has no image on. Stored values
// must be exactly what the shift computed: the MIS weights compare a path's target in one domain as
// its own reservoir holds it with the same path's target as a shift finds it, and the two only form a
// partition of unity while they agree. The shifted path's Jacobian terms are the base path's times the
// Jacobian, so they need no room of their own.
uvec4 PtPackShift(PtShift s, bool accepted)
{
    if (!accepted)
    {
        return uvec4(0u);
    }
    if (!s.valid || !(s.jacobian > 0.0) || isinf(s.jacobian))
    {
        return uvec4(0u, 0u, 0u, floatBitsToUint(-1.0));
    }
    return uvec4(floatBitsToUint(s.F), floatBitsToUint(s.jacobian));
}

// Whether the spatial shift pass accepted the pair (PtPackShift): the same for both of its records.
bool PtShiftAccepted(uvec4 packed)
{
    return packed.w != 0u;
}

PtShift PtUnpackShift(uvec4 packed, float baseRcJacobian)
{
    PtShift s;
    s.F = uintBitsToFloat(packed.xyz);
    s.jacobian = uintBitsToFloat(packed.w);
    s.valid = s.jacobian > 0.0;
    s.rcJacobian = s.jacobian * baseRcJacobian;
    return s;
}

// The suffix as the reservoir stores it, and F adjusted to match: a shift onto the same surface reads the
// stored suffix, so the reservoir's own F must be what that shift computes (the integrand is the suffix
// times the rest, channel by channel).
uint PtStoreSuffix(inout vec3 F, vec3 suffix)
{
    uint packed = PtPackRgb9e5(suffix * PT_SUFFIX_SCALE);
    vec3 stored = PtUnpackRgb9e5(packed) / PT_SUFFIX_SCALE;
    F = mix(vec3(0.0), F * stored / max(suffix, vec3(1e-30)), greaterThan(suffix, vec3(0.0)));
    return packed;
}

// ---------------------------------------------------------------------------
// Neighbours
// ---------------------------------------------------------------------------

// Whether two primary surfaces are alike enough to reuse each other's paths; symmetric, so a pixel
// rejects its partner exactly when the partner rejects it.
bool PtSimilarSurfaces(PtSurface a, PtSurface b, vec3 camera, float normalCos, float planeTolerance)
{
    if (!a.valid || !b.valid || dot(a.normal, b.normal) < normalCos)
    {
        return false;
    }
    vec3 offset = b.position - a.position;
    float planeDistance = abs(dot(a.faceNormal, offset)) + abs(dot(b.faceNormal, offset));
    float depth = 0.5 * (length(a.position - camera) + length(b.position - camera));
    return planeDistance <= planeTolerance * depth;
}

// The partner of pixel in pairing texture t this frame: the texture flipped, transposed and offset at
// random each frame (section 3.2), which keeps every link mutual.
ivec2 PtPartner(ivec2 pixel, uint t)
{
    uint size = PT_PAIRING_SIZES[t];
    uint h = PtHash(pt.frameIndex * 3u + t + 0x9e3779b9u);
    ivec2 p = pixel;
    if ((h & 4u) != 0u)
    {
        p = p.yx;
    }
    if ((h & 1u) != 0u)
    {
        p.x = -p.x;
    }
    if ((h & 2u) != 0u)
    {
        p.y = -p.y;
    }
    ivec2 offset = ivec2(int((h >> 8) % size), int((h >> 20) % size));
    // GLSL's % is undefined for negative operands, and the flips make p negative: wrap a multiple of
    // the size above the largest screen first.
    ivec2 c = (p + offset + ivec2(int(size) * 64)) % int(size);
    uint texel = PT_PAIRING_OFFSETS[t] + uint(c.y) * size + uint(c.x);
    uint word = pairing[texel >> 1];
    uint bits = (texel & 1u) != 0u ? word >> 16 : word & 0xFFFFu;
    ivec2 delta = ivec2(int(bits << 24) >> 24, int((bits >> 8) << 24) >> 24);
    if ((h & 2u) != 0u)
    {
        delta.y = -delta.y;
    }
    if ((h & 1u) != 0u)
    {
        delta.x = -delta.x;
    }
    if ((h & 4u) != 0u)
    {
        delta = delta.yx;
    }
    return pixel + delta;
}

// The temporal confidence cap for a history pixel (section 5).
float PtTemporalCap(uint historyIndex)
{
    if (!PtFlag(PT_FLAG_DECORRELATION))
    {
        return pt.cap;
    }
    // The buffer starts undefined: a score the map never wrote counts as none.
    float score = duplication[historyIndex];
    score = score >= 0.0 && score <= 1.0 ? score : 0.0;
    return mix(pt.cap, pt.capMin, pow(score, pt.capGamma));
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

vec3 PtHeat(float t)
{
    t = clamp(t, 0.0, 1.0);
    return clamp(vec3(1.5) - abs(vec3(4.0 * t) - vec3(3.0, 2.0, 1.0)), 0.0, 1.0);
}

// Writes the pixel's reflected light (physical radiance), pre-exposed, or the debug view; accumulates in
// the accumulate mode. The lighting pass adds it (rgb) or shows it as is (the debug views, a = 1).
void PtWriteOutput(ivec2 pixel, vec3 radiance, PtReservoir r)
{
    uint index = PtPixelIndex(pixel);
    if (any(isnan(radiance)) || any(isinf(radiance)))
    {
        radiance = vec3(0.0);
    }
    if (PtFlag(PT_FLAG_ACCUMULATE))
    {
        vec4 previous = pt.accumulatedFrames > 0u ? accumulation[index] : vec4(0.0);
        float count = float(pt.accumulatedFrames);
        vec3 average = (previous.rgb * count + radiance) / (count + 1.0);
        accumulation[index] = vec4(average, 1.0);
        radiance = average;
    }
    if (pt.debugView != 0u)
    {
        vec3 color = vec3(0.0);
        if (pt.debugView == PT_DEBUG_DUPLICATION)
        {
            color = PtHeat(duplication[index] * 5.0);
        }
        else if (pt.debugView == PT_DEBUG_RECONNECTION)
        {
            uint k = PtRcIndex(r.flags);
            uint type = PtRcType(r.flags);
            // Black none (pure replay), then by index; the sky blue-white, a light vertex yellow.
            color = PtReservoirEmpty(r) ? vec3(0.0) : (k == 0u ? vec3(0.3, 0.0, 0.3)
                    : (type == PT_RC_SKY ? vec3(0.6, 0.8, 1.0) : (type == PT_RC_LIGHT ? vec3(1.0, 0.9, 0.2) : PtHeat(float(k - 2u) / 4.0))));
        }
        else if (pt.debugView == PT_DEBUG_CONFIDENCE)
        {
            // log2 of the confidence: blue 1, red 4096 and more.
            color = PtHeat(log2(max(r.M, 1.0)) / 12.0);
        }
        else if (pt.debugView == PT_DEBUG_LENGTH)
        {
            color = PtReservoirEmpty(r) ? vec3(0.0) : PtHeat(float(PtPathLength(r.flags) - 2u) / 5.0);
        }
        else if (pt.debugView == PT_DEBUG_PAIRING)
        {
            // Red where a pairing texture's link is not mutual; green where all three are.
            color = vec3(0.0, 1.0, 0.0);
            for (uint t = 0u; t < PT_SPATIAL_NEIGHBOURS; ++t)
            {
                if (PtPartner(PtPartner(pixel, t), t) != pixel)
                {
                    color = vec3(1.0, 0.0, 0.0);
                }
            }
        }
        imageStore(ptOutput, pixel, vec4(color * kFrameBufferUnitsPerExposed, 1.0));
        return;
    }
    imageStore(ptOutput, pixel, vec4(min(radiance * ubo.exposure.x, vec3(65504.0)), 0.0));
}

#endif
