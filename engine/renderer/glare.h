#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace me
{

// Glare as GT7 models it (docs/references/gt7-rendering-notes.md, section 3): Fraunhofer diffraction
// through a camera aperture that the exposure chooses. A bright scene is shot stopped down and glares
// visibly; a dark one wide open and barely.

// The shutter the aperture is derived with. At ISO 100, EV100 = log2(N^2 / t).
inline constexpr float kGlareShutterSeconds = 1.0f / 125.0f;
// The aperture range of real lenses.
inline constexpr float kGlareMinFNumber = 1.4f;
inline constexpr float kGlareMaxFNumber = 22.0f;
// A full-frame sensor: the viewport's height spans 24 mm, which sets the pixel pitch.
inline constexpr float kGlareSensorHeightMicrons = 24000.0f;
// Dominant wavelengths of the Rec.709 primaries: red diffracts further than blue.
inline constexpr glm::vec3 kGlareWavelengthsMicrons{0.612f, 0.549f, 0.465f};
// The composite keeps at least this share of every pixel where it is, whatever the strength.
inline constexpr float kGlareMaxMovedEnergy = 0.95f;

// The display peak GT7's SDR output stands for (its paper white).
inline constexpr float kGlareSdrPeakNits = 250.0f;

// The f-number a camera at kGlareShutterSeconds uses for this exposure, clamped to the lens range.
// On an HDR display GT7 treats the peak above SDR's as extra exposure latitude: the aperture opens
// by log2(peak / kGlareSdrPeakNits) stops, so a brighter display needs less glare to read as bright.
float GlareFNumberFromEv100(float ev100, float displayPeakNits = kGlareSdrPeakNits);

// The share of each pixel's energy that bloom level k spreads, per channel. Past its first rings the
// Airy pattern leaves 2 lambda N / (pi^2 rho) of its energy beyond focal-plane radius rho, so with
// pitch p the energy beyond R pixels is K / R, K = 2 lambda N / (pi^2 p). Level k (texels 2^(k+1)
// pixels wide) stands for the ring from R_k = 2^(k+1) to R_(k+1); the last level takes everything
// beyond it. The energy within 2 pixels stays put, so the bands sum to K / 2, times strength (and
// scaled down if that would pass kGlareMaxMovedEnergy).
std::vector<glm::vec3> ComputeGlareBands(float fNumber, uint32_t viewportHeight, size_t levelCount, float strength);
}
