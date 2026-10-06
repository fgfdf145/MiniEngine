#include "tyre_brush.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace me::tyre
{

namespace
{
constexpr int kMaxRibs = kBrushMaxRibs;

struct Rib
{
    double y = 0.0; // across the tread, left positive
    double width = 0.0;
    double length = 0.0;    // contact length, 0 off the ground
    double rollSpeed = 0.0; // V_r = omega R_e, m/s
};

// What a step's forces depend on besides the carcass: the patch and the wheel's motion.
struct Patch
{
    std::array<Rib, kMaxRibs> ribs{};
    int ribCount = 0;
    double pressureScale = 0.0; // Fz / A (8)
    double staticFriction = 0.0;
    double kineticFriction = 0.0;
    double vx = 0.0, vy = 0.0;
    double spin = 0.0; // turn rate of the tread about the normal: yaw plus camber's share
    double load = 0.0;
    double effectiveRadius = 0.0;
    double contactLength = 0.0;
    std::array<double, 5> pressure{}; // quartic coefficients c0..c4 of (7)
    std::array<double, 3> damping{};  // the carcass's dampers this step
};

struct Forces
{
    std::array<double, 3> f{}; // Fx, Fy, Mz
    double slidingLoad = 0.0;
    // Each rib's stuck length from its leading edge.
    std::array<double, kMaxRibs> stuckLength{};
};

// The quartic pressure shape of (7): zero at both edges, unit area, centroid at 1/2 - delta, second
// derivative -lambda at the middle.
std::array<double, 5> PressureCoefficients(double lambda, double delta)
{
    return {0.0,
            -lambda / 3.0 + 10.0 + 60.0 * delta,
            2.0 * lambda - 30.0 - 180.0 * delta,
            -10.0 / 3.0 * lambda + 40.0 + 120.0 * delta,
            5.0 / 3.0 * lambda - 20.0};
}

double Pressure(const std::array<double, 5>& c, double chi)
{
    const double value = c[0] + chi * (c[1] + chi * (c[2] + chi * (c[3] + chi * c[4])));
    return std::max(value, 0.0);
}

// The contact length of (6) at a radial deflection.
double ContactLength(const BrushTyreParameters& p, double deflection)
{
    if (deflection <= 0.0)
    {
        return 0.0;
    }
    const double span = 2.0 * (p.unloadedRadius - p.transitionRadius) - deflection;
    return span > 0.0 ? 2.0 * std::sqrt(span * deflection) : 0.0;
}

Patch MakePatch(const BrushTyreParameters& p, const BrushTyreInput& in)
{
    Patch patch;
    patch.load = std::max(in.load, 0.0);
    patch.ribCount = std::clamp(p.ribs, 1, kMaxRibs);
    patch.pressure = PressureCoefficients(p.pressureConvexity, p.pressureShift);
    const double deflection = patch.load / std::max(p.verticalRate, 1.0);
    const double sinCamber = std::sin(in.camber);
    const double ribWidth = p.width / patch.ribCount;
    double area = 0.0;
    double slipStiffness = 0.0; // the stuck tread's dF_x / d(slip), over k_x
    for (int i = 0; i < patch.ribCount; ++i)
    {
        Rib& rib = patch.ribs[i];
        rib.y = (i + 0.5) * ribWidth - 0.5 * p.width;
        rib.width = ribWidth;
        // Camber about the forward axis lifts the tread's left side (positive camber, top right).
        const double ribDeflection = deflection - rib.y * sinCamber;
        rib.length = ContactLength(p, ribDeflection);
        rib.rollSpeed = in.wheelSpeed * (p.unloadedRadius - std::max(ribDeflection, 0.0) / 3.0); // (23)
        area += rib.width * rib.length;
        slipStiffness += 0.5 * rib.width * rib.length * rib.length;
        patch.contactLength = std::max(patch.contactLength, rib.length);
    }
    patch.effectiveRadius = p.unloadedRadius - deflection / 3.0;
    patch.damping = p.carcassDamping;
    const double treadSpeed = std::hypot(in.wheelSpeed * patch.effectiveRadius, p.lowSpeed);
    patch.damping[0] = std::min(p.carcassDamping[0], p.rollingDampingShare * p.bristleStiffnessX * slipStiffness / treadSpeed);
    patch.pressureScale = area > 0.0 ? patch.load / area : 0.0;
    const double loadRatio = std::max(patch.load, 1.0) / std::max(p.referenceLoad, 1.0);
    patch.staticFriction = std::max(p.staticFriction * std::pow(loadRatio, p.loadExponent - 1.0) * in.frictionScale, 0.0);
    if (in.frictionCap > 0.0)
    {
        patch.staticFriction = std::min(patch.staticFriction, in.frictionCap);
    }
    patch.kineticFriction = patch.staticFriction * std::clamp(in.slidingShare > 0.0 ? in.slidingShare : p.kineticShare, 0.0, 1.0);
    patch.vx = in.forwardVelocity;
    patch.vy = in.lateralVelocity;
    patch.spin = in.yawRate + p.camberSpinShare * in.wheelSpeed * sinCamber;
    return patch;
}

// The road's force and moment on the tread with the carcass at c moving at rate.
Forces TreadForces(const BrushTyreParameters& p, const Patch& patch, const std::array<double, 3>& c, const std::array<double, 3>& rate)
{
    Forces out;
    if (patch.pressureScale <= 0.0)
    {
        return out;
    }
    const int segments = std::max(p.segmentsPerRib, 2);
    const double xc = c[0], yc = c[1], thetac = c[2];
    const double vxc = rate[0], vyc = rate[1], vthetac = rate[2];
    const double psi = p.bendingShape;
    const double slideRegularisation = 0.1 * p.lowSpeed;
    const double slideRegularisation2 = slideRegularisation * slideRegularisation;
    const double vMu = std::max(p.stribeckVelocity, 1e-3);
    const double frictionDrop = patch.staticFriction - patch.kineticFriction;
    for (int i = 0; i < patch.ribCount; ++i)
    {
        const Rib& rib = patch.ribs[i];
        if (rib.length <= 0.0)
        {
            continue;
        }
        out.stuckLength[i] = rib.length;
        const double h = rib.length / segments;
        const double vr = rib.rollSpeed;
        // The tread enters at the front rolling forward, at the back rolling backward.
        const double direction = vr >= 0.0 ? 1.0 : -1.0;
        const double transport = std::sqrt(vr * vr + p.lowSpeed * p.lowSpeed);
        double ux = 0.0, uy = 0.0;
        bool sliding = false;
        for (int j = 0; j < segments; ++j)
        {
            const double xi = (j + 0.5) * h;
            const double x = direction * (0.5 * rib.length - xi);
            const double qz = patch.pressureScale * Pressure(patch.pressure, xi / rib.length);
            const double dx = x - xc;
            const double yb = yc + thetac * dx - 0.5 * yc * psi * dx * dx;
            const double slope = thetac - yc * psi * dx;
            // The bristle root's velocity over the road (20): the wheel's, the tread's roll, the
            // carcass's own motion and the tread's passage along the bent carcass.
            const double vbx = patch.vx - vr + vxc - patch.spin * (rib.y + yb);
            const double vby = patch.vy + patch.spin * x + vyc + vthetac * dx - 0.5 * vyc * psi * dx * dx - vr * slope;
            // The share of this segment the bristles stick through: all of it before the transition,
            // none after, and in the segment where their pull reaches static friction the share up to
            // that point (48), found linearly, so the forces move smoothly as the transition does.
            double stuck = 0.0;
            double qx = 0.0, qy = 0.0;
            if (!sliding)
            {
                // Stuck to the road, the bristle bends by what its root has moved since it came in (33).
                const double dux = -vbx * h / transport;
                const double duy = -vby * h / transport;
                const auto excess = [&](double share, double pressure)
                {
                    const double fx = p.bristleStiffnessX * (ux + share * dux);
                    const double fy = p.bristleStiffnessY * (uy + share * duy);
                    return std::sqrt(fx * fx + fy * fy) - patch.staticFriction * pressure;
                };
                const double atStart = std::min(excess(0.0, patch.pressureScale * Pressure(patch.pressure, xi / rib.length - 0.5 / segments)), 0.0);
                const double atEnd = excess(1.0, patch.pressureScale * Pressure(patch.pressure, xi / rib.length + 0.5 / segments));
                stuck = atEnd <= 0.0 ? 1.0 : atStart / (atStart - atEnd);
                qx = p.bristleStiffnessX * (ux + 0.5 * stuck * dux);
                qy = p.bristleStiffnessY * (uy + 0.5 * stuck * duy);
                if (stuck < 1.0)
                {
                    sliding = true; // from the transition on, the rest of the rib slides
                    out.stuckLength[i] = (j + stuck) * h;
                }
                ux += dux;
                uy += duy;
            }
            const double area = h * rib.width;
            double sx = 0.0, sy = 0.0;
            if (stuck < 1.0)
            {
                const double speed2 = vbx * vbx + vby * vby;
                const double speed = std::sqrt(speed2);
                const double mu = patch.kineticFriction + frictionDrop * std::exp(-(speed * speed) / (vMu * vMu));
                const double scale = -mu * qz / std::sqrt(speed2 + slideRegularisation2);
                sx = scale * vbx;
                sy = scale * vby;
                out.slidingLoad += (1.0 - stuck) * qz * area;
            }
            const double fx = (stuck * qx + (1.0 - stuck) * sx) * area;
            const double fy = (stuck * qy + (1.0 - stuck) * sy) * area;
            out.f[0] += fx;
            out.f[1] += fy;
            out.f[2] += x * fy - (rib.y + yb) * fx;
        }
    }
    return out;
}

// The carcass's spring forces with bottoming (14), (15).
double PositivePart(double x, double h)
{
    const double root = std::sqrt(x * x + h * h);
    return x > 0.0 ? 0.5 * (root + x) : 0.5 * h * h / (root - x);
}

double CarcassSpring(const BrushTyreParameters& p, int axis, double c)
{
    const double b = p.bottomingDeflection[axis];
    const double h = 1e-3 * std::max(b, 1e-6);
    const double excess = PositivePart(-c - b, h) + PositivePart(c - b, h);
    return (p.carcassStiffness[axis] + p.bottomingStiffness[axis] * excess * excess) * c;
}

struct Residual
{
    std::array<double, 3> g{};
    Forces forces;
};

Residual Balance(const BrushTyreParameters& p, const Patch& patch, const std::array<double, 3>& c, const std::array<double, 3>& previous, double dt)
{
    std::array<double, 3> rate{};
    if (dt > 0.0)
    {
        for (int k = 0; k < 3; ++k)
        {
            rate[k] = (c[k] - previous[k]) / dt;
        }
    }
    Residual r;
    r.forces = TreadForces(p, patch, c, rate);
    for (int k = 0; k < 3; ++k)
    {
        r.g[k] = CarcassSpring(p, k, c[k]) + patch.damping[k] * rate[k] - r.forces.f[k];
    }
    return r;
}

// The residual measured against the load: forces in units of Fz, the moment in Fz times 5 cm.
double ScaledNorm(const std::array<double, 3>& g, double load)
{
    const double scale = 1.0 / (load + 1.0);
    const double a = g[0] * scale, b = g[1] * scale, m = g[2] * scale / 0.05;
    return std::sqrt(a * a + b * b + m * m);
}

bool Solve3(const std::array<double, 9>& j, const std::array<double, 3>& b, std::array<double, 3>& x)
{
    const double det = j[0] * (j[4] * j[8] - j[5] * j[7]) - j[1] * (j[3] * j[8] - j[5] * j[6]) + j[2] * (j[3] * j[7] - j[4] * j[6]);
    if (!(std::abs(det) > 1e-300) || !std::isfinite(det))
    {
        return false;
    }
    const double inv = 1.0 / det;
    x[0] = (b[0] * (j[4] * j[8] - j[5] * j[7]) - j[1] * (b[1] * j[8] - j[5] * b[2]) + j[2] * (b[1] * j[7] - j[4] * b[2])) * inv;
    x[1] = (j[0] * (b[1] * j[8] - j[5] * b[2]) - b[0] * (j[3] * j[8] - j[5] * j[6]) + j[2] * (j[3] * b[2] - b[1] * j[6])) * inv;
    x[2] = (j[0] * (j[4] * b[2] - b[1] * j[7]) - j[1] * (j[3] * b[2] - b[1] * j[6]) + b[0] * (j[3] * j[7] - j[4] * j[6])) * inv;
    return std::isfinite(x[0]) && std::isfinite(x[1]) && std::isfinite(x[2]);
}

constexpr std::array<double, 3> kJacobianSteps{1e-6, 1e-6, 1e-5};

std::array<double, 9> FiniteDifferenceJacobian(const BrushTyreParameters& p, const Patch& patch, const std::array<double, 3>& c,
                                               const std::array<double, 3>& previous, double dt, const Residual& at, int& evaluations)
{
    std::array<double, 9> j{};
    for (int k = 0; k < 3; ++k)
    {
        std::array<double, 3> shifted = c;
        shifted[k] += kJacobianSteps[k];
        const Residual r = Balance(p, patch, shifted, previous, dt);
        ++evaluations;
        for (int row = 0; row < 3; ++row)
        {
            j[row * 3 + k] = (r.g[row] - at.g[row]) / kJacobianSteps[k];
        }
    }
    return j;
}

// Newton's method on the carcass's balance with Broyden's ("good") updates of the Jacobian between
// fresh finite-difference ones, and a halving line search. The Jacobian is carried to the next step.
struct SolveResult
{
    std::array<double, 3> c{};
    Residual residual;
    int evaluations = 0;
    bool converged = false;
};

SolveResult SolveBalance(const BrushTyreParameters& p, const Patch& patch, const std::array<double, 3>& previous, double dt,
                         std::array<double, 9>& jacobian, bool& jacobianValid)
{
    constexpr double kTolerance = 1e-7;
    constexpr int kMaxIterations = 25;
    SolveResult s;
    s.c = previous;
    s.residual = Balance(p, patch, s.c, previous, dt);
    s.evaluations = 1;
    double norm = ScaledNorm(s.residual.g, patch.load);
    int sinceFresh = jacobianValid ? 99 : 0;
    if (!jacobianValid)
    {
        jacobian = FiniteDifferenceJacobian(p, patch, s.c, previous, dt, s.residual, s.evaluations);
        jacobianValid = true;
    }
    for (int iteration = 0; iteration < kMaxIterations && norm > kTolerance; ++iteration)
    {
        std::array<double, 3> step{};
        if (!Solve3(jacobian, {-s.residual.g[0], -s.residual.g[1], -s.residual.g[2]}, step))
        {
            jacobian = FiniteDifferenceJacobian(p, patch, s.c, previous, dt, s.residual, s.evaluations);
            sinceFresh = 0;
            continue;
        }
        double t = 1.0;
        Residual trial;
        std::array<double, 3> next{};
        double trialNorm = 0.0;
        for (int halving = 0; halving < 6; ++halving)
        {
            for (int k = 0; k < 3; ++k)
            {
                next[k] = s.c[k] + t * step[k];
            }
            trial = Balance(p, patch, next, previous, dt);
            ++s.evaluations;
            trialNorm = ScaledNorm(trial.g, patch.load);
            if (trialNorm < norm)
            {
                break;
            }
            t *= 0.5;
        }
        if (!(trialNorm < norm))
        {
            if (sinceFresh == 0)
            {
                break; // a fresh Jacobian gives no descent: as good as it gets
            }
            jacobian = FiniteDifferenceJacobian(p, patch, s.c, previous, dt, s.residual, s.evaluations);
            sinceFresh = 0;
            continue;
        }
        // Broyden: J += (dG - J dc) dc^T / (dc . dc).
        std::array<double, 3> dc{}, dg{};
        double dc2 = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            dc[k] = next[k] - s.c[k];
            dg[k] = trial.g[k] - s.residual.g[k];
            dc2 += dc[k] * dc[k];
        }
        if (dc2 > 0.0)
        {
            for (int row = 0; row < 3; ++row)
            {
                const double predicted = jacobian[row * 3] * dc[0] + jacobian[row * 3 + 1] * dc[1] + jacobian[row * 3 + 2] * dc[2];
                const double correction = (dg[row] - predicted) / dc2;
                for (int k = 0; k < 3; ++k)
                {
                    jacobian[row * 3 + k] += correction * dc[k];
                }
            }
        }
        s.c = next;
        s.residual = trial;
        norm = trialNorm;
        ++sinceFresh;
        if (sinceFresh > 6 && norm > kTolerance)
        {
            jacobian = FiniteDifferenceJacobian(p, patch, s.c, previous, dt, s.residual, s.evaluations);
            sinceFresh = 0;
        }
    }
    s.converged = norm <= kTolerance * 10.0;
    return s;
}

BrushTyreOutput MakeOutput(const BrushTyreParameters& p, const Patch& patch, const BrushTyreInput& in, const SolveResult& s)
{
    BrushTyreOutput out;
    out.Fx = s.residual.forces.f[0];
    out.Fy = s.residual.forces.f[1];
    out.Mz = s.residual.forces.f[2];
    out.effectiveRadius = patch.effectiveRadius;
    out.contactLength = patch.contactLength;
    out.slidingShare = patch.load > 0.0 ? std::clamp(s.residual.forces.slidingLoad / patch.load, 0.0, 1.0) : 0.0;
    out.peakFriction = patch.staticFriction;
    const double reference = std::max(std::abs(in.forwardVelocity), p.lowSpeed);
    out.slipRatio = (in.wheelSpeed * patch.effectiveRadius - in.forwardVelocity) / reference;
    out.slipAngle = std::atan2(in.lateralVelocity, std::max(std::abs(in.forwardVelocity), p.lowSpeed));
    // Rolling resistance turns smoothly through a standstill.
    out.rollingResistanceTorque = -(p.rollingResistance + std::max(in.extraRollingResistance, 0.0)) * patch.load * patch.effectiveRadius * std::tanh(in.wheelSpeed / 0.5);
    out.evaluations = s.evaluations;
    out.converged = s.converged;
    out.ribCount = patch.ribCount;
    for (int i = 0; i < patch.ribCount; ++i)
    {
        out.ribs[i] = {patch.ribs[i].y, patch.ribs[i].length, s.residual.forces.stuckLength[i]};
    }
    out.rollingForward = in.wheelSpeed >= 0.0;
    return out;
}
}

