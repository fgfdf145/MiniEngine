#include "tyre_brush.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

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
    std::array<double, 2> staticFriction{}; // along and across the wheel
    double kineticShare = 0.0;
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
    for (int axis = 0; axis < 2; ++axis)
    {
        double mu = std::max(p.staticFriction[axis] * std::pow(loadRatio, p.loadExponent[axis] - 1.0) * in.frictionScale, 0.0);
        if (in.frictionCap > 0.0)
        {
            mu = std::min(mu, in.frictionCap);
        }
        // Kept off zero: the ellipse divides by it.
        patch.staticFriction[axis] = std::max(mu, 1e-6);
    }
    patch.kineticShare = std::clamp(in.slidingShare > 0.0 ? in.slidingShare : p.kineticShare, 0.0, 1.0);
    patch.vx = in.forwardVelocity;
    patch.vy = in.lateralVelocity;
    patch.spin = in.yawRate + p.camberSpinShare * in.wheelSpeed * sinCamber;
    return patch;
}

// The bristle root at x along a rib: where the carcass line has it sideways, and its velocity over the
// road (20): the wheel's, the tread's roll, the carcass's own motion and the tread's passage along the
// bent carcass.
struct Root
{
    double vx, vy, yb;
};

struct RibMotion
{
    const Patch& patch;
    const Rib& rib;
    double xc, yc, thetac, vxc, vyc, vthetac, psi;

    Root At(double x) const
    {
        const double dx = x - xc;
        const double yb = yc + thetac * dx - 0.5 * yc * psi * dx * dx;
        const double slope = thetac - yc * psi * dx;
        return Root{patch.vx - rib.rollSpeed + vxc - patch.spin * (rib.y + yb),
                    patch.vy + patch.spin * x + vyc + vthetac * dx - 0.5 * vyc * psi * dx * dx - rib.rollSpeed * slope, yb};
    }
};

// The friction ellipse: a stress against it is 1 on it for each unit of pressure.
struct Ellipse
{
    double inverseX, inverseY;

    double operator()(double fx, double fy) const
    {
        return std::sqrt(fx * fx * inverseX * inverseX + fy * fy * inverseY * inverseY);
    }
};

// The road's force and moment on the tread in the steady state, with the carcass at c moving at rate: a
// stuck bristle's bend is its root's speed over the road times the time it has spent in the patch (33),
// the tread running through at the roll speed, held to lowSpeed; past the rib's transition (48) the
// bristles slide against their velocity on the friction ellipse, less the Stribeck fall.
Forces SteadyTreadForces(const BrushTyreParameters& p, const Patch& patch, const std::array<double, 3>& c, const std::array<double, 3>& rate)
{
    Forces out;
    const int segments = std::max(p.segmentsPerRib, 2);
    const double slideRegularisation = 0.1 * p.lowSpeed;
    const double vMu = std::max(p.stribeckVelocity, 1e-3);
    const Ellipse ellipse{1.0 / patch.staticFriction[0], 1.0 / patch.staticFriction[1]};
    const double regularisedSlide2 = slideRegularisation * slideRegularisation * ellipse.inverseX * ellipse.inverseY;
    for (int i = 0; i < patch.ribCount; ++i)
    {
        const Rib& rib = patch.ribs[i];
        if (rib.length <= 0.0)
        {
            continue;
        }
        out.stuckLength[i] = rib.length;
        const RibMotion motion{patch, rib, c[0], c[1], c[2], rate[0], rate[1], rate[2], p.bendingShape};
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
            const Root r = motion.At(x);
            // The share of this segment the bristles stick through: all of it before the transition,
            // none after, and in the segment where their pull reaches static friction the share up to
            // that point (48), found linearly, so the forces move smoothly as the transition does.
            double stuck = 0.0;
            double qx = 0.0, qy = 0.0;
            if (!sliding)
            {
                // Stuck to the road, the bristle bends by what its root has moved since it came in (33).
                const double dux = -r.vx * h / transport;
                const double duy = -r.vy * h / transport;
                const auto excess = [&](double share, double pressure)
                { return ellipse(p.bristleStiffnessX * (ux + share * dux), p.bristleStiffnessY * (uy + share * duy)) - pressure; };
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
                // Sliding, the road's stress opposes the sliding velocity and lies on the friction ellipse,
                // less the Stribeck fall: -q v / |(v_x / mu_x, v_y / mu_y)|.
                const double speed2 = r.vx * r.vx + r.vy * r.vy;
                const double fall = patch.kineticShare + (1.0 - patch.kineticShare) * std::exp(-speed2 / (vMu * vMu));
                const double scale = -fall * qz / std::sqrt(r.vx * r.vx * ellipse.inverseX * ellipse.inverseX + r.vy * r.vy * ellipse.inverseY * ellipse.inverseY + regularisedSlide2);
                sx = scale * r.vx;
                sy = scale * r.vy;
                out.slidingLoad += (1.0 - stuck) * qz * area;
            }
            const double fx = (stuck * qx + (1.0 - stuck) * sx) * area;
            const double fy = (stuck * qy + (1.0 - stuck) * sy) * area;
            out.f[0] += fx;
            out.f[1] += fy;
            out.f[2] += x * fy - (rib.y + r.yb) * fx;
        }
    }
    return out;
}

