#pragma once

#include "flex_ring_tyre.h"

#include <array>
#include <vector>

namespace me::tyre::flexring
{

// One vibration mode of the unloaded tyre on a fixed rim.
struct ModeInfo
{
    int waveNumber = 0;   // circumferential wave number
    double frequency = 0.0; // Hz, undamped
    double damping = 0.0;   // modal damping ratio
    // Mass-weighted shares of the radial, tangential, lateral and torsion motion.
    std::array<double, 4> share{};
    bool InPlane() const
    {
        return share[0] + share[1] > 0.5;
    }
};

// FTire's six reference modes (FTire documentation, fig. 6.1) and every mode up to a wave number.
struct ModalResult
{
    std::vector<ModeInfo> modes;
    double f1 = 0.0, f2 = 0.0, f3 = 0.0, f4 = 0.0, f5 = 0.0, f6 = 0.0;
    double d1 = 0.0, d2 = 0.0, d3 = 0.0, d4 = 0.0, d5 = 0.0, d6 = 0.0;
    // How far the linearized model is from block-circulant (max relative residual out of the wave
    // number's subspace) and from symmetric: both near 0 for a well-posed unloaded ring.
    double subspaceResidual = 0.0;
    double asymmetry = 0.0;
};

// Linearizes the tyre (inflated, unloaded, rim fixed where Reset put it, no contact, gravity, Maxwell or
// hysteresis elements) by central differences and solves the eigenproblem in each wave number's subspace
// of the rotationally symmetric ring. The tyre is Reset to an identity rim at the origin first.
ModalResult AnalyzeUnloadedModes(FlexRingTyre& tyre, int maxWaveNumber = 4);

// Eigen-decomposition of a small symmetric matrix (Jacobi rotations): values ascending, vectors as columns
// of `vectors` (row-major n x n).
void SymmetricEigen(int n, std::vector<double> a, std::vector<double>& values, std::vector<double>& vectors);

}
