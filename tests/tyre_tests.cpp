#include <engine/tyre/tyre_brush.h>
#include <engine/tyre/tyre_magic_formula.h>
#include <engine/tyre/tyre_tir_file.h>

#include "test_fixture_paths.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace me::tyre;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void RequireNear(double actual, double expected, double tolerance, const std::string& what)
{
    if (!(std::abs(actual - expected) <= tolerance))
    {
        throw std::runtime_error(what + ": got " + std::to_string(actual) + ", expected " + std::to_string(expected) + " (tolerance " + std::to_string(tolerance) + ")");
    }
}

// Pacejka (2006), Appendix 3, Table A3.1: 205/60R15 91V at 2.2 bar.
const MagicFormulaParameters& PacejkaTyre()
{
    static const MagicFormulaParameters parameters = ReadTirFile(MINIENGINE_TYRE_FIXTURES "/pacejka2006_205_60R15.tir").parameters;
    return parameters;
}

MagicFormulaOutput At(double kappa, double alpha, double gamma, double load)
{
    MagicFormulaInput in;
    in.kappa = kappa;
    in.alpha = alpha;
    in.gamma = gamma;
    in.load = load;
    return EvaluateMagicFormula(PacejkaTyre(), in);
}

// ---- The .tir reader ----

void TestTirFileReadsTheTable()
{
    const TirReadResult read = ReadTirFile(MINIENGINE_TYRE_FIXTURES "/pacejka2006_205_60R15.tir");
    const MagicFormulaParameters& p = read.parameters;
    RequireNear(p.nominalLoad, 4000.0, 0.0, "FNOMIN");
    RequireNear(p.unloadedRadius, 0.313, 0.0, "UNLOADED_RADIUS");
    RequireNear(p.referenceVelocity, 16.67, 0.0, "LONGVL");
    RequireNear(p.pKy1, -14.95, 0.0, "PKY1");
    RequireNear(p.pDy3, -11.23, 0.0, "PDY3");
    RequireNear(p.rVy4, 35.44, 0.0, "RVY4");
    RequireNear(p.sSz4, -0.238, 0.0, "SSZ4");
    RequireNear(p.lambda.muV, 0.0, 0.0, "LMUV");
    RequireNear(p.lambda.Kygamma, 1.0, 0.0, "LKYC");
    // Header and unit lines are not coefficients.
    Require(std::find(read.ignoredKeys.begin(), read.ignoredKeys.end(), "FILE_TYPE") != read.ignoredKeys.end(), "FILE_TYPE ignored");
    Require(std::find(read.ignoredKeys.begin(), read.ignoredKeys.end(), "MASS") != read.ignoredKeys.end(), "MASS ignored");
}

