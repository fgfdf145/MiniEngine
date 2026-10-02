#pragma once

#include "suspension_math.h"
#include "suspension_model.h"

#include <vector>

namespace me::suspension
{

// ---- The constraint equations Phi(q; z, u) = 0 of a compiled model ----
//
// q holds the moving points' coordinates (3 each) and the slide coordinates; z is the wheel
// travel and u the rack travel. Distance rows are written as 1/2(|p_a - p_b|^2 - L^2) so that their
// gradient is simply (p_a - p_b).

// Where point `index` is for the unknowns `q` and rack travel `rack`.
Vec3 PointPosition(const Model& model, const std::vector<double>& q, double rack, int index);

// Phi(q; z, u), one entry per row.
void EvaluateConstraints(const Model& model, const std::vector<double>& q, double travel, double rack, std::vector<double>& phi);

// Phi_q (rows x unknowns), and dPhi/dz, dPhi/du.
void EvaluateJacobian(const Model& model, const std::vector<double>& q, double rack, DenseMatrix& jacobian, std::vector<double>* dPhiDTravel, std::vector<double>* dPhiDRack);

// The design configuration (z = 0, u = 0).
std::vector<double> DesignCoordinates(const Model& model);

enum class SolveStatus
{
    Converged,
    Clamped,  // the requested input was out of reach; the state stopped at the last reachable input
    Failed,   // no step could be taken; the state is the previous one
};

struct SolveReport
{
    SolveStatus status = SolveStatus::Converged;
    int iterations = 0;      // corrections taken, chord steps (old factorisation) included
    int factorisations = 0;  // LU decompositions, including the one at the solution
    double residual = 0.0;   // max |Phi| at the solution
    double pivotRatio = 0.0; // of Phi_q at the solution; small means near a singular position
    bool nearSingular = false;
    double travel = 0.0;     // the inputs actually reached
    double rack = 0.0;
};

struct SolverSettings
{
    int maxIterations = 12;
    // Distance rows are 1/2(|d|^2 - L^2) ~ L.delta, so 1e-10 m^2 is a length error below a nanometre on
    // links of 0.1-1 m; slider and travel rows are in metres.
    double residualTolerance = 1e-10;
    double singularPivotRatio = 1e-9; // below this the position is treated as singular
    int maxBisections = 8;            // input halvings before giving up on a request
};

// Kinematics of one corner with ideal joints: solves the position for wheel and rack travel,
// warm-started from the previous frame, and keeps the velocity sensitivities dq/dz and dq/du.
class Kinematics
{
public:
    explicit Kinematics(const Model& model, SolverSettings settings = {});

    const Model& GetModel() const
    {
        return m_model;
    }

    // Moves to (travel, rack). Starts from the current state with a first-order predictor and one
    // chord step using the factorisation at the current state; falls back to full Newton with a
    // backtracking line search, then to halving the input step. Never leaves an invalid state.
    const SolveReport& Solve(double travel, double rack);
    const SolveReport& LastReport() const
    {
        return m_report;
    }

    double Travel() const
    {
        return m_travel;
    }
    double Rack() const
    {
        return m_rack;
    }
    const std::vector<double>& Coordinates() const
    {
        return m_q;
    }
    Vec3 Point(int index) const
    {
        return PointPosition(m_model, m_q, m_rack, index);
    }
    // dq/dz and dq/du at the current position.
    const std::vector<double>& DqDTravel() const
    {
        return m_dqDz;
    }
    const std::vector<double>& DqDRack() const
    {
        return m_dqDu;
    }
    // dp/dz, dp/du of any point (rack points move with u, chassis points not at all).
    Vec3 PointDTravel(int index) const;
    Vec3 PointDRack(int index) const;
    // Slide coordinate s and its derivatives.
    double Slide(int slider) const
    {
        return m_q[m_model.sliderSlot[slider]];
    }

    // qdd for given input rates and accelerations: Phi_q qdd = gamma(q, qdot) (needs the current factorisation).
    void Accelerations(double travelRate, double rackRate, double travelAccel, double rackAccel, std::vector<double>& qdd) const;

