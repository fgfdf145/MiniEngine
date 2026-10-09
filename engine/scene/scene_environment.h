#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace me
{

// What fills the pixels no geometry covers, and whether the atmosphere tints the sun and fogs the
// scene. The values reach the shaders through the camera uniform block and must match the
// ENVIRONMENT_* constants in shaders/vulkan/atmosphere_common.slang.
enum class EnvironmentMode : uint32_t
{
    // The flat viewport background, standing for no physical light.
    None = 0,
    Atmosphere = 1,
    Hdri = 2
};

// Top-of-atmosphere illuminance of the startup scene's sun, in lux.
inline constexpr float kDefaultSunIlluminanceLux = 120000.0f;
// A touch warmer than white above the air's own reddening: the midday sun as the eye remembers it,
// which the white balance's warm target (white_balance.h) then keeps.
inline constexpr glm::vec3 kDefaultSunColor{1.0f, 0.96f, 0.9f};

// The parts of Hillaire 2020's Earth atmosphere the editor exposes. Radii and base coefficients
// are fixed; see engine/renderer/atmosphere.h.
struct AtmosphereSettings
{
    // Dry grass and soil rather than a neutral gray: the ground below the horizon and its light on
    // the sky's underside tint the whole view.
    glm::vec3 groundAlbedo{0.25f, 0.22f, 0.16f};
    // Draws the ground as a surface: an endless plane at world y = 0 with the ground albedo, in the
    // G-buffer and the DDGI ray scene, so it takes shadows, AO and bounce light. Off, the ground
    // shows only as the sky's own lit ground below the horizon.
    bool groundPlane = false;
    // Takes the ground out of the sky: a ray that would meet it reads the sky at its mirror image
    // across the horizon, so the air above and below is one and the horizon has no seam. The sky's
    // ambient light and the clouds' underside follow, with no bounce off the ground below. Off for
    // scenes saved before it existed; EditorScene::AddDefaultSunAndSky turns it on. Not with the
    // ground plane, which is a surface of its own.
    bool seamlessHorizon = false;
    // Multipliers on the base Rayleigh scattering, Mie scattering and extinction, and ozone
    // absorption coefficients.
    float rayleighDensityScale = 1.0f;
    float mieDensityScale = 1.0f;
    // Cornette-Shanks g.
    float mieAnisotropy = 0.8f;
    float ozoneDensityScale = 1.0f;
    // Multiplies every view distance before the aerial perspective lookup, so an editor-sized
    // scene can show the haze the real atmosphere only builds up over kilometres.
    float aerialPerspectiveDistanceScale = 1.0f;
    float sunAngularDiameterDegrees = 0.545f;

    bool operator==(const AtmosphereSettings&) const = default;
};

struct HdriSettings
{
    // An equirectangular .hdr or .exr, referenced like a model: path plus asset uuid.
    std::string path;
    std::string uuid;
    // cd/m^2 per unit of texel value.
    float intensity = 1000.0f;
    // Turns the map about +Y.
    float rotationDegrees = 0.0f;

    bool operator==(const HdriSettings&) const = default;
};

// Unreal's Exponential Height Fog: a grey medium whose extinction falls off exponentially with
// world height, lit by the atmosphere's own light (engine/renderer/height_fog.h). Drawn in the
// Atmosphere mode only; a member of SceneEnvironment so other modes can take it later.
struct HeightFogSettings
{
    // Off for scenes saved before the fog existed; EditorScene::AddDefaultSunAndSky turns it on.
    bool enabled = false;
    // Extinction per metre at fogHeight. Tuned on the Spa grid view: the horizon band is gone and
    // the pit building 100 to 300 m out keeps its contrast.
    float density = 0.001f;
    // Per metre: 1 / falloff is the scale height.
    float heightFalloff = 0.02f;
    // World y of the reference height, metres.
    float fogHeight = 0.0f;
    // Metres from the camera before the fog begins.
    float startDistance = 0.0f;
    float maxOpacity = 1.0f;
    // Tint of the in-scattered light.
    glm::vec3 albedo{1.0f, 1.0f, 1.0f};
    // Henyey-Greenstein g of the glow toward the sun. 0.6 lit the haze so brightly looking toward
    // the sun that it washed out the sky well above the horizon.
    float anisotropy = 0.3f;

    bool operator==(const HeightFogSettings&) const = default;
};

// Volumetric clouds (engine/renderer/volumetric_clouds.h): a layer of cumulus in a shell around the
// planet, ray marched through tiling Perlin-Worley noise and lit by the atmosphere's sun and sky.
// Drawn in the Atmosphere mode only, on the sky and in the environment probe.
struct CloudSettings
{
    // Off for scenes saved before the clouds existed; EditorScene::AddDefaultSunAndSky turns it on.
    bool enabled = false;
    // Share of the sky the layer covers, [0, 1]: 0 clear, 1 overcast.
    float coverage = 0.45f;
    // Altitude of the layer's base above the ground, metres.
    float baseAltitude = 1500.0f;
    // From the base to the tallest tops, metres.
    float thickness = 2500.0f;
    // Extinction per metre where the noise is fully dense. Measured cumulus reaches 0.02 to 0.1;
    // denser than 0.02 the sun no longer reaches past the outer tens of metres and the lit faces
    // turn grey.
    float density = 0.02f;
    // Metres over which the large billows, the small billows and the plume map repeat.
    float shapeScale = 7000.0f;
    float detailScale = 600.0f;
    float weatherScale = 40000.0f;
    // How far the billows push the plumes' surfaces in and out, [0, 2]: 0 smooth domes, 1 the
    // cauliflower of a fair-weather cumulus.
    float billows = 1.0f;
    // Dual-lobe Henyey-Greenstein: the forward lobe gives the silver lining toward the sun, the
    // backward one the lit faces away from it; backWeight blends them.
    float forwardAnisotropy = 0.8f;
    float backAnisotropy = -0.3f;
    float backWeight = 0.3f;
    // Single-scattering albedo: scattering over extinction.
    float albedo = 0.98f;
    // Multiplies the sky light the clouds pick up.
    float ambientScale = 1.0f;
    // Metres over which distant clouds fade into the sky behind them.
    float hazeDistance = 25000.0f;
    // How much of the diffusion field fills in the sun's light where the octaves have died out
    // inside thick clouds, [0, 1]: 0 the octaves alone, 1 never darker than diffusion theory.
    float diffusion = 1.0f;
    // How much the cloud above and below a point hides the sky and ground light it picks up,
    // [0, 1]: 0 every point sees the open sky, 1 a two-stream transmittance through the column.
    // Off by default: measured, it darkens the clouds by under 1 % on average for a third more
    // march cost (docs/design/2026-10-06-cloud-diffusion-and-ambient-occlusion-design.md).
    float ambientOcclusion = 0.0f;
    // The layer moves (docs/design/2026-10-07-dynamic-clouds-design.md): the scene's wind
    // (WindSettings) carries it at the speed it blows at the layer's middle; inside it the billows
    // rise through the plumes at updraft (m/s), and each plume grows, stands and dissipates over
    // lifetime (minutes, a mid-sized plume's; the large clusters live longer, the small ones less).
    // timeScale runs the clouds' clock faster than real time (0 holds them).
    float updraft = 1.5f;
    float lifetime = 15.0f;
    float timeScale = 1.0f;

    bool operator==(const CloudSettings&) const = default;
};

// The scene's wind (engine/scene/wind.h): one mean wind over the scene, which the clouds drift
// with; anything else that should move with the wind asks wind.h for its velocity at a height.
struct WindSettings
{
    // Mean speed at the meteorological reference height, 10 m above the ground, m/s. It rises with
    // height through the boundary layer (WindSpeedAt).
    float speed = 5.0f;
    // The compass bearing the wind blows from, degrees clockwise from north
    // (TimeOfDaySettings::northDegrees places north in the world): 0 a north wind, 270 a west wind,
    // which blows toward the east.
    float fromDegrees = 270.0f;

    bool operator==(const WindSettings&) const = default;
};

// The sun placed by the clock (engine/scene/sun_position.h): a day of the year and a local solar
// time at a latitude turn the scene's sun, its directional light, along the arc it takes across a
// northern-hemisphere sky, rising in the east and crossing the south at noon. The sun's colour and
// strength then come from the air it passes through: the atmosphere's transmittance reddens it low
// and puts it out below the horizon.
struct TimeOfDaySettings
{
    // Off for scenes saved before it existed: the sun keeps its hand-set rotation.
    // EditorScene::AddDefaultSunAndSky turns it on.
    bool enabled = false;
    // Local solar time in hours, [0, 24): 12 is the sun due south, at its highest. With the
    // defaults below, 14:00 puts the sun 40 degrees up in the south-west, near where the startup
    // scene's hand-set sun stood (35 degrees up, due south).
    float hours = 14.0f;
    // Day of the year, 1 to 365: 80 is the March equinox, 172 the June solstice, 279 early October.
    int dayOfYear = 279;
    // Degrees north, clamped to [0, 90]: the trajectory is always a northern-hemisphere one. Tokyo,
    // for the Japanese cars, is 35.7.
    float latitudeDegrees = 35.7f;
    // Where north lies in the world: 0 puts it along -Z (east +X, south +Z, behind the default
    // camera); positive values turn it about +Y.
    float northDegrees = 0.0f;
    // Scene seconds per real second the clock runs while the editor is open; 0 holds the time.
    float timeScale = 0.0f;
    // Once the sun has sunk past twilight the moon takes over as the scene's light: it lights the
    // sky, casts the shadows and shows as the disk (engine/scene/sun_position.h).
    bool moonEnabled = true;
    // Days into the lunar month over its length, [0, 1): 0 new, 0.25 first quarter, 0.5 full (up
    // all night, opposite the sun), 0.75 last quarter. Sets both where the moon stands and how much
    // of its lit face the scene sees.
    float moonPhase = 0.5f;
    // Multiplies the moon's physical illuminance (0.267 lux full, above the air).
    float moonBrightness = 1.0f;
    // Luminance of the moonless night sky, cd/m^2: airglow and starlight, added to the sky above the
    // horizon so it lights the scene when neither sun nor moon does. The real sky is about 0.0002;
    // the exposure stops at EV100 -2, short of the eye's dark adaptation, so the default is brightened
    // to what a dark-adapted eye makes of it: a new-moon night reads dark but not black.
    float nightSkyLuminance = 0.01f;

    bool operator==(const TimeOfDaySettings&) const = default;
};

struct SceneEnvironment
{
    EnvironmentMode mode = EnvironmentMode::None;
    AtmosphereSettings atmosphere;
    HdriSettings hdri;
    HeightFogSettings heightFog;
    CloudSettings clouds;
    TimeOfDaySettings timeOfDay;
    WindSettings wind;
    // Stops added to the camera's auto exposure compensation while this scene is shown. The meter
    // brings whatever fills the frame to a middle grey, so a white photo studio or a snowfield
    // reads grey; a scene made for that look carries the stops it needs. Manual exposure ignores it.
    float exposureCompensationEv = 0.0f;

    bool operator==(const SceneEnvironment&) const = default;
};
}