void TestTirReaderAcceptsMf52NamesAndRejectsBadInput()
{
    std::istringstream mf52("[VERTICAL]\nfnomin = 3000 $ lower case\nUNLOADED_RADIUS=0.3\nLONGVL = 1.65E+1\n[SCALING_COEFFICIENTS]\nLGAY = 0.5 ! MF 5.2 name\nLGAZ = 0.25\n");
    const MagicFormulaParameters p = ReadTir(mf52).parameters;
    RequireNear(p.nominalLoad, 3000.0, 0.0, "lower-case key");
    RequireNear(p.referenceVelocity, 16.5, 1e-12, "exponent");
    RequireNear(p.lambda.Kygamma, 0.5, 0.0, "LGAY");
    RequireNear(p.lambda.Kzgamma, 0.25, 0.0, "LGAZ");

    std::istringstream missing("UNLOADED_RADIUS = 0.3\nLONGVL = 16\n");
    bool threw = false;
    try
    {
        ReadTir(missing);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    Require(threw, "missing FNOMIN throws");

    std::istringstream garbage("FNOMIN = 4000\nUNLOADED_RADIUS = 0.3\nLONGVL = 16\nPCX1 = abc\n");
    threw = false;
    try
    {
        ReadTir(garbage);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    Require(threw, "a non-numeric coefficient throws");
}

// ---- Against the independent Python transcription ----

struct ReferencePoint
{
    double kappa, alpha, gamma, load, speed;
    double Fx, Fy, Mz, My;
};

// clang-format off
const ReferencePoint kReference[] = {
    // tools/tyre_reference/reference_points.py
    // {kappa, alpha, gamma, Fz, Vcx, Fx, Fy, Mz, My}
    {-0.5, 0.0, 0.0, 2000.0, 16.67, -1986.1194085661953, -88.25461910604399, -27.07868352989113, -2.9024340323450466},
    {-0.1, 0.0, 0.0, 2000.0, 16.67, -2315.7695764992172, -172.57498320459445, -32.969584382954665, -4.5874225371835315},
    {-0.02, 0.0, 0.0, 2000.0, 16.67, -844.6736590862312, -53.3367769489806, -16.071376501981717, -4.853362330043911},
    {0.0, 0.0, 0.0, 2000.0, 16.67, -114.53345404288142, 20.907364401362287, -6.844920804743436, -4.916592502868027},
    {0.02, 0.0, 0.0, 2000.0, 16.67, 635.4227365044682, 94.75549551368744, 3.8556858457999166, -4.978570675708834},
    {0.05, 0.0, 0.0, 2000.0, 16.67, 1543.8019246602173, 172.27695673672477, 17.587996260837457, -5.069245164289244},
    {0.1, 0.0, 0.0, 2000.0, 16.67, 2275.6199883833046, 206.6903505970036, 28.836133069566205, -5.2144627293819426},
    {0.3, 0.0, 0.0, 2000.0, 16.67, 2277.7768724961525, 134.56732921005928, 30.07576110672682, -5.728530385464036},
    {1.0, 0.0, 0.0, 2000.0, 16.67, 1649.0433511737676, 63.13089476404954, 22.018009117376895, -6.930750973391007},
    {0.0, -0.2617993877991494, 0.0, 2000.0, 16.67, -21.05363784744802, 2238.9944928922255, -1.5698088558476873, -4.916592502868027},
    {0.0, -0.10471975511965978, 0.0, 2000.0, 16.67, -65.8087042868421, 1969.3428055291329, -19.819270731012516, -4.916592502868027},
    {0.0, -0.03490658503988659, 0.0, 2000.0, 16.67, -107.56268163539993, 918.2960786494125, -19.327105511780164, -4.916592502868027},
    {0.0, -0.008726646259971648, 0.0, 2000.0, 16.67, -115.01198284444207, 252.9361154592326, -10.407338554114643, -4.916592502868027},
    {0.0, 0.008726646259971648, 0.0, 2000.0, 16.67, -112.52805437941703, -209.6934420968626, -3.21510609484087, -4.916592502868027},
    {0.0, 0.03490658503988659, 0.0, 2000.0, 16.67, -99.74079869264452, -854.9944968558626, 6.319249122413891, -4.916592502868027},
    {0.0, 0.10471975511965978, 0.0, 2000.0, 16.67, -59.00611273085508, -1800.3150677461022, 8.769436857911519, -4.916592502868027},
    {0.0, 0.2617993877991494, 0.0, 2000.0, 16.67, -19.26322747633676, -2010.572625420888, -2.431330362333054, -4.916592502868027},
    {-0.5, 0.0, 0.0, 4000.0, 16.67, -3828.796542677463, -93.56090983925327, -52.020859398997025, -5.804868064690093},
    {-0.1, 0.0, 0.0, 4000.0, 16.67, -4681.078533444123, -174.51678517797131, -65.85290785141677, -9.174845074367063},
    {-0.02, 0.0, 0.0, 4000.0, 16.67, -1804.5329191183978, -39.95269557088275, -33.670276593878725, -9.706724660087822},
    {0.0, 0.0, 0.0, 4000.0, 16.67, -172.00946110146887, 41.99718332234983, -13.573589310153654, -9.833185005736054},
    {0.02, 0.0, 0.0, 4000.0, 16.67, 1499.3634882257213, 123.15158583490218, 10.826061475391116, -9.957141351417668},
    {0.05, 0.0, 0.0, 4000.0, 16.67, 3377.6156498002474, 207.63171150297202, 39.83838954622067, -10.138490328578488},
    {0.1, 0.0, 0.0, 4000.0, 16.67, 4642.134416186746, 243.04523816628617, 59.66146294492366, -10.428925458763885},
    {0.3, 0.0, 0.0, 4000.0, 16.67, 4391.0760898265735, 155.45659261045594, 58.280297067555466, -11.457060770928072},
    {1.0, 0.0, 0.0, 4000.0, 16.67, 3193.9197411244863, 70.38087199204708, 42.728813087217645, -13.861501946782013},
    {0.0, -0.2617993877991494, 0.0, 4000.0, 16.67, -31.618926807264177, 4139.999982840628, -3.3848171078081473, -9.833185005736054},
    {0.0, -0.10471975511965978, 0.0, 4000.0, 16.67, -98.83330468604852, 3594.7597836135246, -67.00529522323804, -9.833185005736054},
    {0.0, -0.03490658503988659, 0.0, 4000.0, 16.67, -161.5405652204195, 1608.3945254373825, -58.29878463331622, -9.833185005736054},
    {0.0, -0.008726646259971648, 0.0, 4000.0, 16.67, -172.72812869047883, 443.271264759261, -25.921342371828743, -9.833185005736054},
    {0.0, 0.008726646259971648, 0.0, 4000.0, 16.67, -168.9976972610417, -358.0418163986284, -1.1391241457143053, -9.833185005736054},
    {0.0, 0.03490658503988659, 0.0, 4000.0, 16.67, -149.7934483537755, -1505.522106253814, 31.681607135511516, -9.833185005736054},
    {0.0, 0.10471975511965978, 0.0, 4000.0, 16.67, -88.61698738891505, -3346.4349571737075, 39.682185638630685, -9.833185005736054},
    {0.0, 0.2617993877991494, 0.0, 4000.0, 16.67, -28.93003974226721, -3778.877274254787, -6.908894758700384, -9.833185005736054},
    {-0.5, 0.0, 0.0, 6000.0, 16.67, -5528.306614213382, -32.58004133177967, -74.88992056246501, -8.70730209703514},
    {-0.1, 0.0, 0.0, 6000.0, 16.67, -7039.029489400439, -42.643411617590075, -97.81155342549754, -13.762267611550595},
    {-0.02, 0.0, 0.0, 6000.0, 16.67, -2881.740919765317, 21.745716094432417, -51.90718407848796, -14.560086990131733},
    {0.0, 0.0, 0.0, 6000.0, 16.67, -145.3064785226258, 57.25003523675764, -19.54212646998917, -14.74977750860408},
    {0.02, 0.0, 0.0, 6000.0, 16.67, 2633.559222940932, 91.66997092235434, 22.386345222333684, -14.935712027126504},
    {0.05, 0.0, 0.0, 6000.0, 16.67, 5471.977235168212, 126.02622309233735, 66.94139002014296, -15.207735492867734},
    {0.1, 0.0, 0.0, 6000.0, 16.67, 7020.21664117894, 136.06054195674923, 91.46498105208536, -15.643388188145826},
    {0.3, 0.0, 0.0, 6000.0, 16.67, 6336.258084037392, 81.64060876908262, 84.47780333816699, -17.185591156392107},
    {1.0, 0.0, 0.0, 6000.0, 16.67, 4633.571663073798, 31.93227469202708, 62.08756453718743, -20.79225292017302},
    {0.0, -0.2617993877991494, 0.0, 6000.0, 16.67, -26.71036162550349, 5701.696040483438, -10.158126570658238, -14.74977750860408},
    {0.0, -0.10471975511965978, 0.0, 6000.0, 16.67, -83.4902881081162, 4751.3902844544045, -136.99200734264997, -14.74977750860408},
    {0.0, -0.03490658503988659, 0.0, 6000.0, 16.67, -136.4627882700417, 1993.5195983549186, -103.12691544013306, -14.74977750860408},
    {0.0, -0.008726646259971648, 0.0, 6000.0, 16.67, -145.9135791781286, 548.4075953193924, -42.12424824997855, -14.74977750860408},
    {0.0, 0.008726646259971648, 0.0, 6000.0, 16.67, -142.76226499511472, -433.4194395143064, 3.218955786957041, -14.74977750860408},
    {0.0, 0.03490658503988659, 0.0, 6000.0, 16.67, -126.53930979533814, -1873.2020800243831, 65.55821020473176, -14.74977750860408},
    {0.0, 0.10471975511965978, 0.0, 6000.0, 16.67, -74.8599657967141, -4517.504927082664, 94.8866042973026, -14.74977750860408},
    {0.0, 0.2617993877991494, 0.0, 6000.0, 16.67, -24.438901043871496, -5306.947264719752, -8.62405016796792, -14.74977750860408},
    {0.05, 0.03490658503988659, 0.0, 4000.0, 16.67, 3026.296411329449, -1291.3060220469745, 57.64499334529745, -10.138490328578488},
    {0.1, 0.06981317007977318, 0.0, 4000.0, 16.67, 3767.8366949677534, -2098.612895409405, 53.23000800903095, -10.428925458763885},
    {-0.1, 0.06981317007977318, 0.0, 4000.0, 16.67, -3799.446092908455, -2385.4748885373065, -46.947824740532766, -9.174845074367063},
    {-0.3, -0.13962634015954636, 0.0, 4000.0, 16.67, -3852.5413913017182, 1712.9103114386708, -44.10867792328077, -7.646289074152891},
    {0.2, -0.20943951023931956, 0.0, 4000.0, 16.67, 2970.4907661809034, 2684.4585867385267, 49.3506641569585, -10.968246793489381},
    {0.0, 0.0, -0.06981317007977318, 4000.0, 16.67, -172.00946110146887, 298.65793759288755, 6.173948306561534, -9.833185005736054},
    {0.0, 0.05235987755982989, -0.06981317007977318, 4000.0, 16.67, -133.11859464099214, -1919.8713985999857, 67.29300752884483, -9.833185005736054},
    {0.08, -0.08726646259971647, -0.06981317007977318, 4000.0, 16.67, 3315.0498549075983, 3139.372764160111, -22.982694318171696, -10.314486232801327},
    {0.0, 0.0, 0.05235987755982989, 4000.0, 16.67, -172.00946110146887, -150.57847736471706, -28.758735693699897, -9.833185005736054},
    {0.0, 0.05235987755982989, 0.05235987755982989, 4000.0, 16.67, -133.11859464099214, -2385.7276336237837, 34.297090488732955, -9.833185005736054},
    {0.08, -0.08726646259971647, 0.05235987755982989, 4000.0, 16.67, 3315.0498549075983, 2781.7924192571986, 57.98289918067938, -10.314486232801327},
    {0.0, 0.0, 0.10471975511965978, 4000.0, 16.67, -172.00946110146887, -342.6937330970982, -42.57278271182229, -9.833185005736054},
    {0.0, 0.05235987755982989, 0.10471975511965978, 4000.0, 16.67, -133.11859464099214, -2633.25951874689, 29.027792918223906, -9.833185005736054},
    {0.08, -0.08726646259971647, 0.10471975511965978, 4000.0, 16.67, 3315.0498549075983, 2748.5158209249466, 88.94956689072322, -10.314486232801327},
    {0.05, 0.05235987755982989, 0.03490658503988659, 3000.0, 5.0, 1972.055061942976, -1599.510437194192, 54.7749560845233, -2.864922070969424},
    {-0.05, 0.05235987755982989, 0.0, 4000.0, -10.0, -3127.823450632934, 2133.220746708667, -81.8397795419221, 7.037448894700784},
    // at Fz0: Kxk 86040.000 N, Kya -46009.139 N/rad (-803.01 N/deg), mux 1.2100, muy -0.9900, trail 31.03 mm
};
// clang-format on

void TestMatchesThePythonTranscription()
{
    for (const ReferencePoint& r : kReference)
    {
        MagicFormulaInput in;
        in.kappa = r.kappa;
        in.alpha = r.alpha;
        in.gamma = r.gamma;
        in.load = r.load;
        in.forwardSpeed = r.speed;
        const MagicFormulaOutput out = EvaluateMagicFormula(PacejkaTyre(), in);
        const std::string where = "kappa " + std::to_string(r.kappa) + ", alpha " + std::to_string(r.alpha / kDeg) + " deg, gamma " + std::to_string(r.gamma / kDeg) + " deg, Fz " + std::to_string(r.load) + ", V " + std::to_string(r.speed);
        RequireNear(out.Fx, r.Fx, 1e-9 * (1.0 + std::abs(r.Fx)), "Fx at " + where);
        RequireNear(out.Fy, r.Fy, 1e-9 * (1.0 + std::abs(r.Fy)), "Fy at " + where);
        RequireNear(out.Mz, r.Mz, 1e-9 * (1.0 + std::abs(r.Mz)), "Mz at " + where);
        RequireNear(out.My, r.My, 1e-9 * (1.0 + std::abs(r.My)), "My at " + where);
    }
}

// ---- Closed forms and the book's own definitions ----

// Kxk and Kya are the slopes of Fx0 and Fy0 where the shifted slip is zero (4.E15, 4.E25).
void TestSlipStiffnessesAreTheCurvesSlopes()
{
    const MagicFormulaParameters& p = PacejkaTyre();
    for (double load : {2000.0, 4000.0, 6000.0})
    {
        const double dfz = (load - p.nominalLoad) / p.nominalLoad;
        const double SHx = p.pHx1 + p.pHx2 * dfz;
        const double h = 1e-6;
        const MagicFormulaOutput centre = At(-SHx, 0.0, 0.0, load);
        const double slopeX = (At(-SHx + h, 0.0, 0.0, load).Fx0 - At(-SHx - h, 0.0, 0.0, load).Fx0) / (2.0 * h);
        RequireNear(slopeX, centre.Kxk, 1e-4 * centre.Kxk, "dFx0/dkappa at Fz " + std::to_string(load));
        RequireNear(centre.Kxk, load * (p.pKx1 + p.pKx2 * dfz) * std::exp(p.pKx3 * dfz), 1e-9, "Kxk closed form");

        // At zero camber SHy = pHy1 + pHy2 dfz; slope with respect to tan(alpha).
        const double SHy = p.pHy1 + p.pHy2 * dfz;
        const double alpha0 = std::atan(-SHy);
        const double slopeY = (At(0.0, std::atan(-SHy + h), 0.0, load).Fy0 - At(0.0, std::atan(-SHy - h), 0.0, load).Fy0) / (2.0 * h);
        const double Kya = At(0.0, alpha0, 0.0, load).Kya;
        RequireNear(slopeY, Kya, 1e-4 * std::abs(Kya), "dFy0/dalpha at Fz " + std::to_string(load));
        const double closed = p.pKy1 * p.nominalLoad * std::sin(p.pKy4 * std::atan(load / (p.pKy2 * p.nominalLoad)));
        RequireNear(Kya, closed, 1e-9, "Kya closed form");
    }
    // The table's tyre at its rated load: 86.0 kN per unit slip, 803 N/deg.
    RequireNear(At(0.0, 0.0, 0.0, 4000.0).Kxk, 86040.0, 1e-9, "Kxk at Fz0 = pKx1 Fz0");
    RequireNear(At(0.0, 0.0, 0.0, 4000.0).Kya * kDeg, -803.0, 0.1, "Kya at Fz0, N/deg");
}

// With C > 1 the sine reaches one, so the peaks are D + S_V: mu_x Fz and mu_y Fz + S_Vy.
void TestPeaksAreTheDFactors()
{
    for (double load : {2000.0, 4000.0, 6000.0})
    {
        double peakX = 0.0;
        double peakY = 0.0;
        for (int i = 0; i <= 4000; ++i)
        {
            peakX = std::max(peakX, At(i * 2.5e-4, 0.0, 0.0, load).Fx0);
            peakY = std::min(peakY, At(0.0, i * 0.01 * kDeg, 0.0, load).Fy0);
        }
        const MagicFormulaOutput o = At(0.0, 0.0, 0.0, load);
        RequireNear(peakX, o.muX * load, 1e-3 * load, "peak Fx at Fz " + std::to_string(load));
        // S_Vy at zero camber: Fz (pVy1 + pVy2 dfz).
        const double dfz = (load - 4000.0) / 4000.0;
        RequireNear(peakY, o.muY * load + load * (0.045 - 0.024 * dfz), 1e-3 * load, "peak Fy at Fz " + std::to_string(load));
    }
    RequireNear(At(0.0, 0.0, 0.0, 4000.0).muX, 1.21, 1e-12, "mu_x at Fz0 = pDx1");
    RequireNear(At(0.0, 0.0, 0.0, 4000.0).muY, -0.99, 1e-12, "mu_y at Fz0 = pDy1");
}

// ISO signs: driving slip pushes forward, a positive slip angle pushes to the right (-y) and the
// aligning torque turns the wheel back (+z); the pneumatic trail Dt0 at Fz0 is R0 qDz1.
void TestIsoSignsAndTrail()
{
    Require(At(0.1, 0.0, 0.0, 4000.0).Fx > 0.0, "driving slip gives +Fx");
    Require(At(-0.1, 0.0, 0.0, 4000.0).Fx < 0.0, "braking slip gives -Fx");
    Require(At(0.0, 3.0 * kDeg, 0.0, 4000.0).Fy < 0.0, "positive alpha gives -Fy");
    Require(At(0.0, -3.0 * kDeg, 0.0, 4000.0).Fy > 0.0, "negative alpha gives +Fy");
    Require(At(0.0, 3.0 * kDeg, 0.0, 4000.0).Mz > 0.0, "positive alpha gives +Mz");
    Require(At(0.0, 0.0, 0.0, 4000.0).My < 0.0, "rolling resistance opposes forward rolling");
    // At zero slip the trail's cosine factors are 1 and cos'alpha = V0 / (V0 + 0.1).
    const double trail = At(0.0, std::atan(-0.007), 0.0, 4000.0).trail;
    RequireNear(trail, 0.313 * 0.1 * 16.67 / (16.67 + 0.1), 1e-3 * 0.0313, "trail at the trail curve's zero, Fz0");
}

// Combined slip weakens each force (the G functions stay at or below one near the tyre's range)
// and the aligning torque collapses past the side force's peak.
void TestCombinedSlipAndAligningTorque()
{
    const double pureFy = std::abs(At(0.0, 4.0 * kDeg, 0.0, 4000.0).Fy);
    double previous = pureFy;
    for (double kappa : {0.05, 0.1, 0.2, 0.4})
    {
        const double fy = std::abs(At(kappa, 4.0 * kDeg, 0.0, 4000.0).Fy);
        Require(fy < previous, "|Fy| falls as braking/driving slip grows at kappa " + std::to_string(kappa));
        previous = fy;
    }
    const double pureFx = At(0.1, 0.0, 0.0, 4000.0).Fx;
    Require(At(0.1, 6.0 * kDeg, 0.0, 4000.0).Fx < pureFx, "slip angle weakens Fx");

    double peakMz = 0.0;
    double peakAlpha = 0.0;
    for (int i = 1; i <= 200; ++i)
    {
        const double mz = At(0.0, i * 0.1 * kDeg, 0.0, 4000.0).Mz;
        if (mz > peakMz)
        {
            peakMz = mz;
            peakAlpha = i * 0.1;
        }
    }
    Require(peakAlpha > 2.0 && peakAlpha < 6.0, "Mz peaks between 2 and 6 deg, got " + std::to_string(peakAlpha));
    Require(peakMz > 30.0 && peakMz < 80.0, "Mz peak 30..80 Nm at Fz0, got " + std::to_string(peakMz));
    Require(std::abs(At(0.0, 12.0 * kDeg, 0.0, 4000.0).Mz) < 0.2 * peakMz, "Mz collapses past the Fy peak");
}

// Load sensitivity: |mu_y| falls with load (pDy2 against pDy1), so Fy grows less than in proportion.
void TestLoadSensitivity()
{
    const double light = std::abs(At(0.0, 0.0, 0.0, 2000.0).muY);
    const double heavy = std::abs(At(0.0, 0.0, 0.0, 6000.0).muY);
    Require(heavy < light, "|mu_y| falls with load");
    const double fy2 = std::abs(At(0.0, 10.0 * kDeg, 0.0, 2000.0).Fy);
    const double fy6 = std::abs(At(0.0, 10.0 * kDeg, 0.0, 6000.0).Fy);
    Require(fy6 < 3.0 * fy2, "tripling the load gives less than three times the side force");
}

void TestZeroLoadGivesNothing()
{
    for (double load : {0.0, -100.0})
    {
        const MagicFormulaOutput o = At(0.2, 5.0 * kDeg, 2.0 * kDeg, load);
        Require(o.Fx == 0.0 && o.Fy == 0.0 && o.Mz == 0.0 && o.Mx == 0.0 && o.My == 0.0, "no load, no force");
    }
}

// One evaluation per wheel per step at 1000 Hz must be cheap; report the cost.
void ReportCost()
{
    const auto start = std::chrono::steady_clock::now();
    double sink = 0.0;
    int count = 0;
    for (int i = 0; i < 200; ++i)
    {
        for (int j = 0; j < 200; ++j)
        {
            sink += At(-0.5 + i * 0.005, (-15.0 + j * 0.15) * kDeg, 0.0, 4000.0).Fy;
            ++count;
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "  Magic Formula: " << seconds / count * 1e9 << " ns per evaluation (checksum " << sink << ")\n";
}

// ---- The brush tyre with a flexible carcass ----

// A wheel rolling at 20 m/s on the brush tyre at a load, with sideways velocity for a slip angle
// (alpha = atan(V_y / V_x)) and the wheel's spin for a theoretical longitudinal slip (V_r - V_x) / V_r.
BrushTyreInput Rolling(const BrushTyreParameters& p, double load, double alpha, double slip = 0.0, double speed = 20.0)
{
    BrushTyreInput in;
    in.load = load;
    in.forwardVelocity = speed;
    in.lateralVelocity = std::abs(speed) * std::tan(alpha);
    const double radius = p.unloadedRadius - load / p.verticalRate / 3.0;
    in.wheelSpeed = speed / (1.0 - slip) / radius;
    return in;
}

// A tyre's figures with only its grip, peak, size and rate given.
BrushTyreFigures Figures(double mu, double load, double peakSlipAngle, double radius, double width, double verticalRate)
{
    BrushTyreFigures f;
    f.peakFriction = mu;
    f.referenceLoad = load;
    f.peakSlipAngle = peakSlipAngle;
    f.kineticShare = 0.85;
    f.radius = radius;
    f.sectionWidth = width;
    f.verticalRate = verticalRate;
    return f;
}

// The distance rolled at 20 m/s until a step in slip has built 63% of its steady force, along the wheel
// (slip ratio) or across it (slip angle).
double RelaxationDistance(const BrushTyreParameters& p, double load, bool lateral)
{
    BrushTyre tyre(p);
    const BrushTyreInput in = lateral ? Rolling(p, load, 1.0 * kDeg) : Rolling(p, load, 0.0, 0.01);
    const BrushTyreOutput steady = tyre.Steady(in);
    const double target = 0.632 * std::abs(lateral ? steady.Fy : steady.Fx);
    constexpr double kStep = 1e-4;
    for (int step = 1; step <= 5000; ++step)
    {
        const BrushTyreOutput o = tyre.Step(in, kStep);
        if (std::abs(lateral ? o.Fy : o.Fx) >= target)
        {
            return step * kStep * 20.0;
        }
    }
    return -1.0;
}

// The patch from the inflation pressure and the tread less its shoulders: Pacejka (2006, Table 9.1)
// measures a 205/60R15 at 4 kN and 2.2 bar with a 107 mm patch (a = 0.0535 m); its vertical stiffness
// is q_Fz1 Fz0 / R0 of Table A3.1.
void TestBrushPatchFollowsTheInflationPressure()
{
    BrushTyreFigures f = Figures(1.0, 4000.0, 6.0 * kDeg, 0.313, 0.205, 13.37 * 4000.0 / 0.313);
    f.rimRadius = 0.313 - 0.205 * 0.6;
    f.inflationPressure = 2.2e5;
    const BrushTyreParameters p = MakeBrushTyreParameters(f);
    RequireNear(p.width, 0.205 - 2.0 * 0.15 * 0.123, 1e-9, "the tread is the section less its shoulders");
    const BrushTyreOutput o = BrushTyre(p).Steady(Rolling(p, 4000.0, 0.0));
    std::cout << "  brush: 205/60R15 at 4 kN, 2.2 bar: contact length " << o.contactLength * 1000.0 << " mm (measured 107)\n";
    RequireNear(o.contactLength, 0.107, 0.005, "contact length against Pacejka's measured patch");
    RequireNear(o.contactLength * p.width, 4000.0 / 2.2e5, 1e-6, "mean contact pressure is the inflation pressure");
    // More load lengthens the patch, more pressure shortens it.
    Require(BrushTyre(p).Steady(Rolling(p, 6000.0, 0.0)).contactLength > o.contactLength, "a longer patch under more load");
    f.inflationPressure = 3.0e5;
    const BrushTyreParameters firmer = MakeBrushTyreParameters(f);
    Require(BrushTyre(firmer).Steady(Rolling(firmer, 4000.0, 0.0)).contactLength < o.contactLength, "a shorter patch at more pressure");
}

// The carcass is stiffened to the data's relaxation length both ways, rolling; the tread's fore-aft
// stiffness is the data's multiple of its sideways one.
void TestBrushRelaxesOverTheGivenLength()
{
    for (double length : {0.0757, 0.2})
    {
        BrushTyreFigures f = Figures(1.2, 4200.0, 8.0 * kDeg, 0.3266, 0.245, 325000.0);
        f.rimRadius = 0.254;
        f.inflationPressure = 1.93e5;
        f.relaxationLength = length;
        f.longitudinalStiffnessRatio = 1.04;
        const BrushTyreParameters p = MakeBrushTyreParameters(f);
        const auto stiffness = BrushTyre(p).BristleSlipStiffness(4200.0);
        RequireNear(stiffness[0] / stiffness[1], 1.04, 1e-9, "fore-aft over sideways stiffness");
        const double lateral = RelaxationDistance(p, 4200.0, true);
        const double longitudinal = RelaxationDistance(p, 4200.0, false);
        std::cout << "  brush: relaxation length " << length << " m asked, " << lateral << " m sideways, " << longitudinal << " m fore and aft\n";
        RequireNear(lateral, length, 0.03 * length, "sideways relaxation length");
        RequireNear(longitudinal, length, 0.03 * length, "fore-aft relaxation length");
    }
}

// One rib, a parabolic pressure (lambda = 12), one friction coefficient and a carcass too stiff to move:
// the classic brush, whose force Pacejka (2006, 3.2.1-3.2.2) gives in closed form,
//   F = 3 mu Fz theta s (1 - |theta s| + (theta s)^2 / 3), theta = 2 c a^2 / (3 mu Fz),
// up to full sliding at |theta s| = 1, with c the bristles' stiffness per unit length and a half the patch.
void TestRigidBrushMatchesTheClosedForm()
{
    BrushTyreParameters p;
    p.ribs = 1;
    p.segmentsPerRib = 400;
    p.pressureConvexity = 12.0;
    p.pressureShift = 0.0;
    p.kineticShare = 1.0;
    p.loadExponent = {1.0, 1.0};
    p.lowSpeed = 1e-4;
    p.carcassStiffness = {1e12, 1e12, 1e12};
    p.camberSpinShare = 0.0;
    const BrushTyre tyre(p);
    const double load = 4000.0;
    const double deflection = load / p.verticalRate;
    const double a = std::sqrt((2.0 * (p.unloadedRadius - p.transitionRadius) - deflection) * deflection);
    const double theta = 2.0 * p.bristleStiffnessY * p.width * a * a / (3.0 * p.staticFriction[1] * load);
    const auto closed = [&](double s)
    {
        const double ts = std::min(std::abs(theta * s), 1.0);
        const double f = std::abs(theta * s) >= 1.0 ? 1.0 : 3.0 * ts * (1.0 - ts + ts * ts / 3.0);
        return p.staticFriction[1] * load * f;
    };
    for (double degrees : {0.5, 1.0, 2.0, 3.0, 5.0, 10.0})
    {
        const double s = std::tan(degrees * kDeg);
        const BrushTyreOutput o = tyre.Steady(Rolling(p, load, degrees * kDeg));
        RequireNear(o.Fy, -closed(s), 3e-3 * closed(s) + 1.0, "rigid brush Fy at " + std::to_string(degrees) + " deg");
    }
    for (double slip : {0.01, 0.03, 0.08, 0.2})
    {
        const BrushTyreOutput o = tyre.Steady(Rolling(p, load, 0.0, slip));
        RequireNear(o.Fx, closed(slip) * p.bristleStiffnessX / p.bristleStiffnessY, 3e-3 * closed(slip) + 1.0, "rigid brush Fx at slip " + std::to_string(slip));
    }
}

// The road's force never passes the friction ellipse, whatever the slips (local friction is at most mu_s
// each way), on a tyre with one friction and on one gripping more along the wheel than across it.
void TestBrushStaysInsideTheFrictionEllipse()
{
    BrushTyreFigures figures = Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0);
    for (double along : {0.0, 1.3})
    {
        figures.longitudinalPeakFriction = along;
        const BrushTyreParameters p = MakeBrushTyreParameters(figures);
        const BrushTyre tyre(p);
        for (double slip : {-0.6, -0.15, -0.04, 0.0, 0.03, 0.1, 0.5})
        {
            for (double degrees : {-20.0, -6.0, -1.0, 0.0, 2.0, 8.0, 25.0})
            {
                for (double camber : {-3.0, 0.0, 4.0})
                {
                    BrushTyreInput in = Rolling(p, 4000.0, degrees * kDeg, slip);
                    in.camber = camber * kDeg;
                    const BrushTyreOutput o = tyre.Steady(in);
                    Require(o.converged, "steady balance converges at slip " + std::to_string(slip) + ", " + std::to_string(degrees) + " deg");
                    Require(std::hypot(o.Fx / o.peakFrictionX, o.Fy / o.peakFrictionY) <= 4000.0 * 1.001,
                            "inside the friction ellipse at slip " + std::to_string(slip) + ", " + std::to_string(degrees) + " deg");
                }
            }
        }
    }
}

// The fitted tyre peaks where it was asked to, and with the sign conventions of the ISO axes: a slip
// angle to the left pushes right with an aligning moment, a lean to the right pushes right, driving slip
// pushes forward; the flexible carcass softens the bristles' cornering stiffness.
void TestFittedBrushPeaksWhereAskedAndPointsTheRightWay()
{
    for (double peak : {4.0, 7.0, 10.0})
    {
        const BrushTyreParameters p = MakeBrushTyreParameters(Figures(1.2, 3500.0, peak * kDeg, 0.33, 0.25, 0.0));
        const BrushTyre tyre(p);
        double best = 0.0;
        double bestAngle = 0.0;
        for (double degrees = 0.25; degrees <= 20.0; degrees += 0.25)
        {
            const double fy = std::abs(tyre.Steady(Rolling(p, 3500.0, degrees * kDeg)).Fy);
            if (fy > best)
            {
                best = fy;
                bestAngle = degrees;
            }
        }
        RequireNear(bestAngle, peak, 0.6, "lateral peak angle");
        Require(best > 0.85 * 1.2 * 3500.0 && best <= 1.2 * 3500.0, "the peak is near mu Fz, got " + std::to_string(best));
    }
    const BrushTyreParameters p = MakeBrushTyreParameters(Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0));
    const BrushTyre tyre(p);
    const BrushTyreOutput left = tyre.Steady(Rolling(p, 4000.0, 2.0 * kDeg));
    Require(left.Fy < 0.0 && left.Mz > 0.0, "a slip angle to the left pushes right and aligns");
    const BrushTyreOutput right = tyre.Steady(Rolling(p, 4000.0, -2.0 * kDeg));
    RequireNear(right.Fy, -left.Fy, 1e-6 * std::abs(left.Fy) + 1e-3, "symmetric in slip angle (no pressure shift)");
    RequireNear(right.Mz, -left.Mz, 1e-6 * std::abs(left.Mz) + 1e-3, "aligning moment symmetric");
    BrushTyreInput leaning = Rolling(p, 4000.0, 0.0);
    leaning.camber = 3.0 * kDeg;
    Require(tyre.Steady(leaning).Fy < 0.0, "a lean to the right pushes right (camber thrust)");
    Require(tyre.Steady(Rolling(p, 4000.0, 0.0, 0.05)).Fx > 0.0, "driving slip pushes forward");
    Require(tyre.Steady(Rolling(p, 4000.0, 0.0, -0.05)).Fx < 0.0, "braking slip pushes back");

    // The carcass's twist and bend add to the slip: the cornering stiffness falls below the bristles'.
    const double small = 0.2 * kDeg;
    const double flexible = std::abs(tyre.Steady(Rolling(p, 4000.0, small)).Fy) / std::tan(small);
    const double bristles = tyre.BristleSlipStiffness(4000.0)[1];
    Require(flexible < 0.97 * bristles && flexible > 0.5 * bristles,
            "carcass softens the cornering stiffness: " + std::to_string(flexible) + " against " + std::to_string(bristles));

    // Rolling backwards it pushes the same way against the same sideways motion, its moment turned round.
    const BrushTyreOutput forward = tyre.Steady(Rolling(p, 4000.0, 0.0, 0.0, 5.0));
    BrushTyreInput sideways = Rolling(p, 4000.0, 0.0, 0.0, 5.0);
    sideways.lateralVelocity = 0.2;
    BrushTyreInput backwards = Rolling(p, 4000.0, 0.0, 0.0, -5.0);
    backwards.lateralVelocity = 0.2;
    const BrushTyreOutput ahead = tyre.Steady(sideways);
    const BrushTyreOutput behind = tyre.Steady(backwards);
    Require(std::abs(forward.Fy) < 1e-6, "no side force rolling straight");
    RequireNear(behind.Fy, ahead.Fy, 1e-3 * std::abs(ahead.Fy), "same side force rolling backwards");
    Require(behind.Mz * ahead.Mz < 0.0, "the aligning moment turns round rolling backwards");
}

// A step in slip angle: the force builds over the carcass's relaxation length, not at once.
void TestBrushForceBuildsOverItsRelaxationLength()
{
    const BrushTyreParameters p = MakeBrushTyreParameters(Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0));
    BrushTyre tyre(p);
    const BrushTyreInput in = Rolling(p, 4000.0, 2.0 * kDeg);
    const double steady = tyre.Steady(in).Fy;
    double previous = 0.0;
    double distanceTo63 = -1.0;
    int evaluations = 0;
    for (int step = 1; step <= 200; ++step)
    {
        const BrushTyreOutput o = tyre.Step(in, 1e-3);
        evaluations += o.evaluations;
        Require(o.converged, "each step's balance converges");
        Require(std::abs(o.Fy) >= std::abs(previous) - 1e-5 * std::abs(steady), "the force builds without overshoot");
        if (step == 1)
        {
            Require(std::abs(o.Fy) < 0.2 * std::abs(steady), "not all at once: first millisecond " + std::to_string(o.Fy / steady));
        }
        if (distanceTo63 < 0.0 && std::abs(o.Fy) >= 0.632 * std::abs(steady))
        {
            distanceTo63 = step * 1e-3 * 20.0;
        }
        previous = o.Fy;
    }
    RequireNear(previous, steady, 1e-3 * std::abs(steady), "settles on the steady force");
    std::cout << "  brush: 63% of a slip-angle step after " << distanceTo63 << " m, " << evaluations / 200.0 << " evaluations per step\n";
    Require(distanceTo63 > 0.02 && distanceTo63 < 0.6, "a relaxation length of centimetres to decimetres, got " + std::to_string(distanceTo63) + " m");
}

// Standing still the tyre makes no force and does not drift. Pushed a few millimetres it holds like a
// spring within friction, and held there it keeps holding: the bristles keep their bend, so nothing
// creeps. Pushed on past friction it slides and, stopped, holds what friction leaves.
void TestBrushStandsAndHolds()
{
    const BrushTyreParameters p = MakeBrushTyreParameters(Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0));
    BrushTyre tyre(p);
    BrushTyreInput still;
    still.load = 4000.0;
    for (int step = 0; step < 2000; ++step)
    {
        const BrushTyreOutput o = tyre.Step(still, 1e-3);
        Require(o.Fx == 0.0 && o.Fy == 0.0, "no force standing still");
    }
    BrushTyreInput pushed = still;
    pushed.forwardVelocity = 0.01;
    pushed.lateralVelocity = 0.005;
    double most = 0.0;
    for (int step = 0; step < 500; ++step)
    {
        const BrushTyreOutput o = tyre.Step(pushed, 1e-3);
        Require(std::isfinite(o.Fx) && std::isfinite(o.Fy), "finite");
        most = std::max(most, std::hypot(o.Fx, o.Fy));
        Require(std::hypot(o.Fx / o.peakFrictionX, o.Fy / o.peakFrictionY) <= 4000.0 * 1.001, "within friction while pushed");
    }
    Require(most > 500.0, "a 5 mm push is held by a spring's force, got " + std::to_string(most) + " N");
    const BrushTyreOutput held = tyre.Step(still, 1e-3);
    Require(held.Fx < 0.0 && held.Fy < 0.0, "the force resists the push");
    BrushTyreOutput later = held;
    for (int step = 0; step < 3000; ++step)
    {
        later = tyre.Step(still, 1e-3);
    }
    std::cout << "  brush: held after a 5 mm push " << std::hypot(held.Fx, held.Fy) << " N, 3 s later " << std::hypot(later.Fx, later.Fy) << " N\n";
    RequireNear(std::hypot(later.Fx, later.Fy), std::hypot(held.Fx, held.Fy), 0.05 * std::hypot(held.Fx, held.Fy), "held still, the force stays: no creep");

    // Dragged 10 cm sideways the tread slides at friction; stopped, it holds no more than friction.
    BrushTyreInput dragged = still;
    dragged.lateralVelocity = 0.2;
    BrushTyreOutput sliding;
    for (int step = 0; step < 500; ++step)
    {
        sliding = tyre.Step(dragged, 1e-3);
    }
    Require(sliding.slidingShare > 0.9, "dragged, the tread slides: " + std::to_string(sliding.slidingShare));
    RequireNear(-sliding.Fy, sliding.peakFrictionY * 4000.0, 0.1 * sliding.peakFrictionY * 4000.0, "at about friction");
    BrushTyreOutput stopped;
    for (int step = 0; step < 1000; ++step)
    {
        stopped = tyre.Step(still, 1e-3);
    }
    Require(std::hypot(stopped.Fx / stopped.peakFrictionX, stopped.Fy / stopped.peakFrictionY) <= 4000.0 * 1.001, "stopped, within friction");
    Require(-stopped.Fy > 0.5 * stopped.peakFrictionY * 4000.0, "and still holding what the slide bent: " + std::to_string(stopped.Fy));
}

