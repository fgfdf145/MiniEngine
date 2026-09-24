#include "white_balance.h"

#include <algorithm>
#include <cmath>

namespace me
{

namespace
{
// Matrices written row by row as they appear in the literature; glm stores columns, hence the
// transpose.
glm::mat3 RowMajor(float a, float b, float c, float d, float e, float f, float g, float h, float i)
{
    return glm::transpose(glm::mat3(a, b, c, d, e, f, g, h, i));
}

const glm::mat3 kRec709ToXyz = RowMajor(
    0.4124564f, 0.3575761f, 0.1804375f,
    0.2126729f, 0.7151522f, 0.0721750f,
    0.0193339f, 0.1191920f, 0.9503041f);
const glm::mat3 kXyzToRec709 = glm::inverse(kRec709ToXyz);
const glm::mat3 kBradford = RowMajor(
    0.8951f, 0.2664f, -0.1614f,
    -0.7502f, 1.7135f, 0.0367f,
    0.0389f, -0.0685f, 1.0296f);
const glm::mat3 kBradfordInverse = glm::inverse(kBradford);

glm::vec3 XyToXyz(const glm::vec2& xy)
{
    return glm::vec3(xy.x / xy.y, 1.0f, (1.0f - xy.x - xy.y) / xy.y);
}
}

glm::vec3 Rec709ToXyz(const glm::vec3& rgb)
{
    return kRec709ToXyz * rgb;
}

glm::vec3 XyzToRec709(const glm::vec3& xyz)
{
    return kXyzToRec709 * xyz;
}

glm::vec2 XyzToXy(const glm::vec3& xyz)
{
    const float sum = xyz.x + xyz.y + xyz.z;
    return sum > 0.0f ? glm::vec2(xyz.x, xyz.y) / sum : kD65WhiteXy;
}

float CorrelatedColorTemperature(const glm::vec2& xy)
{
    const float n = (xy.x - 0.3320f) / (0.1858f - xy.y);
    return ((449.0f * n + 3525.0f) * n + 6823.3f) * n + 5520.33f;
}

glm::vec2 PlanckianXy(float kelvin)
{
    const float t = std::clamp(kelvin, 1667.0f, 25000.0f);
    const float t1 = 1.0e3f / t;
    const float t2 = t1 * t1;
    const float t3 = t2 * t1;
    const float x = t <= 4000.0f ? -0.2661239f * t3 - 0.2343589f * t2 + 0.8776956f * t1 + 0.179910f
                                 : -3.0258469f * t3 + 2.1070379f * t2 + 0.2226347f * t1 + 0.240390f;
    const float x2 = x * x;
    const float x3 = x2 * x;
    float y;
    if (t <= 2222.0f)
    {
        y = -1.1063814f * x3 - 1.34811020f * x2 + 2.18555832f * x - 0.20219683f;
    }
    else if (t <= 4000.0f)
    {
        y = -0.9549476f * x3 - 1.37418593f * x2 + 2.09137015f * x - 0.16748867f;
    }
    else
    {
        y = 3.0817580f * x3 - 5.87338670f * x2 + 3.75112997f * x - 0.37001483f;
    }
    return glm::vec2(x, y);
}

glm::vec2 LimitWhitePoint(const glm::vec2& xy)
{
    const float kelvin = CorrelatedColorTemperature(xy);
    if (!std::isfinite(kelvin))
    {
        return kD65WhiteXy;
    }
    if (kelvin < kMinWhiteBalanceKelvin)
    {
        return PlanckianXy(kMinWhiteBalanceKelvin);
    }
    if (kelvin > kMaxWhiteBalanceKelvin)
    {
        return PlanckianXy(kMaxWhiteBalanceKelvin);
    }
    return xy;
}

glm::vec2 EstimateIlluminantXy(const WhiteBalanceReferences& references)
{
    glm::vec3 sum(0.0f);
    for (const std::optional<glm::vec3>& reference : {references.sunIlluminanceRgb, references.skyIlluminanceRgb})
    {
        if (reference.has_value())
        {
            const glm::vec3 xyz = Rec709ToXyz(glm::max(*reference, glm::vec3(0.0f)));
            if (std::isfinite(xyz.x + xyz.y + xyz.z))
            {
                sum += xyz;
            }
        }
    }
    const float virtualLux = kWhiteBalanceVirtualLightShare * sum.y + kWhiteBalanceVirtualLightLux;
    sum += XyToXyz(kD65WhiteXy) * virtualLux;
    return LimitWhitePoint(XyzToXy(sum));
}

glm::vec2 AdaptWhitePointXy(const glm::vec2& current, const glm::vec2& target, float deltaSeconds, float ratePerSecond)
{
    if (!(deltaSeconds > 0.0f))
    {
        return current;
    }
    const float blend = 1.0f - std::exp(-std::max(ratePerSecond, 0.0f) * deltaSeconds);
    return current + (target - current) * blend;
}

glm::mat3 WhiteBalanceMatrix(const glm::vec2& whiteXy, float degree)
{
    const float d = std::clamp(degree, 0.0f, 1.0f);
    const glm::vec3 sourceCone = kBradford * XyToXyz(whiteXy);
    const glm::vec3 targetCone = kBradford * XyToXyz(kD65WhiteXy);
    const glm::vec3 scale = d * (targetCone / sourceCone) + (1.0f - d);
    const glm::mat3 adapt = kBradfordInverse * glm::mat3(scale.x, 0.0f, 0.0f, 0.0f, scale.y, 0.0f, 0.0f, 0.0f, scale.z) * kBradford;
    return kXyzToRec709 * adapt * kRec709ToXyz;
}
}
