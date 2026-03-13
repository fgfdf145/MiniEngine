#include "camera.h"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>

glm::mat4 Camera::GetModelMatrix() const
{
    return glm::mat4(1.0f);
}

glm::mat4 Camera::GetViewMatrix() const
{
    return glm::lookAt(position, position + GetForward(), worldUp);
}

glm::mat4 Camera::GetProjectionMatrix(VkExtent2D extent, bool invertYAxisForVulkan) const
{
    const float aspect = extent.height == 0 ? 1.0f : static_cast<float>(extent.width) / static_cast<float>(extent.height);
    glm::mat4 projection = glm::perspective(glm::radians(fovDegrees), aspect, nearPlane, farPlane);
    if (invertYAxisForVulkan)
    {
        projection[1][1] *= -1.0f;
    }
    return projection;
}

glm::vec3 Camera::GetForward() const
{
    const float yaw = glm::radians(yawDegrees);
    const float pitch = glm::radians(pitchDegrees);

    glm::vec3 forward{};
    forward.x = std::cos(yaw) * std::cos(pitch);
    forward.y = std::sin(pitch);
    forward.z = std::sin(yaw) * std::cos(pitch);
    return glm::normalize(forward);
}

glm::vec3 Camera::GetRight() const
{
    return glm::normalize(glm::cross(GetForward(), worldUp));
}

void Camera::SetFromViewMatrix(const glm::mat4& viewMatrix)
{
    const glm::mat4 inverseView = glm::inverse(viewMatrix);
    position = glm::vec3(inverseView[3]);

    worldUp = glm::normalize(glm::vec3(inverseView[1]));
    const glm::vec3 forward = glm::normalize(-glm::vec3(inverseView[2]));
    yawDegrees = glm::degrees(std::atan2(forward.z, forward.x));
    pitchDegrees = glm::degrees(std::asin(glm::clamp(forward.y, -1.0f, 1.0f)));
}

void Camera::MoveForward(float amount)
{
    position += GetForward() * amount;
}

void Camera::MoveRight(float amount)
{
    position += GetRight() * amount;
}

void Camera::Rotate(float deltaYaw, float deltaPitch)
{
    yawDegrees += deltaYaw;
    pitchDegrees += deltaPitch;
    pitchDegrees = glm::clamp(pitchDegrees, -89.0f, 89.0f);
}

void Camera::FrameBounds(const glm::vec3& minBounds, const glm::vec3& maxBounds)
{
    const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
    const glm::vec3 extent = maxBounds - minBounds;
    const float radius = std::max(glm::length(extent) * 0.5f, 0.5f);
    const float halfFovRadians = glm::radians(fovDegrees) * 0.5f;
    const float distance = radius / std::tan(std::max(halfFovRadians, 0.2f));

    position = center + glm::vec3(0.0f, radius * 0.35f, distance * 1.35f);

    const glm::vec3 forward = glm::normalize(center - position);
    yawDegrees = glm::degrees(std::atan2(forward.z, forward.x));
    pitchDegrees = glm::degrees(std::asin(glm::clamp(forward.y, -1.0f, 1.0f)));

    nearPlane = std::max(0.01f, radius * 0.01f);
    farPlane = std::max(100.0f, distance + radius * 8.0f);
}
