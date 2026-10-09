#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace me::tyre::flexring
{

// A flexible ring tyre's basic data, in the names and units of FTire's data files (M. Gipser, "FTire -
// Flexible Structure Tire Model, Modelization and Parameter Specification", rev. 2022-1-r26108, chapter 6;
// docs/design/2026-10-09-flex-ring-tyre-design.md). Pre-processing (flex_ring_preprocess.h) turns it into
// the model's own stiffnesses, masses and dampers, fitted so that the model shows these global properties.
//
// Units are the data file's: mm, inch, kg, bar, N, Hz, %, deg, s. A value of 0 where the comment says so
// means "not given": pre-processing then derives it or uses its default.
struct FlexRingData
{
    std::string name;
    std::string source;

    // 6.2 Size, geometry and tread pattern.
    double rollingCircumference = 0.0; // mm, 2 pi r_belt; 0: from the outer radius less the tread
    double unloadedRadius = 0.0;       // mm, outer radius of the inflated tyre at rest (MiniEngine: the data's RADIUS); 0: from the size
    double tireSectionWidth = 245.0;   // mm
    double tireAspectRatio = 40.0;     // %
    double rimDiameter = 18.0;         // inch
    double rimWidth = 9.0;             // inch
    double beltWidth = 0.0;            // mm; 0: 95 % of the tread width
    double treadWidth = 0.0;           // mm; 0: the section width less the shoulders
    double beltLatCurvatureRadius = 1000.0; // mm, lateral curvature of the outer belt layer, inflated and unloaded
    double relTreadShoulderWidth = 0.0;     // %, of the tread width, each side
    double relMinTreadShoulderHeight = 100.0; // %, tread depth at the shoulder's outer end over the full depth
    std::string speedSymbol = "Y";            // maximum speed (Y = 300 km/h, ZR = 240, or a number in km/h)

    // 6.3 Mass and inflation pressure.
    double tireMass = 10.0;           // kg
    double freeMassPercentage = 0.0;  // %, of the tyre's mass on the belt nodes; 0: from f2
    double inflationPressure = 2.0;   // bar, at which the data below were measured

    // 6.4 Structural stiffness, damping and hysteresis.
    double firstDeflection = 10.0;          // mm, vertical, from first touch, flat road, no camber
    double statWheelLoadAtFirstDefl = 2900.0; // N
    double secondDeflection = 20.0;         // mm; 0: none (slightly progressive)
    double statWheelLoadAtSecondDefl = 0.0; // N
    double maxRadialProgressivity = 85.0;   // %, bound of the belt-rim radial element's slope increase
    double radDynamicStiffening = 10.0;     // %, radial stiffness gain at high speed (Maxwell elements)
    double tangDynamicStiffening = 10.0;    // %
    double latDynamicStiffening = 10.0;     // %
    double timeConstDynamicStiffening = 0.003; // s, damper over spring of the Maxwell elements
    double radialHystereticStiffening = 0.0;   // %, short-term stiffness increase on reversing
    double radialHysteresisForce = 0.0;        // N, loop width at 30 deg of the tread in contact
    double tangHystereticStiffening = 0.0;     // %
    double tangHysteresisForce = 0.0;          // N
    double latHystereticStiffening = 0.0;      // %
    double latHysteresisForce = 0.0;           // N
    double beltExtensionDueToVmax = 1.5;       // %, rolling circumference growth at the maximum speed
    double beltExtensionDamp = 2.0;            // %, of critical, of the belt's extension
    double f1 = 72.8;  // Hz, in-plane rotation mode of the unloaded tyre on a fixed rim
    double f2 = 84.8;  // Hz, in-plane translation mode
    double f3 = 0.0;   // Hz, out-of-plane translation mode (MiniEngine; 0: not used, the torsion stiffness below is)
    double f4 = 54.3;  // Hz, out-of-plane rotation (tilt) mode
    double d1 = 0.034; // modal damping, 0..1
    double d2 = 0.028;
    double d4 = 0.044;
    double f5 = 108.0;                 // Hz, first in-plane bending mode; 0: the bending stiffness below
    double beltInPlaneBendStiffn = 0.0; // N m^2, EI of the uninflated belt about the lateral axis
    double f6 = 112.0;                 // Hz, first out-of-plane bending mode; 0: the bending stiffness below
    double beltOutOfPlaneBendStiffn = 0.0; // N m^2, EI about the radial axis, inflated
    double beltTorsionStiffn = 100.0;  // N/deg, torque about the circumferential axis per unit belt length; used when f3 is 0
    double beltTwistStiffn = 5.0;      // N m^2/deg, twist torque between adjacent segments times unit length
    double beltTorsionTwistDamp = 5.0; // %, of critical, of the belt segments' torsion
    double beltLatBendStiffn = 40.0;   // N m, lateral bending stiffness of the belt per unit circumferential length
    double beltLatBendDamp = 0.001;    // s, lateral bending damping over stiffness
    double beltLatBendStiffnLongCoupl = 1.0; // smoothing of the lateral bending between adjacent segments
    double beltTorsionLatDisplCoupl = 0.0;   // deg/mm, torsion of a segment per lateral displacement
    double beltTorsionOopBendCoupl = 1.0;    // 0..1, rotation of the bending axes with the belt's torsion (FTire recommends 1)

    // 6.5 Tread thickness, stiffness, damping and friction.
    double treadDepth = 5.0;          // mm, groove depth of the new tyre
    double treadDepthActual = 0.0;    // mm, operating condition; 0: the new tyre's
    double treadBaseHeight = 3.0;     // mm, rubber between the belt and the grooves' bottom
    double stiffnessTreadRubber = 60.0; // Shore A
    double youngsModTreadRubber = 0.0;  // N/mm^2; 0: from the Shore A hardness
    double stiffnProgrTreadRubber = 0.0; // %, radial tread stiffness increase as the block's height reaches 0
    double treadPositive = 80.0;        // %, of the footprint in road contact
    double treadPatternShapeFactorTang = 1.0; // G = 1/3 f E
    double latToLongTreadStiffnRatio = 1.0;
    double dampingTreadRubber = 0.0002; // s, damping over elasticity modulus
    double maxFrictionVelocity = 0.05;  // m/s
    double slidingVelocity = 2.0;       // m/s
    double blockingVelocity = 20.0;     // m/s
    double lowGroundPressure = 0.01;    // bar
    double medGroundPressure = 2.0;     // bar
    double highGroundPressure = 10.0;   // bar
    double muAdhesionAtLowP = 1.45;
    double muMaxAtLowP = 1.50;
    double muSlidingAtLowP = 1.25;
    double muBlockingAtLowP = 0.95;
    double muAdhesionAtMedP = 1.30;
    double muMaxAtMedP = 1.35;
    double muSlidingAtMedP = 1.12;
    double muBlockingAtMedP = 0.85;
    double muAdhesionAtHighP = 1.05;
    double muMaxAtHighP = 1.10;
    double muSlidingAtHighP = 0.92;
    double muBlockingAtHighP = 0.70;

    // 6.12 Numerical settings.
    double numberBeltSegments = 80;
    double numberBlocksPerBeltSegm = 50;
    double numberTreadStrips = 10;
    double treadDiscretizationType = 1;  // 0 herring-bone, 1 pseudo-random along equally spaced strips
    double numberBeltBendShapeFunct = 4;
    double maximumTimeStep = 0.0005;     // s
    double maximumAngleIncrement = 1.0;  // deg of rim rotation per step
    double bdfParameter = 0.55;          // 0.5 trapezoidal (FTire's recommendation), 1 implicit Euler; 0.55 damps the stiffest modes
    double jacobianUpdateCycleLength = 1;
    double contactProcessorBound = 35.0; // %, of the diameter above the lowest point checked for contact
    double highPrecisionTangPlane = 1;   // 1: the road's tangent plane from three heights, 0: the belt normal

    // MiniEngine's own settings.
    double newtonIterations = 1;       // per step; more converge the implicit step
    double pressureForceFraction = 0.5; // share of p * area applied on the belt nodes (FTire: 0.5)
    double rimFlangeClearance = 0.0;   // mm, belt deflection where it reaches the rim flange; 0: 70 % of the section height
    double rimFlangeStiffnessFactor = 20.0; // the flange contact over the radial stiffness
    double gravity = 1;
};

enum class FieldKind
{
    Real,
    Text,
};

// One basic data item: its name in data files (FTire's where FTire has it), unit, group and meaning.
struct FieldInfo
{
    const char* name;
    const char* unit;
    const char* group;
    const char* description;
    FieldKind kind = FieldKind::Real;
    double FlexRingData::* real = nullptr;
    std::string FlexRingData::* text = nullptr;
    double minValue = 0.0;
    double maxValue = 0.0;
    bool integer = false;
    // FTire's own item (false: MiniEngine's extension).
    bool ftire = true;
};

// Every basic data item, in the order of the data file.
const std::vector<FieldInfo>& Fields();
const FieldInfo* FindField(std::string_view name);

// Sets or reads an item by name (case-insensitive); false when no such real item.
bool SetField(FlexRingData& data, std::string_view name, double value);
bool GetField(const FlexRingData& data, std::string_view name, double& value);

// A data file in TeimOrbit syntax (".tir": "[SECTION]" headers, "key = value" lines, comments after '$'
// or '!'), basic data in [FTIRE_DATA] (and numerical settings in [MODEL]). Unknown keys are listed.
struct DataReadResult
{
    FlexRingData data;
    std::vector<std::string> unknownKeys;
};
DataReadResult ReadData(std::istream& in);
DataReadResult ReadDataFile(const std::filesystem::path& path);
void WriteData(const FlexRingData& data, std::ostream& out);
void WriteDataFile(const FlexRingData& data, const std::filesystem::path& path);

// The maximum speed of a speed symbol, km/h (300 for an unknown one).
double MaxSpeedKmh(std::string_view speedSymbol);

// What a car's tyre data says (Assetto Corsa's tyres.ini through engine/tyre/tyre_spec.h).
struct CarTyreFigures
{
    std::string name;
    std::string source;
    double radius = 0.3266;       // m, unloaded outer radius
    double width = 0.245;         // m, section width
    double rimRadius = 0.254;     // m
    double verticalRate = 287098; // N/m
    double pressurePsi = 28.0;    // inflation pressure the rate holds at
    double referenceLoad = 2986;  // N
    double longitudinalFriction = 1.30; // peak, at the reference load
    double lateralFriction = 1.28;
    double falloffLevel = 0.86;   // share of the peak left sliding
    double loadExponent = 0.83;   // peak force grows as load^this
};

// Basic data for a car's tyre: its size, rate, pressure and grip from the data, the rest literature-typical
// values for a passenger car tyre (modal data: Massaro et al. 2023; bending modes: Gipser, FETire/modal) as
// the design document lists.
FlexRingData MakeDataFromCar(const CarTyreFigures& figures);

// The Nissan Skyline R34's Semislicks (Assetto Corsa ks_nissan_skyline_r34, compound 1).
FlexRingData MakeDefaultData();

}
