#pragma once

#include <memory>

namespace me::suspension
{

// A friction parameter that depends on the normal load N on the contact (for a strut: the side
// load): value(N) = c0 + c1 |N| + c2 N^2. Deubel, Meinck & Prokop (Tribology International 215,
// 2026) report expressing the Stribeck parameters as side-force dependent; their exact functional
// form was not available, so this quadratic is an assumption to be fitted to measurements.
struct LoadDependent
{
    double c0 = 0.0;
    double c1 = 0.0;
    double c2 = 0.0;

    double At(double normal) const
    {
        const double n = normal < 0.0 ? -normal : normal;
        return c0 + c1 * n + c2 * n * n;
    }
};

// Friction in a sliding pair as a pluggable model. Evaluate() asks what force a relative velocity
// would give over the next step without changing the state (so an implicit solve can try several
// velocities); Commit() then advances the state with the velocity chosen.
class FrictionModel
{
public:
    virtual ~FrictionModel() = default;

    // Friction force opposing motion is returned with the sign of v (it resists v), in N.
    virtual double Evaluate(double v, double normal, double dt) const = 0;
    virtual void Commit(double v, double normal, double dt) = 0;
    virtual void Reset() = 0;
    virtual std::unique_ptr<FrictionModel> Clone() const = 0;
};

// No friction.
class NoFriction final : public FrictionModel
{
public:
    double Evaluate(double, double, double) const override
    {
        return 0.0;
    }
    void Commit(double, double, double) override
    {
    }
    void Reset() override
    {
    }
    std::unique_ptr<FrictionModel> Clone() const override
    {
        return std::make_unique<NoFriction>(*this);
    }
};

struct StribeckParameters
{
    LoadDependent coulomb{50.0, 0.0, 0.0};  // F_c (N)
    LoadDependent breakaway{80.0, 0.0, 0.0}; // F_s (N), >= F_c
    double stribeckVelocity = 0.01;          // v_s (m/s)
    double shape = 2.0;                      // delta in exp(-|v/v_s|^delta)
    double viscous = 0.0;                    // sigma2 (N s/m)
};

// The steady Stribeck curve g(v, N) = F_c + (F_s - F_c) exp(-|v/v_s|^delta).
double StribeckCurve(const StribeckParameters& p, double v, double normal);

// Static (stateless) Stribeck friction, regularised around v = 0 with tanh(v / regularisation):
// F = g(v, N) tanh(v/eps) + sigma2 v. Cheap, but cannot hold a force at rest (no true stiction).
class StribeckFriction final : public FrictionModel
{
public:
    StribeckFriction(StribeckParameters parameters, double regularisation = 1e-3);

    double Evaluate(double v, double normal, double dt) const override;
    void Commit(double, double, double) override
    {
    }
    void Reset() override
    {
    }
    std::unique_ptr<FrictionModel> Clone() const override
    {
        return std::make_unique<StribeckFriction>(*this);
    }

private:
    StribeckParameters m_p;
    double m_regularisation;
};

struct LuGreParameters
{
    StribeckParameters stribeck;
    double bristleStiffness = 1e5; // sigma0 (N/m)
    double bristleDamping = 300.0; // sigma1 (N s/m)
    // sigma1 fades with speed as exp(-(v/v_d)^2) when v_d > 0, a common variant that keeps the
    // bristle damping from adding viscous force in sliding.
    double dampingFadeVelocity = 0.0;
};

// LuGre (Canudas de Wit et al., 1995): bristle deflection z,
//   zdot = v - sigma0 |v| z / g(v, N),   F = sigma0 z + sigma1 zdot + sigma2 v.
// Integrated by backward Euler in z (z enters zdot linearly for a given v), so it is stable at any
// step. With N-dependent g the bristle state can sit above the new steady value when N drops; it is
// then clamped to |z| <= g/sigma0, a simple choice that is not from the literature.
class LuGreFriction final : public FrictionModel
{
public:
    explicit LuGreFriction(LuGreParameters parameters);

    double Evaluate(double v, double normal, double dt) const override;
    void Commit(double v, double normal, double dt) override;
    void Reset() override
    {
        m_z = 0.0;
    }
    std::unique_ptr<FrictionModel> Clone() const override
    {
        return std::make_unique<LuGreFriction>(*this);
    }

    double BristleDeflection() const
    {
        return m_z;
    }

private:
    double Step(double v, double normal, double dt, double& zNext) const;

    LuGreParameters m_p;
    double m_z = 0.0;
};
}
