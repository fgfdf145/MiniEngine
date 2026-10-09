#include "flex_ring_data.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace me::tyre::flexring
{

namespace
{
using D = FlexRingData;

FieldInfo Real(const char* name, const char* unit, const char* group, const char* description, double D::* member, double minValue, double maxValue,
               bool integer = false, bool ftire = true)
{
    FieldInfo f{};
    f.name = name;
    f.unit = unit;
    f.group = group;
    f.description = description;
    f.kind = FieldKind::Real;
    f.real = member;
    f.minValue = minValue;
    f.maxValue = maxValue;
    f.integer = integer;
    f.ftire = ftire;
    return f;
}

FieldInfo Text(const char* name, const char* group, const char* description, std::string D::* member, bool ftire = true)
{
    FieldInfo f{};
    f.name = name;
    f.unit = "";
    f.group = group;
    f.description = description;
    f.kind = FieldKind::Text;
    f.text = member;
    f.ftire = ftire;
    return f;
}

constexpr const char* kGeometry = "Size and geometry";
constexpr const char* kMass = "Mass and pressure";
constexpr const char* kStatic = "Structure: static";
constexpr const char* kDynamic = "Structure: dynamic stiffening and hysteresis";
constexpr const char* kModal = "Structure: modal";
constexpr const char* kBelt = "Belt torsion and lateral bending";
constexpr const char* kTread = "Tread";
constexpr const char* kFriction = "Friction";
constexpr const char* kNumerics = "Numerical settings";
constexpr const char* kMiniEngine = "MiniEngine settings";

std::vector<FieldInfo> BuildFields()
{
    std::vector<FieldInfo> f;
    f.push_back(Text("tire_name", kGeometry, "Name of the tyre", &D::name, false));
    f.push_back(Text("tire_source", kGeometry, "Where the data come from", &D::source, false));
    f.push_back(Real("rolling_circumference", "mm", kGeometry, "2 pi r_belt; 0: outer radius less the tread", &D::rollingCircumference, 0.0, 5000.0));
    f.push_back(Real("unloaded_radius", "mm", kGeometry, "Outer radius, inflated, at rest; 0: from the size", &D::unloadedRadius, 0.0, 1500.0, false, false));
    f.push_back(Real("tire_section_width", "mm", kGeometry, "Nominal section width", &D::tireSectionWidth, 50.0, 1000.0));
    f.push_back(Real("tire_aspect_ratio", "%", kGeometry, "Section height over width", &D::tireAspectRatio, 5.0, 150.0));
    f.push_back(Real("rim_diameter", "inch", kGeometry, "Rim diameter", &D::rimDiameter, 8.0, 30.0));
    f.push_back(Real("rim_width", "inch", kGeometry, "Distance between the rim flanges", &D::rimWidth, 2.0, 20.0));
    f.push_back(Real("belt_width", "mm", kGeometry, "Belt width; 0: 95 % of the tread width", &D::beltWidth, 0.0, 1000.0));
    f.push_back(Real("tread_width", "mm", kGeometry, "Tread width that can touch the road; 0: from the section", &D::treadWidth, 0.0, 1000.0));
    f.push_back(Real("belt_lat_curvature_radius", "mm", kGeometry, "Lateral curvature radius of the outer belt layer", &D::beltLatCurvatureRadius, 50.0, 1.0e6));
    f.push_back(Real("rel_tread_shoulder_width", "%", kGeometry, "Shoulder width over the tread width, each side", &D::relTreadShoulderWidth, 0.0, 50.0));
    f.push_back(Real("rel_min_tread_shoulder_height", "%", kGeometry, "Tread depth at the shoulder's end over the full depth", &D::relMinTreadShoulderHeight, 0.0, 100.0));
    f.push_back(Text("speed_symbol", kGeometry, "Speed symbol (Y, ZR, ... or km/h)", &D::speedSymbol));

    f.push_back(Real("tire_mass", "kg", kMass, "Total tyre mass", &D::tireMass, 0.5, 500.0));
    f.push_back(Real("free_mass_percentage", "%", kMass, "Share of the mass on the belt nodes; 0: from f2", &D::freeMassPercentage, 0.0, 100.0));
    f.push_back(Real("inflation_pressure", "bar", kMass, "Pressure of the measurements", &D::inflationPressure, 0.1, 15.0));

    f.push_back(Real("first_deflection", "mm", kStatic, "First static deflection on a flat road", &D::firstDeflection, 0.5, 200.0));
    f.push_back(Real("stat_wheel_load_at_first_defl", "N", kStatic, "Wheel load at the first deflection", &D::statWheelLoadAtFirstDefl, 1.0, 1.0e6));
    f.push_back(Real("second_deflection", "mm", kStatic, "Second static deflection; 0: none", &D::secondDeflection, 0.0, 300.0));
    f.push_back(Real("stat_wheel_load_at_second_defl", "N", kStatic, "Wheel load at the second deflection", &D::statWheelLoadAtSecondDefl, 0.0, 1.0e6));
    f.push_back(Real("max_radial_progressivity", "%", kStatic, "Bound of the radial element's slope increase", &D::maxRadialProgressivity, 0.0, 1000.0));

    f.push_back(Real("rad_dynamic_stiffening", "%", kDynamic, "Radial stiffness gain at high speed", &D::radDynamicStiffening, 0.0, 200.0));
    f.push_back(Real("tang_dynamic_stiffening", "%", kDynamic, "Tangential stiffness gain at high speed", &D::tangDynamicStiffening, 0.0, 200.0));
    f.push_back(Real("lat_dynamic_stiffening", "%", kDynamic, "Lateral stiffness gain at high speed", &D::latDynamicStiffening, 0.0, 200.0));
    f.push_back(Real("time_const_dynamic_stiffening", "s", kDynamic, "Maxwell elements' damper over spring", &D::timeConstDynamicStiffening, 1.0e-5, 1.0));
    f.push_back(Real("radial_hysteretic_stiffening", "%", kDynamic, "Radial short-term stiffness increase on reversing", &D::radialHystereticStiffening, 0.0, 500.0));
    f.push_back(Real("radial_hysteresis_force", "N", kDynamic, "Radial hysteresis loop width", &D::radialHysteresisForce, 0.0, 1.0e5));
    f.push_back(Real("tang_hysteretic_stiffening", "%", kDynamic, "Tangential short-term stiffness increase", &D::tangHystereticStiffening, 0.0, 500.0));
    f.push_back(Real("tang_hysteresis_force", "N", kDynamic, "Tangential hysteresis loop width", &D::tangHysteresisForce, 0.0, 1.0e5));
    f.push_back(Real("lat_hysteretic_stiffening", "%", kDynamic, "Lateral short-term stiffness increase", &D::latHystereticStiffening, 0.0, 500.0));
    f.push_back(Real("lat_hysteresis_force", "N", kDynamic, "Lateral hysteresis loop width", &D::latHysteresisForce, 0.0, 1.0e5));
    f.push_back(Real("belt_extension_due_to_vmax", "%", kDynamic, "Rolling circumference growth at the maximum speed", &D::beltExtensionDueToVmax, 0.01, 20.0));
    f.push_back(Real("belt_extension_damp", "%", kDynamic, "Damping of the belt extension, of critical", &D::beltExtensionDamp, 0.0, 100.0));

    f.push_back(Real("f1", "Hz", kModal, "In-plane rotation mode, unloaded, fixed rim", &D::f1, 1.0, 1000.0));
    f.push_back(Real("f2", "Hz", kModal, "In-plane translation mode", &D::f2, 1.0, 1000.0));
    f.push_back(Real("f3", "Hz", kModal, "Out-of-plane translation mode; 0: use belt_torsion_stiffn", &D::f3, 0.0, 1000.0, false, false));
    f.push_back(Real("f4", "Hz", kModal, "Out-of-plane rotation (tilt) mode", &D::f4, 1.0, 1000.0));
    f.push_back(Real("D1", "-", kModal, "Modal damping of mode 1", &D::d1, 0.0, 1.0));
    f.push_back(Real("D2", "-", kModal, "Modal damping of mode 2", &D::d2, 0.0, 1.0));
    f.push_back(Real("D4", "-", kModal, "Modal damping of mode 4", &D::d4, 0.0, 1.0));
    f.push_back(Real("f5", "Hz", kModal, "First in-plane bending mode; 0: use the bending stiffness", &D::f5, 0.0, 1000.0));
    f.push_back(Real("belt_in_plane_bend_stiffn", "N m^2", kModal, "In-plane bending stiffness of the uninflated belt", &D::beltInPlaneBendStiffn, 0.0, 1.0e4));
    f.push_back(Real("f6", "Hz", kModal, "First out-of-plane bending mode; 0: use the bending stiffness", &D::f6, 0.0, 1000.0));
    f.push_back(Real("belt_out_of_plane_bend_stiffn", "N m^2", kModal, "Out-of-plane bending stiffness, inflated", &D::beltOutOfPlaneBendStiffn, 0.0, 1.0e5));

    f.push_back(Real("belt_torsion_stiffn", "N/deg", kBelt, "Torsion stiffness to the rim per unit belt length (when f3 is 0)", &D::beltTorsionStiffn, 0.0, 1.0e5));
    f.push_back(Real("belt_twist_stiffn", "N m^2/deg", kBelt, "Twist stiffness between adjacent segments", &D::beltTwistStiffn, 0.0, 1.0e5));
    f.push_back(Real("belt_torsion_twist_damp", "%", kBelt, "Torsion damping, of critical", &D::beltTorsionTwistDamp, 0.0, 100.0));
    f.push_back(Real("belt_lat_bend_stiffn", "N m", kBelt, "Lateral bending stiffness per unit circumferential length", &D::beltLatBendStiffn, 0.01, 1.0e5));
    f.push_back(Real("belt_lat_bend_damp", "s", kBelt, "Lateral bending damping over stiffness", &D::beltLatBendDamp, 0.0, 1.0));
    f.push_back(Real("belt_lat_bend_stiffn_long_coupl", "-", kBelt, "Smoothing of lateral bending between segments", &D::beltLatBendStiffnLongCoupl, 0.0, 100.0));
    f.push_back(Real("belt_torsion_lat_displ_coupl", "deg/mm", kBelt, "Segment torsion per lateral displacement", &D::beltTorsionLatDisplCoupl, -10.0, 10.0));
    f.push_back(Real("belt_torsion_oop_bend_coupl", "-", kBelt, "Rotation of the bending axes with torsion", &D::beltTorsionOopBendCoupl, 0.0, 1.0));

    f.push_back(Real("tread_depth", "mm", kTread, "Groove depth, new", &D::treadDepth, 0.0, 50.0));
    f.push_back(Real("tread_depth_actual", "mm", kTread, "Groove depth now (operating condition); 0: new", &D::treadDepthActual, 0.0, 50.0, false, false));
    f.push_back(Real("tread_base_height", "mm", kTread, "Rubber between belt and groove bottoms", &D::treadBaseHeight, 0.1, 50.0));
    f.push_back(Real("stiffness_tread_rubber", "Shore A", kTread, "Tread rubber hardness", &D::stiffnessTreadRubber, 20.0, 100.0));
    f.push_back(Real("Youngs_mod_tread_rubber", "N/mm^2", kTread, "Tread rubber modulus; 0: from Shore A", &D::youngsModTreadRubber, 0.0, 1000.0));
    f.push_back(Real("stiffn_progr_tread_rubber", "%", kTread, "Radial tread stiffness increase at zero height", &D::stiffnProgrTreadRubber, 0.0, 1000.0));
    f.push_back(Real("tread_positive", "%", kTread, "Share of the footprint in contact", &D::treadPositive, 1.0, 100.0));
    f.push_back(Real("tread_pattern_shape_factor_tang", "-", kTread, "Shear modulus factor, G = f E / 3", &D::treadPatternShapeFactorTang, 0.01, 10.0));
    f.push_back(Real("lat_to_long_tread_stiffn_ratio", "-", kTread, "Lateral over longitudinal tread shear stiffness", &D::latToLongTreadStiffnRatio, 0.05, 20.0));
    f.push_back(Real("damping_tread_rubber", "s", kTread, "Tread rubber damping over elasticity", &D::dampingTreadRubber, 0.0, 0.1));

    f.push_back(Real("max_friction_velocity", "m/s", kFriction, "Sliding speed of maximum friction", &D::maxFrictionVelocity, 1.0e-4, 10.0));
    f.push_back(Real("sliding_velocity", "m/s", kFriction, "Sliding speed of the sliding friction", &D::slidingVelocity, 1.0e-3, 100.0));
    f.push_back(Real("blocking_velocity", "m/s", kFriction, "Sliding speed of the blocking friction", &D::blockingVelocity, 1.0e-2, 500.0));
    f.push_back(Real("low_ground_pressure", "bar", kFriction, "Low ground pressure", &D::lowGroundPressure, 0.0, 100.0));
    f.push_back(Real("med_ground_pressure", "bar", kFriction, "Medium ground pressure", &D::medGroundPressure, 0.0, 100.0));
    f.push_back(Real("high_ground_pressure", "bar", kFriction, "High ground pressure", &D::highGroundPressure, 0.0, 100.0));
    f.push_back(Real("mu_adhesion_at_low_p", "-", kFriction, "Adhesion (at rest) friction, low pressure", &D::muAdhesionAtLowP, 0.0, 5.0));
    f.push_back(Real("mu_max_at_low_p", "-", kFriction, "Maximum friction, low pressure", &D::muMaxAtLowP, 0.0, 5.0));
    f.push_back(Real("mu_sliding_at_low_p", "-", kFriction, "Sliding friction, low pressure", &D::muSlidingAtLowP, 0.0, 5.0));
    f.push_back(Real("mu_blocking_at_low_p", "-", kFriction, "Blocking friction, low pressure", &D::muBlockingAtLowP, 0.0, 5.0));
    f.push_back(Real("mu_adhesion_at_med_p", "-", kFriction, "Adhesion friction, medium pressure", &D::muAdhesionAtMedP, 0.0, 5.0));
    f.push_back(Real("mu_max_at_med_p", "-", kFriction, "Maximum friction, medium pressure", &D::muMaxAtMedP, 0.0, 5.0));
    f.push_back(Real("mu_sliding_at_med_p", "-", kFriction, "Sliding friction, medium pressure", &D::muSlidingAtMedP, 0.0, 5.0));
    f.push_back(Real("mu_blocking_at_med_p", "-", kFriction, "Blocking friction, medium pressure", &D::muBlockingAtMedP, 0.0, 5.0));
    f.push_back(Real("mu_adhesion_at_high_p", "-", kFriction, "Adhesion friction, high pressure", &D::muAdhesionAtHighP, 0.0, 5.0));
    f.push_back(Real("mu_max_at_high_p", "-", kFriction, "Maximum friction, high pressure", &D::muMaxAtHighP, 0.0, 5.0));
    f.push_back(Real("mu_sliding_at_high_p", "-", kFriction, "Sliding friction, high pressure", &D::muSlidingAtHighP, 0.0, 5.0));
    f.push_back(Real("mu_blocking_at_high_p", "-", kFriction, "Blocking friction, high pressure", &D::muBlockingAtHighP, 0.0, 5.0));

    f.push_back(Real("number_belt_segments", "-", kNumerics, "Belt segments (nodes)", &D::numberBeltSegments, 12.0, 400.0, true));
    f.push_back(Real("number_blocks_per_belt_segm", "-", kNumerics, "Contact blocks per segment", &D::numberBlocksPerBeltSegm, 1.0, 400.0, true));
    f.push_back(Real("number_tread_strips", "-", kNumerics, "Lateral strips of blocks", &D::numberTreadStrips, 1.0, 100.0, true));
    f.push_back(Real("tread_discretization_type", "-", kNumerics, "0 herring-bone, 1 pseudo-random strips", &D::treadDiscretizationType, 0.0, 1.0, true));
    f.push_back(Real("number_belt_bend_shape_funct", "-", kNumerics, "Lateral bending shape functions", &D::numberBeltBendShapeFunct, 0.0, 12.0, true));
    f.push_back(Real("maximum_time_step", "s", kNumerics, "Largest internal step", &D::maximumTimeStep, 1.0e-6, 0.01));
    f.push_back(Real("maximum_angle_increment", "deg", kNumerics, "Largest rim rotation per step", &D::maximumAngleIncrement, 0.01, 30.0));
    f.push_back(Real("BDF_parameter", "-", kNumerics, "0 explicit, 0.5 trapezoidal, 1 implicit Euler", &D::bdfParameter, 0.5, 1.0));
    f.push_back(Real("Jacobian_update_cycle_length", "-", kNumerics, "Steps between Jacobian updates", &D::jacobianUpdateCycleLength, 1.0, 100.0, true));
    f.push_back(Real("contact_processor_bound", "%", kNumerics, "Height above the lowest point checked for contact, of the diameter", &D::contactProcessorBound, 1.0, 100.0));
    f.push_back(Real("high_precision_tang_plane", "-", kNumerics, "1: road plane from three heights, 0: belt normal", &D::highPrecisionTangPlane, 0.0, 1.0, true));

    f.push_back(Real("newton_iterations", "-", kMiniEngine, "Newton iterations per step", &D::newtonIterations, 1.0, 20.0, true, false));
    f.push_back(Real("pressure_force_fraction", "-", kMiniEngine, "Share of p times area applied on the belt", &D::pressureForceFraction, 0.0, 1.0, false, false));
    f.push_back(Real("rim_flange_clearance", "mm", kMiniEngine, "Belt deflection reaching the rim flange; 0: 70 % of the section height", &D::rimFlangeClearance, 0.0, 300.0, false, false));
    f.push_back(Real("rim_flange_stiffness_factor", "-", kMiniEngine, "Flange contact stiffness over radial stiffness", &D::rimFlangeStiffnessFactor, 0.0, 1000.0, false, false));
    f.push_back(Real("gravity", "-", kMiniEngine, "1: gravity on the belt nodes", &D::gravity, 0.0, 1.0, true, false));
    return f;
}

std::string Lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string Trim(std::string_view s)
{
    size_t a = 0;
    size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
    {
        ++a;
    }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
    {
        --b;
    }
    return std::string(s.substr(a, b - a));
}

std::string Unquote(const std::string& s)
{
    if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front())
    {
        return s.substr(1, s.size() - 2);
    }
    return s;
}
}

