#include <engine/renderer/specular_aa.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void FlatSurfacesKeepTheirRoughness()
{
    for (float roughness = 0.0f; roughness <= 1.0f; roughness += 0.125f)
    {
        Require(FilterRoughnessForSpecularAA(roughness, 0.0f) == roughness,
                "a flat surface keeps roughness " + std::to_string(roughness) + " exactly");
    }
}

void FilterOnlyWidensAndCaps()
{
    for (float roughness = 0.0f; roughness <= 1.0f; roughness += 0.125f)
    {
        float previous = roughness;
        for (float derivative = 1e-5f; derivative < 100.0f; derivative *= 2.0f)
        {
            const float filtered = FilterRoughnessForSpecularAA(roughness, derivative);
            Require(filtered >= previous, "the filter never narrows a lobe, and widens it as the normal varies more");
            Require(filtered <= 1.0f, "the filter stays a roughness");
            previous = filtered;
        }
        // Past the threshold the kernel stops growing: alpha^2 + threshold, clamped to 1.
        const float alpha = roughness * roughness;
        const float capped = std::sqrt(std::sqrt(std::min(alpha * alpha + kSpecularAAThreshold, 1.0f)));
        Require(std::fabs(previous - capped) < 1e-5f,
                "a violently varying normal caps at the threshold for roughness " + std::to_string(roughness));
    }
}

// A mirror on a sphere 45 pixels in radius: the normal turns by about 1/45 radian per pixel in
// each direction, the case the clearcoat acceptance spheres put in front of the filter.
void MirrorSphereGetsAVisibleLobe()
{
    const float perPixel = 1.0f / 45.0f;
    const float filtered = FilterRoughnessForSpecularAA(0.0f, 2.0f * perPixel * perPixel);
    Require(filtered > 0.1f && filtered < 0.16f, "a small mirror sphere widens to about 0.13, got " + std::to_string(filtered));
}
}

int main()
{
    try
    {
        FlatSurfacesKeepTheirRoughness();
        FilterOnlyWidensAndCaps();
        MirrorSphereGetsAVisibleLobe();
    }
    catch (const std::exception& error)
    {
        std::cerr << "specular AA tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "specular AA tests passed\n";
    return 0;
}
