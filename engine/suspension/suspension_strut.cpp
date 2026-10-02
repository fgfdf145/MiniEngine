#include "suspension_strut.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace me::suspension
{

StrutUnit::StrutUnit(StrutUnitSettings settings, std::unique_ptr<FrictionModel> friction)
    : m_settings(std::move(settings)), m_friction(friction ? std::move(friction) : std::make_unique<NoFriction>())
{
    m_force = m_settings.springPreload;
    m_springForce = m_settings.springPreload;
}

StrutUnit::StrutUnit(const StrutUnit& other)
    : m_settings(other.m_settings),
      m_friction(other.m_friction->Clone()),
      m_springMount(other.m_springMount),
      m_damperMount(other.m_damperMount),
      m_mountRate(other.m_mountRate),
      m_force(other.m_force),
      m_springForce(other.m_springForce),
      m_rodForce(other.m_rodForce),
      m_frictionForce(other.m_frictionForce),
      m_hydraulicForce(other.m_hydraulicForce),
      m_pistonVelocity(other.m_pistonVelocity)
{
}

StrutUnit& StrutUnit::operator=(const StrutUnit& other)
{
    if (this != &other)
    {
        StrutUnit copy(other);
        std::swap(m_settings, copy.m_settings);
        std::swap(m_friction, copy.m_friction);
        m_springMount = copy.m_springMount;
        m_damperMount = copy.m_damperMount;
        m_mountRate = copy.m_mountRate;
        m_force = copy.m_force;
        m_springForce = copy.m_springForce;
        m_rodForce = copy.m_rodForce;
        m_frictionForce = copy.m_frictionForce;
        m_hydraulicForce = copy.m_hydraulicForce;
        m_pistonVelocity = copy.m_pistonVelocity;
    }
    return *this;
}

double StrutUnit::StiffnessSlope(double compression) const
{
    // Coil and seat rubber in series; the bump stop on the rod (its mount's series rubber is left
    // out: it only matters when the stop is hit hard).
    const double coil = m_settings.coilSpring.Slope(compression - m_springMount);
    double spring = coil;
    if (!m_settings.springMount.IsZero())
    {
        const double mount = m_settings.springMount.Slope(m_springMount);
        spring = (coil > 0.0 && mount > 0.0) ? coil * mount / (coil + mount) : 0.0;
    }
    const double stroke = compression - m_damperMount;
    return spring + m_settings.bumpStop.Slope(stroke) + m_settings.reboundStop.Slope(stroke);
}

double StrutUnit::RateSlope(double compressionRate, double normal, double dt) const
{
    const double h = 1e-4;
    const double damper = m_settings.damper.Slope(compressionRate);
    const double friction = (m_friction->Evaluate(compressionRate + h, normal, dt) - m_friction->Evaluate(compressionRate - h, normal, dt)) / (2.0 * h);
    double slope = damper + std::max(friction, 0.0);
    if (!m_settings.damperMount.IsZero() || m_settings.damperMountDamping > 0.0)
    {
        // In series with the rubber (its stiffness over a step acts as a damping k dt).
        const double rubber = m_settings.damperMount.Slope(m_damperMount) * dt + m_settings.damperMountDamping;
        slope = (slope > 0.0 && rubber > 0.0) ? slope * rubber / (slope + rubber) : 0.0;
    }
    return slope;
}

double StrutUnit::RodBalance(double w, double compression, double rate, double normal, double dt) const
{
    // Rubber force (deflection after the step, plus its damping) minus what the rod carries.
    const double mount = m_settings.damperMount.Value(m_damperMount + w * dt) + m_settings.damperMountDamping * w;
    const double piston = rate - w;
    const double stroke = compression - (m_damperMount + w * dt);
    const double rod = m_settings.damper.Value(piston) + m_friction->Evaluate(piston, normal, dt) + m_settings.bumpStop.Value(stroke) + m_settings.reboundStop.Value(stroke);
    return mount - rod;
}

double StrutUnit::Step(double compression, double compressionRate, double normal, double dt)
{
    // Spring path: coil and seat rubber share the compression and carry the same force.
    if (m_settings.springMount.IsZero())
    {
        m_springMount = 0.0;
    }
    else
    {
        double coilCompression = compression - m_springMount;
        for (int it = 0; it < 30; ++it)
        {
            double kc = 0.0;
            double km = 0.0;
            const double fc = m_settings.coilSpring.Evaluate(coilCompression, kc);
            const double fm = m_settings.springMount.Evaluate(compression - coilCompression, km);
            const double slope = kc + km;
            if (slope <= 0.0)
            {
                break;
            }
            const double step = (fc - fm) / slope;
            coilCompression -= step;
            if (std::abs(step) < 1e-12)
            {
                break;
            }
        }
        m_springMount = compression - coilCompression;
    }
    const double coilForce = m_settings.coilSpring.Value(compression - m_springMount);
    m_springForce = m_settings.springPreload + coilForce;

    // Damper path. With a rigid mount the rod moves with the element.
    double w = 0.0;
    if (!m_settings.damperMount.IsZero() || m_settings.damperMountDamping > 0.0)
    {
        // Solve RodBalance(w) = 0 for the mount's deflection rate w. The balance rises with w for
        // any sensible data (rubber stiffness and damping, damper slope, the bristles' sigma0 dt + sigma1),
        // so bracket the root, then take Newton steps (numerical slope) kept inside the bracket.
        const auto f = [&](double x) {
            return RodBalance(x, compression, compressionRate, normal, dt);
        };
        double lo = m_mountRate;
        double hi = m_mountRate;
        double flo = f(lo);
        double fhi = flo;
        double span = std::max(std::abs(compressionRate), 1e-3);
        for (int expand = 0; expand < 80 && flo > 0.0; ++expand)
        {
            hi = lo;
            fhi = flo;
            lo -= span;
            flo = f(lo);
            span *= 2.0;
        }
        for (int expand = 0; expand < 80 && fhi < 0.0; ++expand)
        {
            lo = hi;
            flo = fhi;
            hi += span;
            fhi = f(hi);
            span *= 2.0;
        }
        double x = std::clamp(m_mountRate, lo, hi);
        for (int it = 0; it < 60; ++it)
        {
            const double fx = f(x);
            if (std::abs(fx) < 1e-9)
            {
                break;
            }
            if (fx < 0.0)
            {
                lo = x;
            }
            else
            {
                hi = x;
            }
            const double h = 1e-7 * (1.0 + std::abs(x));
            const double slope = (f(x + h) - fx) / h;
            double next = slope > 0.0 ? x - fx / slope : 0.5 * (lo + hi);
            if (!(next > lo && next < hi))
            {
                next = 0.5 * (lo + hi);
            }
            if (std::abs(next - x) < 1e-13 * (1.0 + std::abs(x)))
            {
                x = next;
                break;
            }
            x = next;
        }
        w = x;
    }
    m_mountRate = w;
    m_pistonVelocity = compressionRate - w;
    const double stroke = compression - (m_damperMount + w * dt);
    m_hydraulicForce = m_settings.damper.Value(m_pistonVelocity);
    m_frictionForce = m_friction->Evaluate(m_pistonVelocity, normal, dt);
    m_friction->Commit(m_pistonVelocity, normal, dt);
    m_rodForce = m_hydraulicForce + m_frictionForce + m_settings.bumpStop.Value(stroke) + m_settings.reboundStop.Value(stroke);
    m_damperMount += w * dt;

    m_force = m_springForce + m_rodForce;
    return m_force;
}
}
