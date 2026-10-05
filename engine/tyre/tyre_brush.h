#pragma once

#include <array>

namespace me::tyre
{

// The most ribs a brush tyre is cut into.
inline constexpr int kBrushMaxRibs = 16;

// One rib's contact where the last step left it, for drawing the patch: its place across the tread
// (m, left positive), its contact length, and how far from the leading edge its bristles stick to the
// road before they slide (the whole length when none slide).
struct BrushRibContact
{
    double y = 0.0;
    double length = 0.0;
    double stuckLength = 0.0;
};

// A brush tyre with a flexible carcass after Stocco, Biral & Bertolazzi, "A physical tire model for
// real-time simulations", Math. Comput. Simul. 223 (2024) 654-676 (docs/references): the tread is
// cut into ribs across its width, each a row of bristles over its own contact length; the bristles'
// roots sit on a carcass that moves against the rim by a fore-aft shift x_c, a sideways shift y_c
// and a twist theta_c, its centre line across the patch the parabola (10)
//
//   y(x) = y_c + theta_c (x - x_c) - y_c Psi/2 (x - x_c)^2,
//
// held by springs K and (here) dampers D. The bristles stick to the road from the leading edge until
// their pull reaches static friction (48), then slide with a friction that falls with sliding speed.
//
// Where the paper solves the carcass's static balance K c = F(c), this model keeps the carcass's
// velocity in the bristles' kinematics (the paper's slips (21), (22) carry x_c', y_c', theta_c')
// and steps K c + D c' = F(c, c') implicitly, so the force builds over the carcass's relaxation
// length and holds the car standing still. dt <= 0 gives the paper's steady state.
//
// Axes: ISO 8855 at the contact centre (x forward along the wheel, y left, z up the road's normal).
// Forces and moments are the road's on the tyre, which the carcass passes to the rim.
struct BrushTyreParameters
{
    // Geometry.
    double unloadedRadius = 0.32; // R0, m
    double width = 0.22;          // tread width, m
    int ribs = 10;
    int segmentsPerRib = 20;
    // R_l of (6): the belt's stiffness shortens the contact length below the plain intersection's.
    double transitionRadius = 0.14; // m
    double verticalRate = 250000.0; // N/m, gives the deflection that sizes the patch

    // Pressure along each rib, the quartic (7): convexity lambda (12 is a parabola) and centroid
    // shift delta (towards the leading edge, a share of the length).
    double pressureConvexity = 4.0;
    double pressureShift = 0.0;

    // The bristles' stiffness per unit area of tread, N/m^3 (34), times the tread's void ratio.
    double bristleStiffnessX = 3.0e7;
    double bristleStiffnessY = 3.0e7;

    // Friction: the static coefficient at referenceLoad, scaled by (Fz / referenceLoad)^(loadExponent - 1);
    // sliding falls from it to kineticShare of it with sliding speed (a Stribeck curve, speed v_mu).
    double staticFriction = 1.1;
    double kineticShare = 0.85;
    double stribeckVelocity = 3.0; // m/s
    double referenceLoad = 4000.0; // N
    double loadExponent = 1.0;

    // The carcass: stiffnesses (N/m, N/m, N m/rad), dampers (N s/m, N s/m, N m s/rad), the bending
    // shape factor Psi (1/m^2 in the parabola above), and the bottoming of (14)-(15): past these
    // deflections the stiffness rises by the bottoming stiffness times the square of the excess.
    std::array<double, 3> carcassStiffness{400000.0, 200000.0, 8000.0};
    std::array<double, 3> carcassDamping{600.0, 600.0, 10.0};
    double bendingShape = 3.0;
    std::array<double, 3> bottomingDeflection{0.05, 0.055, 0.12};
    std::array<double, 3> bottomingStiffness{1.0e7, 1.0e7, 1.0e6};

