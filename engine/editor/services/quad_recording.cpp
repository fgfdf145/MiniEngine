#include "quad_recording.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace me
{

const char* QuadCameraSlotName(QuadCameraSlot slot)
{
    switch (slot)
    {
    case QuadCameraSlot::Front:
        return "Front";
    case QuadCameraSlot::Rear:
        return "Rear";
    case QuadCameraSlot::Left:
        return "Left";
    case QuadCameraSlot::Right:
        return "Right";
    }
    return "";
}

std::array<QuadCameraSettings, kQuadCameraCount> DefaultQuadCameras()
{
    std::array<QuadCameraSettings, kQuadCameraCount> cameras{};
    cameras[static_cast<size_t>(QuadCameraSlot::Front)].position = glm::vec3(0.0f, 0.8f, 6.0f);
    cameras[static_cast<size_t>(QuadCameraSlot::Rear)].position = glm::vec3(0.0f, 0.8f, -6.0f);
    cameras[static_cast<size_t>(QuadCameraSlot::Left)].position = glm::vec3(-6.0f, 0.8f, 0.0f);
    cameras[static_cast<size_t>(QuadCameraSlot::Right)].position = glm::vec3(6.0f, 0.8f, 0.0f);
    return cameras;
}

QuadRecordingSettings ClampQuadRecordingSettings(QuadRecordingSettings settings)
{
    for (QuadCameraSettings& camera : settings.cameras)
    {
        camera.width = std::clamp(camera.width, kQuadCameraMinSize, kQuadCameraMaxSize);
        camera.height = std::clamp(camera.height, kQuadCameraMinSize, kQuadCameraMaxSize);
        camera.fovDegrees = std::clamp(camera.fovDegrees, 5.0f, 120.0f);
    }
    settings.framesPerSecond = std::clamp(settings.framesPerSecond, 1u, 120u);
    return settings;
}

VideoMosaic ComputeQuadMosaic(const QuadRecordingSettings& settings)
{
    std::array<VideoMosaicSize, kQuadCameraCount> sizes{};
    for (size_t index = 0; index < kQuadCameraCount; ++index)
    {
        sizes[index] = VideoMosaicSize{settings.cameras[index].width, settings.cameras[index].height};
    }
    return ComputeVideoMosaic(settings.layout, sizes);
}

glm::quat QuadCameraFrame(const PhysicsPose& carPose, bool followBodyTilt)
{
    if (followBodyTilt)
    {
        return carPose.rotation;
    }
    const glm::vec3 forward = carPose.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 flat(forward.x, 0.0f, forward.z);
    const float length = glm::length(flat);
    if (length < 1e-4f)
    {
        return carPose.rotation;
    }
    // The heading alone: +Z along the flattened forward, +Y up, +X (the car's left) up x forward.
    const glm::vec3 z = flat / length;
    const glm::vec3 y(0.0f, 1.0f, 0.0f);
    const glm::vec3 x = glm::cross(y, z);
    return glm::quat_cast(glm::mat3(x, y, z));
}

Camera PlaceQuadCamera(const Camera& lens, const PhysicsPose& carPose, const QuadCameraSettings& settings)
{
    const glm::quat frame = QuadCameraFrame(carPose, settings.followBodyTilt);
    // Right, up, forward to vehicle space, whose +X is the car's left.
    const auto toVehicle = [](const glm::vec3& rightUpForward)
    {
        return glm::vec3(-rightUpForward.x, rightUpForward.y, rightUpForward.z);
    };
    // Relative to the car in float; the car's position, in double, only places the camera.
    const glm::vec3 offset = frame * toVehicle(settings.position);
    const glm::vec3 position = glm::vec3(carPose.position + glm::dvec3(offset));
    glm::vec3 direction = frame * toVehicle(settings.target) - offset;
    const float distance = glm::length(direction);
    direction = distance > 1e-4f ? direction / distance : frame * glm::vec3(0.0f, 0.0f, 1.0f);

    Camera camera = lens;
    camera.position = position;
    camera.worldUp = glm::normalize(frame * glm::vec3(0.0f, 1.0f, 0.0f));
    camera.fovDegrees = std::clamp(settings.fovDegrees, 5.0f, 120.0f);
    // Close enough for a camera a metre off the bodywork, without losing the far plane's reach.
    camera.nearPlane = std::min(lens.nearPlane, 0.05f);
    camera.yawDegrees = glm::degrees(std::atan2(direction.z, direction.x));
    camera.pitchDegrees = glm::degrees(std::asin(std::clamp(direction.y, -1.0f, 1.0f)));
    return camera;
}
}
