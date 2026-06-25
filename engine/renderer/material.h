#pragma once

#include <glm/glm.hpp>

struct alignas(16) MaterialPushConstants
{
    float baseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float emissiveFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float surfaceFactors[4] = { 0.0f, 1.0f, 1.0f, 1.0f };
    float nodeGraphFactors[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
};

struct alignas(16) ObjectPushConstants
{
    glm::mat4 model{ 1.0f };
    MaterialPushConstants material;
};
