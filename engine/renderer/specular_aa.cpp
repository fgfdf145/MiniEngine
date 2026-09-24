#include "specular_aa.h"

#include <algorithm>
#include <cmath>

namespace me
{

float FilterRoughnessForSpecularAA(float perceptualRoughness, float normalDerivativeLengthSquared)
{
    const float kernel = std::min(2.0f * kSpecularAAVariance * normalDerivativeLengthSquared, kSpecularAAThreshold);
    // Returned untouched rather than round-tripped through alpha^2, which would not give back
    // exactly the same float: a flat surface must shade as it did before the filter existed.
    if (kernel <= 0.0f)
    {
        return perceptualRoughness;
    }
    const float alpha = perceptualRoughness * perceptualRoughness;
    const float filteredAlphaSquared = std::min(alpha * alpha + kernel, 1.0f);
    return std::sqrt(std::sqrt(filteredAlphaSquared));
}
}
