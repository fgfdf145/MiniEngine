#pragma once

#include "flex_ring_banded.h"
#include "flex_ring_parameters.h"
#include "flex_ring_road.h"

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace me::tyre::flexring
{

using Mat3 = glm::dmat3;

// The rim's rigid-body state, the tyre's input (FTire's standard interface, Gipser 2000, fig. 2): its
// centre, the rotation from rim-fixed to global axes (it turns with the wheel; column 1 is the spin axis,
// pointing to the tyre's +lateral side), and their rates, global.
struct RimState
{
    Vec3 position{0.0};
    Mat3 rotation{1.0};
    Vec3 velocity{0.0};
    Vec3 angularVelocity{0.0};
};

// Force and moment of the tyre on the rim, global, about the rim centre.
struct Wrench
{
    Vec3 force{0.0};
    Vec3 moment{0.0};
};

// A contact block as the last step left it.
struct BlockView
{
    Vec3 base{0.0};  // the block's free tip (belt surface plus the block's height), global
    Vec3 tip{0.0};   // where its tip is on the road, global (in contact)
    Vec3 force{0.0}; // the road's force on the block, global
    double normalForce = 0.0;
    double groundPressure = 0.0; // Pa
    double slidingSpeed = 0.0;   // m/s
    double friction = 0.0;       // the friction coefficient it slid with, or would stick up to
    bool contact = false;
    bool sliding = false;
};

struct ContactStats
{
    int blocks = 0;
    int sliding = 0;
    double normalForce = 0.0;   // sum, N
    double area = 0.0;          // m^2 of rubber in contact
    double length = 0.0;        // extent along the wheel's heading, m
    double width = 0.0;         // extent across, m
    double maxPressure = 0.0;   // Pa
    double meanPressure = 0.0;  // Pa
    Vec3 centre{0.0};           // force-weighted centre of the blocks in contact
    double frictionPower = 0.0; // W, sliding blocks
};

struct StepStats
{
    int substeps = 0;
    int factorizations = 0;
    int failedFactorizations = 0;
    double seconds = 0.0; // wall time of the last Advance
};

// A flexible ring tyre after FTire (Gipser 1999, 2000, 2004, 2006; FTire documentation 2022): a ring of
// belt nodes (three translations and the torsion about the circumferential axis each) on radial,
// tangential and lateral foundation elements to the rim, chained by extension springs and in-plane and
// out-of-plane bending stiffness, under the inflation pressure; the belt's lateral bending as free-free
// beam shapes across its width; mass-less tread blocks between belt and road with friction depending on
// sliding speed and ground pressure; an implicit (BDF parameter) integration of the belt with a cyclic band
// system matrix. docs/design/2026-10-09-flex-ring-tyre-design.md has the equations.
class FlexRingTyre
{
  public:
    explicit FlexRingTyre(FlexRingParameters parameters);

    const FlexRingParameters& Parameters() const
    {
        return m_p;
    }
    int Segments() const
    {
        return m_n;
    }
    int BlocksPerSegment() const
    {
        return static_cast<int>(m_p.blocks.size());
    }

    // The road (owned by the caller, outliving the tyre or the next SetRoad). Null: no road.
    void SetRoad(const Road* road);
    const Road* GetRoad() const
    {
        return m_road;
    }

    // Operating conditions (FTire chapter 7): the actual inflation pressure (Pa), the actual tread depth
    // (m) and a friction scale on top of the road's.
    void SetPressure(double pascal);
    double Pressure() const
    {
        return m_pressure;
    }
    void SetTreadDepth(double metres);
    double TreadDepth() const
    {
        return m_treadDepth;
    }
    void SetFrictionScale(double scale)
    {
        m_frictionScale = scale;
    }

    // Puts the belt at rest on the rim (inflated, unloaded, turning with it), drops all contact.
    void Reset(const RimState& rim);

    // Integrates from the last rim state to `rimEnd`, dt seconds later, in internal steps (no longer than
    // the maximum step, no more rim rotation than the maximum angle increment), the rim moving along a
    // cubic between the two states. Returns the mean force and moment on the rim over dt.
    Wrench Advance(const RimState& rimEnd, double dt);

    // Quasi-static settling at a fixed rim (FTire's statics: implicit Euler steps of h seconds with the
    // masses scaled down) until the belt's speed is below `speedTolerance`; false if maxSteps ran out.
    bool SettleStatic(const RimState& rim, int maxSteps = 2000, double h = 0.003, double massScale = 0.01, double speedTolerance = 1.0e-5);

    // State and outputs.
    const RimState& Rim() const
    {
        return m_rim;
    }
    double Time() const
    {
        return m_time;
    }
    const Wrench& LastWrench() const
    {
        return m_wrench;
    }
    const std::vector<Vec3>& NodePositions() const
    {
        return m_x;
    }
    const std::vector<Vec3>& NodeVelocities() const
    {
        return m_v;
    }
    const std::vector<double>& Torsion() const
    {
        return m_psi;
    }
    // Lateral bending coefficients, [mode * segments + node].
    const std::vector<double>& LateralBending() const
    {
        return m_gamma;
    }
    const std::vector<BlockView>& Blocks() const
    {
        return m_blocks;
    }
    const ContactStats& Contact() const
    {
        return m_contact;
    }
    const StepStats& LastStep() const
    {
        return m_stepStats;
    }
    // The node's rest position on the rim now (global).
    Vec3 NodeRestPosition(int node) const;
    // A point of the belt's outer surface (before the tread) at a segment's sigma and a lateral position,
    // and the outward normal there; with `withTread` the tread's free surface instead.
    Vec3 SurfacePoint(int segment, double sigma, double lateral, bool withTread, Vec3* normal = nullptr) const;
    // Value and slope of lateral bending shape i at a lateral position (m).
    double BendShape(int mode, double lateral, double* slope = nullptr) const;

    // For linearization (modal analysis, tests): the structure's generalized forces at given node
    // positions, velocities, torsion angles and rates, the rim held where it is, without contact, gravity,
    // Maxwell or hysteresis elements. Forces per node in `force`, torsion moments in `torque`.
    void StructuralForces(const std::vector<Vec3>& x, const std::vector<Vec3>& v, const std::vector<double>& psi, const std::vector<double>& psiDot,
                          std::vector<Vec3>& force, std::vector<double>& torque);
    double NodeMass() const
    {
        return m_p.nodeMass;
    }
    double NodeInertia() const
    {
        return m_p.nodeInertia;
    }

  private:
    struct Frame
    {
        Vec3 centre{0.0};
        Vec3 velocity{0.0};
        Vec3 omega{0.0};
        Mat3 rotation{1.0};
        Vec3 axis{0.0, 1.0, 0.0};
        std::vector<Vec3> attach;
        std::vector<Vec3> attachVelocity;
        std::vector<Vec3> radial;
        std::vector<Vec3> tangential;
    };
    struct EvalSettings
    {
        double h = 1.0e-3;
        double beta = 0.5;
        bool assemble = false;
        bool commit = false;
        bool contact = true;
        bool gravity = true;
        bool internalElements = true; // Maxwell and hysteresis
    };
    struct BlockMemory
    {
        Vec3 tip{0.0}; // road-fixed tip position (global for a road at rest)
        bool contact = false;
        bool sliding = false;
    };

    void BuildFrame(const RimState& rim, Frame& frame) const;
    RimState Interpolate(const RimState& a, const RimState& b, double dt, double s) const;
    void Step(const RimState& rimEnd, double h, double beta, double massScale);
    // Forces (into m_force / m_torque) at the trial state; assembles the system matrix when asked; with
    // commit, keeps the internal elements' and blocks' new states and the outputs.
    void Evaluate(const std::vector<Vec3>& x, const std::vector<Vec3>& v, const std::vector<double>& psi, const std::vector<double>& psiDot,
                  const Frame& frame, const EvalSettings& settings, double massScale);
    void EvaluateContact(const std::vector<Vec3>& x, const std::vector<Vec3>& v, const std::vector<double>& psi, const std::vector<double>& psiDot,
                         const Frame& frame, const EvalSettings& settings);
    void UpdateLateralBending(double h);
    void AddBlock3(int nodeA, int nodeB, const Mat3& m, double coefficient);
    void AddScalar(int nodeA, int dofA, int nodeB, int dofB, double value);
    int Dof(int node, int component) const
    {
        return m_perm[node] * 4 + component;
    }
    double PressureRatio() const
    {
        return m_pressure / m_p.measuredPressure;
    }
    double BlockHeight(const BlockLayout& b) const
    {
        return m_treadDepth * b.depthShare + m_p.treadBase;
    }

    FlexRingParameters m_p;
    int m_n = 0;
    const Road* m_road = nullptr;
    double m_pressure = 2.0e5;
    double m_treadDepth = 0.005;
    double m_frictionScale = 1.0;

    // Rim-local reference geometry of each node.
    std::vector<Vec3> m_localRest;
    std::vector<Vec3> m_localRadial;
    std::vector<Vec3> m_localTangential;
    std::vector<int> m_perm;

    // State.
    RimState m_rim;
    Frame m_frame;
    Frame m_stepFrame;
    double m_time = 0.0;
    bool m_initialized = false;
    std::vector<Vec3> m_x, m_v;
    std::vector<double> m_psi, m_psiDot;
    std::vector<std::array<double, 3>> m_maxwell;  // internal displacement per direction
    std::vector<std::array<double, 7>> m_slider;   // hysteresis sliders: 5 radial, tangential, lateral
    std::vector<double> m_gamma;                    // lateral bending coefficients
    std::vector<double> m_bendLoad;                 // generalized lateral bending loads of the last commit
    std::vector<double> m_bendLambda;               // per mode
    std::vector<double> m_bendNorm;                 // shape normalization per mode
    std::vector<double> m_shape;                    // [block * modes + mode] at each block's lateral position
    std::vector<double> m_shapeSlope;
    std::vector<double> m_crownSlope;               // per block
    std::vector<double> m_work;
    std::vector<BlockMemory> m_memory;
    std::vector<BlockView> m_blocks;

    // Forces of the last evaluation, and of the last committed state (F0 of the next step).
    std::vector<Vec3> m_force;
    std::vector<double> m_torque;
    std::vector<Vec3> m_force0;
    std::vector<double> m_torque0;
    Wrench m_evalWrench;
    Wrench m_wrench;
    ContactStats m_contact;
    ContactStats m_evalContact;
    StepStats m_stepStats;

    // Pending internal states of the trial evaluation.
    std::vector<std::array<double, 3>> m_maxwellNew;
    std::vector<std::array<double, 7>> m_sliderNew;
    std::vector<BlockMemory> m_memoryNew;
    std::vector<double> m_bendLoadNew;
    std::vector<double> m_bendStiff;                // the blocks' normal stiffness on each shape, last commit
    std::vector<double> m_bendStiffNew;

    // Implicit step.
    BandMatrix m_matrix;
    int m_stepsSinceFactor = 1 << 30;
    int m_iterationsOverride = 0;
    bool m_factorValid = false;
    std::vector<double> m_rhs;
    std::vector<Vec3> m_xTrial, m_vTrial;
    std::vector<double> m_psiTrial, m_psiDotTrial;
};

}
