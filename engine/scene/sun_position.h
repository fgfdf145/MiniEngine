#pragma once

#include "scene_environment.h"

#include <glm/glm.hpp>

#include <optional>

namespace me
{

// The full moon's illuminance above the atmosphere, lux: apparent magnitude -12.74.
inline constexpr float kFullMoonIlluminanceLux = 0.267f;
// Moonlight is sunlight off grey rock, a touch redder than the sun; at night the eye's rods see it
// bluish (the Purkinje shift), and that is how night is remembered and painted.
inline constexpr glm::vec3 kMoonlightColor{0.78f, 0.87f, 1.0f};
// The night sky's tint at unit luminance (Rec. 709 luma 1): the blue of rod vision, as moonlight.
inline constexpr glm::vec3 kNightSkyColor{0.85f, 1.015f, 1.295f};
// The sun's elevation, degrees, below which the moon lights the scene instead. Below the horizon
// the sun still lights the upper air (twilight). Measured on the R34 test scene looking south in
// early October at 35.7 N (mean 8-bit frame brightness, exposure at its EV100 floor): twilight
// 49 at -10 and 13 at -12, the full moon (then 10 to 12 degrees up) about 20; so at -11 the view
// barely changes at the swap.
inline constexpr float kMoonTakesOverSunElevationDegrees = -11.0f;

// A body seen from the ground. Elevation above the horizon in degrees (negative below it);
// azimuth clockwise from north in degrees, [0, 360): 90 east, 180 south.
struct SolarAngles
{
    float elevationDegrees = 0.0f;
    float azimuthDegrees = 0.0f;
};

// The settings as the sun uses them: hours wrapped into [0, 24), the day into [1, 365], the
// latitude into the northern hemisphere's [0, 90], the moon's phase into [0, 1) and the time scale
// and moon brightness non-negative.
TimeOfDaySettings ClampTimeOfDaySettings(const TimeOfDaySettings& settings);

// The sun's declination on a day of the year, degrees: the sun on a circular orbit, at the March
// equinox on day 80; +23.44 at the June solstice and -23.44 at the December one.
float SolarDeclinationDegrees(int dayOfYear);

// Where the sun stands at the settings' local solar time, on a spherical Earth with no
// refraction and no equation of time.
SolarAngles ComputeSolarAngles(const TimeOfDaySettings& settings);

// Unit vector from the scene toward the sun in world space (+Y up, north per northDegrees).
glm::vec3 ComputeDirectionToSun(const TimeOfDaySettings& settings);

// The moon, on the ecliptic (its 5 degree tilt left out) and moonPhase of a turn east of the sun:
// a full moon rises at sunset opposite it, a first quarter stands south at sunset.
SolarAngles ComputeLunarAngles(const TimeOfDaySettings& settings);
glm::vec3 ComputeDirectionToMoon(const TimeOfDaySettings& settings);

// Share of the full moon's light at a phase: Allen's magnitude-against-phase-angle fit, 1 at full,
// about 0.09 at the quarters, near 0 at new.
float MoonPhaseIlluminanceFraction(float moonPhase);

// The light that stands in for the sun.
struct SkyLight
{
    // Unit vector from the scene toward the body, world space.
    glm::vec3 directionToLight{0.0f, 1.0f, 0.0f};
    // Above the atmosphere, lux per channel.
    glm::vec3 illuminance{0.0f};
};

// The moon's light when it should light the scene in the sun's place: the clock is on, the moon is
// on and the sun is below kMoonTakesOverSunElevationDegrees. Empty otherwise, and the sun stays.
std::optional<SkyLight> ComputeMoonlight(const TimeOfDaySettings& settings);

// The night sky's luminance per channel, cd/m^2, while the clock is on; zero otherwise.
glm::vec3 NightSkyLuminance(const TimeOfDaySettings& settings);

// The Euler rotation (degrees, the XYZ order BuildTransformMatrix uses) that turns a directional
// light, which shines along its local -Y, to shine away from directionToSun. Y stays zero.
glm::vec3 DirectionalLightRotationDegrees(const glm::vec3& directionToSun);
}
