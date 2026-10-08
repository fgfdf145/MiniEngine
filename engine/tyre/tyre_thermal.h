#pragma once

#include <glm/vec2.hpp>

#include <array>
#include <vector>

namespace me::tyre
{

// Assetto Corsa's tyre temperatures and pressure (tyre model V10), as acs.exe has them (TyreThermalModel,
// Tyre::stepThermalModel, Tyre::step; docs/design/2026-10-08-tyre-thermal-design.md): the tread is three lanes
// (one side, the middle, the other side) of twelve patches round the tyre about a core. The patch on the road
// is driven towards a heat level from sliding and rolling; every patch cools towards the air, faster with
// speed; patches share heat with their neighbours and with the core, which rolling heats on its own. Grip is
// the performance curve at a mix of the core and the contact patch, and the pressure follows the core.
inline constexpr int kThermalLanes = 3;
inline constexpr int kThermalPatches = 12;
// The temperature the game takes for the air and the road, and that the pressure's gain counts from (C).
inline constexpr double kThermalAmbient = 26.0;

struct TyreThermalParameters
{
    // [THERMAL_*] as tyres.ini gives them; coolFactor as written (the game cools by (COOL_FACTOR - 1) *
    // 0.000324 per (m/s)^2 of speed), 0 when the file has none.
    double surfaceTransfer = 0.0;
    double patchTransfer = 0.0;
    double coreTransfer = 0.0;
    double internalCoreTransfer = 0.0;
    double frictionK = 0.0;
    double rollingK = 0.0;
    double surfaceRollingK = 0.0;
    double coolFactor = 0.0;
    // Grip by temperature (C); none leaves the grip alone.
    std::vector<glm::vec2> performanceCurve;
    // How far camber moves heat and the contact's temperature to one side ([ADDITIONAL1] CAMBER_TEMP_SPREAD_K).
    double camberSpread = 1.4;
    // The pressure (psi): cold (PRESSURE_STATIC), the grip's best (PRESSURE_IDEAL), its rise with the core's
    // temperature ([ADDITIONAL1] PRESSURE_TEMPERATURE_GAIN, psi per degree over 26 C) and how much it changes
    // the rolling resistance (PRESSURE_RR_GAIN).
    double staticPressure = 26.0;
    double idealPressure = 0.0;
    double temperatureGain = 0.16;
    double rollingResistanceGain = 0.0;

    bool operator==(const TyreThermalParameters&) const = default;
};

struct TyreThermalInput
{
    double dt = 0.0;          // s
    double wheelSpeed = 0.0;  // rad/s
    double camber = 0.0;      // rad, top leaning right positive
    double slideSpeed = 0.0;  // m/s, of the contact over the road
    double load = 0.0;        // N
    double grip = 0.0;        // the tyre's lateral friction at this load, before temperature and pressure
    double surfaceGrip = 1.0; // the road's
    double carSpeed = 0.0;    // m/s, what cools the tread
    double air = kThermalAmbient;
    double road = kThermalAmbient;
};

class TyreThermalModel
{
  public:
    explicit TyreThermalModel(TyreThermalParameters parameters = {}, double temperature = kThermalAmbient);

    // Every patch and the core at this temperature, and the pressure with them.
    void Reset(double temperature);
    void Step(const TyreThermalInput& input);

    const TyreThermalParameters& Parameters() const
    {
        return m_p;
    }
    double CoreTemperature() const
    {
        return m_core;
    }
    // The pressure (psi): cold plus the gain times the core over 26 C.
    double Pressure() const;
    // What the pressure makes of the rolling resistance and the rolling heat: 1 + (ideal / P - 1) * gain.
    double PressureFactor() const;
    // The patch on the road, its three lanes, and their camber-weighted mean.
    std::array<double, kThermalLanes> ContactTemperatures() const;
    double ContactTemperature(double camber) const;
    // Each lane's mean round the tyre (lane 0 the side camber to the right loads).
    std::array<double, kThermalLanes> LaneTemperatures() const;
    // The core plus a quarter of the way to the contact, and the grip the performance curve gives it.
    double PracticalTemperature() const
    {
        return m_practical;
    }
    double Performance() const
    {
        return m_performance;
    }
    double Patch(int lane, int index) const
    {
        return m_patches[static_cast<size_t>(lane)][static_cast<size_t>(index)];
    }

  private:
    int ContactIndex() const;
    void AddSurfaceInput(double camber, double pressureRatio, double heat, double road);
    void Settle(double camber);

    TyreThermalParameters m_p;
    std::array<std::array<double, kThermalPatches>, kThermalLanes> m_patches{};
    std::array<std::array<double, kThermalPatches>, kThermalLanes> m_inputs{};
    double m_core = kThermalAmbient;
    double m_coreInput = 0.0;
    double m_angle = 0.0;
    double m_practical = kThermalAmbient;
    double m_performance = 1.0;
};

// A curve's value at x, straight between its points and held beyond its ends; 1 for none.
double EvaluateThermalCurve(const std::vector<glm::vec2>& curve, double x);
}
