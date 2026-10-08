#pragma once

#include <glm/vec2.hpp>

#include <vector>

namespace me::tyre
{

// Assetto Corsa's tyre wear, graining and blistering (tyre model V10), as acs.exe has them
// (Tyre::addTyreForcesV10, Tyre::stepGrainBlister, Tyre::getCorrectedD, Tyre::getDX/getDY;
// docs/design/2026-10-08-tyre-wear-design.md):
// - the tread's sliding piles up "virtual km" (km slid, times the load over FZ0 with [VIRTUALKM] USE_LOAD), and
//   WEAR_CURVE gives the grip left by them (its % scaled to 1);
// - below the performance curve's window (the first temperature it reaches 1) the core grains, above it (the
//   last) it blisters, each with the slip, speed and distance; driving on wears graining off. Blistering takes up
//   to a fifth of the grip (100 % blister); graining makes the peak slip larger (1 + grain %), which the brush
//   tyre does not take.
struct TyreWearParameters
{
    std::vector<glm::vec2> wearCurve; // virtual km to grip, % as the file gives it
    bool useLoad = false;
    double referenceLoad = 0.0;       // FZ0, N
    double grainGain = 0.0;
    double grainGamma = 0.0;
    double blisterGain = 0.0;
    double blisterGamma = 0.0;
    std::vector<glm::vec2> performanceCurve; // where the window is

    bool operator==(const TyreWearParameters&) const = default;
};

struct TyreWearInput
{
    double dt = 0.0;
    double slideSpeed = 0.0;   // m/s, of the tread over the road
    double contactSpeed = 0.0; // m/s, of the wheel's centre over the road
    double load = 0.0;         // N
    double slip = 0.0;         // the theoretical slip over its peak's
    double coreTemperature = 0.0;
    bool temperatures = false; // whether coreTemperature is simulated (graining and blistering need it)
    double surfaceGrip = 1.0;  // the road's
    double rate = 1.0;         // the session's wear rate (the game's 1x)
};

class TyreWearModel
{
  public:
    explicit TyreWearModel(TyreWearParameters parameters = {});

    void Reset();
    void Step(const TyreWearInput& input);

    const TyreWearParameters& Parameters() const
    {
        return m_p;
    }
    double VirtualKm() const
    {
        return m_virtualKm;
    }
    double Grain() const
    {
        return m_grain;
    }
    double Blister() const
    {
        return m_blister;
    }
    // The core temperatures the window starts and ends at (0 without a curve that reaches 1).
    double GrainBelow() const
    {
        return m_grainBelow;
    }
    double BlisterAbove() const
    {
        return m_blisterAbove;
    }
    // The grip wear and blistering leave: WEAR_CURVE at the virtual km over 1 + 0.2 (blister / 100).
    double Grip() const;

  private:
    TyreWearParameters m_p;
    double m_virtualKm = 0.0;
    double m_grain = 0.0;
    double m_blister = 0.0;
    double m_grainBelow = 0.0;
    double m_blisterAbove = 0.0;
};
}