// Friction along the wheel apart from across it: without any fall from sliding, the tread sliding
// throughout pushes with each friction along its own axis, and on the ellipse between them.
void TestBrushGripsEachWayItsOwn()
{
    BrushTyreFigures figures = Figures(1.0, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0);
    figures.longitudinalPeakFriction = 1.3;
    figures.kineticShare = 1.0;
    figures.longitudinalStiffnessRatio = 1.0;
    BrushTyreParameters p = MakeBrushTyreParameters(figures);
    p.loadExponent = {1.0, 1.0};
    const BrushTyre tyre(p);
    const BrushTyreOutput along = tyre.Steady(Rolling(p, 4000.0, 0.0, 0.5));
    const BrushTyreOutput across = tyre.Steady(Rolling(p, 4000.0, 25.0 * kDeg));
    BrushTyreInput both = Rolling(p, 4000.0, 0.0, 0.3);
    both.lateralVelocity = 0.5 * (both.wheelSpeed * (p.unloadedRadius - 4000.0 / p.verticalRate / 3.0) - both.forwardVelocity);
    const BrushTyreOutput combined = tyre.Steady(both);
    std::cout << "  brush: mu 1.3 along, 1.0 across: sliding Fx/Fz " << along.Fx / 4000.0 << ", Fy/Fz " << -across.Fy / 4000.0 << ", combined on the ellipse at "
              << std::hypot(combined.Fx / 1.3, combined.Fy / 1.0) / 4000.0 << "\n";
    RequireNear(along.Fx, 1.3 * 4000.0, 0.01 * 1.3 * 4000.0, "along the wheel, its own friction");
    RequireNear(-across.Fy, 1.0 * 4000.0, 0.01 * 4000.0, "across it, its own");
    RequireNear(std::hypot(combined.Fx / 1.3, combined.Fy / 1.0), 4000.0, 0.01 * 4000.0, "between them, the ellipse");
    Require(combined.Fx > 0.0 && combined.Fy < 0.0, "pushing against the sliding");
}

