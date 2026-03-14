#pragma once

#include <glm/glm.hpp>

namespace WorldUnits
{
constexpr float kMetersPerUnit = 1.0f;

constexpr float kDefaultCubeSizeMeters = 1.0f;
constexpr float kHalfDefaultCubeSizeMeters = kDefaultCubeSizeMeters * 0.5f;
constexpr glm::vec3 kDefaultCubeMinBoundsMeters(
    -kHalfDefaultCubeSizeMeters,
    -kHalfDefaultCubeSizeMeters,
    -kHalfDefaultCubeSizeMeters
);
constexpr glm::vec3 kDefaultCubeMaxBoundsMeters(
    kHalfDefaultCubeSizeMeters,
    kHalfDefaultCubeSizeMeters,
    kHalfDefaultCubeSizeMeters
);

constexpr float kMinimumScale = 0.001f; // 1 mm
constexpr glm::vec3 kMinimumScale3(kMinimumScale, kMinimumScale, kMinimumScale);

constexpr glm::vec3 kDefaultCameraPositionMeters(0.0f, 0.0f, 4.0f);
constexpr float kDefaultCameraNearPlaneMeters = 0.1f;
constexpr float kDefaultCameraFarPlaneMeters = 100.0f;
constexpr float kDefaultCameraMoveSpeedMetersPerSecond = 4.0f;
constexpr float kMinimumFramedRadiusMeters = 0.5f;

constexpr glm::vec3 kDefaultTranslationSnapMeters(0.5f, 0.5f, 0.5f);
constexpr float kDefaultRotationSnapDegrees = 15.0f;
constexpr glm::vec3 kDefaultScaleSnap(0.1f, 0.1f, 0.1f);

constexpr float kUiCameraPositionRangeMeters = 100.0f;
constexpr float kUiCameraMoveSpeedMinMetersPerSecond = 0.1f;
constexpr float kUiCameraMoveSpeedMaxMetersPerSecond = 50.0f;
constexpr float kUiCameraNearMinMeters = 0.01f;
constexpr float kUiCameraNearMaxMeters = 10.0f;
constexpr float kUiCameraFarMinMeters = 1.0f;
constexpr float kUiCameraFarMaxMeters = 1000.0f;

constexpr float kUiTransformTranslationRangeMeters = 1000.0f;
constexpr float kUiTransformScaleMax = 1000.0f;
constexpr float kUiTranslationSnapMaxMeters = 100.0f;
constexpr float kUiScaleSnapMax = 100.0f;
}