const std::vector<FieldInfo>& Fields()
{
    static const std::vector<FieldInfo> fields = BuildFields();
    return fields;
}

const FieldInfo* FindField(std::string_view name)
{
    const std::string key = Lower(name);
    for (const FieldInfo& f : Fields())
    {
        if (Lower(f.name) == key)
        {
            return &f;
        }
    }
    return nullptr;
}

bool SetField(FlexRingData& data, std::string_view name, double value)
{
    const FieldInfo* f = FindField(name);
    if (f == nullptr || f->kind != FieldKind::Real || !std::isfinite(value))
    {
        return false;
    }
    data.*(f->real) = f->integer ? std::round(value) : value;
    return true;
}

bool GetField(const FlexRingData& data, std::string_view name, double& value)
{
    const FieldInfo* f = FindField(name);
    if (f == nullptr || f->kind != FieldKind::Real)
    {
        return false;
    }
    value = data.*(f->real);
    return true;
}

DataReadResult ReadData(std::istream& in)
{
    DataReadResult result;
    std::string line;
    // The tyre's items live in [FTIRE_DATA], [MODEL] and [MINIENGINE] (or before any section); other
    // sections ([MDI_HEADER], [UNITS], [OPERATING_CONDITIONS], ...) are not this reader's.
    bool ours = true;
    while (std::getline(in, line))
    {
        const size_t comment = line.find_first_of("$!");
        if (comment != std::string::npos)
        {
            line.resize(comment);
        }
        const std::string t = Trim(line);
        if (!t.empty() && t.front() == '[')
        {
            const std::string section = Lower(t);
            ours = section == "[ftire_data]" || section == "[model]" || section == "[miniengine]";
            continue;
        }
        if (t.empty() || t.front() == '(' || !ours)
        {
            continue;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const std::string key = Trim(t.substr(0, eq));
        const std::string value = Unquote(Trim(t.substr(eq + 1)));
        const FieldInfo* f = FindField(key);
        if (f == nullptr)
        {
            result.unknownKeys.push_back(key);
            continue;
        }
        if (f->kind == FieldKind::Text)
        {
            result.data.*(f->text) = value;
            continue;
        }
        try
        {
            size_t used = 0;
            const double v = std::stod(value, &used);
            SetField(result.data, f->name, v);
        }
        catch (const std::exception&)
        {
            result.unknownKeys.push_back(key + " (not a number)");
        }
    }
    return result;
}

DataReadResult ReadDataFile(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in)
    {
        throw std::runtime_error("Cannot open " + path.string());
    }
    return ReadData(in);
}

