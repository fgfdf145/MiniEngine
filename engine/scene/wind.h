#pragma once

#include "scene_environment.h"

#include <glm/glm.hpp>

namespace me
{

// The scene's wind (WindSettings; docs/design/2026-10-07-dynamic-clouds-design.md): one mean wind
// that blows the same way everywhere and strengthens with height. The clouds drift with it; anything
// else that should move with the wind (smoke, flags, grass, particles) asks for its velocity here,
// so everything follows the one setting.

// The height WindSettings::speed is given at: the meteorological standard, 10 m.
inline constexpr float kWindReferenceHeightMeters = 10.0f;
// Through the boundary layer the speed rises as (h / 10 m)^(1/7), the neutral-atmosphere power law
// over open country; above its top it stays at the speed there, 1.93 times the 10 m speed.
inline constexpr float kWindShearExponent = 1.0f / 7.0f;
inline constexpr float kWindBoundaryLayerMeters = 1000.0f;
// The power law is never asked below this: at the ground it would fall to nothing.
inline constexpr float kWindMinHeightMeters = 0.5f;
// The fastest wind the editor offers, m/s at 10 m: a hurricane's.
inline constexpr float kWindMaxSpeed = 70.0f;

// The settings held to the ranges the editor offers: speed in [0, kWindMaxSpeed], the bearing
// wrapped into [0, 360).
WindSettings ClampWindSettings(const WindSettings& settings);

// The unit vector in world space (+Y up, horizontal) the wind blows toward, north placed by
// northDegrees as TimeOfDaySettings::northDegrees places it.
glm::vec3 WindDirection(const WindSettings& settings, float northDegrees);

// The wind's speed heightMeters above the ground, m/s.
float WindSpeedAt(const WindSettings& settings, float heightMeters);

// The wind's velocity in world space, m/s, heightMeters above the ground of the scene: its
// direction (north from environment.timeOfDay) times its speed there.
glm::vec3 WindVelocity(const SceneEnvironment& environment, float heightMeters);
}
