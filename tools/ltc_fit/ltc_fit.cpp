// Fits the linearly transformed cosine (LTC) tables the area lights use (Heitz, Dupuy, Hill and
// Neubelt 2016) to this engine's specular lobe: GGX with the height-correlated Smith visibility,
// the lobe brdf_common.glsl and pbr_common.glsl draw. Writes engine/renderer/ltc_table.cpp.
//
// The method is the paper's: for each (roughness, N.V) texel, a 3 x 3 matrix M with the shape
// frame * [[m11, 0, m13], [0, m22, 0], [0, 0, 1]] maps the clamped cosine distribution onto the
// normalised BRDF lobe. The frame's Z is the lobe's average direction, and (m11, m22, m13) are
// found by Nelder-Mead, minimising the cubed difference between the two distributions under
// multiple importance sampling of both. Each fit starts from its neighbour's, rough to smooth and
// head-on to grazing, so the table varies smoothly. Run it by hand after changing the lobe:
//
//   c++ -O2 -std=c++20 -I<glm include dir> tools/ltc_fit/ltc_fit.cpp -o ltc_fit && ./ltc_fit > engine/renderer/ltc_table.cpp

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>

namespace
{
constexpr int kSize = 64;
constexpr int kSampleGrid = 32;
constexpr float kPi = 3.14159265358979f;
constexpr float kMinRoughness = 0.04f; // pbr_common.glsl floors roughness here

float VisibilitySmithGgxCorrelated(float NdV, float NdL, float alpha)
{
    const float a2 = alpha * alpha;
    const float ggxV = NdL * std::sqrt(NdV * NdV * (1.0f - a2) + a2);
    const float ggxL = NdV * std::sqrt(NdL * NdL * (1.0f - a2) + a2);
    return 0.5f / std::max(ggxV + ggxL, 1e-7f);
}

float DistributionGgx(float NdH, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = std::max(NdH * NdH * (a2 - 1.0f) + 1.0f, 1e-7f);
    return a2 / (kPi * d * d);
}

// The lobe times the cosine, with F = 1, and the pdf of sampling L through GGX's half vectors.
float EvalBrdf(const glm::vec3& V, const glm::vec3& L, float alpha, float& pdf)
{
    pdf = 0.0f;
    if (L.z <= 0.0f || V.z <= 0.0f)
    {
        return 0.0f;
    }
    const glm::vec3 H = glm::normalize(V + L);
    const float D = DistributionGgx(H.z, alpha);
    pdf = D * H.z / (4.0f * std::max(glm::dot(V, H), 1e-7f));
    return D * VisibilitySmithGgxCorrelated(V.z, L.z, alpha) * L.z;
}

glm::vec3 SampleBrdf(const glm::vec3& V, float alpha, float u1, float u2)
{
    const float phi = 2.0f * kPi * u1;
    const float cosTheta = std::sqrt((1.0f - u2) / (1.0f + (alpha * alpha - 1.0f) * u2));
    const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    const glm::vec3 H(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
    return 2.0f * glm::dot(V, H) * H - V;
}

struct Ltc
{
    float m11 = 1.0f;
    float m22 = 1.0f;
    float m13 = 0.0f;
    glm::vec3 X{1.0f, 0.0f, 0.0f};
    glm::vec3 Y{0.0f, 1.0f, 0.0f};
    glm::vec3 Z{0.0f, 0.0f, 1.0f};
    float amplitude = 1.0f;
    glm::mat3 M{1.0f};
    glm::mat3 invM{1.0f};
    float detM = 1.0f;

    void Update()
    {
        M = glm::mat3(X, Y, Z) * glm::mat3(glm::vec3(m11, 0.0f, 0.0f), glm::vec3(0.0f, m22, 0.0f), glm::vec3(m13, 0.0f, 1.0f));
        invM = glm::inverse(M);
        detM = std::abs(glm::determinant(M));
    }

    // The distribution times the amplitude: amplitude * D_o(M^-1 L) * Jacobian.
    float Eval(const glm::vec3& L) const
    {
        glm::vec3 Lo = invM * L;
        const float length = glm::length(Lo);
        Lo /= length;
        const float Do = std::max(0.0f, Lo.z) / kPi;
        const float jacobian = 1.0f / (detM * length * length * length);
        return amplitude * Do * jacobian;
    }

    glm::vec3 Sample(float u1, float u2) const
    {
        const float theta = std::acos(std::sqrt(u1));
        const float phi = 2.0f * kPi * u2;
        const glm::vec3 Lo(std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta));
        return glm::normalize(M * Lo);
    }
};

float ComputeError(const Ltc& ltc, const glm::vec3& V, float alpha)
{
    double error = 0.0;
    for (int j = 0; j < kSampleGrid; ++j)
    {
        for (int i = 0; i < kSampleGrid; ++i)
        {
            const float u1 = (i + 0.5f) / kSampleGrid;
            const float u2 = (j + 0.5f) / kSampleGrid;
            for (int technique = 0; technique < 2; ++technique)
            {
                const glm::vec3 L = technique == 0 ? ltc.Sample(u1, u2) : SampleBrdf(V, alpha, u1, u2);
                float pdfBrdf = 0.0f;
                const float evalBrdf = EvalBrdf(V, L, alpha, pdfBrdf);
                const float evalLtc = ltc.Eval(L);
                const float pdfLtc = evalLtc / ltc.amplitude;
                const double difference = std::abs(evalBrdf - evalLtc);
                const double denominator = pdfLtc + pdfBrdf;
                if (denominator > 0.0)
                {
                    error += difference * difference * difference / denominator;
                }
            }
        }
    }
    return static_cast<float>(error / (2.0 * kSampleGrid * kSampleGrid));
}

// The lobe's albedo (the amplitude), its Fresnel share (the integral weighted by (1 - V.H)^5),
// and its average direction.
void ComputeMoments(const glm::vec3& V, float alpha, float& norm, float& fresnel, glm::vec3& average)
{
    norm = 0.0f;
    fresnel = 0.0f;
    average = glm::vec3(0.0f);
    const int n = 256;
    for (int j = 0; j < n; ++j)
    {
        for (int i = 0; i < n; ++i)
        {
            const glm::vec3 L = SampleBrdf(V, alpha, (i + 0.5f) / n, (j + 0.5f) / n);
            float pdf = 0.0f;
            const float value = EvalBrdf(V, L, alpha, pdf);
            if (pdf > 0.0f)
            {
                const float weight = value / pdf;
                const glm::vec3 H = glm::normalize(V + L);
                norm += weight;
                fresnel += weight * std::pow(1.0f - std::max(glm::dot(V, H), 0.0f), 5.0f);
                average += weight * L;
            }
        }
    }
    norm /= static_cast<float>(n * n);
    fresnel /= static_cast<float>(n * n);
    average.y = 0.0f; // isotropic: the average stays in the plane of N and V
    average = glm::normalize(average);
}

// Nelder-Mead over three parameters.
std::array<float, 3> Minimise(const std::function<float(const std::array<float, 3>&)>& f, std::array<float, 3> start, float scale)
{
    std::array<std::array<float, 3>, 4> simplex{};
    std::array<float, 4> values{};
    for (int i = 0; i < 4; ++i)
    {
        simplex[i] = start;
        if (i > 0)
        {
            simplex[i][i - 1] += scale;
        }
        values[i] = f(simplex[i]);
    }
    for (int iteration = 0; iteration < 300; ++iteration)
    {
        std::array<int, 4> order = {0, 1, 2, 3};
        std::sort(order.begin(), order.end(), [&](int a, int b)
                  {
                      return values[a] < values[b];
                  });
        const int best = order[0];
        const int worst = order[3];
        const int secondWorst = order[2];
        if (std::abs(values[worst] - values[best]) <= 1e-9f * (std::abs(values[best]) + 1e-12f))
        {
            break;
        }
        std::array<float, 3> centroid{};
        for (int i = 0; i < 4; ++i)
        {
            if (i != worst)
            {
                for (int k = 0; k < 3; ++k)
                {
                    centroid[k] += simplex[i][k] / 3.0f;
                }
            }
        }
        const auto along = [&](float t)
        {
            std::array<float, 3> p{};
            for (int k = 0; k < 3; ++k)
            {
                p[k] = centroid[k] + t * (simplex[worst][k] - centroid[k]);
            }
            return p;
        };
        const std::array<float, 3> reflected = along(-1.0f);
        const float reflectedValue = f(reflected);
        if (reflectedValue < values[best])
        {
            const std::array<float, 3> expanded = along(-2.0f);
            const float expandedValue = f(expanded);
            if (expandedValue < reflectedValue)
            {
                simplex[worst] = expanded;
                values[worst] = expandedValue;
            }
            else
            {
                simplex[worst] = reflected;
                values[worst] = reflectedValue;
            }
        }
        else if (reflectedValue < values[secondWorst])
        {
            simplex[worst] = reflected;
            values[worst] = reflectedValue;
        }
        else
        {
            const std::array<float, 3> contracted = along(0.5f);
            const float contractedValue = f(contracted);
            if (contractedValue < values[worst])
            {
                simplex[worst] = contracted;
                values[worst] = contractedValue;
            }
            else
            {
                for (int i = 0; i < 4; ++i)
                {
                    if (i != best)
                    {
                        for (int k = 0; k < 3; ++k)
                        {
                            simplex[i][k] = simplex[best][k] + 0.5f * (simplex[i][k] - simplex[best][k]);
                        }
                        values[i] = f(simplex[i]);
                    }
                }
            }
        }
    }
    int best = 0;
    for (int i = 1; i < 4; ++i)
    {
        if (values[i] < values[best])
        {
            best = i;
        }
    }
    return simplex[best];
}
}