// Without the data's stiffness ratio the tread's fore-aft stiffness puts the longitudinal peak at the
// slip ratio asked for; with it, the ratio stands. (At 20 m/s the sliding friction's fall holds the
// peak below about 0.2 however soft the tread.)
void TestBrushPeaksAtTheSlipRatioAskedFor()
{
    for (double asked : {0.06, 0.1, 0.15})
    {
        BrushTyreFigures figures = Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0);
        figures.peakSlipRatio = asked;
        const BrushTyreParameters p = MakeBrushTyreParameters(figures);
        const BrushTyre tyre(p);
        const double radius = p.unloadedRadius - 4000.0 / p.verticalRate / 3.0;
        double best = 0.0;
        double bestRatio = 0.0;
        for (double ratio = 0.002; ratio <= 0.8; ratio += 0.002)
        {
            BrushTyreInput in = Rolling(p, 4000.0, 0.0);
            in.wheelSpeed = 20.0 * (1.0 + ratio) / radius;
            const double fx = tyre.Steady(in).Fx;
            if (fx > best)
            {
                best = fx;
                bestRatio = ratio;
            }
        }
        std::cout << "  brush: longitudinal peak asked at " << asked << ", got " << bestRatio << " (fore-aft over sideways stiffness "
                  << p.bristleStiffnessX / p.bristleStiffnessY << ")\n";
        RequireNear(bestRatio, asked, 0.04 * asked + 0.004, "the longitudinal peak's slip ratio");
    }
    BrushTyreFigures figures = Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0);
    figures.peakSlipRatio = 0.08;
    figures.longitudinalStiffnessRatio = 1.04;
    const BrushTyreParameters p = MakeBrushTyreParameters(figures);
    RequireNear(p.bristleStiffnessX / p.bristleStiffnessY, 1.04, 1e-9, "the data's stiffness ratio stands");
}

