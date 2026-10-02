#pragma once

#include "suspension_curves.h"
#include "suspension_math.h"

#include <array>
#include <string>
#include <vector>

namespace me::suspension
{

// Axes of every suspension quantity: the chassis frame of the corner's axle, x forward, y left,
// z up (Rill's convention), metres, wheel centre or axle centre as the origin as the data has it.
// A right-hand corner is the mirror image (see MirrorToRight); `side` tells the outputs which way
// "outward" is.

// What a hardpoint is attached to.
enum class PointRole
{
    Chassis, // fixed to the chassis (or held by a bushing in compliance mode)
    Moving,  // solved for: on the knuckle or on a link
    Rack,    // on the chassis but moved along the rack axis by the rack travel
};

struct PointDef
{
    std::string name;
    Vec3 position{0.0};
    PointRole role = PointRole::Moving;
};

// A rigid body made of hardpoints: the knuckle, a control arm, a tie rod. Every body is massless in
// the kinematics (Choi et al.'s massless links); masses only enter EffectiveMass().
struct BodyDef
{
    std::string name;
    std::vector<std::string> points;
    double mass = 0.0;
};

// `through` (normally the strut top mount, a chassis point) stays on the line from `base` to `tip`,
// two points fixed in one body (the strut tube on the knuckle): a sliding pair with a ball joint
// at `through`. The slide adds one coordinate, the distance from `base` to `through`.
struct SliderDef
{
    std::string through;
    std::string base;
    std::string tip;
};

// A chassis point held by a rubber bushing instead of a rigid pivot. Only used by compliance
// solves; the kinematics keeps the point fixed. `axes` columns are the bushing's local x, y, z in
// the chassis frame; each local axis has its own force law F(deflection) (restoring).
struct BushingDef
{
    std::string point;
    Mat3 axes{1.0};
    Curve x;
    Curve y;
    Curve z;
};

// A spring, damper or strut acting along the line between two hardpoints: only its geometry
// (length, installation ratio) is the kinematics' business.
struct ElementDef
{
    std::string name;
    std::string a;
    std::string b;
};

// Makes the distance between two points of a body `delta` longer than the design positions say (a
// tie rod shortened or lengthened to set toe). The design position then no longer closes; the
// kinematics assembles it on construction.
struct LengthAdjust
{
    std::string a;
    std::string b;
    double delta = 0.0;
};

struct SuspensionDefinition
{
    std::string name;
    // +1 for a left-hand corner (outward is +y), -1 for a right-hand one.
    int side = 1;

    std::vector<PointDef> points;
    std::vector<BodyDef> bodies;
    std::vector<SliderDef> sliders;
    std::vector<BushingDef> bushings;
    std::vector<ElementDef> elements;
    std::vector<LengthAdjust> lengthAdjusts;

    // The body that carries the wheel, its centre point, and the wheel's spin axis in the design
    // position (unit, pointing outward; static camber and toe go here).
    std::string knuckle;
    std::string wheelCenter;
    Vec3 wheelAxis{0.0, 1.0, 0.0};
    double tyreRadius = 0.3;

    // Wheel travel moves the wheel centre along `travelAxis` (chassis frame, unit).
    Vec3 travelAxis{0.0, 0.0, 1.0};
    // Rack travel moves every Rack point along `rackAxis` (chassis frame, unit). An unsteered corner
    // keeps its toe link's inner end as a Rack point that never moves (`steered` false), so the
    // outputs can still name a virtual kingpin from the knuckle's response to it.
    Vec3 rackAxis{0.0, 1.0, 0.0};
    bool steered = true;

    // A point on the vehicle's centre plane (only its y is used), for the roll centre.
    Vec3 vehicleCenter{0.0};