    // How much of the wheel's spin about the road's normal that camber gives reaches the tread as
    // turn slip (1 - epsilon_gamma; Pacejka (4.76) puts epsilon up to about 0.7 for radial car tyres).
    double camberSpinShare = 0.3;
    // Rolling resistance: a moment opposing the wheel's roll of coefficient * Fz * radius.
    double rollingResistance = 0.012;
    // Below this speed of the tread through the patch (m/s) the bristles' transport is held to it,
    // and sliding directions are regularised over a tenth of it: the tyre then creeps under a steady
    // pull at about F / (C / v0) instead of dividing by zero.
    double lowSpeed = 0.3;
};

// The tyre's moving parts between steps: the carcass's deflection and the last Jacobian of its balance.
struct BrushTyreState
{
    std::array<double, 3> carcass{0.0, 0.0, 0.0}; // x_c, y_c (m), theta_c (rad)
    std::array<double, 9> jacobian{};
    bool jacobianValid = false;
};

struct BrushTyreInput
{
    double load = 0.0; // Fz, N
    // The wheel centre's velocity over the ground in the contact frame (m/s) and the wheel's turn
    // rate about the road's normal (rad/s, yaw positive left).
    double forwardVelocity = 0.0;
    double lateralVelocity = 0.0;
    double yawRate = 0.0;
    double wheelSpeed = 0.0;    // rad/s about the axle, positive rolling forward
    double camber = 0.0;        // rad, positive with the wheel's top leaning right
    double frictionScale = 1.0; // the road's ratio to the friction the tyre's figures are for
    // The road's own limit on the static coefficient (frozen, loose ground; 0 for none), the share of
    // it left sliding there (0 keeps the tyre's kineticShare), and its rolling resistance coefficient,
    // added to the tyre's.
    double frictionCap = 0.0;
    double slidingShare = 0.0;
    double extraRollingResistance = 0.0;
};

struct BrushTyreOutput
{
    double Fx = 0.0, Fy = 0.0, Mz = 0.0; // N, N, N m
    // The rolling resistance moment on the wheel about its axle (opposing its roll), N m.
    double rollingResistanceTorque = 0.0;
    double effectiveRadius = 0.0; // R0 - deflection / 3 at the middle rib (23)
    double contactLength = 0.0;   // the longest rib's, m
    double slidingShare = 0.0;    // of the load, on sliding bristles
    double peakFriction = 0.0;    // the static coefficient at this load and road
    double slipRatio = 0.0;       // diagnostics: (omega R_e - V_x) / |V_x|
    double slipAngle = 0.0;       // rad, atan(V_y / |V_x|) as MF-Tyre's (positive moving left)
    int evaluations = 0;
    bool converged = true;
    // Each rib's contact, and whether the tread runs through it forwards (enters at +x).
    std::array<BrushRibContact, kBrushMaxRibs> ribs{};
    int ribCount = 0;
    bool rollingForward = true;
};

class BrushTyre
{
  public:
    explicit BrushTyre(BrushTyreParameters parameters = {});

    const BrushTyreParameters& Parameters() const
    {
        return m_p;
    }
    const BrushTyreState& State() const
    {
        return m_state;
    }
    void Reset();

    // One step of dt seconds: the carcass moves to its implicit balance and the forces are the road's
    // at the end of it. dt <= 0 solves the steady state instead (the carcass at rest in balance).
    BrushTyreOutput Step(const BrushTyreInput& input, double dt);

    // The steady-state forces without changing the state (for curves and tests).
    BrushTyreOutput Steady(const BrushTyreInput& input) const;

    // The linear slip stiffnesses of the bristles alone at this load (rigid carcass): dFx/dkappa and
    // dFy/dtan(alpha), N.
    std::array<double, 2> BristleSlipStiffness(double load) const;

  private:
    BrushTyreParameters m_p;
    BrushTyreState m_state;
};

// Parameters for a road or race tyre from the figures a car's data gives: peak friction at a reference
// load, the slip angle where the lateral force peaks, the falloff past it, the tyre's radius, width and
// vertical rate. The bristles' stiffness is set so that a rigid-carcass brush would peak at that angle
// (tan alpha_sl = 3 mu Fz / C_alpha); the carcass so that its relaxation lengths are about 0.6 R0
// sideways and 0.4 R0 fore-aft.
BrushTyreParameters MakeBrushTyreParameters(double peakFriction, double referenceLoad, double peakSlipAngle, double kineticShare,
                                            double radius, double width, double verticalRate);

}
