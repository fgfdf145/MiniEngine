#include "camera.h"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <cmath>

glm::mat4 Camera::GetModelMatrix() const
{
    return glm::mat4(1.0f);
}

glm::mat4 Camera::GetViewMatrix() const
{
    return glm::lookAt(position, position + GetForward(), worldUp);
}

glm::mat4 Camera::GetProjectionMatrix(VkExtent2D extent) const
{
    const float aspect = extent.height == 0 ? 1.0f : static_cast<float>(extent.width) / static_cast<float>(extent.height);
    glm::mat4 projection = glm::perspective(glm::radians(fovDegrees), aspect, nearPlane, farPlane);
    projection[1][1] *= -1.0f;
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