BrushTyre::BrushTyre(BrushTyreParameters parameters)
    : m_p(parameters)
{
}

void BrushTyre::Reset()
{
    m_state = {};
}

BrushTyreOutput BrushTyre::Step(const BrushTyreInput& input, double dt)
{
    if (!(input.load > 0.0))
    {
        // Off the ground the carcass springs back through its dampers.
        if (dt > 0.0)
        {
            for (int k = 0; k < 3; ++k)
            {
                const double d = m_p.carcassDamping[k];
                m_state.carcass[k] *= d / (d + m_p.carcassStiffness[k] * dt);
            }
        }
        else
        {
            m_state.carcass = {};
        }
        m_state.jacobianValid = false;
        BrushTyreOutput out;
        out.effectiveRadius = m_p.unloadedRadius;
        return out;
    }
    const Patch patch = MakePatch(m_p, input);
    const SolveResult s = SolveBalance(m_p, patch, m_state.carcass, dt, m_state.jacobian, m_state.jacobianValid);
    if (s.converged || ScaledNorm(s.residual.g, patch.load) < 1e-3)
    {
        m_state.carcass = s.c;
    }
    else
    {
        // Not balanced: keep the carcass where it was and start the Jacobian afresh next step.
        m_state.jacobianValid = false;
    }
    return MakeOutput(m_p, patch, input, s);
}

