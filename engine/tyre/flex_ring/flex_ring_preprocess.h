#pragma once

#include "flex_ring_data.h"
#include "flex_ring_modal.h"
#include "flex_ring_parameters.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace me::tyre::flexring
{

// One global property the pre-processing fits the model to, and what the fitted model shows.
struct FitTarget
{
    std::string name;
    std::string unit;
    double target = 0.0;
    double achieved = 0.0;
    bool fitted = true; // false: only reported
    double RelativeError() const
    {
        return target != 0.0 ? (achieved - target) / target : 0.0;
    }
};

struct PreprocessOptions
{
    int maxCycles = 6;
    double tolerance = 2.0e-4; // relative, on every fitted target
    bool fitStatic = true;
    bool fitModal = true;
    std::function<void(const std::string& stage, double progress)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

struct PreprocessResult
{
    FlexRingParameters parameters;
    std::vector<FitTarget> targets;
    std::vector<std::string> notes;
    ModalResult modal;
    int cycles = 0;
    bool converged = false;
    double seconds = 0.0;
    // Derived figures for reports.
    double freeMass = 0.0;   // kg on the belt nodes
    double beltRadius = 0.0;
    double outerRadius = 0.0;
};

// The model's parameters straight from the data, before fitting: geometry, masses, the tread, friction,
// numerics, and first guesses of the structure's stiffnesses from closed forms.
FlexRingParameters BuildParameters(const FlexRingData& data);

// Pre-processing (FTire documentation 5.1): fits the structure so that the model shows the data's static
// wheel loads at the two deflections and the unloaded tyre's natural frequencies f1..f6, alternating the
// static fit (radial stiffness and progressivity) and the modal fit (tangential, lateral and torsion
// stiffness, in-plane and out-of-plane bending stiffness, and the belt's mass when not given).
PreprocessResult Preprocess(const FlexRingData& data, const PreprocessOptions& options = {});

// The roots beta_i W of the free-free beam's elastic modes, cos(x) cosh(x) = 1.
std::vector<double> FreeBeamRoots(int count);

}
