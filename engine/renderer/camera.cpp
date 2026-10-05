#include "camera.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>

namespace me
{

float Camera::GetExposure() const
{
    return ExposureFromEv100(exposureEv100);
}

glm::mat4 Camera::GetViewMatrix() const
{
    return glm::lookAt(position, position + GetForward(), worldUp);
}

glm::mat4 Camera::GetProjectionMatrix(RenderExtent extent, bool invertYAxis, bool useZeroToOneDepth, bool reverseDepth) const
{
    const float aspect = extent.height == 0 ? 1.0f : static_cast<float>(extent.width) / static_cast<float>(extent.height);
    glm::mat4 projection{1.0f};
    if (useZeroToOneDepth && reverseDepth)
    {
        // Swapping the planes is all reverse-Z takes: depth = near * (far - d) / (d * (far - near))
        // for view distance d, 1 at the near plane and 0 at the far one.
        projection = glm::perspectiveRH_ZO(glm::radians(fovDegrees), aspect, farPlane, nearPlane);
    }
    else
    {
        projection = useZeroToOneDepth
                         ? glm::perspectiveRH_ZO(glm::radians(fovDegrees), aspect, nearPlane, farPlane)
                         : glm::perspectiveRH_NO(glm::radians(fovDegrees), aspect, nearPlane, farPlane);
    }
    if (invertYAxis)
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

void Camera::MoveUp(float amount)
{
    position += glm::normalize(worldUp) * amount;
}

void Camera::Rotate(float deltaYaw, float deltaPitch)
{
    yawDegrees += deltaYaw;
    pitchDegrees += deltaPitch;
    pitchDegrees = glm::clamp(pitchDegrees, -89.0f, 89.0f);
}

void Camera::Orbit(const glm::vec3& pivot, float deltaYaw, float deltaPitch)
{
    const float pitchBefore = pitchDegrees;
    Rotate(deltaYaw, deltaPitch);
    // The pitch Rotate actually applied, after its clamp, so the position turns no further than the view.
    const float appliedPitch = pitchDegrees - pitchBefore;

    // A larger yaw turns the view from +X towards +Z, which is a turn about -Y; a larger pitch tilts
    // it up, a turn about the camera's right axis.
    const glm::mat4 yawTurn = glm::rotate(glm::mat4(1.0f), glm::radians(-deltaYaw), glm::normalize(worldUp));
    const glm::mat4 pitchTurn = glm::rotate(glm::mat4(1.0f), glm::radians(appliedPitch), GetRight());
    const glm::vec3 offset = position - pivot;
    position = pivot + glm::vec3(pitchTurn * yawTurn * glm::vec4(offset, 0.0f));
}

void Camera::FrameBoundsLikeKhronosViewer(const glm::vec3& minBounds, const glm::vec3& maxBounds, float aspectRatio)
{
    fovDegrees = 45.0f;
    const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
    const glm::vec3 extent = maxBounds - minBounds;
    // No minimum size: the viewer frames a 10 cm test model as closely as a 10 m one.
    const float maxAxisLength = std::max(std::max(extent.x, extent.y), 1e-4f);
    const float yfov = glm::radians(fovDegrees);
    // Kept below 180 degrees, where the viewer's own rule breaks down (aspect ratios past 4).
    const float xfov = std::min(yfov * std::max(aspectRatio, 1e-3f), 3.1f);
    const float distance = std::max(maxAxisLength / 2.0f / std::tan(yfov / 2.0f), maxAxisLength / 2.0f / std::tan(xfov / 2.0f));

    position = center + glm::vec3(0.0f, 0.0f, distance);
    yawDegrees = -90.0f;
    pitchDegrees = 0.0f;

    const float radius = glm::length(extent) * 0.5f;
    nearPlane = std::max(1e-3f, std::max(distance - radius, 0.0f) * 0.1f);
    farPlane = std::max(WorldUnits::kDefaultCameraFarPlaneMeters, distance + radius * 8.0f);
}

void Camera::FocusOn(const glm::vec3& center, float radius)
{
    radius = std::max(radius, WorldUnits::kMinimumFramedRadiusMeters);
    // The distance at which the sphere just touches the vertical FOV, with a little margin around it.
    const float halfFovRadians = std::max(glm::radians(fovDegrees) * 0.5f, 0.2f);
    const float distance = radius / std::sin(halfFovRadians) * 1.1f;
    position = center - GetForward() * distance;

    if (distance - radius < nearPlane)
    {
        nearPlane = std::max(WorldUnits::kUiCameraNearMinMeters, (distance - radius) * 0.5f);
    }
    farPlane = std::max(farPlane, distance + radius * 8.0f);
}

void Camera::FrameBounds(const glm::vec3& minBounds, const glm::vec3& maxBounds)
{
    const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
    const glm::vec3 extent = maxBounds - minBounds;
    const float radius = std::max(glm::length(extent) * 0.5f, WorldUnits::kMinimumFramedRadiusMeters);
    const float halfFovRadians = glm::radians(fovDegrees) * 0.5f;
    const float distance = radius / std::tan(std::max(halfFovRadians, 0.2f));

    position = center + glm::vec3(0.0f, radius * 0.35f, distance * 1.35f);

    const glm::vec3 forward = glm::normalize(center - position);
    yawDegrees = glm::degrees(std::atan2(forward.z, forward.x));
    pitchDegrees = glm::degrees(std::asin(glm::clamp(forward.y, -1.0f, 1.0f)));

    nearPlane = std::max(WorldUnits::kUiCameraNearMinMeters, radius * 0.01f);
    farPlane = std::max(WorldUnits::kDefaultCameraFarPlaneMeters, distance + radius * 8.0f);
}
}
