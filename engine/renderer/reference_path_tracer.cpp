#include "reference_path_tracer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace me
{

namespace
{
constexpr float kPi = 3.14159265358979f;

// PCG32 (O'Neill), one stream per call.
class Random
{
  public:
    explicit Random(uint64_t seed)
    {
        m_state = 0u;
        Next();
        m_state += seed * 0x9e3779b97f4a7c15ull + 0x853c49e6748fea9bull;
        Next();
    }

    uint32_t Next()
    {
        const uint64_t old = m_state;
        m_state = old * 6364136223846793005ull + 1442695040888963407ull;
        const uint32_t shifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rotation = static_cast<uint32_t>(old >> 59u);
        return (shifted >> rotation) | (shifted << ((32u - rotation) & 31u));
    }

    float Uniform()
    {
        return static_cast<float>(Next() >> 8) * (1.0f / 16777216.0f);
    }

  private:
    uint64_t m_state = 0;
};

// A direction around N with density cos / pi (Malley's method over an orthonormal basis).
glm::vec3 SampleCosine(const glm::vec3& N, Random& random)
{
    const float u1 = random.Uniform();
    const float u2 = random.Uniform();
    const float radius = std::sqrt(u1);
    const float phi = 2.0f * kPi * u2;
    const float x = radius * std::cos(phi);
    const float y = radius * std::sin(phi);
    const float z = std::sqrt(std::max(0.0f, 1.0f - u1));
    // Duff et al. 2017, "Building an Orthonormal Basis, Revisited".
    const float sign = std::copysign(1.0f, N.z);
    const float a = -1.0f / (sign + N.z);
    const float b = N.x * N.y * a;
    const glm::vec3 tangent(1.0f + sign * N.x * N.x * a, sign * b, -sign * N.x);
    const glm::vec3 bitangent(b, sign + N.y * N.y * a, -N.y);
    return glm::normalize(tangent * x + bitangent * y + N * z);
}

// OffsetRayOrigin in ray_tracing_common.glsl (Wachter and Binder, Ray Tracing Gems chapter 6).
glm::vec3 OffsetRayOrigin(const glm::vec3& position, const glm::vec3& normal)
{
    constexpr float kOriginScale = 1.0f / 32.0f;
    constexpr float kFloatScale = 1.0f / 65536.0f;
    constexpr float kIntScale = 256.0f;
    glm::vec3 result;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::abs(position[axis]) < kOriginScale)
        {
            result[axis] = position[axis] + kFloatScale * normal[axis];
            continue;
        }
        const int32_t offset = static_cast<int32_t>(kIntScale * normal[axis]);
        int32_t bits = 0;
        std::memcpy(&bits, &position[axis], sizeof(bits));
        bits += position[axis] < 0.0f ? -offset : offset;
        std::memcpy(&result[axis], &bits, sizeof(bits));
    }
    return result;
}

// RayHitNormal: the hit triangle's world normal on its counter-clockwise side.
glm::vec3 HitNormal(const RayScene& scene, const RayHit& hit)
{
    const RayInstance& instance = scene.instances[hit.instance];
    const BvhTriangle& triangle = scene.meshTriangles[hit.triangle];
    const glm::vec3 local = glm::cross(glm::vec3(triangle.e1), glm::vec3(triangle.e2));
    const glm::vec3 world = local.x * glm::vec3(instance.worldToObject[0]) + local.y * glm::vec3(instance.worldToObject[1]) +
                            local.z * glm::vec3(instance.worldToObject[2]);
    return glm::normalize(world);
}

const ReferenceMaterial& HitMaterial(const RayScene& scene, std::span<const ReferenceMaterial> materials, const RayHit& hit)
{
    static const ReferenceMaterial kMissing{};
    const uint32_t index = scene.instances[hit.instance].data.z;
    return index < materials.size() ? materials[index] : kMissing;
}

bool Trace(
    const RayScene& scene,
    std::span<const ReferenceMaterial> materials,
    const glm::vec3& origin,
    const glm::vec3& direction,
    bool anyHit,
    Random& random,
    RayHit& hit)
{
    Ray ray;
    ray.origin = origin;
    ray.direction = direction;
    // A partly covered surface stops a random share of the rays that reach it, as AcceptHit does.
    const RayHitFilter filter = [&](const RayHit& candidate)
    {
        const float coverage = HitMaterial(scene, materials, candidate).coverage;
        return coverage >= 1.0f || random.Uniform() < coverage;
    };
    return TraceRay(scene, ray, hit, anyHit, filter);
}

