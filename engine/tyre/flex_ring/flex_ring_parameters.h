#pragma once

#include <array>
#include <vector>

namespace me::tyre::flexring
{

// One contact (tread) block of a belt segment. Every segment carries the same layout: block b of
// segment k sits at sigma (0..1, from node k towards node k + 1) and at `lateral` across the tread
// (m, +y = the spin axis' side).
struct BlockLayout
{
    double sigma = 0.0;
    double lateral = 0.0;
    double crownDrop = 0.0;  // m, the belt surface's drop at `lateral` from the lateral curvature (<= 0)
    double depthShare = 1.0; // the tread depth here over the full depth (shoulders)
    int strip = 0;
};

// Friction of the tread rubber on the road, FTire's (chapter 6.5): values at four sliding speeds
// (0, max friction, sliding, blocking) and three ground pressures, quadratic in pressure, piecewise
// linear in speed, constant past the blocking speed.
struct FrictionTable
{
    std::array<double, 3> pressure{1.0e3, 2.0e5, 1.0e6}; // Pa
    std::array<double, 4> speed{0.0, 0.05, 2.0, 20.0};   // m/s
    // mu[pressure][speed]
    std::array<std::array<double, 4>, 3> mu{};

    double Mu(double slidingSpeed, double groundPressure) const;
};

// The pre-processed model: everything in SI units, per belt node or per block, at the measured
// inflation pressure (the tyre scales the structure to the actual one).
struct FlexRingParameters
{
    // Geometry.
    int segments = 80;
    double beltRadius = 0.31;
    double chord = 0.0;       // node to node at rest, m
    double beltWidth = 0.2;
    double treadWidth = 0.21;
    double rimRadius = 0.23;
    double latCurvatureRadius = 1.0; // m, of the belt across its width
    double outerRadius = 0.32; // belt radius plus the new tyre's rubber at the crown
    double sectionHeight = 0.08;
    double maxSpeed = 83.3; // m/s

    // Masses, per node.
    double nodeMass = 0.08;
    double nodeInertia = 0.0; // about the circumferential axis, kg m^2
    double rimFixedMass = 0.0; // the tyre's mass the caller adds to its rim, kg

    // Belt node to rim (per node, at the measured pressure).
    double radialStiffness = 4.0e4;      // N/m at zero deflection
    double radialProgressivity = 0.0;    // relative slope increase at large compression
    double radialProgressionScale = 0.01; // m, the compression where half of it is reached
    double tangentialStiffness = 2.0e4;
    double lateralStiffness = 1.0e4;
    double radialDamping = 10.0;
    double tangentialDamping = 10.0;
    double lateralDamping = 10.0;
    std::array<double, 3> maxwellStiffness{}; // radial, tangential, lateral
    double maxwellTime = 0.003;
    std::array<double, 5> radialHysteresisStiffness{};
    std::array<double, 5> radialHysteresisForce{};
    double tangentialHysteresisStiffness = 0.0;
    double tangentialHysteresisForce = 0.0;
    double lateralHysteresisStiffness = 0.0;
    double lateralHysteresisForce = 0.0;
    double flangeClearance = 0.05;
    double flangeStiffness = 1.0e6;

    // Torsion about the circumferential axis.
    double torsionStiffness = 50.0; // N m/rad per node, to the rim
    double torsionDamping = 0.1;
    double twistStiffness = 100.0; // N m/rad between adjacent nodes
    double twistDamping = 0.1;
    double torsionLateralCoupling = 0.0; // rad/m
    double torsionBendCoupling = 0.0;

    // Belt.
    double chordStiffness = 1.0e7; // N/m
    double chordDamping = 0.0;
    double chordRestLength = 0.0; // m, at the measured pressure
    double bendInStiffness = 0.0;  // N/m on the second difference of the node positions (EI / chord^3)
    double bendOutStiffness = 0.0;
    double pressureForce = 0.0;    // N per node at the measured pressure
    double measuredPressure = 2.0e5; // Pa

    // Lateral bending (Gipser 2006, (3)-(6)): free-free beam modes over the belt width.
    std::vector<double> bendShapeRoots; // beta_i * width
    double lateralBendStiffness = 0.0;  // EI of one segment's width, N m^2
    double lateralBendDamping = 0.0;    // s
    double lateralBendCoupling = 1.0;

    // Tread.
    std::vector<BlockLayout> blocks; // per segment
    double blockArea = 0.0;           // m^2 of footprint per block
    double treadDepth = 0.005;        // m, new
    double treadBase = 0.003;         // m
    double treadModulus = 3.0e6;      // Pa
    double treadPositive = 0.8;
    double shearFactor = 1.0 / 3.0;
    double lateralShearRatio = 1.0;
    double treadProgressivity = 0.0;
    double treadDampingTime = 2.0e-4;
    FrictionTable friction;

    // Numerics.
    double maxStep = 5.0e-4;
    double maxAngleIncrement = 0.0174533; // rad
    double beta = 0.5;
    int jacobianCycle = 1;
    int newtonIterations = 1;
    double contactBound = 0.35;
    bool highPrecisionPlane = true;
    bool gravity = true;
};

}
