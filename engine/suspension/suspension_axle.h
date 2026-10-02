#pragma once

#include "suspension_corner.h"
#include "suspension_strut.h"

#include <array>
#include <memory>
#include <vector>

namespace me::suspension
{

// ---- Solid (live) axle ----
//
// The axle frame: x forward, y left, z up, from the axle's centre (midway between the wheel centres)
// at the design position. Each link joins a chassis point to a point on the axle by a ball joint at
// either end.
struct AxleLinkDef
{
    Vec3 chassis{0.0};
    Vec3 axle{0.0};
};

struct SolidAxleDefinition
{
    std::vector<AxleLinkDef> links;
    double track = 1.5;      // wheel centre to wheel centre, m
    double tyreRadius = 0.3; // unloaded, m
    // Where the springs and dampers sit along each half of the axle: 0 at its centre, 1 at the wheel.
    double springPosition = 1.0;
    // What else holds the axle sideways (leaf springs), N/m at its centre.
    double lateralStiffness = 0.0;
    // The links as very stiff springs (their bushings): a layout with more links than the axle has
    // freedoms (four trailing links and a Panhard rod) settles where they give least.
    double linkStiffness = 1e9;
};

// The axle's pose: its centre's displacement from the design position and its yaw, pitch and roll
// (R = Rz(yaw) Ry(pitch) Rx(roll), radians).
struct SolidAxlePose
{
    Vec3 center{0.0};
    double yaw = 0.0;
    double pitch = 0.0;
    double roll = 0.0;
    Mat3 rotation{1.0};
};

// A solid axle carrying both wheels of an axle: posed by the two wheels' travel (bump positive),
// located by its links, sprung and damped by a unit on each side at springPosition. Its outputs
// have the shape of a SuspensionCorner's, each in its own wheel's corner frame (from that wheel's
// centre at the design position), side 0 the left wheel and 1 the right.
class SolidAxle
{
public:
    SolidAxle(SolidAxleDefinition definition, StrutUnit left, StrutUnit right);

    // Poses the axle for the wheels' travel (m).
    void Solve(double leftTravel, double rightTravel);
    const SolidAxlePose& Pose() const
    {
        return m_pose;
    }
    // A point fixed in the axle (axle frame at design) where the axle has it now, axle frame.
    Vec3 Point(const Vec3& design) const;
    // The largest stretch or squeeze of a link from its length (m): what its bushings give.
    double LinkStretch() const
    {
        return m_stretch;
    }
    const SolidAxleDefinition& Definition() const
    {
        return m_definition;
    }
    double Travel(int side) const
    {
        return m_travel[side];
    }

    // The wheel's geometry at the current pose, in its corner frame.
    void ComputeOutputs(int side, KinematicOutputs& out) const;

    // One step: kinematics, both units, and the reactions as CornerOutputs (rack and side load are
    // not used).
    void Step(const std::array<CornerInput, 2>& in);
    const CornerOutput& Output(int side) const
    {
        return m_out[side];
    }

private:
    struct Rates
    {
        // Per metre of the inputs: the axle's angular velocity, and the velocities of both wheels'
        // centres and of the axle-fixed points under their contact points.
        Vec3 omega{0.0};
        std::array<Vec3, 2> wheel{};
        std::array<Vec3, 2> contact{};
    };
    SolidAxlePose PoseAt(const std::array<double, 4>& free, double leftTravel, double rightTravel) const;
    void Residuals(const std::array<double, 4>& free, double leftTravel, double rightTravel, std::vector<double>& r) const;
    Rates RatesFor(double dLeft, double dRight) const;
    void FillOutputs(int side, const Rates& own, const Rates& roll, const Rates& heave, KinematicOutputs& out) const;
    Vec3 WheelCenterAt(int side) const;
    Vec3 SpinAxisAt(int side) const;
    Vec3 ContactAt(int side) const;

    SolidAxleDefinition m_definition;
    std::array<StrutUnit, 2> m_units;
    std::vector<double> m_length;
    SolidAxlePose m_pose;
    std::array<double, 4> m_free{}; // x, y, yaw, pitch
    std::array<std::array<double, 4>, 2> m_sensitivity{}; // d free / d travel, each wheel
    std::array<double, 2> m_travel{};
    double m_stretch = 0.0;
    std::array<CornerOutput, 2> m_out;
};

// ---- An axle's linkage, for drawing ----
//
// In the axle frame (x forward, y left, z up from midway between the wheel centres at the design
// position): the rods and arms, the wheel carriers (each upright from its wheel centre to its joints,
// or a solid axle's beam), the chassis pivots and the joints on the moving parts, and the wheel
// centres.
struct LinkageSketch
{
    std::vector<std::array<Vec3, 2>> links;
    std::vector<std::array<Vec3, 2>> carriers;
    std::vector<Vec3> chassis;
    std::vector<Vec3> joints;
    std::array<Vec3, 2> wheelCenters{};
};

// ---- An axle's two wheels, stepped together ----
//
// Independent corners (each its own SuspensionCorner) or a solid axle: the rigs and the vehicle step
// an axle at a time and read each wheel's output the same way.
class AxleSuspension
{
public:
    AxleSuspension(std::unique_ptr<SuspensionCorner> left, std::unique_ptr<SuspensionCorner> right);
    explicit AxleSuspension(std::unique_ptr<SolidAxle> axle);

    void Step(const std::array<CornerInput, 2>& in);
    const CornerOutput& Output(int side) const;

    // The independent corner on a side (null for a solid axle), and the solid axle (null otherwise).
    SuspensionCorner* Corner(int side) const
    {
        return m_corners[side].get();
    }
    // Moves the linkage to the wheels' travel and the rack without stepping its units (for drawing).
    void Pose(double leftTravel, double rightTravel, double rack);
    // Where the linkage is now. `halfTrack` places independent corners' frames (their wheel centres at
    // the design position) either side of the axle's centre; a solid axle uses its own track.
    void Sketch(double halfTrack, LinkageSketch& out) const;
    double SketchHalfTrack(double halfTrack) const
    {
        return m_solid ? 0.5 * m_solid->Definition().track : halfTrack;
    }
    SolidAxle* Solid() const
    {
        return m_solid.get();
    }

private:
    std::array<std::unique_ptr<SuspensionCorner>, 2> m_corners;
    std::unique_ptr<SolidAxle> m_solid;
};
}
