#include "suspension_friction.h"

#include <algorithm>
#include <cmath>

namespace me::suspension
{

double StribeckCurve(const StribeckParameters& p, double v, double normal)
{
    const double coulomb = std::max(p.coulomb.At(normal), 0.0);
    const double breakaway = std::max(p.breakaway.At(normal), coulomb);
    const double ratio = std::abs(v) / std::max(p.stribeckVelocity, 1e-12);
    return coulomb + (breakaway - coulomb) * std::exp(-std::pow(ratio, p.shape));
}

StribeckFriction::StribeckFriction(StribeckParameters parameters, double regularisation)
    : m_p(parameters), m_regularisation(regularisation)
{
}

double StribeckFriction::Evaluate(double v, double normal, double) const
{
    return StribeckCurve(m_p, v, normal) * std::tanh(v / m_regularisation) + m_p.viscous * v;
}

LuGreFriction::LuGreFriction(LuGreParameters parameters)
    : m_p(parameters)
{
}

double LuGreFriction::Step(double v, double normal, double dt, double& zNext) const
{
    const double g = std::max(StribeckCurve(m_p.stribeck, v, normal), 1e-9);
    const double sigma0 = m_p.bristleStiffness;
    // Backward Euler: z1 = z0 + dt (v - sigma0 |v| z1 / g).
    zNext = (m_z + v * dt) / (1.0 + dt * sigma0 * std::abs(v) / g);
    const double limit = g / sigma0;
    zNext = std::clamp(zNext, -limit, limit);
    const double zRate = (zNext - m_z) / dt;
    double sigma1 = m_p.bristleDamping;
    if (m_p.dampingFadeVelocity > 0.0)
    {
        const double r = v / m_p.dampingFadeVelocity;
        sigma1 *= std::exp(-r * r);
    }
    return sigma0 * zNext + sigma1 * zRate + m_p.stribeck.viscous * v;
}

double LuGreFriction::Evaluate(double v, double normal, double dt) const
{
    double z = 0.0;
    return Step(v, normal, dt, z);
}

void LuGreFriction::Commit(double v, double normal, double dt)
{
    double z = 0.0;
    Step(v, normal, dt, z);
    m_z = z;
}
}