BrushTyreOutput BrushTyre::Steady(const BrushTyreInput& input) const
{
    BrushTyre copy(*this);
    copy.m_state.jacobianValid = false;
    return copy.Step(input, 0.0);
}

std::array<double, 2> BrushTyre::BristleSlipStiffness(double load) const
{
    BrushTyreInput in;
    in.load = load;
    const Patch patch = MakePatch(m_p, in);
    // Linear brush, stuck throughout: F = k (slip) integral over ribs of w l^2 / 2, shaped by the
    // pressure only through where sliding starts, so not at all here.
    double sum = 0.0;
    for (int i = 0; i < patch.ribCount; ++i)
    {
        sum += patch.ribs[i].width * patch.ribs[i].length * patch.ribs[i].length * 0.5;
    }
    return {m_p.bristleStiffnessX * sum, m_p.bristleStiffnessY * sum};
}

BrushTyreParameters MakeBrushTyreParameters(const BrushTyreFigures& figures)
{
    BrushTyreParameters p;
    p.unloadedRadius = std::max(figures.radius, 0.05);
    // The shoulders round off into the sidewalls and carry little: a share of the section height each side.
    constexpr double kShoulderShare = 0.15;
    const double sectionHeight = figures.rimRadius > 0.0 ? std::max(p.unloadedRadius - figures.rimRadius, 0.0) : 0.0;
    p.width = std::max(figures.sectionWidth - 2.0 * kShoulderShare * sectionHeight, 0.05);
    p.verticalRate = figures.verticalRate > 0.0 ? figures.verticalRate : 250000.0;
    p.staticFriction = std::max(figures.peakFriction, 0.05);
    p.kineticShare = figures.kineticShare > 0.0 ? std::clamp(figures.kineticShare, 0.3, 1.0) : 0.85;
    p.referenceLoad = std::max(figures.referenceLoad, 100.0);
    p.loadExponent = 0.9;
    const double angle = std::clamp(figures.peakSlipAngle > 0.0 ? figures.peakSlipAngle : 6.0 * std::numbers::pi / 180.0, 0.02, 0.4);
    const double stiffnessRatio = figures.longitudinalStiffnessRatio > 0.0 ? std::clamp(figures.longitudinalStiffnessRatio, 0.2, 5.0) : 1.0;
    const double lateralRelaxation = figures.relaxationLength > 0.0 ? figures.relaxationLength : 0.6 * p.unloadedRadius;
    const double longitudinalRelaxation = figures.relaxationLength > 0.0 ? figures.relaxationLength : 0.4 * p.unloadedRadius;

    // The patch: (6) with the transition radius that makes its area at the reference load the load over
    // the inflation pressure, (l/2)^2 = (2 (R0 - R_l) - deflection) deflection.
    const double deflection = p.referenceLoad / p.verticalRate;
    p.transitionRadius = 0.45 * p.unloadedRadius;
    if (figures.inflationPressure > 0.0)
    {
        const double length = p.referenceLoad / (figures.inflationPressure * p.width);
        const double span = 0.5 * (0.25 * length * length / deflection + deflection);
        p.transitionRadius = std::clamp(p.unloadedRadius - span, 0.0, p.unloadedRadius);
    }

    // The steady cornering stiffness on the flexible carcass, at a small angle.
    const auto flexibleCornering = [&]()
    {
        const BrushTyre tyre(p);
        BrushTyreInput in;
        in.load = p.referenceLoad;
        in.forwardVelocity = 20.0;
        in.wheelSpeed = 20.0 / (p.unloadedRadius - deflection / 3.0);
        constexpr double kSmall = 1e-3;
        in.lateralVelocity = -20.0 * kSmall;
        return std::abs(tyre.Steady(in).Fy) / kSmall;
    };
    const auto setStiffness = [&](double cornering)
    {
        const double length = ContactLength(p, deflection);
        const double k = 2.0 * cornering / (p.width * std::max(length * length, 1e-6));
        p.bristleStiffnessY = k;
        p.bristleStiffnessX = stiffnessRatio * k;
        // Fore and aft the carcass's shift leaves the steady slip alone, so C'_x is the bristles' own.
        p.carcassStiffness = {stiffnessRatio * cornering / longitudinalRelaxation, cornering / lateralRelaxation,
                              1.5 * cornering * p.unloadedRadius * p.unloadedRadius};
        // Sideways C'_y falls with the carcass's own stiffness: a few rounds settle sigma = C'_y / K_y.
        for (int round = 0; round < 3; ++round)
        {
            p.carcassStiffness[1] = flexibleCornering() / lateralRelaxation;
        }
        // The carcass's damping, as a time constant on its stiffness: 0.5 ms sideways and in twist, so a
        // step in slip still builds its force over the relaxation length, and 10 ms fore and aft standing
        // still (rollingDampingShare lowers it rolling). At 0.5 ms fore and aft a car stopped on its brakes
        // rocked on its tyres' carcasses (the R34 at 7 Hz, a damping ratio of about 0.05, rolling back and
        // forth for over two seconds); at 10 ms that mode settles in a cycle or two.
        constexpr double kCarcassDampingSeconds = 0.5e-3;
        constexpr double kLongitudinalCarcassDampingSeconds = 10e-3;
        p.carcassDamping = {p.carcassStiffness[0] * kLongitudinalCarcassDampingSeconds, p.carcassStiffness[1] * kCarcassDampingSeconds,
                            p.carcassStiffness[2] * kCarcassDampingSeconds};
    };
    // A rigid-carcass brush slides throughout at tan(alpha_sl) = 3 mu Fz / C_alpha.
    double cornering = 3.0 * p.staticFriction * p.referenceLoad / std::tan(angle);
    setStiffness(cornering);

    // The flexible carcass moves the peak past the brush's own: scale the bristles until the steady
    // lateral force peaks at the angle asked for.
    for (int pass = 0; pass < 4; ++pass)
    {
        const BrushTyre tyre(p);
        BrushTyreInput in;
        in.load = p.referenceLoad;
        in.forwardVelocity = 20.0;
        in.wheelSpeed = 20.0 / (p.unloadedRadius - deflection / 3.0);
        double best = 0.0;
        double bestAngle = angle;
        for (double a = 0.25; a <= 25.0; a += 0.25)
        {
            const double alpha = a * std::numbers::pi / 180.0;
            in.lateralVelocity = -20.0 * std::tan(alpha);
            const double fy = std::abs(tyre.Steady(in).Fy);
            if (fy > best)
            {
                best = fy;
                bestAngle = alpha;
            }
        }
        const double ratio = std::tan(bestAngle) / std::tan(angle);
        if (std::abs(ratio - 1.0) < 0.03)
        {
            break;
        }
        cornering *= std::clamp(ratio, 0.5, 2.0);
        setStiffness(cornering);
    }
    return p;
}

}