// A tyre that loses no grip sliding has no peak to put at the angle asked for: it is fitted to reach its
// limit there instead, and its bristles stay those of a tyre that does fall.
void TestBrushWithoutFalloffReachesItsLimitWhereAsked()
{
    BrushTyreFigures figures = Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0);
    const BrushTyreParameters falls = MakeBrushTyreParameters(figures);
    figures.kineticShare = 1.0;
    const BrushTyreParameters p = MakeBrushTyreParameters(figures);
    const BrushTyre tyre(p);
    double limit = 0.0;
    for (double degrees = 0.1; degrees <= 25.0; degrees += 0.1)
    {
        limit = std::max(limit, std::abs(tyre.Steady(Rolling(p, 4000.0, degrees * kDeg)).Fy));
    }
    double reached = 0.0;
    for (double degrees = 0.1; degrees <= 25.0; degrees += 0.1)
    {
        if (std::abs(tyre.Steady(Rolling(p, 4000.0, degrees * kDeg)).Fy) >= 0.999 * limit)
        {
            reached = degrees;
            break;
        }
    }
    std::cout << "  brush without fall: at its limit from " << reached << " deg (asked 7), bristles " << p.bristleStiffnessY / falls.bristleStiffnessY
              << " times a falling tyre's\n";
    RequireNear(reached, 7.0, 0.6, "reaches its limit at the angle asked for");
    Require(p.bristleStiffnessY < 2.0 * falls.bristleStiffnessY && p.bristleStiffnessY > 0.5 * falls.bristleStiffnessY,
            "bristles near a falling tyre's, not inflated");
}
}