    // Phi_q at the current position, factorised (reused by the statics).
    const DenseMatrix& Jacobian() const
    {
        return m_jacobian;
    }
    const DenseLu& Factorisation() const
    {
        return m_lu;
    }

private:
    bool Attempt(double travel, double rack, int& iterations, int& factorisations);
    bool Refactor();
    bool OrientationKept() const;
    void UpdateSensitivities();

    Model m_model;
    SolverSettings m_settings;
    std::vector<double> m_q;
    double m_travel = 0.0;
    double m_rack = 0.0;
    DenseMatrix m_jacobian;
    DenseLu m_lu;
    std::vector<double> m_dqDz;
    std::vector<double> m_dqDu;
    std::vector<double> m_phiZ;
    std::vector<double> m_phiU;
    SolveReport m_report;
    // Four knuckle points spanning the largest tetrahedron, and its design orientation, to catch a
    // solve that lands on the mirror-image assembly.
    int m_tetra[4] = {-1, -1, -1, -1};
    double m_tetraSign = 0.0;
};

// ---- What a K&C rig would read off one corner ----

struct ElementGeometry
{
    double length = 0.0;
    double lengthPerTravel = 0.0; // dl/dz (negative when the element shortens in bump)
    double motionRatio = 0.0;     // -dl/dz
};

struct KinematicOutputs
{
    Vec3 wheelCenter{0.0};
    Vec3 spinAxis{0.0};      // unit, outward
    Vec3 contactPoint{0.0};  // lowest point of the unloaded tyre circle on a road parallel to z = 0

    // Signs: camber positive when the wheel's top leans outward; toe positive toe-in. Same on both
    // sides. Angles in radians.
    double camber = 0.0;
    double toe = 0.0;
    double camberPerTravel = 0.0; // rad/m
    double toePerTravel = 0.0;    // rad/m (bump steer)
    double toePerRack = 0.0;      // rad/m (steering ratio at the wheel)

    // Steering axis: the knuckle's instantaneous screw axis for rack motion (the ball-joint line for
    // a double wishbone and C-top mount for a MacPherson; a virtual kingpin for multi-links).
    bool kingpinValid = false;
    Vec3 kingpinPoint{0.0};
    Vec3 kingpinAxis{0.0};      // unit, upward
    double kingpinInclination = 0.0; // positive with the axis' top leaning inboard
    double caster = 0.0;             // positive with the axis' top leaning rearward
    double casterTrail = 0.0;        // m, positive when the axis meets the road ahead of the contact point
    double scrubRadius = 0.0;        // m, positive when the axis meets the road inboard of the contact point
    double kingpinOffset = 0.0;      // m, wheel centre to the axis

    // Displacements from the design position.
    Vec3 wheelCenterChange{0.0};
    double halfTrackChange = 0.0;    // contact point, outward positive
    double wheelbaseChange = 0.0;    // contact point, forward positive

    // Front view (y-z): instant centre of the knuckle and the roll centre height on the vehicle's
    // centre plane (symmetric axle assumed). Side view (x-z): the paths' angles for anti-dive /
    // anti-squat (forward motion per upward motion), and the instant centre.
    bool frontInstantCenterValid = false;
    Vec3 frontInstantCenter{0.0};
    double rollCenterHeight = 0.0;      // above the road at the contact point
    double contactPathAngle = 0.0;     // atan(dx_P/dz): outboard brakes
    double wheelCenterPathAngle = 0.0; // atan(dx_W/dz): inboard brakes, driven axle
    bool sideInstantCenterValid = false;
    Vec3 sideInstantCenter{0.0};

    double effectiveMass = 0.0; // sum m_i |dp_i/dz|^2, kg

    std::vector<ElementGeometry> elements;
};

void ComputeOutputs(const Kinematics& kinematics, KinematicOutputs& out);

// The knuckle's angular velocity (least squares over its points) for a rate dq of the unknowns
// and `rackRate` of the rack points.
Vec3 KnuckleAngularRate(const Kinematics& kinematics, const std::vector<double>& dq, double rackRate);

// dq/du with the steering reference point's height held instead of the wheel travel.
std::vector<double> SteeringSensitivity(const Kinematics& kinematics);
}
