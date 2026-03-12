#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

class Camera
{
public:
    glm::mat4 GetModelMatrix() const;
    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix(VkExtent2D extent) const;
    glm::vec3 GetForward() const;
    glm::vec3 GetRight() const;
    void MoveForward(float amount);
    void MoveRight(float amount);
    void Rotate(float deltaYaw, float deltaPitch);
    void FrameBounds(const glm::vec3& minBounds, const glm::vec3& maxBounds);

    glm::vec3 position = { 0.0f, 0.0f, 4.0f };
    glm::vec3 worldUp = { 0.0f, 1.0f, 0.0f };
    float fovDegrees = 45.0f;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
    float moveSpeed = 4.0f;
    float mouseSensitivity = 0.1f;
    float yawDegrees = -90.0f;
    float pitchDegrees = 0.0f;
};