void WriteData(const FlexRingData& data, std::ostream& out)
{
    out << "[MDI_HEADER]\n"
           "FILE_TYPE = 'tir'\n"
           "FILE_VERSION = 3.0\n"
           "FILE_FORMAT = 'ASCII'\n"
           "$ MiniEngine flexible ring tyre (FTire item names and units, docs/design/2026-10-09-flex-ring-tyre-design.md)\n"
           "[UNITS]\n"
           "LENGTH = 'mm'\n"
           "FORCE = 'N'\n"
           "ANGLE = 'deg'\n"
           "MASS = 'kg'\n"
           "TIME = 's'\n"
           "PRESSURE = 'bar'\n"
           "[FTIRE_DATA]\n";
    out << std::setprecision(10);
    bool inMiniEngine = false;
    std::string group;
    for (const FieldInfo& f : Fields())
    {
        if (!f.ftire && !inMiniEngine && std::string(f.group) == kMiniEngine)
        {
            out << "[MINIENGINE]\n";
            inMiniEngine = true;
        }
        if (group != f.group)
        {
            group = f.group;
            out << "$ ---- " << group << "\n";
        }
        out << f.name << " = ";
        if (f.kind == FieldKind::Text)
        {
            out << "'" << data.*(f.text) << "'";
        }
        else
        {
            out << data.*(f.real);
        }
        out << "    $ [" << f.unit << "] " << f.description << "\n";
    }
}