// The finest cut the tyre takes (kBrushMaxRibs x kBrushMaxSegments) steps to its own steady state and to
// about the default cut's forces; report what a step costs at each cut.
void TestFinelyCutBrushAgreesWithTheDefault()
{
    const BrushTyreParameters coarse = MakeBrushTyreParameters(Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0));
    BrushTyreParameters fine = coarse;
    fine.ribs = kBrushMaxRibs;
    fine.segmentsPerRib = kBrushMaxSegments;
    const double limit = 1.1 * 4000.0;
    for (const auto& [degrees, slip] : {std::pair{2.0, 0.0}, std::pair{6.0, 0.03}, std::pair{12.0, -0.05}})
    {
        const BrushTyreInput in = Rolling(coarse, 4000.0, degrees * kDeg, slip);
        BrushTyre stepped(fine);
        BrushTyreOutput o;
        for (int step = 0; step < 300; ++step)
        {
            o = stepped.Step(in, 1e-3);
        }
        const std::string where = std::to_string(degrees) + " deg, slip " + std::to_string(slip);
        Require(o.converged && o.ribCount == kBrushMaxRibs, "finest cut converges at " + where);
        const BrushTyreOutput steady = BrushTyre(fine).Steady(in);
        RequireNear(o.Fx, steady.Fx, 2e-3 * limit, "finest cut stepped Fx = steady at " + where);
        RequireNear(o.Fy, steady.Fy, 2e-3 * limit, "finest cut stepped Fy = steady at " + where);
        const BrushTyreOutput usual = BrushTyre(coarse).Steady(in);
        RequireNear(steady.Fx, usual.Fx, 0.02 * limit, "finest cut Fx near the default's at " + where);
        RequireNear(steady.Fy, usual.Fy, 0.02 * limit, "finest cut Fy near the default's at " + where);
        std::cout << "  brush " << where << ": Fy default " << usual.Fy << " N, finest " << steady.Fy << " N; Fx " << usual.Fx << " / " << steady.Fx << " N\n";
    }
    for (const auto& [ribs, segments] : {std::pair{10, 20}, std::pair{32, 32}, std::pair{100, 20}, std::pair{64, 64}, std::pair{128, 128}})
    {
        BrushTyreParameters p = coarse;
        p.ribs = ribs;
        p.segmentsPerRib = segments;
        BrushTyre tyre(p);
        const BrushTyreInput in = Rolling(p, 4000.0, 4.0 * kDeg, 0.02);
        constexpr int kSteps = 200;
        const auto start = std::chrono::steady_clock::now();
        for (int step = 0; step < kSteps; ++step)
        {
            tyre.Step(in, 1e-3);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "  brush " << ribs << " x " << segments << ": " << seconds / kSteps * 1e6 << " us per step\n";
    }
}

