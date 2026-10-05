#include "sun_position.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace me
{
namespace
{
constexpr float kDaysPerYear = 365.0f;
constexpr float kAxialTiltDegrees = 23.44f;
}

TimeOfDaySettings ClampTimeOfDaySettings(const TimeOfDaySettings& settings)
{
    TimeOfDaySettings clamped = settings;
    const float hours = std::isfinite(settings.hours) ? std::fmod(settings.hours, 24.0f) : 12.0f;
    clamped.hours = hours < 0.0f ? hours + 24.0f : hours;
    // fmod of a value just below zero can land on 24 after the shift.
    if (clamped.hours >= 24.0f)
    {
        clamped.hours = 0.0f;
    }
    clamped.dayOfYear = std::clamp(settings.dayOfYear, 1, 365);
    clamped.latitudeDegrees = std::isfinite(settings.latitudeDegrees) ? std::clamp(settings.latitudeDegrees, 0.0f, 90.0f) : 0.0f;
    clamped.northDegrees = std::isfinite(settings.northDegrees) ? settings.northDegrees : 0.0f;
    clamped.timeScale = std::isfinite(settings.timeScale) ? std::max(settings.timeScale, 0.0f) : 0.0f;
    return clamped;
}

float SolarDeclinationDegrees(int dayOfYear)
{
    // Ten days after the December solstice is the first of January.
    const float yearAngle = glm::two_pi<float>() * static_cast<float>(dayOfYear + 10) / kDaysPerYear;
    return -kAxialTiltDegrees * std::cos(yearAngle);
}

namespace
{
// The sun in the local east-north-up frame.
glm::vec3 DirectionToSunEastNorthUp(const TimeOfDaySettings& clamped)
{
    const float latitude = glm::radians(clamped.latitudeDegrees);
    const float declination = glm::radians(SolarDeclinationDegrees(clamped.dayOfYear));
    // Fifteen degrees an hour, zero at solar noon, negative in the morning.
    const float hourAngle = glm::radians(15.0f * (clamped.hours - 12.0f));
    const float east = -std::cos(declination) * std::sin(hourAngle);
    const float north = std::cos(latitude) * std::sin(declination) -
                        std::sin(latitude) * std::cos(declination) * std::cos(hourAngle);
    const float up = std::sin(latitude) * std::sin(declination) +
                     std::cos(latitude) * std::cos(declination) * std::cos(hourAngle);
    return glm::normalize(glm::vec3(east, north, up));
}
}

SolarAngles ComputeSolarAngles(const TimeOfDaySettings& settings)
{
    const glm::vec3 enu = DirectionToSunEastNorthUp(ClampTimeOfDaySettings(settings));
    SolarAngles angles{};
    angles.elevationDegrees = glm::degrees(std::asin(std::clamp(enu.z, -1.0f, 1.0f)));
    float azimuth = glm::degrees(std::atan2(enu.x, enu.y));
    angles.azimuthDegrees = azimuth < 0.0f ? azimuth + 360.0f : azimuth;
    return angles;
}

glm::vec3 ComputeDirectionToSun(const TimeOfDaySettings& settings)
{
    const TimeOfDaySettings clamped = ClampTimeOfDaySettings(settings);
    const glm::vec3 enu = DirectionToSunEastNorthUp(clamped);
    // North along -Z and east along +X, both turned about +Y by northDegrees.
    const float turn = glm::radians(clamped.northDegrees);
    const float c = std::cos(turn);
    const float s = std::sin(turn);
    const glm::vec3 eastAxis(c, 0.0f, -s);
    const glm::vec3 northAxis(-s, 0.0f, -c);
    return glm::normalize(enu.x * eastAxis + enu.y * northAxis + glm::vec3(0.0f, enu.z, 0.0f));
}

glm::vec3 DirectionalLightRotationDegrees(const glm::vec3& directionToSun)
{
    // Rx(x) * Rz(z) * (0, -1, 0) = (sin z, -cos z cos x, -cos z sin x); solve that for the light's
    // direction, the opposite of the way to the sun. cos z >= 0, so x comes from the y and z parts.
    const glm::vec3 shine = -glm::normalize(directionToSun);
    const float z = std::asin(std::clamp(shine.x, -1.0f, 1.0f));
    const float x = std::atan2(-shine.z, -shine.y);
    return glm::vec3(glm::degrees(x), 0.0f, glm::degrees(z));
}
}
