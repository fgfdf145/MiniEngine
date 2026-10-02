#pragma once

#include "suspension_curves.h"
#include "suspension_friction.h"

#include <memory>

namespace me::suspension
{

// The force along one spring/damper element (a MacPherson strut or a double wishbone's coil-over),
// with the top mount split into the two load paths a strut has:
//
//   spring path:  coil spring --(series)-- spring-seat rubber of the top mount
//   damper path:  hydraulic damper + friction + bump stop --(series)-- damper-rod rubber (k, c)
//
// The two paths act in parallel between the element's ends. A rigid mount (Curve with no samples)
// removes its rubber. With friction in series with a compliant mount the rod can stick and slip.
//
// Signs: compression x (shortening from the design length) and compression rate are positive; a
// positive force pushes the element's ends apart.
struct StrutUnitSettings
{
    double springPreload = 0.0; // N at the design length
    Curve coilSpring;           // added force over the preload, against coil compression
    Curve springMount;          // spring-seat rubber, force against its deflection (Zero: rigid)

    Curve damper;               // hydraulic force against piston compression velocity
    Curve bumpStop;             // force against rod compression (damper path)
    Curve reboundStop;          // force against rod compression, acting in extension (negative)
    Curve damperMount;          // damper-rod rubber, force against its deflection (Zero: rigid)
    double damperMountDamping = 0.0; // N s/m in parallel with that rubber
};

class StrutUnit
{
public:
    StrutUnit(StrutUnitSettings settings, std::unique_ptr<FrictionModel> friction);
    StrutUnit(const StrutUnit& other);
    StrutUnit& operator=(const StrutUnit& other);

    // Advances one step and returns the element's force. `normal` is the friction contacts' normal
    // load (the strut side load for a MacPherson; anything for a model that ignores it).
    double Step(double compression, double compressionRate, double normal, double dt);

    double Force() const
    {
        return m_force;
    }
    // The force's slopes at the last step: against compression (springs and stops through their
    // mounts) and against compression rate (damper and friction through the damper mount, for the
    // next step from the current state). For handing the force to a solver that linearises it.
    double StiffnessSlope(double compression) const;
    double RateSlope(double compressionRate, double normal, double dt) const;
    double SpringPathForce() const
    {
        return m_springForce;
    }
    double DamperPathForce() const
    {
        return m_rodForce;
    }
    double FrictionForce() const
    {
        return m_frictionForce;
    }
    double HydraulicForce() const
    {
        return m_hydraulicForce;
    }
    double PistonVelocity() const
    {
        return m_pistonVelocity;
    }
    double DamperMountDeflection() const
    {
        return m_damperMount;
    }
    double SpringMountDeflection() const
    {
        return m_springMount;
    }
    const FrictionModel& Friction() const
    {
        return *m_friction;
    }

private:
    double RodBalance(double w, double compression, double rate, double normal, double dt) const;

    StrutUnitSettings m_settings;
    std::unique_ptr<FrictionModel> m_friction;
    double m_springMount = 0.0; // deflection of the spring-seat rubber
    double m_damperMount = 0.0; // deflection of the damper-rod rubber
    double m_mountRate = 0.0;   // its last rate
    double m_force = 0.0;
    double m_springForce = 0.0;
    double m_rodForce = 0.0;
    double m_frictionForce = 0.0;
    double m_hydraulicForce = 0.0;
    double m_pistonVelocity = 0.0;
};
}
