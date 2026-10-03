#include "tyre_magic_formula.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace me::tyre
{

namespace
{
// Singularity guards: "a small additional quantity epsilon (with same sign as its neighbouring
// main quantity)" (p.185), and eps_V of Eq. (4.E6a).
constexpr double kEpsilon = 1e-6;
constexpr double kEpsilonV = 0.1;
// Eq. (4.E8): the degressive friction factor's A_mu.
constexpr double kAmu = 10.0;

double Sign(double x)
{
    return static_cast<double>((x > 0.0) - (x < 0.0));
}

double Guard(double x)
{
    return x + (x >= 0.0 ? kEpsilon : -kEpsilon);
}

// The curvature factors are bounded above by one (the "(<= 1)" of the book).
double CapE(double e)
{
    return std::min(e, 1.0);
}

// The Magic Formula's core: C arctan{B x - E (B x - arctan(B x))}.
double Shape(double B, double C, double E, double x)
{
    const double bx = B * x;
    return C * std::atan(bx - E * (bx - std::atan(bx)));
}
}

MagicFormulaOutput EvaluateMagicFormula(const MagicFormulaParameters& p, const MagicFormulaInput& in)
{
    MagicFormulaOutput out;
    const double Fz = in.load;
    if (!(Fz > 0.0))
    {
        return out;
    }
    const auto& l = p.lambda;
    const double R0 = p.unloadedRadius;
    const double V0 = p.referenceVelocity;
    const double Vcx = in.forwardSpeed.value_or(V0);
    const double kappa = in.kappa;

    const double Fz0 = l.Fz0 * p.nominalLoad;        // (4.E1)
    const double dfz = (Fz - Fz0) / Fz0;             // (4.E2)
    const double a = std::tan(in.alpha) * Sign(Vcx); // (4.E3) alpha*
    const double g = std::sin(in.gamma);             // (4.E4) gamma*
    const double g2 = g * g;
    const double Vcy = -a * std::abs(Vcx);
    const double cosAlpha = Vcx / (std::hypot(Vcx, Vcy) + kEpsilonV); // (4.E6), (4.E6a)
    // (4.E7) with the slip speed decay off, then (4.E8).
    const double muXStar = l.muX / (1.0 + l.muV);
    const double muYStar = l.muY / (1.0 + l.muV);
    const double muXPrime = kAmu * muXStar / (1.0 + (kAmu - 1.0) * muXStar);
    const double muYPrime = kAmu * muYStar / (1.0 + (kAmu - 1.0) * muYStar);

    // Longitudinal force, pure longitudinal slip (4.E9-4.E18).
    const double SHx = (p.pHx1 + p.pHx2 * dfz) * l.Hx;
    const double kx = kappa + SHx;
    const double Cx = p.pCx1 * l.Cx;
    const double muX = (p.pDx1 + p.pDx2 * dfz) * muXStar;
    const double Dx = muX * Fz;
    const double Ex = CapE((p.pEx1 + p.pEx2 * dfz + p.pEx3 * dfz * dfz) * (1.0 - p.pEx4 * Sign(kx)) * l.Ex);
    const double Kxk = Fz * (p.pKx1 + p.pKx2 * dfz) * std::exp(p.pKx3 * dfz) * l.Kx;
    const double Bx = Kxk / Guard(Cx * Dx);
    const double SVx = Fz * (p.pVx1 + p.pVx2 * dfz) * (std::abs(Vcx) / (kEpsilonV + std::abs(Vcx))) * l.Vx * muXPrime;
    const double Fx0 = Dx * std::sin(Shape(Bx, Cx, Ex, kx)) + SVx;

    // Lateral force, pure side slip (4.E19-4.E30).
    const double Cy = p.pCy1 * l.Cy;
    const double muY = (p.pDy1 + p.pDy2 * dfz) / (1.0 + p.pDy3 * g2) * muYStar;
    const double Dy = muY * Fz;
    const double Kya = p.pKy1 * Fz0 * std::sin(p.pKy4 * std::atan(Fz / ((p.pKy2 + p.pKy5 * g2) * Fz0))) / (1.0 + p.pKy3 * g2) * l.Ky;
    const double By = Kya / Guard(Cy * Dy);
    const double SVyg = Fz * (p.pVy3 + p.pVy4 * dfz) * g * l.Kygamma * muYPrime;
    const double SVy = Fz * (p.pVy1 + p.pVy2 * dfz) * l.Vy * muYPrime + SVyg;
    const double Kyg0 = Fz * (p.pKy6 + p.pKy7 * dfz) * l.Kygamma;
    const double KyaGuarded = Guard(Kya); // K'_ya of (4.E39)
    const double SHy = (p.pHy1 + p.pHy2 * dfz) * l.Hy + (Kyg0 * g - SVyg) / KyaGuarded;
    const double ay = a + SHy;
    const double Ey = CapE((p.pEy1 + p.pEy2 * dfz) * (1.0 + p.pEy5 * g2 - (p.pEy3 + p.pEy4 * g) * Sign(ay)) * l.Ey);
    const double Fy0 = Dy * std::sin(Shape(By, Cy, Ey, ay)) + SVy;

    // Aligning torque, pure side slip (4.E31-4.E49).
    const double SHt = p.qHz1 + p.qHz2 * dfz + (p.qHz3 + p.qHz4 * dfz) * g;
    const double at = a + SHt;
    const double SHf = SHy + SVy / KyaGuarded;
    const double ar = a + SHf;
    const double Bt = (p.qBz1 + p.qBz2 * dfz + p.qBz3 * dfz * dfz) * (1.0 + p.qBz5 * std::abs(g) + p.qBz6 * g2) * l.Ky / muYStar;
    const double Ct = p.qCz1;
    const double Dt0 = Fz * (R0 / Fz0) * (p.qDz1 + p.qDz2 * dfz) * l.trail * Sign(Vcx);
    const double Dt = Dt0 * (1.0 + p.qDz3 * std::abs(g) + p.qDz4 * g2);
    const double Et = CapE((p.qEz1 + p.qEz2 * dfz + p.qEz3 * dfz * dfz) * (1.0 + (p.qEz4 + p.qEz5 * g) * (2.0 / std::numbers::pi) * std::atan(Bt * Ct * at)));
    const double Br = p.qBz9 * l.Ky / muYStar + p.qBz10 * By * Cy;
    const double Cr = 1.0;
    const double Dr = Fz * R0 * ((p.qDz6 + p.qDz7 * dfz) * l.residualTorque + (p.qDz8 + p.qDz9 * dfz) * g * l.Kzgamma + (p.qDz10 + p.qDz11 * dfz) * g * std::abs(g)) * cosAlpha * muYStar * Sign(Vcx);
    const auto trail = [&](double x)
    {
        return Dt * std::cos(Shape(Bt, Ct, Et, x)) * cosAlpha;
    };
    const double Mz0 = -trail(at) * Fy0 + Dr * std::cos(Cr * std::atan(Br * ar));

    // Longitudinal force, combined slip (4.E50-4.E57).
    const double Bxa = (p.rBx1 + p.rBx3 * g2) * std::cos(std::atan(p.rBx2 * kappa)) * l.xAlpha;
    const double Cxa = p.rCx1;
    const double Exa = CapE(p.rEx1 + p.rEx2 * dfz);
    const double SHxa = p.rHx1;
    const double Gxa = std::cos(Shape(Bxa, Cxa, Exa, a + SHxa)) / std::cos(Shape(Bxa, Cxa, Exa, SHxa));
    const double Fx = Gxa * Fx0;

    // Lateral force, combined slip (4.E58-4.E67).
    const double DVyk = muY * Fz * (p.rVy1 + p.rVy2 * dfz + p.rVy3 * g) * std::cos(std::atan(p.rVy4 * a));
    const double SVyk = DVyk * std::sin(p.rVy5 * std::atan(p.rVy6 * kappa)) * l.VyKappa;
    const double SHyk = p.rHy1 + p.rHy2 * dfz;
    const double Byk = (p.rBy1 + p.rBy4 * g2) * std::cos(std::atan(p.rBy2 * (a - p.rBy3))) * l.yKappa;
    const double Cyk = p.rCy1;
    const double Eyk = CapE(p.rEy1 + p.rEy2 * dfz);
    const double Gyk = std::cos(Shape(Byk, Cyk, Eyk, kappa + SHyk)) / std::cos(Shape(Byk, Cyk, Eyk, SHyk));
    const double Fy = Gyk * Fy0 + SVyk;

    // Overturning couple (4.E69) and rolling resistance moment (4.E70); V_r = V_cx - V_sx.
    const double Mx = Fz * R0 * (p.qSx1 - p.qSx2 * g + p.qSx3 * Fy / Fz0) * l.Mx;
    const double Vr = Vcx + kappa * std::abs(Vcx);
    const double My = -Fz * R0 * (p.qSy1 * std::atan(Vr / V0) + p.qSy2 * Fx / Fz0) * l.My;

    // Aligning torque, combined slip (4.E71-4.E78).
    const double stiffnessRatio = Kxk / KyaGuarded;
    const double kappaTerm = stiffnessRatio * stiffnessRatio * kappa * kappa;
    const double atEq = std::sqrt(at * at + kappaTerm) * Sign(at);
    const double arEq = std::sqrt(ar * ar + kappaTerm) * Sign(ar);
    const double t = trail(atEq);
    const double Mzr = Dr * std::cos(Cr * std::atan(Br * arEq));
    const double s = R0 * (p.sSz1 + p.sSz2 * (Fy / Fz0) + (p.sSz3 + p.sSz4 * dfz) * g) * l.s;
    const double Mz = -t * (Fy - SVyk) + Mzr + s * Fx;

    out.Fx = Fx;
    out.Fy = Fy;
    out.Mz = Mz;
    out.Mx = Mx;
    out.My = My;
    out.Fx0 = Fx0;
    out.Fy0 = Fy0;
    out.Mz0 = Mz0;
    out.Kxk = Kxk;
    out.Kya = Kya;
    out.muX = muX;
    out.muY = muY;
    out.trail = t;
    return out;
}

}
