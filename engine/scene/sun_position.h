#pragma once

#include "scene_environment.h"

#include <glm/glm.hpp>

namespace me
{

// The sun seen from the ground. Elevation above the horizon in degrees (negative below it);
// azimuth clockwise from north in degrees, [0, 360): 90 east, 180 south.
struct SolarAngles
{
    float elevationDegrees = 0.0f;
    float azimuthDegrees = 0.0f;
};

// The settings as the sun uses them: hours wrapped into [0, 24), the day into [1, 365], the
// latitude into the northern hemisphere's [0, 90] and the time scale non-negative.
TimeOfDaySettings ClampTimeOfDaySettings(const TimeOfDaySettings& settings);

// The sun's declination on a day of the year, degrees: Cooper's cosine fit, -23.44 at the December
// solstice and +23.44 at the June one, good to about half a degree.
float SolarDeclinationDegrees(int dayOfYear);

// Where the sun stands at the settings' local solar time, on a spherical Earth with no
// refraction and no equation of time.
SolarAngles ComputeSolarAngles(const TimeOfDaySettings& settings);

// Unit vector from the scene toward the sun in world space (+Y up, north per northDegrees).
glm::vec3 ComputeDirectionToSun(const TimeOfDaySettings& settings);

// The Euler rotation (degrees, the XYZ order BuildTransformMatrix uses) that turns a directional
// light, which shines along its local -Y, to shine away from directionToSun. Y stays zero.
glm::vec3 DirectionalLightRotationDegrees(const glm::vec3& directionToSun);
}