// The road's force and moment on the tread over a step of dt, with the carcass at c moving at rate. Each
// bristle keeps the bend it had a step ago where the tread then was (kept, carried along the patch at the
// roll speed) and bends further by what its root moved over the road in the step, at the mean of its
// root's velocity then and now (the trapezoidal rule); one that came into the patch during the step
// bends only by what its root moved since, at the mean of the leading edge's and its own. Stuck from the leading edge to the rib's transition (48) as
// in the steady state; past it a bristle slides as far as it must to stay on the friction ellipse, less
// the Stribeck fall at its root's speed: its bend is drawn back onto the ellipse, so the force turns with
// the bend and not with a sliding velocity that a stopped tyre's carcass may swing through zero. Rolling
// steadily this comes to the steady state's forces; standing still the bristles hold like springs.
// bendOut takes the bend the bristles end the step with.
Forces SteppedTreadForces(const BrushTyreParameters& p, const Patch& patch, const std::array<double, 3>& c, const std::array<double, 3>& rate,
                          const BrushBristles& kept, double dt, BrushBristles* bendOut)
{
    Forces out;
    if (bendOut != nullptr)
    {
        bendOut->length.fill(0.0);
    }
    const int segments = std::clamp(p.segmentsPerRib, 2, kBrushMaxSegments);
    const double vMu = std::max(p.stribeckVelocity, 1e-3);
    const Ellipse ellipse{1.0 / patch.staticFriction[0], 1.0 / patch.staticFriction[1]};
    const auto fall = [&](const Root& r) { return patch.kineticShare + (1.0 - patch.kineticShare) * std::exp(-(r.vx * r.vx + r.vy * r.vy) / (vMu * vMu)); };
    // A bend drawn back onto the ellipse of a pressure's friction (left be inside it).
    const auto onto = [&](const std::array<double, 2>& u, double limit)
    {
        const double e = ellipse(p.bristleStiffnessX * u[0], p.bristleStiffnessY * u[1]);
        const double scale = e > limit ? limit / e : 1.0;
        return std::array<double, 2>{u[0] * scale, u[1] * scale};
    };
    for (int i = 0; i < patch.ribCount; ++i)
    {
        const Rib& rib = patch.ribs[i];
        if (rib.length <= 0.0)
        {
            continue;
        }
        out.stuckLength[i] = rib.length;
        const RibMotion motion{patch, rib, c[0], c[1], c[2], rate[0], rate[1], rate[2], p.bendingShape};
        const double h = rib.length / segments;
        const double vr = rib.rollSpeed;
        const double direction = vr >= 0.0 ? 1.0 : -1.0;
        const double keptLength = kept.length[i];
        const auto nodeX = [&](int m) { return direction * (0.5 * rib.length - m * h); };
        const auto pressureAt = [&](double share) { return patch.pressureScale * Pressure(patch.pressure, share); };
        const Root leading = motion.At(nodeX(0));
        // The bend the bristle at the m-th segment end from the leading edge would have stuck throughout
        // the step, its root moving at `here` now.
        const auto trial = [&](int m, const Root& here)
        {
            const double x = nodeX(m);
            // Where this tread was a step ago, on the patch the step started with (front first).
            const double upstream = x + vr * dt;
            if (keptLength > 0.0 && std::abs(upstream) <= 0.5 * keptLength)
            {
                const double at = std::clamp((0.5 * keptLength - upstream) / keptLength * segments, 0.0, static_cast<double>(segments));
                const int k = std::min(static_cast<int>(at), segments - 1);
                const double share = at - k;
                const BrushBristles::Node& a = kept.nodes[i][k];
                const BrushBristles::Node& b = kept.nodes[i][k + 1];
                const double thenX = a.rootVelocity[0] + share * (b.rootVelocity[0] - a.rootVelocity[0]);
                const double thenY = a.rootVelocity[1] + share * (b.rootVelocity[1] - a.rootVelocity[1]);
                return std::array<double, 2>{a.bend[0] + share * (b.bend[0] - a.bend[0]) - 0.5 * (thenX + here.vx) * dt,
                                             a.bend[1] + share * (b.bend[1] - a.bend[1]) - 0.5 * (thenY + here.vy) * dt};
            }
            // Came in during the step (or the patch grew there): bent only since it touched.
            const double since = std::min(dt, m * h / std::max(std::abs(vr), 1e-9));
            return std::array<double, 2>{-0.5 * (leading.vx + here.vx) * since, -0.5 * (leading.vy + here.vy) * since};
        };
        const auto store = [&](int m, const std::array<double, 2>& u, const Root& r)
        {
            if (bendOut != nullptr)
            {
                // In the front-first order whichever way the tread runs.
                bendOut->nodes[i][direction > 0.0 ? m : segments - m] = {u, {r.vx, r.vy}};
            }
        };
        if (bendOut != nullptr)
        {
            bendOut->length[i] = rib.length;
        }
        // The edges carry no pressure and so no bend.
        std::array<double, 2> u{0.0, 0.0};
        store(0, u, leading);
        Root start = leading;
        bool sliding = false;
        for (int j = 0; j < segments; ++j)
        {
            const double xi = (j + 0.5) * h;
            const double x = direction * (0.5 * rib.length - xi);
            const double qz = pressureAt(xi / rib.length);
            const double qEnd = pressureAt(static_cast<double>(j + 1) / segments);
            const Root end = motion.At(nodeX(j + 1));
            const Root r{0.5 * (start.vx + end.vx), 0.5 * (start.vy + end.vy), 0.5 * (start.yb + end.yb)};
            const std::array<double, 2> next = trial(j + 1, end);
            double stuck = 0.0;
            double qx = 0.0, qy = 0.0;
            if (!sliding)
            {
                const double dux = next[0] - u[0];
                const double duy = next[1] - u[1];
                const auto excess = [&](double share, double pressure)
                { return ellipse(p.bristleStiffnessX * (u[0] + share * dux), p.bristleStiffnessY * (u[1] + share * duy)) - pressure; };
                const double atStart = std::min(excess(0.0, pressureAt(static_cast<double>(j) / segments)), 0.0);
                const double atEnd = excess(1.0, qEnd);
                stuck = atEnd <= 0.0 ? 1.0 : atStart / (atStart - atEnd);
                qx = p.bristleStiffnessX * (u[0] + 0.5 * stuck * dux);
                qy = p.bristleStiffnessY * (u[1] + 0.5 * stuck * duy);
                if (stuck < 1.0)
                {
                    sliding = true; // from the transition on, the rest of the rib slides
                    out.stuckLength[i] = (j + stuck) * h;
                }
            }
            const double area = h * rib.width;
            double sx = 0.0, sy = 0.0;
            if (stuck < 1.0)
            {
                // The middle of the segment, drawn back onto the ellipse of sliding friction there.
                const std::array<double, 2> trialMiddle{0.5 * (u[0] + next[0]), 0.5 * (u[1] + next[1])};
                const std::array<double, 2> middle = onto(trialMiddle, fall(r) * qz);
                sx = p.bristleStiffnessX * middle[0];
                sy = p.bristleStiffnessY * middle[1];
                if (middle != trialMiddle)
                {
                    out.slidingLoad += (1.0 - stuck) * qz * area;
                }
                store(j + 1, onto(next, fall(end) * qEnd), end);
            }
            else
            {
                store(j + 1, next, end);
            }
            u = next;
            start = end;
            const double fx = (stuck * qx + (1.0 - stuck) * sx) * area;
            const double fy = (stuck * qy + (1.0 - stuck) * sy) * area;
            out.f[0] += fx;
            out.f[1] += fy;
            out.f[2] += x * fy - (rib.y + r.yb) * fx;
        }
    }
    return out;
}

