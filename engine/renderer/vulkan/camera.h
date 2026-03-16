#pragma once

#include <world_units.h>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

struct ViewportMatrices
{
    glm::mat4 view{ 1.0f };
    glm::mat4 projection{ 1.0f };
    glm::mat4 renderProjection{ 1.0f };
    glm::mat4 model{ 1.0f };
};

class Camera
{
public:
    glm::mat4 GetModelMatrix() const;
    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix(VkExtent2D extent, bool invertYAxisForVulkan) const;
    glm::vec3 GetForward() const;
    glm::vec3 GetRight() const;
    glm::vec3 GetUp() const;
    void SetFromViewMatrix(const glm::mat4& viewMatrix);
    void MoveForward(float amount);
    void MoveRight(float amount);
    void MoveUp(float amount);
    void Rotate(float deltaYaw, float deltaPitch);
    void FrameBounds(const glm::vec3& minBounds, const glm::vec3& maxBounds);

    glm::vec3 position = WorldUnits::kDefaultCameraPositionMeters;
    glm::vec3 worldUp = { 0.0f, 1.0f, 0.0f };
    float fovDegrees = 45.0f;
    float nearPlane = WorldUnits::kDefaultCameraNearPlaneMeters;
    float farPlane = WorldUnits::kDefaultCameraFarPlaneMeters;
    float moveSpeed = WorldUnits::kDefaultCameraMoveSpeedMetersPerSecond;
    float mouseSensitivity = 0.1f;
    float yawDegrees = -90.0f;
    float pitchDegrees = 0.0f;
};
