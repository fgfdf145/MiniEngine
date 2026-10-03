#pragma once

#include <optional>

namespace me::tyre
{

// The coefficients of the steady-state Magic Formula as Pacejka writes them, Tyre and Vehicle
// Dynamics, 2nd ed. (2006), Section 4.3.2, Eqs. (4.E1-4.E78) ("version 2004"). Names follow the
// book (pCx1 is PCX1 in a .tir file). Coefficients a data set leaves out are zero; the user
// scaling factors lambda default to one. Non-dimensional except where a unit is given.
struct MagicFormulaParameters
{
    double unloadedRadius = 0.0;    // R0, m
    double nominalLoad = 0.0;       // Fz0, N
    double referenceVelocity = 0.0; // V0, m/s

    // User scaling factors (p.186). lambdaMuV is the friction decay with slip speed, unused at 0.
    struct Scaling
    {
        double Fz0 = 1.0;
        double Cx = 1.0, muX = 1.0, Ex = 1.0, Kx = 1.0, Hx = 1.0, Vx = 1.0;
        double Cy = 1.0, muY = 1.0, Ey = 1.0, Ky = 1.0, Kygamma = 1.0, Kzgamma = 1.0, Hy = 1.0, Vy = 1.0;
        double trail = 1.0, residualTorque = 1.0;
        double xAlpha = 1.0, yKappa = 1.0, VyKappa = 1.0, s = 1.0;
        double Mx = 1.0, My = 1.0;
        double muV = 0.0;
    } lambda;

    // Longitudinal force, pure and combined slip.
    double pCx1 = 0.0;
    double pDx1 = 0.0, pDx2 = 0.0;
    double pEx1 = 0.0, pEx2 = 0.0, pEx3 = 0.0, pEx4 = 0.0;
    double pKx1 = 0.0, pKx2 = 0.0, pKx3 = 0.0;
    double pHx1 = 0.0, pHx2 = 0.0;
    double pVx1 = 0.0, pVx2 = 0.0;
    double rBx1 = 0.0, rBx2 = 0.0, rBx3 = 0.0;
    double rCx1 = 0.0;
    double rEx1 = 0.0, rEx2 = 0.0;
    double rHx1 = 0.0;

    // Lateral force, pure and combined slip.
    double pCy1 = 0.0;
    double pDy1 = 0.0, pDy2 = 0.0, pDy3 = 0.0;
    double pEy1 = 0.0, pEy2 = 0.0, pEy3 = 0.0, pEy4 = 0.0, pEy5 = 0.0;
    double pKy1 = 0.0, pKy2 = 0.0, pKy3 = 0.0, pKy4 = 0.0, pKy5 = 0.0, pKy6 = 0.0, pKy7 = 0.0;
    double pHy1 = 0.0, pHy2 = 0.0;
    double pVy1 = 0.0, pVy2 = 0.0, pVy3 = 0.0, pVy4 = 0.0;
    double rBy1 = 0.0, rBy2 = 0.0, rBy3 = 0.0, rBy4 = 0.0;
    double rCy1 = 0.0;
    double rEy1 = 0.0, rEy2 = 0.0;
    double rHy1 = 0.0, rHy2 = 0.0;
    double rVy1 = 0.0, rVy2 = 0.0, rVy3 = 0.0, rVy4 = 0.0, rVy5 = 0.0, rVy6 = 0.0;

    // Aligning torque.
    double qBz1 = 0.0, qBz2 = 0.0, qBz3 = 0.0, qBz5 = 0.0, qBz6 = 0.0, qBz9 = 0.0, qBz10 = 0.0;
    double qCz1 = 0.0;
    double qDz1 = 0.0, qDz2 = 0.0, qDz3 = 0.0, qDz4 = 0.0, qDz6 = 0.0, qDz7 = 0.0, qDz8 = 0.0, qDz9 = 0.0, qDz10 = 0.0, qDz11 = 0.0;
    double qEz1 = 0.0, qEz2 = 0.0, qEz3 = 0.0, qEz4 = 0.0, qEz5 = 0.0;
    double qHz1 = 0.0, qHz2 = 0.0, qHz3 = 0.0, qHz4 = 0.0;
    double sSz1 = 0.0, sSz2 = 0.0, sSz3 = 0.0, sSz4 = 0.0;

    // Overturning couple and rolling resistance moment.
    double qSx1 = 0.0, qSx2 = 0.0, qSx3 = 0.0;
    double qSy1 = 0.0, qSy2 = 0.0;
};

// The tyre's operating point. ISO 8855 axes: x forward along the wheel, z up, slip angle alpha
// positive when the contact centre moves to the right of the wheel plane (-y), so a positive alpha
// gives a negative Fy. kappa is -V_sx / |V_cx|. Camber gamma is a rotation about +x: positive with
// the wheel's top leaning to the right.
struct MagicFormulaInput
{
    double kappa = 0.0;
    double alpha = 0.0; // rad
    double gamma = 0.0; // rad
    double load = 0.0;  // Fz, N
    // Forward speed of the contact centre V_cx, m/s; unset means the reference velocity V0. Only
    // its sign and the speed-dependent terms (cos' alpha, S_Vx, rolling resistance) use it.
    std::optional<double> forwardSpeed;
};

// Forces and moments from road to tyre at the contact centre, ISO axes, plus the quantities the
// tests and rigs look at.
struct MagicFormulaOutput
{
    double Fx = 0.0, Fy = 0.0, Mz = 0.0;    // N, N, Nm
    double Mx = 0.0, My = 0.0;              // Nm
    double Fx0 = 0.0, Fy0 = 0.0, Mz0 = 0.0; // the pure-slip values
    double Kxk = 0.0;                       // longitudinal slip stiffness at this load, N
    double Kya = 0.0;                       // cornering stiffness at this load and camber, N/rad
    double muX = 0.0, muY = 0.0;            // peak friction coefficients
    double trail = 0.0;                     // pneumatic trail of the combined-slip Mz, m
};

// Evaluates Eqs. (4.E1-4.E78) without turn slip (every zeta = 1). Zero load or less gives zeros.
// Caveat of the book's equations: mu_y divides by 1 + pDy3 gamma*^2, which a negative pDy3 (the
// Table A3.1 tyre has -11.23) drives to zero near |gamma| = 0.3 rad; keep camber well inside that.
MagicFormulaOutput EvaluateMagicFormula(const MagicFormulaParameters& p, const MagicFormulaInput& in);

}