int main()
{
    const struct
    {
        const char* name;
        void (*run)();
    } tests[] = {
        {"TestTirFileReadsTheTable", TestTirFileReadsTheTable},
        {"TestTirReaderAcceptsMf52NamesAndRejectsBadInput", TestTirReaderAcceptsMf52NamesAndRejectsBadInput},
        {"TestMatchesThePythonTranscription", TestMatchesThePythonTranscription},
        {"TestSlipStiffnessesAreTheCurvesSlopes", TestSlipStiffnessesAreTheCurvesSlopes},
        {"TestPeaksAreTheDFactors", TestPeaksAreTheDFactors},
        {"TestIsoSignsAndTrail", TestIsoSignsAndTrail},
        {"TestCombinedSlipAndAligningTorque", TestCombinedSlipAndAligningTorque},
        {"TestLoadSensitivity", TestLoadSensitivity},
        {"TestZeroLoadGivesNothing", TestZeroLoadGivesNothing},
        {"ReportCost", ReportCost},
        {"TestRigidBrushMatchesTheClosedForm", TestRigidBrushMatchesTheClosedForm},
        {"TestBrushStaysInsideTheFrictionEllipse", TestBrushStaysInsideTheFrictionEllipse},
        {"TestFittedBrushPeaksWhereAskedAndPointsTheRightWay", TestFittedBrushPeaksWhereAskedAndPointsTheRightWay},
        {"TestBrushPatchFollowsTheInflationPressure", TestBrushPatchFollowsTheInflationPressure},
        {"TestBrushRelaxesOverTheGivenLength", TestBrushRelaxesOverTheGivenLength},
        {"TestBrushForceBuildsOverItsRelaxationLength", TestBrushForceBuildsOverItsRelaxationLength},
        {"TestBrushStandsAndHolds", TestBrushStandsAndHolds},
        {"TestBrushGripsEachWayItsOwn", TestBrushGripsEachWayItsOwn},
        {"TestBrushPeaksAtTheSlipRatioAskedFor", TestBrushPeaksAtTheSlipRatioAskedFor},
        {"TestBrushWithoutFalloffReachesItsLimitWhereAsked", TestBrushWithoutFalloffReachesItsLimitWhereAsked},
        {"TestFinelyCutBrushAgreesWithTheDefault", TestFinelyCutBrushAgreesWithTheDefault},
    };
    int failures = 0;
    for (const auto& test : tests)
    {
        try
        {
            test.run();
            std::cout << "[pass] " << test.name << "\n";
        }
        catch (const std::exception& e)
        {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << e.what() << "\n";
        }
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
