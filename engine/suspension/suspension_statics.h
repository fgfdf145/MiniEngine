#pragma once

#include "suspension_kinematics.h"
#include "suspension_math.h"
#include "suspension_model.h"

#include <vector>

namespace me::suspension
{

// The road's force and moment on the tyre at the contact point (chassis frame).
struct WheelLoad
{
    Vec3 force{0.0};
    Vec3 moment{0.0};
};

// Spreads a force and moment acting at `at` over the points `positions` with the least sum of
// squared forces that has the same resultant and moment: f_k = lambda + mu x (p_k - at). For a rigid body
// this does the same virtual work as the load itself.
std::vector<Vec3> DistributeLoad(const std::vector<Vec3>& positions, const Vec3& at, const WheelLoad& load);

// Generalised forces Q (one entry per unknown) of a wheel load and of the elements' forces
// (`elementForces[e]` pushes element e's two points apart when positive).
void GeneralisedForces(const Model& model, const std::vector<double>& q, double rack, const Vec3& contactPoint, const WheelLoad& load, const std::vector<double>& elementForces, std::vector<double>& forces);

struct Reactions
{
    // Generalised force along the wheel travel (N): what the load and elements do per metre of
    // travel. Positive pushes the wheel up. A free corner is in equilibrium when this is zero.
    double travelForce = 0.0;
    // The tie rods' pull on the rack along the rack axis (N).
    double rackForce = 0.0;
    // Lagrange multipliers, one per row of Phi (Phi_q^T mu = Q).
    std::vector<double> multipliers;
    // Force on each point from the mechanism's joints (indexed like the model's points; only
    // chassis and rack points are meaningful as loads into the body), plus direct element forces.
    std::vector<Vec3> pointForces;
    // For each slider: the force its sliding joint puts on the `through` point (the top mount),
    // perpendicular to the strut axis, and its size: the strut's side load.
    std::vector<Vec3> sliderSideForce;
    std::vector<double> sliderSideLoad;
};

// Quasi-static force analysis with ideal joints and massless links, using the kinematics'
// factorisation of Phi_q (the transposed solve). `elementForces` may be empty.
void SolveReactions(const Kinematics& kinematics, const WheelLoad& load, const std::vector<double>& elementForces, Reactions& out);

struct WheelAttitude
{
    Vec3 wheelCenter{0.0};
    Vec3 spinAxis{0.0};
    Vec3 contactPoint{0.0};
    double camber = 0.0; // rad, top outward +
    double toe = 0.0;    // rad, toe-in +
};

WheelAttitude ComputeAttitude(const Model& model, const std::vector<double>& q, double rack);

// Elasto-kinematics: quasi-static equilibrium of a model compiled in ModelMode::Compliant (bushed
// chassis points are unknowns held by nonlinear bushings) for a wheel load at fixed wheel and rack
// travel. Solves the full nonlinear equilibrium (no superposition of linearised responses):
//   grad V(q) - Q(q) + Phi_q^T mu = 0,   Phi(q) = 0
// by Newton on the KKT system [H Phi_q^T; Phi_q 0], H = grad ^2V + sum mu_i grad ^2Phi_i (the load's own dependence on
// q, a follower effect, is left out of H).
class Compliance
{
public:
    explicit Compliance(const Model& compliantModel);

    struct Report
    {
        bool converged = false;
        int iterations = 0;     // KKT solves taken
        double residual = 0.0;  // largest force imbalance on an unknown, N
    };

    // Warm-started from the previous solution.
    Report Solve(double travel, double rack, const WheelLoad& load, const std::vector<double>& elementForces);

    const Model& GetModel() const
    {
        return m_model;
    }
    const std::vector<double>& Coordinates() const
    {
        return m_q;
    }
    const std::vector<double>& Multipliers() const
    {
        return m_mu;
    }
    double Rack() const
    {
        return m_rack;
    }
    Vec3 Point(int index) const
    {
        return PointPosition(m_model, m_q, m_rack, index);
    }
    // Bushing deflection in its own axes (m).
    Vec3 Deflection(int bushing) const;
    WheelAttitude Attitude() const
    {
        return ComputeAttitude(m_model, m_q, m_rack);
    }
    // Side load on slider `slider`'s through point (as in Reactions).
    double SliderSideLoad(int slider) const;

private:
    Model m_model;
    std::vector<double> m_q;
    std::vector<double> m_mu;
    double m_rack = 0.0;
};
}
