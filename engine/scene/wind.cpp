#include "wind.h"

#include <algorithm>
#include <cmath>

namespace me
{

WindSettings ClampWindSettings(const WindSettings& settings)
{
    WindSettings clamped = settings;
    clamped.speed = std::isfinite(settings.speed) ? std::clamp(settings.speed, 0.0f, kWindMaxSpeed) : 0.0f;
    const float bearing = std::isfinite(settings.fromDegrees) ? std::fmod(settings.fromDegrees, 360.0f) : 0.0f;
    clamped.fromDegrees = bearing < 0.0f ? bearing + 360.0f : bearing;
    // fmod of a value just below zero can round up to exactly 360.
    if (clamped.fromDegrees >= 360.0f)
    {
        clamped.fromDegrees = 0.0f;
    }
    return clamped;
}

glm::vec3 WindDirection(const WindSettings& settings, float northDegrees)
{
    // It blows toward the bearing opposite the one it comes from. North along -Z and east along +X
    // (as EastNorthUpToWorld in sun_position.cpp), both turned about +Y by northDegrees.
    const float toward = glm::radians(ClampWindSettings(settings).fromDegrees + 180.0f);
    const float east = std::sin(toward);
    const float north = std::cos(toward);
    const float turn = glm::radians(std::isfinite(northDegrees) ? northDegrees : 0.0f);
    const glm::vec3 eastAxis(std::cos(turn), 0.0f, -std::sin(turn));
    const glm::vec3 northAxis(-std::sin(turn), 0.0f, -std::cos(turn));
    return glm::normalize(east * eastAxis + north * northAxis);
}

float WindSpeedAt(const WindSettings& settings, float heightMeters)
{
    const float height = std::isfinite(heightMeters) ? std::clamp(heightMeters, kWindMinHeightMeters, kWindBoundaryLayerMeters)
                                                     : kWindReferenceHeightMeters;
    return ClampWindSettings(settings).speed * std::pow(height / kWindReferenceHeightMeters, kWindShearExponent);
}

glm::vec3 WindVelocity(const SceneEnvironment& environment, float heightMeters)
{
    return WindDirection(environment.wind, environment.timeOfDay.northDegrees) * WindSpeedAt(environment.wind, heightMeters);
}
}