void WriteDataFile(const FlexRingData& data, const std::filesystem::path& path)
{
    std::ofstream out(path);
    if (!out)
    {
        throw std::runtime_error("Cannot write " + path.string());
    }
    WriteData(data, out);
}

double MaxSpeedKmh(std::string_view speedSymbol)
{
    const std::string s = Lower(Trim(speedSymbol));
    struct Entry
    {
        const char* symbol;
        double kmh;
    };
    static constexpr Entry kTable[] = {{"l", 120}, {"m", 130}, {"n", 140}, {"p", 150}, {"q", 160}, {"r", 170}, {"s", 180}, {"t", 190}, {"u", 200},
                                       {"h", 210}, {"v", 240}, {"zr", 240}, {"w", 270}, {"y", 300}, {"(y)", 300}};
    for (const Entry& e : kTable)
    {
        if (s == e.symbol)
        {
            return e.kmh;
        }
    }
    try
    {
        const double v = std::stod(s);
        if (v > 10.0)
        {
            return v;
        }
    }
    catch (const std::exception&)
    {
    }
    return 300.0;
}

FlexRingData MakeDataFromCar(const CarTyreFigures& c)
{
    FlexRingData d;
    d.name = c.name;
    d.source = c.source;
    const double sectionHeight = std::max(c.radius - c.rimRadius, 0.01);
    d.unloadedRadius = c.radius * 1000.0;
    d.rollingCircumference = 0.0;
    d.tireSectionWidth = c.width * 1000.0;
    d.tireAspectRatio = sectionHeight / c.width * 100.0;
    d.rimDiameter = 2.0 * c.rimRadius / 0.0254;
    d.rimWidth = std::round(c.width / 0.0254 * 0.9 * 2.0) / 2.0;
    // The tread: the section width less a shoulder each side of 0.15 of the section height, as the brush
    // tyre's (tyre_brush.h, MakeBrushTyreParameters).
    d.treadWidth = (c.width - 2.0 * 0.15 * sectionHeight) * 1000.0;
    d.beltWidth = 0.0;
    d.inflationPressure = c.pressurePsi * 0.0689476;
    // The game's tyre is a linear spring: its rate at two deflections.
    d.firstDeflection = 10.0;
    d.statWheelLoadAtFirstDefl = c.verticalRate * 0.010;
    d.secondDeflection = 20.0;
    d.statWheelLoadAtSecondDefl = c.verticalRate * 0.020;

    // Friction: the peak of the data at medium ground pressure, the sliding share of its fall-off, the
    // load sensitivity spread over ground pressure (assumed, docs/design/2026-10-09-flex-ring-tyre-design.md).
    const double peak = 0.5 * (c.longitudinalFriction + c.lateralFriction);
    const double sliding = peak * std::clamp(c.falloffLevel, 0.3, 1.0);
    const double lowFactor = 1.0 + 0.6 * (1.0 - std::clamp(c.loadExponent, 0.5, 1.0));
    const double highFactor = 1.0 - 1.4 * (1.0 - std::clamp(c.loadExponent, 0.5, 1.0));
    d.muAdhesionAtMedP = peak;
    d.muMaxAtMedP = peak * 1.04;
    d.muSlidingAtMedP = sliding;
    d.muBlockingAtMedP = sliding * 0.76;
    d.muAdhesionAtLowP = d.muAdhesionAtMedP * lowFactor;
    d.muMaxAtLowP = d.muMaxAtMedP * lowFactor;
    d.muSlidingAtLowP = d.muSlidingAtMedP * lowFactor;
    d.muBlockingAtLowP = d.muBlockingAtMedP * lowFactor;
    d.muAdhesionAtHighP = d.muAdhesionAtMedP * highFactor;
    d.muMaxAtHighP = d.muMaxAtMedP * highFactor;
    d.muSlidingAtHighP = d.muSlidingAtMedP * highFactor;
    d.muBlockingAtHighP = d.muBlockingAtMedP * highFactor;
    return d;
}

FlexRingData MakeDefaultData()
{
    CarTyreFigures r34;
    r34.name = "R34 Semislicks (flexible ring)";
    r34.source = "Assetto Corsa ks_nissan_skyline_r34 compound 1 Semislicks; structure literature-typical";
    r34.radius = 0.3266;
    r34.width = 0.245;
    r34.rimRadius = 0.254;
    r34.verticalRate = 287098.0;
    r34.pressurePsi = 28.0;
    r34.referenceLoad = 2986.0;
    r34.longitudinalFriction = 1.30;
    r34.lateralFriction = 1.28;
    r34.falloffLevel = 0.86;
    r34.loadExponent = 0.83;
    FlexRingData d = MakeDataFromCar(r34);
    d.tireMass = 10.5;
    d.freeMassPercentage = 70.0;
    d.speedSymbol = "ZR";
    return d;
}

}