// The radiance arriving at origin from direction, every bounce included.
glm::vec3 PathRadiance(
    const RayScene& scene,
    std::span<const ReferenceMaterial> materials,
    std::span<const ReferenceLight> lights,
    const ReferenceSettings& settings,
    glm::vec3 origin,
    glm::vec3 direction,
    Random& random)
{
    glm::vec3 result(0.0f);
    glm::vec3 throughput(1.0f);
    for (uint32_t bounce = 0; bounce < settings.maxBounces; ++bounce)
    {
        RayHit hit;
        if (!Trace(scene, materials, origin, direction, false, random, hit))
        {
            result += throughput * settings.skyRadiance;
            break;
        }
        const ReferenceMaterial& material = HitMaterial(scene, materials, hit);
        if (!hit.frontFace && !material.doubleSided)
        {
            // The back of a single-sided surface: black, as the probe rays take it.
            break;
        }
        glm::vec3 N = HitNormal(scene, hit);
        if (glm::dot(N, direction) > 0.0f)
        {
            N = -N;
        }
        const glm::vec3 P = origin + direction * hit.t;
        const glm::vec3 next = OffsetRayOrigin(P, N);

        glm::vec3 irradiance(0.0f);
        for (const ReferenceLight& light : lights)
        {
            const float cosine = glm::dot(N, light.directionToLight);
            RayHit shadowHit;
            if (cosine > 0.0f && !Trace(scene, materials, next, light.directionToLight, true, random, shadowHit))
            {
                irradiance += light.illuminance * cosine;
            }
        }
        result += throughput * (material.emission + material.albedo * irradiance / kPi);

        throughput *= material.albedo;
        // Russian roulette after a few bounces, keeping the estimate unbiased.
        if (bounce >= 3)
        {
            const float survive = std::clamp(std::max(throughput.x, std::max(throughput.y, throughput.z)), 0.05f, 1.0f);
            if (random.Uniform() >= survive)
            {
                break;
            }
            throughput /= survive;
        }
        if (throughput == glm::vec3(0.0f))
        {
            break;
        }
        origin = next;
        direction = SampleCosine(N, random);
    }
    return result;
}
}

glm::vec3 ReferenceIndirectIrradiance(
    const RayScene& scene,
    std::span<const ReferenceMaterial> materials,
    std::span<const ReferenceLight> lights,
    const glm::vec3& P,
    const glm::vec3& N,
    const ReferenceSettings& settings,
    uint64_t seed)
{
    Random random(seed);
    const glm::vec3 origin = OffsetRayOrigin(P, N);
    glm::vec3 sum(0.0f);
    const uint32_t samples = std::max(settings.samples, 1u);
    for (uint32_t sample = 0; sample < samples; ++sample)
    {
        // Cosine-weighted directions: the mean radiance is the irradiance / pi.
        sum += PathRadiance(scene, materials, lights, settings, origin, SampleCosine(N, random), random);
    }
    return sum / static_cast<float>(samples);
}

ReferenceSurface ReferencePrimaryHit(
    const RayScene& scene,
    std::span<const ReferenceMaterial> materials,
    const glm::vec3& origin,
    const glm::vec3& direction)
{
    ReferenceSurface surface;
    Ray ray;
    ray.origin = origin;
    ray.direction = direction;
    // Only full surfaces: a foliage card's coverage would make the chosen point random.
    const RayHitFilter filter = [&](const RayHit& candidate)
    {
        return HitMaterial(scene, materials, candidate).coverage >= 1.0f;
    };
    RayHit hit;
    if (!TraceRay(scene, ray, hit, false, filter))
    {
        return surface;
    }
    const ReferenceMaterial& material = HitMaterial(scene, materials, hit);
    surface.valid = hit.frontFace || material.doubleSided;
    surface.position = origin + direction * hit.t;
    surface.normal = HitNormal(scene, hit);
    if (glm::dot(surface.normal, direction) > 0.0f)
    {
        surface.normal = -surface.normal;
    }
    return surface;
}
}