int main()
{
    std::array<glm::vec4, kSize * kSize> matrices{};
    std::array<glm::vec4, kSize * kSize> amplitudes{};
    Ltc previousRow[kSize];

    for (int a = kSize - 1; a >= 0; --a)
    {
        const float roughness = std::max((a + 0.5f) / kSize, kMinRoughness);
        const float alpha = roughness * roughness;
        Ltc ltc;
        for (int t = 0; t < kSize; ++t)
        {
            // The shader looks up x = sqrt(1 - N.V), at texel centres.
            const float x = (t + 0.5f) / kSize;
            const float NdV = std::max(1.0f - x * x, 1e-4f);
            const glm::vec3 V(std::sqrt(1.0f - NdV * NdV), 0.0f, NdV);

            float norm = 0.0f;
            float fresnel = 0.0f;
            glm::vec3 average;
            ComputeMoments(V, alpha, norm, fresnel, average);

            if (t == 0)
            {
                // Nearly head on: start from the rougher row's fit, or from the cosine.
                ltc = a == kSize - 1 ? Ltc{} : previousRow[0];
            }
            ltc.amplitude = norm;
            ltc.Z = average;
            ltc.X = glm::normalize(glm::cross(ltc.Y, ltc.Z));
            ltc.Update();

            std::array<float, 3> start = {ltc.m11, ltc.m22, ltc.m13};
            const std::array<float, 3> fit = Minimise(
                [&](const std::array<float, 3>& p)
                {
                    Ltc candidate = ltc;
                    candidate.m11 = std::max(std::abs(p[0]), 1e-7f);
                    candidate.m22 = std::max(std::abs(p[1]), 1e-7f);
                    candidate.m13 = p[2];
                    candidate.Update();
                    return ComputeError(candidate, V, alpha);
                },
                start,
                0.05f);
            ltc.m11 = std::max(std::abs(fit[0]), 1e-7f);
            ltc.m22 = std::max(std::abs(fit[1]), 1e-7f);
            ltc.m13 = fit[2];
            ltc.Update();
            previousRow[t] = ltc;

            // The inverse, scaled so its middle element is 1 (the distribution does not change
            // under a uniform scale of M), as pbr_common.glsl rebuilds it:
            // mat3(vec3(x, 0, y), vec3(0, 1, 0), vec3(z, 0, w)).
            const glm::mat3 invM = ltc.invM / ltc.invM[1][1];
            matrices[a * kSize + t] = glm::vec4(invM[0][0], invM[0][2], invM[2][0], invM[2][2]);
            amplitudes[a * kSize + t] = glm::vec4(norm, fresnel, 0.0f, 0.0f);
        }
        std::fprintf(stderr, "roughness row %d of %d\n", kSize - a, kSize);
    }

    std::printf("// Generated by tools/ltc_fit/ltc_fit.cpp; do not edit by hand. The LTC fit of this engine's\n");
    std::printf("// GGX lobe with the height-correlated Smith visibility, %d x %d, rows by roughness (texel\n", kSize, kSize);
    std::printf("// centres, floored at %.2f), columns by sqrt(1 - N.V).\n\n", kMinRoughness);
    std::printf("#include \"ltc_table.h\"\n\nnamespace me\n{\n// clang-format off\n");
    std::printf("const std::array<float, kLtcTableSize * kLtcTableSize * 4> kLtcInverseMatrices = {\n");
    for (const glm::vec4& v : matrices)
    {
        std::printf("    %.8ef, %.8ef, %.8ef, %.8ef,\n", v.x, v.y, v.z, v.w);
    }
    std::printf("};\n\nconst std::array<float, kLtcTableSize * kLtcTableSize * 4> kLtcAmplitudes = {\n");
    for (const glm::vec4& v : amplitudes)
    {
        std::printf("    %.8ef, %.8ef, %.8ef, %.8ef,\n", v.x, v.y, v.z, v.w);
    }
    std::printf("};\n// clang-format on\n}\n");
    return 0;
}
