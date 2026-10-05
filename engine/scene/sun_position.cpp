#include "sun_position.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace me
{
namespace
{
constexpr float kDaysPerYear = 365.0f;
constexpr int kMarchEquinoxDay = 80;
constexpr float kAxialTiltDegrees = 23.44f;

// The sun's ecliptic longitude, radians: zero at the March equinox, a turn a year.
float SolarEclipticLongitude(int dayOfYear)
{
    return glm::two_pi<float>() * static_cast<float>(dayOfYear - kMarchEquinoxDay) / kDaysPerYear;
}

// A point on the ecliptic in equatorial coordinates: declination and right ascension, radians.
struct Equatorial
{
    float declination = 0.0f;
    float rightAscension = 0.0f;
};

Equatorial EclipticToEquatorial(float longitude)
{
    const float tilt = glm::radians(kAxialTiltDegrees);
    Equatorial equatorial{};
    equatorial.declination = std::asin(std::sin(tilt) * std::sin(longitude));
    equatorial.rightAscension = std::atan2(std::cos(tilt) * std::sin(longitude), std::cos(longitude));
    return equatorial;
}

// The sun's hour angle, radians: fifteen degrees an hour, zero at solar noon, negative in the
// morning.
float SolarHourAngle(const TimeOfDaySettings& clamped)
{
    return glm::radians(15.0f * (clamped.hours - 12.0f));
}

// A body at a declination and hour angle in the local east-north-up frame.
glm::vec3 EastNorthUp(float latitudeDegrees, float declination, float hourAngle)
{
    const float latitude = glm::radians(latitudeDegrees);
    const float east = -std::cos(declination) * std::sin(hourAngle);
    const float north = std::cos(latitude) * std::sin(declination) -
                        std::sin(latitude) * std::cos(declination) * std::cos(hourAngle);
    const float up = std::sin(latitude) * std::sin(declination) +
                     std::cos(latitude) * std::cos(declination) * std::cos(hourAngle);
    return glm::normalize(glm::vec3(east, north, up));
}

glm::vec3 SunEastNorthUp(const TimeOfDaySettings& clamped)
{
    const float declination = EclipticToEquatorial(SolarEclipticLongitude(clamped.dayOfYear)).declination;
    return EastNorthUp(clamped.latitudeDegrees, declination, SolarHourAngle(clamped));
}

glm::vec3 MoonEastNorthUp(const TimeOfDaySettings& clamped)
{
    const float sunLongitude = SolarEclipticLongitude(clamped.dayOfYear);
    const Equatorial sun = EclipticToEquatorial(sunLongitude);
    const Equatorial moon = EclipticToEquatorial(sunLongitude + glm::two_pi<float>() * clamped.moonPhase);
    // Hour angle is sidereal time less right ascension, and the sun's fixes the sidereal time.
    const float hourAngle = SolarHourAngle(clamped) + sun.rightAscension - moon.rightAscension;
    return EastNorthUp(clamped.latitudeDegrees, moon.declination, hourAngle);
}

SolarAngles AnglesFromEastNorthUp(const glm::vec3& enu)
{
    SolarAngles angles{};
    angles.elevationDegrees = glm::degrees(std::asin(std::clamp(enu.z, -1.0f, 1.0f)));
    const float azimuth = glm::degrees(std::atan2(enu.x, enu.y));
    angles.azimuthDegrees = azimuth < 0.0f ? azimuth + 360.0f : azimuth;
    return angles;
}

// North along -Z and east along +X, both turned about +Y by northDegrees.
glm::vec3 EastNorthUpToWorld(const glm::vec3& enu, float northDegrees)
{
    const float turn = glm::radians(northDegrees);
    const float c = std::cos(turn);
    const float s = std::sin(turn);
    const glm::vec3 eastAxis(c, 0.0f, -s);
    const glm::vec3 northAxis(-s, 0.0f, -c);
    return glm::normalize(enu.x * eastAxis + enu.y * northAxis + glm::vec3(0.0f, enu.z, 0.0f));
}

float WrapUnit(float value, float fallback)
{
    if (!std::isfinite(value))
    {
        return fallback;
    }
    float wrapped = value - std::floor(value);
    // floor of a value just below an integer can leave exactly 1.
    return wrapped >= 1.0f ? 0.0f : wrapped;
}
}

TimeOfDaySettings ClampTimeOfDaySettings(const TimeOfDaySettings& settings)
{
    TimeOfDaySettings clamped = settings;
    clamped.hours = WrapUnit(settings.hours / 24.0f, 0.5f) * 24.0f;
    // The product can round up to 24.
    if (clamped.hours >= 24.0f)
    {
        clamped.hours = 0.0f;
    }
    clamped.dayOfYear = std::clamp(settings.dayOfYear, 1, 365);
    clamped.latitudeDegrees = std::isfinite(settings.latitudeDegrees) ? std::clamp(settings.latitudeDegrees, 0.0f, 90.0f) : 0.0f;
    clamped.northDegrees = std::isfinite(settings.northDegrees) ? settings.northDegrees : 0.0f;
    clamped.timeScale = std::isfinite(settings.timeScale) ? std::max(settings.timeScale, 0.0f) : 0.0f;
    clamped.moonPhase = WrapUnit(settings.moonPhase, 0.5f);
    clamped.moonBrightness = std::isfinite(settings.moonBrightness) ? std::max(settings.moonBrightness, 0.0f) : 1.0f;
    clamped.nightSkyLuminance = std::isfinite(settings.nightSkyLuminance) ? std::max(settings.nightSkyLuminance, 0.0f) : 0.0f;
    return clamped;
}

float SolarDeclinationDegrees(int dayOfYear)
{
    return glm::degrees(EclipticToEquatorial(SolarEclipticLongitude(dayOfYear)).declination);
}

SolarAngles ComputeSolarAngles(const TimeOfDaySettings& settings)
{
    return AnglesFromEastNorthUp(SunEastNorthUp(ClampTimeOfDaySettings(settings)));
}

glm::vec3 ComputeDirectionToSun(const TimeOfDaySettings& settings)
{
    const TimeOfDaySettings clamped = ClampTimeOfDaySettings(settings);
    return EastNorthUpToWorld(SunEastNorthUp(clamped), clamped.northDegrees);
}

SolarAngles ComputeLunarAngles(const TimeOfDaySettings& settings)
{
    return AnglesFromEastNorthUp(MoonEastNorthUp(ClampTimeOfDaySettings(settings)));
}

glm::vec3 ComputeDirectionToMoon(const TimeOfDaySettings& settings)
{
    const TimeOfDaySettings clamped = ClampTimeOfDaySettings(settings);
    return EastNorthUpToWorld(MoonEastNorthUp(clamped), clamped.northDegrees);
}

float MoonPhaseIlluminanceFraction(float moonPhase)
{
    // The phase angle (sun-moon-earth), degrees: 180 less the moon's elongation from the sun, so 0
    // at full and 180 at new.
    const float phaseAngle = 360.0f * std::abs(WrapUnit(moonPhase, 0.5f) - 0.5f);
    const float magnitude = 0.026f * phaseAngle + 4.0e-9f * std::pow(phaseAngle, 4.0f);
    return std::pow(10.0f, -0.4f * magnitude);
}

std::optional<SkyLight> ComputeMoonlight(const TimeOfDaySettings& settings)
{
    const TimeOfDaySettings clamped = ClampTimeOfDaySettings(settings);
    if (!clamped.enabled || !clamped.moonEnabled ||
        AnglesFromEastNorthUp(SunEastNorthUp(clamped)).elevationDegrees >= kMoonTakesOverSunElevationDegrees)
    {
        return std::nullopt;
    }
    SkyLight moon{};
    moon.directionToLight = EastNorthUpToWorld(MoonEastNorthUp(clamped), clamped.northDegrees);
    moon.illuminance =
        kMoonlightColor * (kFullMoonIlluminanceLux * MoonPhaseIlluminanceFraction(clamped.moonPhase) * clamped.moonBrightness);
    return moon;
}

glm::vec3 NightSkyLuminance(const TimeOfDaySettings& settings)
{
    const TimeOfDaySettings clamped = ClampTimeOfDaySettings(settings);
    return clamped.enabled ? kNightSkyColor * clamped.nightSkyLuminance : glm::vec3(0.0f);
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