Forces TreadForces(const BrushTyreParameters& p, const Patch& patch, const std::array<double, 3>& c, const std::array<double, 3>& rate,
                   const BrushBristles* kept, double dt, BrushBristles* bendOut)
{
    if (patch.pressureScale <= 0.0)
    {
        if (bendOut != nullptr)
        {
            bendOut->length.fill(0.0);
        }
        return {};
    }
    return kept != nullptr && dt > 0.0 ? SteppedTreadForces(p, patch, c, rate, *kept, dt, bendOut) : SteadyTreadForces(p, patch, c, rate);
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

// What a step starts from: the carcass and the bristles' bend the last one left, and its length (0, and
// no bristles, for the steady state).
struct Start
{
    std::array<double, 3> carcass{};
    const BrushBristles* bristles = nullptr;
    double dt = 0.0;
};

Residual Balance(const BrushTyreParameters& p, const Patch& patch, const std::array<double, 3>& c, const Start& start, BrushBristles* bendOut = nullptr)
{
    const double dt = start.dt;
    std::array<double, 3> rate{};
    if (dt > 0.0)
    {
        for (int k = 0; k < 3; ++k)
        {
            rate[k] = (c[k] - start.carcass[k]) / dt;
        }
    }
    Residual r;
    r.forces = TreadForces(p, patch, c, rate, start.bristles, dt, bendOut);
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

std::array<double, 9> FiniteDifferenceJacobian(const BrushTyreParameters& p, const Patch& patch, const std::array<double, 3>& c, const Start& start,
                                               const Residual& at, int& evaluations)
{
    std::array<double, 9> j{};
    for (int k = 0; k < 3; ++k)
    {
        std::array<double, 3> shifted = c;
        shifted[k] += kJacobianSteps[k];
        const Residual r = Balance(p, patch, shifted, start);
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
// With bend (two buffers), the bristles' bend at the solution ends up in the one SolveResult::bend names.
struct SolveResult
{
    std::array<double, 3> c{};
    Residual residual;
    int evaluations = 0;
    bool converged = false;
    int bend = 0;
};

SolveResult SolveBalance(const BrushTyreParameters& p, const Patch& patch, const Start& start, std::array<double, 9>& jacobian, bool& jacobianValid,
                         const std::array<BrushBristles*, 2>& bend = {nullptr, nullptr})
{
    constexpr double kTolerance = 1e-7;
    constexpr int kMaxIterations = 25;
    BrushBristles* current = bend[0];
    BrushBristles* spare = bend[1];
    SolveResult s;
    s.c = start.carcass;
    s.residual = Balance(p, patch, s.c, start, current);
    s.evaluations = 1;
    double norm = ScaledNorm(s.residual.g, patch.load);
    int sinceFresh = jacobianValid ? 99 : 0;
    if (!jacobianValid)
    {
        jacobian = FiniteDifferenceJacobian(p, patch, s.c, start, s.residual, s.evaluations);
        jacobianValid = true;
    }
    for (int iteration = 0; iteration < kMaxIterations && norm > kTolerance; ++iteration)
    {
        std::array<double, 3> step{};
        if (!Solve3(jacobian, {-s.residual.g[0], -s.residual.g[1], -s.residual.g[2]}, step))
        {
            jacobian = FiniteDifferenceJacobian(p, patch, s.c, start, s.residual, s.evaluations);
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
            trial = Balance(p, patch, next, start, spare);
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
            jacobian = FiniteDifferenceJacobian(p, patch, s.c, start, s.residual, s.evaluations);
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
        std::swap(current, spare);
        norm = trialNorm;
        ++sinceFresh;
        if (sinceFresh > 6 && norm > kTolerance)
        {
            jacobian = FiniteDifferenceJacobian(p, patch, s.c, start, s.residual, s.evaluations);
            sinceFresh = 0;
        }
    }
    s.converged = norm <= kTolerance * 10.0;
    s.bend = current == bend[0] ? 0 : 1;
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
    out.peakFrictionX = patch.staticFriction[0];
    out.peakFrictionY = patch.staticFriction[1];
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

// Where a steady force curve peaks over a slip, scanned from `from` to `to` by `step` and refined to a
// tenth of it: at its maximum when it falls past that (by over 1 % at the scan's end); a curve that does
// not, with no grip lost sliding, where it first comes within 0.1 % of its limit (its arg max would sit
// anywhere along the flat).
template <class Force>
double PeakSlip(const Force& force, double from, double to, double step)
{
    std::vector<double> values;
    for (double slip = from; slip <= to + 1e-9 * step; slip += step)
    {
        values.push_back(force(slip));
    }
    const size_t best = static_cast<size_t>(std::max_element(values.begin(), values.end()) - values.begin());
    const double fine = 0.1 * step;
    if (values.back() < 0.99 * values[best])
    {
        double slip = from + best * step;
        double most = values[best];
        for (double s = std::max(from, slip - step); s <= slip + step + 1e-9 * fine; s += fine)
        {
            const double f = force(s);
            if (f > most)
            {
                most = f;
                slip = s;
            }
        }
        return slip;
    }
    const double limit = 0.999 * values[best];
    const size_t reached = static_cast<size_t>(std::find_if(values.begin(), values.end(), [&](double f) { return f >= limit; }) - values.begin());
    const double upper = from + reached * step;
    for (double s = std::max(from, upper - step); s < upper; s += fine)
    {
        if (force(s) >= limit)
        {
            return s;
        }
    }
    return upper;
}

// The distance rolled at 20 m/s and the reference load until a small step in slip has built 63 % of its
// steady force, along the wheel (a theoretical slip of 0.01) or across it (1 degree): stepped at 0.1 ms
// and read between the steps.
double RelaxationDistance(const BrushTyreParameters& p, bool lateral)
{
    BrushTyre tyre(p);
    BrushTyreInput in;
    in.load = p.referenceLoad;
    in.forwardVelocity = 20.0;
    const double radius = p.unloadedRadius - p.referenceLoad / p.verticalRate / 3.0;
    in.wheelSpeed = 20.0 / radius / (lateral ? 1.0 : 0.99);
    in.lateralVelocity = lateral ? 20.0 * std::tan(std::numbers::pi / 180.0) : 0.0;
    const BrushTyreOutput steady = tyre.Steady(in);
    const double target = 0.632 * std::abs(lateral ? steady.Fy : steady.Fx);
    constexpr double kStep = 1e-4;
    double previous = 0.0;
    for (int step = 1; step <= 20000; ++step)
    {
        const BrushTyreOutput o = tyre.Step(in, kStep);
        const double force = std::abs(lateral ? o.Fy : o.Fx);
        if (force >= target)
        {
            const double share = (target - previous) / std::max(force - previous, 1e-12);
            return (step - 1 + share) * kStep * 20.0;
        }
        previous = force;
    }
    return -1.0;
}
}

BrushTyre::BrushTyre(BrushTyreParameters parameters)
    : m_p(parameters)
{
}

void BrushTyre::Reset()
{
    m_state = {};
    m_bristles[m_kept].length.fill(0.0);
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
        // And the bristles leave the road.
        m_bristles[m_kept].length.fill(0.0);
        m_state.jacobianValid = false;
        BrushTyreOutput out;
        out.effectiveRadius = m_p.unloadedRadius;
        return out;
    }
    const Patch patch = MakePatch(m_p, input);
    const bool stepped = dt > 0.0;
    // The bend the last step left, and two buffers for the solver's iterates.
    const std::array<int, 2> spare{(m_kept + 1) % 3, (m_kept + 2) % 3};
    const Start start{m_state.carcass, stepped ? &m_bristles[m_kept] : nullptr, std::max(dt, 0.0)};
    const std::array<BrushBristles*, 2> buffers{stepped ? &m_bristles[spare[0]] : nullptr, stepped ? &m_bristles[spare[1]] : nullptr};
    const SolveResult s = SolveBalance(m_p, patch, start, m_state.jacobian, m_state.jacobianValid, buffers);
    if (s.converged || ScaledNorm(s.residual.g, patch.load) < 1e-3)
    {
        m_state.carcass = s.c;
        if (stepped)
        {
            m_kept = spare[s.bend];
        }
        else
        {
            // The steady state keeps no bend: the next step starts the bristles afresh.
            m_bristles[m_kept].length.fill(0.0);
        }
    }
    else
    {
        // Not balanced: keep the carcass and bristles where they were and start the Jacobian afresh next step.
        m_state.jacobianValid = false;
    }
    return MakeOutput(m_p, patch, input, s);
}

BrushTyreOutput BrushTyre::Steady(const BrushTyreInput& input) const
{
    if (!(input.load > 0.0))
    {
        BrushTyreOutput out;
        out.effectiveRadius = m_p.unloadedRadius;
        return out;
    }
    const Patch patch = MakePatch(m_p, input);
    std::array<double, 9> jacobian{};
    bool jacobianValid = false;
    const SolveResult s = SolveBalance(m_p, patch, Start{m_state.carcass, nullptr, 0.0}, jacobian, jacobianValid);
    return MakeOutput(m_p, patch, input, s);
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
    const double lateralFriction = std::max(figures.peakFriction, 0.05);
    p.staticFriction = {figures.longitudinalPeakFriction > 0.0 ? std::max(figures.longitudinalPeakFriction, 0.05) : lateralFriction, lateralFriction};
    p.kineticShare = figures.kineticShare > 0.0 ? std::clamp(figures.kineticShare, 0.3, 1.0) : 0.85;
    p.referenceLoad = std::max(figures.referenceLoad, 100.0);
    p.loadExponent = {0.9, 0.9};
    const double angle = std::clamp(figures.peakSlipAngle > 0.0 ? figures.peakSlipAngle : 6.0 * std::numbers::pi / 180.0, 0.02, 0.4);
    // The bristles' fore-aft stiffness over their sideways one: the data's, or fitted to a peak slip ratio below.
    double stiffnessRatio = figures.longitudinalStiffnessRatio > 0.0 ? std::clamp(figures.longitudinalStiffnessRatio, 0.2, 5.0) : 1.0;
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
    // The relaxation lengths the carcass alone is stiffened to: the data's at first, then shortened by the
    // bristles' own lag (below).
    std::array<double, 2> carcassLength{longitudinalRelaxation, lateralRelaxation};
    const auto setStiffness = [&](double cornering)
    {
        const double length = ContactLength(p, deflection);
        const double k = 2.0 * cornering / (p.width * std::max(length * length, 1e-6));
        p.bristleStiffnessY = k;
        p.bristleStiffnessX = stiffnessRatio * k;
        // Fore and aft the carcass's shift leaves the steady slip alone, so C'_x is the bristles' own.
        p.carcassStiffness = {stiffnessRatio * cornering / carcassLength[0], cornering / carcassLength[1],
                              1.5 * cornering * p.unloadedRadius * p.unloadedRadius};
        // Sideways C'_y falls with the carcass's own stiffness: a few rounds settle sigma = C'_y / K_y.
        for (int round = 0; round < 3; ++round)
        {
            p.carcassStiffness[1] = flexibleCornering() / carcassLength[1];
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
    double cornering = 3.0 * p.staticFriction[1] * p.referenceLoad / std::tan(angle);
    setStiffness(cornering);

    // The steady force at 20 m/s and the reference load, across the wheel at a slip angle or along it
    // at a slip ratio.
    const double rollingRadius = p.unloadedRadius - deflection / 3.0;
    const auto lateralForce = [&](const BrushTyre& tyre, double degrees)
    {
        BrushTyreInput in;
        in.load = p.referenceLoad;
        in.forwardVelocity = 20.0;
        in.wheelSpeed = 20.0 / rollingRadius;
        in.lateralVelocity = -20.0 * std::tan(degrees * std::numbers::pi / 180.0);
        return std::abs(tyre.Steady(in).Fy);
    };
    const auto longitudinalForce = [&](const BrushTyre& tyre, double slipRatio)
    {
        BrushTyreInput in;
        in.load = p.referenceLoad;
        in.forwardVelocity = 20.0;
        in.wheelSpeed = 20.0 * (1.0 + slipRatio) / rollingRadius;
        return std::abs(tyre.Steady(in).Fx);
    };

    const auto fitPeaks = [&]()
    {
        // The flexible carcass moves the peak past the brush's own: scale the bristles until the steady
        // lateral force peaks at the angle asked for.
        for (int pass = 0; pass < 6; ++pass)
        {
            const BrushTyre tyre(p);
            const double peak = PeakSlip([&](double degrees) { return lateralForce(tyre, degrees); }, 0.5, 25.0, 0.5) * std::numbers::pi / 180.0;
            const double ratio = std::tan(peak) / std::tan(angle);
            if (std::abs(ratio - 1.0) < 0.02)
            {
                break;
            }
            cornering *= std::clamp(ratio, 0.5, 2.0);
            setStiffness(cornering);
        }

        // Without the data's stiffness ratio, the fore-aft stiffness that puts the longitudinal peak at the
        // slip ratio asked for. Along the wheel the bristles slide throughout at a theoretical slip
        // kappa / (1 + kappa) inversely proportional to their stiffness (3 mu Fz / C_x), as across it.
        if (!(figures.longitudinalStiffnessRatio > 0.0) && figures.peakSlipRatio > 0.0)
        {
            const double target = std::clamp(figures.peakSlipRatio, 0.01, 0.6);
            for (int pass = 0; pass < 8; ++pass)
            {
                const BrushTyre tyre(p);
                const double peak = PeakSlip([&](double slipRatio) { return longitudinalForce(tyre, slipRatio); }, 0.01, 1.0, 0.01);
                const double ratio = (peak / (1.0 + peak)) / (target / (1.0 + target));
                if (std::abs(ratio - 1.0) < 0.02)
                {
                    break;
                }
                stiffnessRatio = std::clamp(stiffnessRatio * std::clamp(ratio, 0.5, 2.0), 0.05, 20.0);
                setStiffness(cornering);
            }
        }
    };

    // Stepped, the bristles keep their bend and take the tread's passage through the patch to build theirs
    // anew (about 0.4 of the contact length to 63 % on their own), on top of the carcass's lag: shorten the
    // carcass's own length until the two together relax over the length asked for.
    const auto fitRelaxation = [&]()
    {
        const std::array<double, 2> target{longitudinalRelaxation, lateralRelaxation};
        for (int axis = 0; axis < 2; ++axis)
        {
            for (int pass = 0; pass < 4; ++pass)
            {
                const double measured = RelaxationDistance(p, axis == 1);
                if (!(measured > 0.0) || std::abs(measured - target[axis]) < 0.01 * target[axis])
                {
                    break;
                }
                carcassLength[axis] = std::max(carcassLength[axis] + (target[axis] - measured), 0.2 * target[axis]);
                setStiffness(cornering);
            }
        }
    };

    // A stiffer carcass moves the lateral peak in a little: fit the peaks, the relaxation, the peaks again.
    fitPeaks();
    fitRelaxation();
    fitPeaks();
    return p;
}

}
