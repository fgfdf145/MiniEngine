#pragma once

#include <engine/core/video/video_mosaic.h>
#include <engine/physics/physics_world.h>
#include <engine/renderer/camera.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace me
{

// A quad recording films a car from four sides at once, each camera following it, and composes the
// four pictures into one video as it goes, to compare the body's roll and pitch from every side
// (docs/design/2026-10-07-quad-vehicle-recording-design.md). The cameras, in the canvas's order:
enum class QuadCameraSlot : uint8_t
{
    Front,
    Rear,
    Left,
    Right,
};
inline constexpr size_t kQuadCameraCount = kVideoMosaicImageCount;

const char* QuadCameraSlotName(QuadCameraSlot slot);

// The sizes a camera's picture may have, in pixels.
inline constexpr uint32_t kQuadCameraMinSize = 64;
inline constexpr uint32_t kQuadCameraMaxSize = 3840;

struct QuadCameraSettings
{
    bool operator==(const QuadCameraSettings&) const = default;

    uint32_t width = 960;
    uint32_t height = 540;
    // Where the camera sits and the point it looks at, from the car's origin, in metres: to the
    // car's right, up, forward (as the Vehicle panel's seat offset).
    glm::vec3 position{0.0f, 0.8f, 6.0f};
    glm::vec3 target{0.0f, 0.6f, 0.0f};
    // Vertical.
    float fovDegrees = 35.0f;
    // Fixed to the body, pitching and rolling with it. Off, the camera turns with the car's heading
    // alone and keeps the horizon level, so the body's roll and pitch show in the picture.
    bool followBodyTilt = false;
};

// Six metres out on each side, a little above the body's middle, looking at it level.
std::array<QuadCameraSettings, kQuadCameraCount> DefaultQuadCameras();

struct QuadRecordingSettings
{
    bool operator==(const QuadRecordingSettings&) const = default;

    std::array<QuadCameraSettings, kQuadCameraCount> cameras = DefaultQuadCameras();
    VideoMosaicLayout layout = VideoMosaicLayout::Grid;
    uint32_t framesPerSecond = 30;
    // Each picture's camera name in its top-left corner.
    bool labels = true;
};

// The settings held to what the window offers: sizes within the limits, a field of view of 5 to 120
// degrees, 1 to 120 frames a second.
QuadRecordingSettings ClampQuadRecordingSettings(QuadRecordingSettings settings);

// The canvas the four pictures make, and where each goes on it.
VideoMosaic ComputeQuadMosaic(const QuadRecordingSettings& settings);

// The camera at `settings` around a car whose body is at `carPose` (vehicle space: +Z forward, +X
// the car's left, +Y up), with the lens limits and exposure settings of `lens`.
Camera PlaceQuadCamera(const Camera& lens, const PhysicsPose& carPose, const QuadCameraSettings& settings);

// The frame the cameras turn with: the body's rotation when they follow its tilt, else its heading
// alone, the body's forward laid flat (world +Y up). A body pointing straight up or down keeps its
// rotation.
glm::quat QuadCameraFrame(const PhysicsPose& carPose, bool followBodyTilt);
}