    // The steering axis is the knuckle's screw axis under rack motion with this point's height held
    // (empty: the wheel centre, which is what a K&C rig with its pads at fixed height measures). The
    // presets hold the lower ball joint, which makes it the textbook kingpin: the ball-joint line
    // for a double wishbone, lower ball joint to top mount for a MacPherson.
    std::string steeringAxisReference;
};

// Mirrors a left-hand corner (y -> -y) into a right-hand one.
SuspensionDefinition MirrorToRight(const SuspensionDefinition& left);

// ---- Presets: the same generic description filled from named hardpoints ----

struct DoubleWishboneHardpoints
{
    Vec3 lowerFront;    // lower arm, chassis pivot, front
    Vec3 lowerRear;     // lower arm, chassis pivot, rear
    Vec3 lowerBall;     // lower ball joint on the knuckle
    Vec3 upperFront;    // upper arm, chassis pivot, front
    Vec3 upperRear;     // upper arm, chassis pivot, rear
    Vec3 upperBall;     // upper ball joint on the knuckle
    Vec3 tieInner;      // tie rod (or toe link) on the rack (or chassis)
    Vec3 tieOuter;      // tie rod on the knuckle
    Vec3 wheelCenter;
    Vec3 springLower;   // spring/damper seat on the lower arm
    Vec3 springUpper;   // spring/damper mount on the chassis
    bool steered = true; // false: the tie rod's inner end is a chassis point (rear toe link)
};

struct MacPhersonHardpoints
{
    Vec3 lowerFront;
    Vec3 lowerRear;
    Vec3 lowerBall;
    Vec3 strutTop;      // top mount (ball joint) on the chassis
    Vec3 strutLower;    // a point of the strut axis on the knuckle (the tube)
    Vec3 tieInner;
    Vec3 tieOuter;
    Vec3 wheelCenter;
    bool steered = true;
};

// Five rods with a ball joint at either end between the chassis and the knuckle (a multi-link; a
// double wishbone whose arms are split into single rods). Rods that meet the knuckle at the same
// point share its ball joint. `steerLink` names the tie rod (or toe link): its chassis end is on the
// rack, which moves it when `steered`.
struct FiveLinkHardpoints
{
    std::array<Vec3, 5> chassis{};
    std::array<Vec3, 5> knuckle{};
    int steerLink = 4;
    Vec3 wheelCenter{0.0};
    bool steered = true;
};

SuspensionDefinition MakeDoubleWishbone(const DoubleWishboneHardpoints& hardpoints, double tyreRadius, const Vec3& wheelAxis);
SuspensionDefinition MakeFiveLink(const FiveLinkHardpoints& hardpoints, double tyreRadius, const Vec3& wheelAxis);
SuspensionDefinition MakeMacPherson(const MacPhersonHardpoints& hardpoints, double tyreRadius, const Vec3& wheelAxis);

// ---- The compiled form the solvers work on ----

enum class ConstraintKind
{
    Distance, // |p_a - p_b| = length (one row)
    Slider,   // p_a = p_b + s (p_c - p_b) / length (three rows)
    Travel,   // travelAxis . (p_a - p_a design) = z (one row)
};

struct Constraint
{
    ConstraintKind kind = ConstraintKind::Distance;
    int a = -1;
    int b = -1;
    int c = -1;
    double length = 0.0;
    int slider = -1; // which slide coordinate (Slider only)
    int row = 0;     // first row in Phi
};

struct Bushing
{
    int point = -1;
    Vec3 anchor{0.0};
    Mat3 axes{1.0};
    Curve curves[3];
};

enum class ModelMode
{
    Kinematic,  // ideal joints: as many equations as unknowns
    Compliant,  // bushed chassis points become unknowns held by their bushings
};

struct Model
{
    SuspensionDefinition definition;
    ModelMode mode = ModelMode::Kinematic;

    std::vector<std::string> pointNames;
    std::vector<Vec3> design;
    std::vector<PointRole> roles;
    // Index of each point's x in q, or -1 for a point that is not an unknown.
    std::vector<int> slot;
    // q index of each slide coordinate, and its design value.
    std::vector<int> sliderSlot;
    std::vector<double> sliderDesign;

    std::vector<Constraint> constraints;
    std::vector<Bushing> bushings;

    int unknowns = 0;
    int rows = 0;

    int knuckle = -1;            // body index
    std::vector<int> knucklePoints;
    int wheelCenter = -1;
    int wheelAxisPoint = -1;      // knuckle point one wheelAxisLength outboard of the centre
    int steeringReference = -1;   // point whose height is held for the steering axis
    double wheelAxisLength = 0.1;
    std::vector<int> elementA;
    std::vector<int> elementB;
    std::vector<double> pointMass; // lumped masses, for EffectiveMass()

    int Find(const std::string& name) const;
};

// Turns a description into equations. Throws std::invalid_argument when the description is not a
// determinate mechanism in Kinematic mode (unknowns != equations), names a missing point, or has a
// body whose points are collinear.
Model Compile(const SuspensionDefinition& definition, ModelMode mode = ModelMode::Kinematic);
}
