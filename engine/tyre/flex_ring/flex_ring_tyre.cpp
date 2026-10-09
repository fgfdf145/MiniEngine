#include "flex_ring_tyre.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

namespace me::tyre::flexring
{

namespace
{
constexpr double kGravity = 9.81;
// Statics (FTire 6.12: stat_BDF_parameter 3.5, stat_mass_reduction 0.01): a heavily damped implicit
// scheme, and Newton iterations to converge each step on non-smooth contact (cleat edges).
constexpr double kStaticBeta = 3.5;
constexpr int kStaticIterations = 3;
constexpr double kPi = std::numbers::pi;

Mat3 Outer(const Vec3& a)
{
    return glm::outerProduct(a, a);
}

// The belt-rim radial element: linear in tension, progressive in compression, slope
// c (1 + P a^2 / (a^2 + l^2)) at compression a. Returns the spring force along +u and its slope.
void RadialElastic(double u, double c, double progressivity, double scale, double& force, double& slope)
{
    if (u >= 0.0 || progressivity == 0.0)
    {
        force = c * u;
        slope = c;
        return;
    }
    const double a = -u;
    force = -c * (a + progressivity * (a - scale * std::atan(a / scale)));
    slope = c * (1.0 + progressivity * a * a / (a * a + scale * scale));
}

// A spring in series with a dry friction slider (FTire's elasto-plastic element): force, slope, and the
// slider's new position.
double Slider(double u, double stiffness, double limit, double slider0, double& slope, double& slider1)
{
    if (stiffness <= 0.0 || limit <= 0.0)
    {
        slope = 0.0;
        slider1 = slider0;
        return 0.0;
    }
    const double trial = stiffness * (u - slider0);
    if (std::abs(trial) <= limit)
    {
        slope = stiffness;
        slider1 = slider0;
        return trial;
    }
    const double f = trial > 0.0 ? limit : -limit;
    slope = 0.0;
    slider1 = u - f / stiffness;
    return f;
}

// Catmull-Rom weights of the four nodes k-1, k, k+1, k+2 at sigma, and their derivatives.
void CatmullRom(double s, double w[4], double dw[4])
{
    const double s2 = s * s;
    const double s3 = s2 * s;
    w[0] = 0.5 * (-s3 + 2.0 * s2 - s);
    w[1] = 0.5 * (3.0 * s3 - 5.0 * s2 + 2.0);
    w[2] = 0.5 * (-3.0 * s3 + 4.0 * s2 + s);
    w[3] = 0.5 * (s3 - s2);
    dw[0] = 0.5 * (-3.0 * s2 + 4.0 * s - 1.0);
    dw[1] = 0.5 * (9.0 * s2 - 10.0 * s);
    dw[2] = 0.5 * (-9.0 * s2 + 8.0 * s + 1.0);
    dw[3] = 0.5 * (3.0 * s2 - 2.0 * s);
}

// The free-free beam's elastic mode i (0-based) over [-w/2, w/2] before normalization.
double RawShape(int mode, double root, double width, double s, double* slope)
{
    const double beta = root / width;
    const double a = 0.5 * root;
    const double x = beta * s;
    if (mode % 2 == 0)
    {
        // Symmetric: cos(a) cosh(beta s) + cosh(a) cos(beta s).
        if (slope != nullptr)
        {
            *slope = beta * (std::cos(a) * std::sinh(x) - std::cosh(a) * std::sin(x));
        }
        return std::cos(a) * std::cosh(x) + std::cosh(a) * std::cos(x);
    }
    // Antisymmetric: sin(a) sinh(beta s) + sinh(a) sin(beta s).
    if (slope != nullptr)
    {
        *slope = beta * (std::sin(a) * std::cosh(x) + std::sinh(a) * std::cos(x));
    }
    return std::sin(a) * std::sinh(x) + std::sinh(a) * std::sin(x);
}

// Solves the cyclic tridiagonal system diag_k x_k + off (x_{k-1} + x_{k+1}) = r_k in place
// (Sherman-Morrison on the corner terms).
void SolveCyclicTridiagonal(const std::vector<double>& diag, double off, std::vector<double>& r, std::vector<double>& work)
{
    const int n = static_cast<int>(r.size());
    if (off == 0.0 || n < 3)
    {
        for (int i = 0; i < n; ++i)
        {
            r[static_cast<size_t>(i)] /= diag[static_cast<size_t>(i)];
        }
        return;
    }
    // A = T + u v^T, u = (gamma, 0, .., off), v = (1, 0, .., off / gamma), T's first and last diagonals
    // reduced by gamma and off^2 / gamma.
    const double gamma = -diag[0];
    work.assign(static_cast<size_t>(4 * n), 0.0);
    double* cp = work.data();
    double* y = work.data() + n;
    double* z = work.data() + 2 * n;
    double* u = work.data() + 3 * n;
    u[0] = gamma;
    u[n - 1] = off;
    const auto solve = [&](const double* rhs, double* out) {
        double b = diag[0] - gamma;
        cp[0] = off / b;
        out[0] = rhs[0] / b;
        for (int i = 1; i < n; ++i)
        {
            const double di = i == n - 1 ? diag[static_cast<size_t>(i)] - off * off / gamma : diag[static_cast<size_t>(i)];
            b = di - off * cp[i - 1];
            cp[i] = off / b;
            out[i] = (rhs[i] - off * out[i - 1]) / b;
        }
        for (int i = n - 2; i >= 0; --i)
        {
            out[i] -= cp[i] * out[i + 1];
        }
    };
    solve(r.data(), y);
    solve(u, z);
    const double vy = y[0] + off / gamma * y[n - 1];
    const double vz = z[0] + off / gamma * z[n - 1];
    const double f = vy / (1.0 + vz);
    for (int i = 0; i < n; ++i)
    {
        r[static_cast<size_t>(i)] = y[i] - f * z[i];
    }
}
}

double FrictionTable::Mu(double slidingSpeed, double groundPressure) const
{
    const double p = std::clamp(groundPressure, pressure[0], pressure[2]);
    // Quadratic (Lagrange) in pressure through the three pressures.
    const double p0 = pressure[0];
    const double p1 = pressure[1];
    const double p2 = pressure[2];
    const double l0 = (p - p1) * (p - p2) / ((p0 - p1) * (p0 - p2));
    const double l1 = (p - p0) * (p - p2) / ((p1 - p0) * (p1 - p2));
    const double l2 = (p - p0) * (p - p1) / ((p2 - p0) * (p2 - p1));
    const double v = std::max(slidingSpeed, 0.0);
    int i = 0;
    while (i < 3 && v > speed[i + 1])
    {
        ++i;
    }
    const auto at = [&](int s) {
        return std::max(l0 * mu[0][s] + l1 * mu[1][s] + l2 * mu[2][s], 0.0);
    };
    if (i >= 3)
    {
        return at(3);
    }
    const double t = (v - speed[i]) / std::max(speed[i + 1] - speed[i], 1.0e-12);
    return at(i) + (at(i + 1) - at(i)) * t;
}

FlexRingTyre::FlexRingTyre(FlexRingParameters parameters)
    : m_p(std::move(parameters))
{
    m_n = std::max(m_p.segments, 6);
    m_p.segments = m_n;
    m_pressure = m_p.measuredPressure;
    m_treadDepth = m_p.treadDepth;
    m_localRest.resize(m_n);
    m_localRadial.resize(m_n);
    m_localTangential.resize(m_n);
    m_perm.resize(m_n);
    for (int k = 0; k < m_n; ++k)
    {
        const double phi = 2.0 * kPi * k / m_n;
        m_localRadial[k] = Vec3(std::sin(phi), 0.0, -std::cos(phi));
        m_localTangential[k] = Vec3(std::cos(phi), 0.0, std::sin(phi));
        m_localRest[k] = m_p.beltRadius * m_localRadial[k];
        m_perm[k] = k < (m_n + 1) / 2 ? 2 * k : 2 * (m_n - 1 - k) + 1;
    }
    // The band: every coupling reaches at most three nodes along the ring (a block's four Catmull-Rom
    // nodes).
    int band = 0;
    for (int k = 0; k < m_n; ++k)
    {
        for (int d = 1; d <= 3; ++d)
        {
            band = std::max(band, std::abs(m_perm[k] - m_perm[(k + d) % m_n]));
        }
    }
    m_matrix.Resize(4 * m_n, 4 * band + 3);

    // Lateral bending shapes at the blocks.
    const int modes = static_cast<int>(m_p.bendShapeRoots.size());
    m_bendNorm.assign(static_cast<size_t>(modes), 1.0);
    for (int i = 0; i < modes; ++i)
    {
        constexpr int kSamples = 2000;
        double sum = 0.0;
        for (int j = 0; j <= kSamples; ++j)
        {
            const double s = -0.5 * m_p.beltWidth + m_p.beltWidth * j / kSamples;
            const double f = RawShape(i, m_p.bendShapeRoots[i], m_p.beltWidth, s, nullptr);
            sum += (j == 0 || j == kSamples ? 0.5 : 1.0) * f * f;
        }
        m_bendNorm[i] = 1.0 / std::sqrt(sum * m_p.beltWidth / kSamples);
    }
    const size_t nb = m_p.blocks.size();
    m_shape.assign(nb * modes, 0.0);
    m_shapeSlope.assign(nb * modes, 0.0);
    m_crownSlope.assign(nb, 0.0);
    for (size_t b = 0; b < nb; ++b)
    {
        const double s = std::clamp(m_p.blocks[b].lateral, -0.5 * m_p.beltWidth, 0.5 * m_p.beltWidth);
        for (int i = 0; i < modes; ++i)
        {
            m_shape[b * modes + i] = BendShape(i, s, &m_shapeSlope[b * modes + i]);
        }
        // The crown z0 = -(R - sqrt(R^2 - s^2)) and its slope.
        const double radius = m_p.latCurvatureRadius;
        const double lateral = m_p.blocks[b].lateral;
        if (radius > std::abs(lateral))
        {
            m_crownSlope[b] = -lateral / std::sqrt(radius * radius - lateral * lateral);
        }
    }

    m_x.assign(m_n, Vec3(0.0));
    m_v.assign(m_n, Vec3(0.0));
    m_psi.assign(m_n, 0.0);
    m_psiDot.assign(m_n, 0.0);
    m_maxwell.assign(m_n, {0.0, 0.0, 0.0});
    m_slider.assign(m_n, {});
    m_maxwellNew = m_maxwell;
    m_sliderNew = m_slider;
    m_gamma.assign(static_cast<size_t>(modes) * m_n, 0.0);
    m_bendLoad.assign(m_gamma.size(), 0.0);
    m_bendLoadNew.assign(m_gamma.size(), 0.0);
    m_bendStiff.assign(m_gamma.size(), 0.0);
    m_bendStiffNew.assign(m_gamma.size(), 0.0);
    m_bendLambda.assign(static_cast<size_t>(modes), 0.0);
    for (int i = 0; i < modes; ++i)
    {
        const double beta = m_p.bendShapeRoots[i] / m_p.beltWidth;
        m_bendLambda[i] = m_p.lateralBendStiffness * beta * beta * beta * beta;
    }
    m_memory.assign(static_cast<size_t>(m_n) * nb, BlockMemory{});
    m_memoryNew = m_memory;
    m_blocks.assign(m_memory.size(), BlockView{});
    m_force.assign(m_n, Vec3(0.0));
    m_torque.assign(m_n, 0.0);
    m_force0 = m_force;
    m_torque0 = m_torque;
    m_rhs.assign(static_cast<size_t>(4 * m_n), 0.0);
    m_frame.attach.resize(m_n);
    m_frame.attachVelocity.resize(m_n);
    m_frame.radial.resize(m_n);
    m_frame.tangential.resize(m_n);
    m_stepFrame = m_frame;
}

double FlexRingTyre::BendShape(int mode, double lateral, double* slope) const
{
    if (mode < 0 || mode >= static_cast<int>(m_p.bendShapeRoots.size()))
    {
        if (slope != nullptr)
        {
            *slope = 0.0;
        }
        return 0.0;
    }
    const double s = std::clamp(lateral, -0.5 * m_p.beltWidth, 0.5 * m_p.beltWidth);
    double d = 0.0;
    const double f = RawShape(mode, m_p.bendShapeRoots[mode], m_p.beltWidth, s, &d);
    if (slope != nullptr)
    {
        *slope = d * m_bendNorm[mode];
    }
    return f * m_bendNorm[mode];
}

void FlexRingTyre::SetRoad(const Road* road)
{
    m_road = road;
}

void FlexRingTyre::SetPressure(double pascal)
{
    m_pressure = std::max(pascal, 1.0);
}

void FlexRingTyre::SetTreadDepth(double metres)
{
    m_treadDepth = std::max(metres, 0.0);
}

void FlexRingTyre::BuildFrame(const RimState& rim, Frame& frame) const
{
    frame.centre = rim.position;
    frame.velocity = rim.velocity;
    frame.omega = rim.angularVelocity;
    frame.rotation = rim.rotation;
    frame.axis = glm::normalize(rim.rotation[1]);
    frame.attach.resize(m_n);
    frame.attachVelocity.resize(m_n);
    frame.radial.resize(m_n);
    frame.tangential.resize(m_n);
    for (int k = 0; k < m_n; ++k)
    {
        const Vec3 arm = rim.rotation * m_localRest[k];
        frame.attach[k] = rim.position + arm;
        frame.attachVelocity[k] = rim.velocity + glm::cross(rim.angularVelocity, arm);
        frame.radial[k] = rim.rotation * m_localRadial[k];
        frame.tangential[k] = rim.rotation * m_localTangential[k];
    }
}

Vec3 FlexRingTyre::NodeRestPosition(int node) const
{
    return m_frame.attach[static_cast<size_t>(node)];
}

RimState FlexRingTyre::Interpolate(const RimState& a, const RimState& b, double dt, double s) const
{
    RimState r;
    const double s2 = s * s;
    const double s3 = s2 * s;
    const double h00 = 2.0 * s3 - 3.0 * s2 + 1.0;
    const double h10 = s3 - 2.0 * s2 + s;
    const double h01 = -2.0 * s3 + 3.0 * s2;
    const double h11 = s3 - s2;
    r.position = h00 * a.position + h10 * dt * a.velocity + h01 * b.position + h11 * dt * b.velocity;
    const double d00 = 6.0 * s2 - 6.0 * s;
    const double d10 = 3.0 * s2 - 4.0 * s + 1.0;
    const double d01 = -6.0 * s2 + 6.0 * s;
    const double d11 = 3.0 * s2 - 2.0 * s;
    r.velocity = dt > 0.0 ? (d00 * a.position + d10 * dt * a.velocity + d01 * b.position + d11 * dt * b.velocity) / dt : b.velocity;
    const glm::dquat qa = glm::quat_cast(a.rotation);
    const glm::dquat qb = glm::quat_cast(b.rotation);
    r.rotation = glm::mat3_cast(glm::slerp(qa, qb, s));
    r.angularVelocity = a.angularVelocity + (b.angularVelocity - a.angularVelocity) * s;
    return r;
}

void FlexRingTyre::Reset(const RimState& rim)
{
    m_rim = rim;
    BuildFrame(rim, m_frame);
    for (int k = 0; k < m_n; ++k)
    {
        m_x[k] = m_frame.attach[k];
        m_v[k] = m_frame.attachVelocity[k];
        m_psi[k] = 0.0;
        m_psiDot[k] = 0.0;
        m_maxwell[k] = {0.0, 0.0, 0.0};
        m_slider[k] = {};
    }
    std::fill(m_gamma.begin(), m_gamma.end(), 0.0);
    std::fill(m_bendLoad.begin(), m_bendLoad.end(), 0.0);
    std::fill(m_bendStiff.begin(), m_bendStiff.end(), 0.0);
    std::fill(m_memory.begin(), m_memory.end(), BlockMemory{});
    std::fill(m_blocks.begin(), m_blocks.end(), BlockView{});
    m_time = 0.0;
    m_initialized = true;
    m_factorValid = false;
    EvalSettings settings;
    settings.commit = true;
    settings.h = m_p.maxStep;
    Evaluate(m_x, m_v, m_psi, m_psiDot, m_frame, settings, 1.0);
    m_force0 = m_force;
    m_torque0 = m_torque;
    m_wrench = m_evalWrench;
}

void FlexRingTyre::AddBlock3(int nodeA, int nodeB, const Mat3& m, double coefficient)
{
    if (coefficient == 0.0)
    {
        return;
    }
    if (nodeA == nodeB)
    {
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j <= i; ++j)
            {
                m_matrix.Add(Dof(nodeA, i), Dof(nodeA, j), coefficient * m[j][i]);
            }
        }
        return;
    }
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            m_matrix.Add(Dof(nodeA, i), Dof(nodeB, j), coefficient * m[j][i]);
        }
    }
}

void FlexRingTyre::AddScalar(int nodeA, int dofA, int nodeB, int dofB, double value)
{
    m_matrix.Add(Dof(nodeA, dofA), Dof(nodeB, dofB), value);
}

void FlexRingTyre::Evaluate(const std::vector<Vec3>& x, const std::vector<Vec3>& v, const std::vector<double>& psi, const std::vector<double>& psiDot,
                            const Frame& frame, const EvalSettings& settings, double massScale)
{
    const FlexRingParameters& p = m_p;
    const int n = m_n;
    const double pr = PressureRatio();
    const double sRad = 0.2 + 0.8 * pr;
    const double sTang = 0.2 + 0.8 * pr;
    const double sLat = 0.1 + 0.9 * pr;
    const double sBendIn = 0.9 + 0.1 * pr;
    const double sBendOut = 0.1 + 0.9 * pr;
    const double sTors = pr;
    const double c1 = settings.h * settings.beta;
    const double c2 = c1 * c1;
    const bool assemble = settings.assemble;
    const Vec3 gravity = (settings.gravity && p.gravity) ? Vec3(0.0, 0.0, -kGravity * p.nodeMass) : Vec3(0.0);

    Wrench wrench;
    for (int k = 0; k < n; ++k)
    {
        m_force[k] = gravity;
        m_torque[k] = 0.0;
    }
    if (assemble)
    {
        m_matrix.SetZero();
        for (int k = 0; k < n; ++k)
        {
            for (int c = 0; c < 3; ++c)
            {
                AddScalar(k, c, k, c, p.nodeMass * massScale);
            }
            AddScalar(k, 3, k, 3, p.nodeInertia * massScale);
        }
    }

    // Foundation: radial, tangential and lateral elements and the torsion spring, node to rim.
    const double cr = p.radialStiffness * sRad;
    const double ct = p.tangentialStiffness * sTang;
    const double cy = p.lateralStiffness * sLat;
    const double cpsi = p.torsionStiffness * sTors;
    const double lambda = p.torsionLateralCoupling;
    const double maxwellA = settings.h / std::max(p.maxwellTime, 1.0e-9);
    const std::array<double, 3> maxwellC = {p.maxwellStiffness[0] * sRad, p.maxwellStiffness[1] * sTang, p.maxwellStiffness[2] * sLat};
    for (int k = 0; k < n; ++k)
    {
        const Vec3& r = frame.radial[k];
        const Vec3& t = frame.tangential[k];
        const Vec3& y = frame.axis;
        const Vec3 d = x[k] - frame.attach[k];
        const Vec3 vrel = v[k] - frame.attachVelocity[k];
        const double ur = glm::dot(d, r);
        const double ut = glm::dot(d, t);
        const double uy = glm::dot(d, y);
        const double urd = glm::dot(vrel, r) + glm::dot(d, glm::cross(frame.omega, r));
        const double utd = glm::dot(vrel, t) + glm::dot(d, glm::cross(frame.omega, t));
        const double uyd = glm::dot(vrel, y) + glm::dot(d, glm::cross(frame.omega, y));

        double fr = 0.0;
        double kr = 0.0;
        RadialElastic(ur, cr, p.radialProgressivity, p.radialProgressionScale, fr, kr);
        if (ur < -p.flangeClearance)
        {
            fr += p.flangeStiffness * (ur + p.flangeClearance);
            kr += p.flangeStiffness;
        }
        double ft = ct * ut;
        double kt = ct;
        double fy = cy * uy;
        double ky = cy;
        if (settings.internalElements)
        {
            const std::array<double, 3> u = {ur, ut, uy};
            std::array<double, 3> fm{};
            std::array<double, 3> km{};
            for (int c = 0; c < 3; ++c)
            {
                if (maxwellC[c] > 0.0)
                {
                    const double z0 = m_maxwell[k][c];
                    fm[c] = maxwellC[c] * (u[c] - z0) / (1.0 + maxwellA);
                    km[c] = maxwellC[c] / (1.0 + maxwellA);
                    m_maxwellNew[k][c] = (z0 + maxwellA * u[c]) / (1.0 + maxwellA);
                }
            }
            fr += fm[0];
            kr += km[0];
            ft += fm[1];
            kt += km[1];
            fy += fm[2];
            ky += km[2];
            double slope = 0.0;
            for (int e = 0; e < 5; ++e)
            {
                fr += Slider(ur, p.radialHysteresisStiffness[e] * sRad, p.radialHysteresisForce[e], m_slider[k][e], slope, m_sliderNew[k][e]);
                kr += slope;
            }
            ft += Slider(ut, p.tangentialHysteresisStiffness * sTang, p.tangentialHysteresisForce, m_slider[k][5], slope, m_sliderNew[k][5]);
            kt += slope;
            fy += Slider(uy, p.lateralHysteresisStiffness * sLat, p.lateralHysteresisForce, m_slider[k][6], slope, m_sliderNew[k][6]);
            ky += slope;
        }
        fr += p.radialDamping * urd;
        ft += p.tangentialDamping * utd;
        fy += p.lateralDamping * uyd;
        // Torsion to the rim, coupled to the lateral displacement: energy cpsi/2 (psi - lambda uy)^2.
        const double twistToRim = psi[k] - lambda * uy;
        const double mpsi = -(cpsi * twistToRim + p.torsionDamping * psiDot[k]);
        fy -= cpsi * lambda * twistToRim;

        const Vec3 foundation = -(fr * r + ft * t + fy * y);
        m_force[k] += foundation;
        m_torque[k] += mpsi;
        wrench.force -= foundation;
        wrench.moment -= glm::cross(x[k] - frame.centre, foundation);
        wrench.moment -= mpsi * t;

        if (assemble)
        {
            const Mat3 kMat = kr * Outer(r) + kt * Outer(t) + (ky + cpsi * lambda * lambda) * Outer(y);
            const Mat3 dMat = p.radialDamping * Outer(r) + p.tangentialDamping * Outer(t) + p.lateralDamping * Outer(y);
            AddBlock3(k, k, c2 * kMat + c1 * dMat, 1.0);
            AddScalar(k, 3, k, 3, c2 * cpsi + c1 * p.torsionDamping);
            if (lambda != 0.0)
            {
                for (int c = 0; c < 3; ++c)
                {
                    AddScalar(k, 3, k, c, -c2 * cpsi * lambda * y[c]);
                }
            }
        }
    }

    // Twist between adjacent segments and the belt's extension springs.
    const double ktw = p.twistStiffness * sTors;
    for (int k = 0; k < n; ++k)
    {
        const int j = (k + 1) % n;
        const double mtw = -(ktw * (psi[k] - psi[j]) + p.twistDamping * (psiDot[k] - psiDot[j]));
        m_torque[k] += mtw;
        m_torque[j] -= mtw;
        const Vec3 e = x[j] - x[k];
        const double length = glm::length(e);
        const Vec3 u = e / std::max(length, 1.0e-12);
        const double rate = glm::dot(v[j] - v[k], u);
        const double elastic = p.chordStiffness * (length - p.chordRestLength);
        const double tension = elastic + p.chordDamping * rate;
        m_force[k] += tension * u;
        m_force[j] -= tension * u;
        if (assemble)
        {
            AddScalar(k, 3, k, 3, c2 * ktw + c1 * p.twistDamping);
            AddScalar(j, 3, j, 3, c2 * ktw + c1 * p.twistDamping);
            AddScalar(k, 3, j, 3, -(c2 * ktw + c1 * p.twistDamping));
            Mat3 kMat = p.chordStiffness * Outer(u);
            if (elastic > 0.0)
            {
                kMat += elastic / std::max(length, 1.0e-12) * (Mat3(1.0) - Outer(u));
            }
            const Mat3 m = c2 * kMat + c1 * p.chordDamping * Outer(u);
            AddBlock3(k, k, m, 1.0);
            AddBlock3(j, j, m, 1.0);
            AddBlock3(k, j, m, -1.0);
        }
    }

    // In-plane and out-of-plane bending on the second difference of the node positions.
    const double cbi = p.bendInStiffness * sBendIn;
    const double cbo = p.bendOutStiffness * sBendOut;
    if (cbi > 0.0 || cbo > 0.0)
    {
        for (int k = 0; k < n; ++k)
        {
            const int a = (k + n - 1) % n;
            const int b = (k + 1) % n;
            // The bending strains are the second difference's components along the segment's in-plane
            // normal and lateral axis less the unloaded ring's, the axes turned with the segment's torsion
            // as far as the coupling says (FTire belt_torsion_oop_bend_coupl; with 1 a rigid tilt of the belt
            // bends nothing).
            const Vec3 raw = x[a] - 2.0 * x[k] + x[b];
            const Vec3 rest = frame.attach[a] - 2.0 * frame.attach[k] + frame.attach[b];
            Vec3 nIn = frame.radial[k];
            Vec3 nOut = frame.axis;
            if (p.torsionBendCoupling != 0.0)
            {
                const double angle = p.torsionBendCoupling * psi[k];
                const Vec3 r0 = nIn;
                nIn = r0 * std::cos(angle) + frame.axis * std::sin(angle);
                nOut = frame.axis * std::cos(angle) - r0 * std::sin(angle);
            }
            const double strainIn = glm::dot(raw, nIn) - glm::dot(rest, frame.radial[k]);
            const double strainOut = glm::dot(raw, nOut) - glm::dot(rest, frame.axis);
            const Vec3 g = cbi * strainIn * nIn + cbo * strainOut * nOut;
            m_force[a] -= g;
            m_force[k] += 2.0 * g;
            m_force[b] -= g;
            wrench.moment += glm::cross(raw, g);
            // The strains' dependence on the torsion angle: d nIn / d psi = f nOut, d nOut / d psi = -f nIn.
            const double f = p.torsionBendCoupling;
            const double dIn = f * glm::dot(raw, nOut);
            const double dOut = -f * glm::dot(raw, nIn);
            if (f != 0.0)
            {
                const double torque = -(cbi * strainIn * dIn + cbo * strainOut * dOut);
                m_torque[k] += torque;
                wrench.moment -= torque * frame.tangential[k];
            }
            if (assemble)
            {
                const Mat3 bMat = c2 * (cbi * Outer(nIn) + cbo * Outer(nOut));
                AddBlock3(a, a, bMat, 1.0);
                AddBlock3(k, k, bMat, 4.0);
                AddBlock3(b, b, bMat, 1.0);
                AddBlock3(a, k, bMat, -2.0);
                AddBlock3(k, b, bMat, -2.0);
                AddBlock3(a, b, bMat, 1.0);
                if (f != 0.0)
                {
                    // K = J^T C J with the strains' gradients: positions (1, -2, 1) along the axes, torsion dIn, dOut.
                    const Vec3 cross = c2 * (cbi * dIn * nIn + cbo * dOut * nOut);
                    const int nodes[3] = {a, k, b};
                    const double weight[3] = {1.0, -2.0, 1.0};
                    for (int j = 0; j < 3; ++j)
                    {
                        for (int comp = 0; comp < 3; ++comp)
                        {
                            AddScalar(nodes[j], comp, k, 3, weight[j] * cross[comp]);
                        }
                    }
                    AddScalar(k, 3, k, 3, c2 * (cbi * dIn * dIn + cbo * dOut * dOut));
                }
            }
        }
    }

    // Inflation pressure: normal to the chord of the two neighbours, in the rim plane (FTire 6.3).
    const double pressureForce = p.pressureForce * pr;
    if (pressureForce != 0.0)
    {
        for (int k = 0; k < n; ++k)
        {
            Vec3 c = x[(k + 1) % n] - x[(k + n - 1) % n];
            c -= glm::dot(c, frame.axis) * frame.axis;
            const Vec3 normal = glm::normalize(glm::cross(frame.axis, c));
            const Vec3 f = pressureForce * normal;
            m_force[k] += f;
            wrench.force -= f;
            wrench.moment -= glm::cross(x[k] - frame.centre, f);
        }
    }

    m_evalWrench = wrench;
    if (settings.contact && m_road != nullptr && !p.blocks.empty())
    {
        EvaluateContact(x, v, psi, psiDot, frame, settings);
    }
    else
    {
        m_evalContact = ContactStats{};
        std::fill(m_bendLoadNew.begin(), m_bendLoadNew.end(), 0.0);
        std::fill(m_bendStiffNew.begin(), m_bendStiffNew.end(), 0.0);
        if (settings.commit)
        {
            for (BlockMemory& m : m_memoryNew)
            {
                m.contact = false;
                m.sliding = false;
            }
            for (BlockView& b : m_blocks)
            {
                b.contact = false;
                b.sliding = false;
            }
        }
    }

    if (settings.commit)
    {
        m_maxwell = m_maxwellNew;
        m_slider = m_sliderNew;
        m_memory = m_memoryNew;
        m_bendLoad = m_bendLoadNew;
        m_bendStiff = m_bendStiffNew;
        m_contact = m_evalContact;
    }
}

void FlexRingTyre::EvaluateContact(const std::vector<Vec3>& x, const std::vector<Vec3>& v, const std::vector<double>& psi, const std::vector<double>& psiDot,
                                   const Frame& frame, const EvalSettings& settings)
{
    const FlexRingParameters& p = m_p;
    const int n = m_n;
    const int nb = static_cast<int>(p.blocks.size());
    const int modes = static_cast<int>(p.bendShapeRoots.size());
    const double h = settings.h;
    const bool assemble = settings.assemble;
    const double c1 = settings.h * settings.beta;
    const double c2 = c1 * c1;
    const Road& road = *m_road;
    const double tau = p.treadDampingTime;
    const double maxTread = m_treadDepth + p.treadBase;
    const double reach = maxTread + 0.5 * p.treadWidth * 0.6 + 0.01;
    const double lowest = frame.centre.z - p.outerRadius;
    const double bound = 2.0 * p.outerRadius * p.contactBound;
    const Vec3 up(0.0, 0.0, 1.0);
    Vec3 heading = glm::cross(frame.axis, up);
    heading = glm::dot(heading, heading) > 1.0e-12 ? glm::normalize(heading) : Vec3(1.0, 0.0, 0.0);

    std::fill(m_bendLoadNew.begin(), m_bendLoadNew.end(), 0.0);
    std::fill(m_bendStiffNew.begin(), m_bendStiffNew.end(), 0.0);
    ContactStats stats;
    double minHeading = 1.0e300;
    double maxHeading = -1.0e300;
    double minLateral = 1.0e300;
    double maxLateral = -1.0e300;
    Vec3 weighted(0.0);

    for (int k = 0; k < n; ++k)
    {
        const int km = (k + n - 1) % n;
        const int k1 = (k + 1) % n;
        const int k2 = (k + 2) % n;
        // Quick test (FTire: the nearest belt nodes): both nodes high above the highest road under the
        // segment, or above the contact processor's bound.
        bool active = true;
        if (std::min(x[k].z, x[k1].z) - lowest > bound)
        {
            active = false;
        }
        else
        {
            const double x0 = std::min(x[k].x, x[k1].x) - reach;
            const double x1 = std::max(x[k].x, x[k1].x) + reach;
            const double y0 = std::min(x[k].y, x[k1].y) - reach;
            const double y1 = std::max(x[k].y, x[k1].y) + reach;
            double low = 0.0;
            double high = 0.0;
            road.HeightRange(x0, y0, x1, y1, low, high);
            if (std::min(x[k].z, x[k1].z) - high > reach)
            {
                active = false;
            }
        }
        if (!active)
        {
            for (int b = 0; b < nb; ++b)
            {
                const size_t index = static_cast<size_t>(k) * nb + b;
                m_memoryNew[index].contact = false;
                m_memoryNew[index].sliding = false;
                if (settings.commit)
                {
                    m_blocks[index].contact = false;
                    m_blocks[index].sliding = false;
                }
            }
            continue;
        }

        const Vec3 nodes[4] = {x[km], x[k], x[k1], x[k2]};
        const Vec3 nodeV[4] = {v[km], v[k], v[k1], v[k2]};
        const int nodeIndex[4] = {km, k, k1, k2};
        for (int b = 0; b < nb; ++b)
        {
            const BlockLayout& layout = p.blocks[b];
            const size_t index = static_cast<size_t>(k) * nb + b;
            const double sg = layout.sigma;
            double w[4];
            double dw[4];
            CatmullRom(sg, w, dw);
            const Vec3 centre = w[0] * nodes[0] + w[1] * nodes[1] + w[2] * nodes[2] + w[3] * nodes[3];
            const Vec3 centreV = w[0] * nodeV[0] + w[1] * nodeV[1] + w[2] * nodeV[2] + w[3] * nodeV[3];
            Vec3 tc = dw[0] * nodes[0] + dw[1] * nodes[1] + dw[2] * nodes[2] + dw[3] * nodes[3];
            tc = glm::normalize(tc);
            Vec3 lat = frame.axis - glm::dot(frame.axis, tc) * tc;
            lat = glm::normalize(lat);
            Vec3 out = glm::cross(lat, tc);
            const double torsion = (1.0 - sg) * psi[k] + sg * psi[k1];
            const double torsionRate = (1.0 - sg) * psiDot[k] + sg * psiDot[k1];
            const double cs = std::cos(torsion);
            const double sn = std::sin(torsion);
            const Vec3 latR = lat * cs - out * sn;
            const Vec3 outR = out * cs + lat * sn;
            double z = layout.crownDrop;
            double dz = m_crownSlope[b];
            for (int i = 0; i < modes; ++i)
            {
                const double g = (1.0 - sg) * m_gamma[static_cast<size_t>(i) * n + k] + sg * m_gamma[static_cast<size_t>(i) * n + k1];
                z += g * m_shape[static_cast<size_t>(b) * modes + i];
                dz += g * m_shapeSlope[static_cast<size_t>(b) * modes + i];
            }
            const Vec3 normal = glm::normalize(outR - dz * latR);
            const double height = m_treadDepth * layout.depthShare + p.treadBase;
            const Vec3 base = centre + layout.lateral * latR + z * outR + height * normal;

            const double roadZ = road.Height(base.x, base.y);
            if (base.z >= roadZ)
            {
                m_memoryNew[index].contact = false;
                m_memoryNew[index].sliding = false;
                if (settings.commit)
                {
                    BlockView& view = m_blocks[index];
                    view = BlockView{};
                    view.base = base;
                    view.tip = base;
                }
                continue;
            }
            // The road's tangent plane: from three heights around the block, or the belt normal.
            Vec3 roadN;
            Vec3 roadP;
            if (p.highPrecisionPlane)
            {
                constexpr double e = 0.002;
                const Vec3 a(base.x - 0.5 * e, base.y - 0.2887 * e, 0.0);
                const Vec3 bb(base.x + 0.5 * e, base.y - 0.2887 * e, 0.0);
                const Vec3 c(base.x, base.y + 0.5774 * e, 0.0);
                const Vec3 pa(a.x, a.y, road.Height(a.x, a.y));
                const Vec3 pb(bb.x, bb.y, road.Height(bb.x, bb.y));
                const Vec3 pc(c.x, c.y, road.Height(c.x, c.y));
                roadN = glm::cross(pb - pa, pc - pa);
                const double len = glm::length(roadN);
                roadN = len > 1.0e-15 ? roadN / len : up;
                if (roadN.z < 0.0)
                {
                    roadN = -roadN;
                }
                roadP = (pa + pb + pc) / 3.0;
            }
            else
            {
                roadN = -normal;
                if (roadN.z < 0.1)
                {
                    roadN = up;
                }
                roadP = Vec3(base.x, base.y, roadZ);
            }
            const double penetration = glm::dot(roadP - base, roadN);
            if (penetration <= 0.0)
            {
                m_memoryNew[index].contact = false;
                m_memoryNew[index].sliding = false;
                if (settings.commit)
                {
                    BlockView& view = m_blocks[index];
                    view = BlockView{};
                    view.base = base;
                    view.tip = base;
                }
                continue;
            }
            const Vec3 roadV = road.SurfaceVelocity(base.x, base.y);
            const Vec3 baseV = centreV + torsionRate * glm::cross(tc, base - centre);
            const double penetrationRate = -glm::dot(baseV - roadV, roadN);

            // Normal force: tread compression stiffness (FTire 6.2) with its progression and damping.
            const double area = p.blockArea;
            const double cn = p.treadPositive * area * p.treadModulus / std::max(height, 1.0e-4);
            const double prog = p.treadProgressivity / std::max(height, 1.0e-4);
            const double elastic = cn * (penetration + 0.5 * prog * penetration * penetration);
            const double kn = cn * (1.0 + prog * penetration);
            const double dn = tau * cn;
            double normalForce = elastic + dn * penetrationRate;
            bool dampingActive = true;
            if (normalForce <= 0.0)
            {
                normalForce = 0.0;
                dampingActive = false;
            }
            const double groundPressure = normalForce / std::max(p.treadPositive * area, 1.0e-12);
            const double frictionScale = m_frictionScale * road.FrictionScale(base.x, base.y);

            // Tangential: the block's tip on the road (Gipser 2004, fig. 9).
            const BlockMemory& memory = m_memory[index];
            const Vec3 onPlane = base + penetration * roadN;
            Vec3 tipStick = memory.contact ? memory.tip + roadV * h : onPlane;
            tipStick -= glm::dot(tipStick - onPlane, roadN) * roadN;
            Vec3 lDir = tc - glm::dot(tc, roadN) * roadN;
            lDir = glm::normalize(lDir);
            Vec3 qDir = glm::cross(roadN, lDir);
            const Vec3 deflection = tipStick - onPlane;
            const Vec3 slip = baseV - roadV;
            const double shear = p.shearFactor * cn;
            const double cl = shear;
            const double cq = shear * p.lateralShearRatio;
            const double el = glm::dot(deflection, lDir);
            const double eq = glm::dot(deflection, qDir);
            const double wl = glm::dot(slip, lDir);
            const double wq = glm::dot(slip, qDir);
            const double bl = cl * el - tau * cl * wl;
            const double bq = cq * eq - tau * cq * wq;
            const double bNorm = std::hypot(bl, bq);
            const double muStick = p.friction.Mu(0.0, groundPressure) * frictionScale;
            double fl = bl;
            double fq = bq;
            double slideSpeed = 0.0;
            double mu = muStick;
            Vec3 tipNew = tipStick;
            bool sliding = false;
            if (bNorm > muStick * normalForce)
            {
                sliding = true;
                const double al = cl * (h + tau);
                const double aq = cq * (h + tau);
                // Sliding speed lambda: sum (b_i / (a_i lambda + mu(lambda) Fn))^2 = 1, smallest root.
                const auto g = [&](double lam) {
                    const double m = p.friction.Mu(lam, groundPressure) * frictionScale * normalForce;
                    const double tl = bl / (al * lam + m);
                    const double tq = bq / (aq * lam + m);
                    return tl * tl + tq * tq - 1.0;
                };
                double lo = 0.0;
                double hi = bNorm / std::max(std::min(al, aq), 1.0e-12);
                if (al == aq)
                {
                    // Isotropic: closed form within each linear piece of mu(lambda).
                    const FrictionTable& ft = p.friction;
                    double found = -1.0;
                    for (int piece = 0; piece < 4 && found < 0.0; ++piece)
                    {
                        const double va = ft.speed[piece];
                        const double vb = piece < 3 ? ft.speed[piece + 1] : 1.0e300;
                        const double ma = ft.Mu(va, groundPressure) * frictionScale * normalForce;
                        const double mb = piece < 3 ? ft.Mu(vb, groundPressure) * frictionScale * normalForce : ma;
                        const double slope = piece < 3 ? (mb - ma) / std::max(vb - va, 1.0e-12) : 0.0;
                        // al lam + ma + slope (lam - va) = |b|
                        const double denom = al + slope;
                        if (std::abs(denom) < 1.0e-15)
                        {
                            continue;
                        }
                        const double lam = (bNorm - ma + slope * va) / denom;
                        if (lam >= va - 1.0e-12 && lam <= vb + 1.0e-12 && lam >= 0.0)
                        {
                            found = lam;
                        }
                    }
                    if (found >= 0.0)
                    {
                        lo = hi = found;
                    }
                }
                if (lo != hi)
                {
                    for (int it = 0; it < 60; ++it)
                    {
                        const double mid = 0.5 * (lo + hi);
                        if (g(mid) > 0.0)
                        {
                            lo = mid;
                        }
                        else
                        {
                            hi = mid;
                        }
                    }
                }
                const double lam = 0.5 * (lo + hi);
                mu = p.friction.Mu(lam, groundPressure) * frictionScale;
                const double m = mu * normalForce;
                const double vl = -bl * lam / (al * lam + m);
                const double vq = -bq * lam / (aq * lam + m);
                fl = bl + al * vl;
                fq = bq + aq * vq;
                slideSpeed = lam;
                tipNew = tipStick + h * (vl * lDir + vq * qDir);
            }
            const Vec3 tangential = fl * lDir + fq * qDir;
            const Vec3 force = normalForce * roadN + tangential;

            // To the belt: positions by the Catmull-Rom weights, torsion and lateral bending linearly.
            for (int j = 0; j < 4; ++j)
            {
                m_force[nodeIndex[j]] += w[j] * force;
            }
            const double torque = glm::dot(glm::cross(base - centre, force), tc);
            m_torque[k] += (1.0 - sg) * torque;
            m_torque[k1] += sg * torque;
            const double outward = glm::dot(force, outR);
            for (int i = 0; i < modes; ++i)
            {
                const double q = m_shape[static_cast<size_t>(b) * modes + i] * outward;
                m_bendLoadNew[static_cast<size_t>(i) * n + k] += (1.0 - sg) * q;
                m_bendLoadNew[static_cast<size_t>(i) * n + k1] += sg * q;
            }
            // The block's stiffness on the road, K = J^T S J with J the block point's dependence on the
            // four nodes (Catmull-Rom weights) and the two torsion angles (rotation about the tangent).
            const double kn2 = kn;
            for (int i = 0; i < modes; ++i)
            {
                const double q = m_shape[static_cast<size_t>(b) * modes + i];
                m_bendStiffNew[static_cast<size_t>(i) * n + k] += (1.0 - sg) * kn2 * q * q;
                m_bendStiffNew[static_cast<size_t>(i) * n + k1] += sg * kn2 * q * q;
            }
            if (assemble)
            {
                Mat3 kMat = kn * Outer(roadN);
                Mat3 dMat = dampingActive ? dn * Outer(roadN) : Mat3(0.0);
                if (!sliding)
                {
                    kMat += cl * Outer(lDir) + cq * Outer(qDir);
                    dMat += tau * (cl * Outer(lDir) + cq * Outer(qDir));
                }
                const Mat3 m = c2 * kMat + c1 * dMat;
                for (int a = 0; a < 4; ++a)
                {
                    for (int c = a; c < 4; ++c)
                    {
                        AddBlock3(nodeIndex[a], nodeIndex[c], m, w[a] * w[c]);
                    }
                }
                const Vec3 rpsi = glm::cross(tc, base - centre);
                const Vec3 g = m * rpsi;
                const int psiNode[2] = {k, k1};
                const double psiWeight[2] = {1.0 - sg, sg};
                for (int a = 0; a < 4; ++a)
                {
                    for (int c = 0; c < 2; ++c)
                    {
                        const double wc = w[a] * psiWeight[c];
                        for (int comp = 0; comp < 3; ++comp)
                        {
                            AddScalar(nodeIndex[a], comp, psiNode[c], 3, wc * g[comp]);
                        }
                    }
                }
                const double tt = glm::dot(rpsi, g);
                AddScalar(k, 3, k, 3, psiWeight[0] * psiWeight[0] * tt);
                AddScalar(k1, 3, k1, 3, psiWeight[1] * psiWeight[1] * tt);
                AddScalar(k, 3, k1, 3, psiWeight[0] * psiWeight[1] * tt);
            }

            BlockMemory& memoryNew = m_memoryNew[index];
            memoryNew.contact = true;
            memoryNew.sliding = sliding;
            memoryNew.tip = tipNew;

            stats.blocks += 1;
            stats.sliding += sliding ? 1 : 0;
            stats.normalForce += normalForce;
            stats.area += p.treadPositive * area;
            stats.maxPressure = std::max(stats.maxPressure, groundPressure);
            stats.frictionPower += sliding ? glm::length(tangential) * slideSpeed : 0.0;
            weighted += normalForce * base;
            const double along = glm::dot(base, heading);
            const double across = glm::dot(base, frame.axis);
            minHeading = std::min(minHeading, along);
            maxHeading = std::max(maxHeading, along);
            minLateral = std::min(minLateral, across);
            maxLateral = std::max(maxLateral, across);

            if (settings.commit)
            {
                BlockView& view = m_blocks[index];
                view.base = base;
                view.tip = tipNew;
                view.force = force;
                view.normalForce = normalForce;
                view.groundPressure = groundPressure;
                view.slidingSpeed = slideSpeed;
                view.friction = mu;
                view.contact = true;
                view.sliding = sliding;
            }
        }
    }
    if (stats.blocks > 0)
    {
        stats.length = maxHeading - minHeading;
        stats.width = maxLateral - minLateral;
        stats.meanPressure = stats.normalForce / std::max(stats.area, 1.0e-12);
        stats.centre = stats.normalForce > 0.0 ? weighted / stats.normalForce : Vec3(0.0);
    }
    m_evalContact = stats;
}

void FlexRingTyre::UpdateLateralBending(double h)
{
    // Gipser 2006, (5) and (6) with damping: lambda_i ((1 + tau/h) g + kappa (-g_{k-1} + 2 g_k - g_{k+1}))
    // = f_i(g) + lambda_i tau/h g_old. The contact load f_i depends on g itself (the tread is far stiffer
    // than the belt across its width), so it is linearized about the last step's shape: f_i(g) ~ f_i(g0)
    // - K_i (g - g0), K_i the blocks' normal stiffness projected on the shape - one Newton step per time step.
    const int modes = static_cast<int>(m_bendLambda.size());
    if (modes == 0)
    {
        return;
    }
    const double pr = PressureRatio();
    const double scale = 0.1 + 0.9 * pr;
    const double tauRatio = m_p.lateralBendDamping / std::max(h, 1.0e-9);
    const double kappa = m_p.lateralBendCoupling;
    std::vector<double> diag(static_cast<size_t>(m_n));
    std::vector<double> rhs(static_cast<size_t>(m_n));
    for (int i = 0; i < modes; ++i)
    {
        const double lam = m_bendLambda[i] * scale;
        if (lam <= 0.0)
        {
            continue;
        }
        for (int k = 0; k < m_n; ++k)
        {
            const size_t at = static_cast<size_t>(i) * m_n + k;
            const double contact = m_bendStiff[at];
            diag[k] = lam * (1.0 + tauRatio + 2.0 * kappa) + contact;
            rhs[k] = m_bendLoad[at] + (lam * tauRatio + contact) * m_gamma[at];
        }
        SolveCyclicTridiagonal(diag, -lam * kappa, rhs, m_work);
        for (int k = 0; k < m_n; ++k)
        {
            m_gamma[static_cast<size_t>(i) * m_n + k] = rhs[k];
        }
    }
}

void FlexRingTyre::Step(const RimState& rimEnd, double h, double beta, double massScale)
{
    const int n = m_n;
    BuildFrame(rimEnd, m_stepFrame);
    m_xTrial.resize(n);
    m_vTrial = m_v;
    m_psiTrial.resize(n);
    m_psiDotTrial = m_psiDot;
    const int iterations = std::max(m_iterationsOverride > 0 ? m_iterationsOverride : m_p.newtonIterations, 1);
    EvalSettings settings;
    settings.h = h;
    settings.beta = beta;
    for (int it = 0; it < iterations; ++it)
    {
        for (int k = 0; k < n; ++k)
        {
            m_xTrial[k] = m_x[k] + h * ((1.0 - beta) * m_v[k] + beta * m_vTrial[k]);
            m_psiTrial[k] = m_psi[k] + h * ((1.0 - beta) * m_psiDot[k] + beta * m_psiDotTrial[k]);
        }
        settings.assemble = it == 0 && (!m_factorValid || ++m_stepsSinceFactor >= std::max(m_p.jacobianCycle, 1));
        settings.commit = false;
        Evaluate(m_xTrial, m_vTrial, m_psiTrial, m_psiDotTrial, m_stepFrame, settings, massScale);
        if (settings.assemble)
        {
            m_stepStats.factorizations += 1;
            m_factorValid = m_matrix.Factor();
            m_stepsSinceFactor = 0;
            if (!m_factorValid)
            {
                m_stepStats.failedFactorizations += 1;
            }
        }
        // Residual of M (v1 - v0) = h ((1 - beta) F0 + beta F(trial)).
        for (int k = 0; k < n; ++k)
        {
            const Vec3 r = m_p.nodeMass * massScale * (m_vTrial[k] - m_v[k]) - h * ((1.0 - beta) * m_force0[k] + beta * m_force[k]);
            const double rt = m_p.nodeInertia * massScale * (m_psiDotTrial[k] - m_psiDot[k]) - h * ((1.0 - beta) * m_torque0[k] + beta * m_torque[k]);
            m_rhs[Dof(k, 0)] = -r.x;
            m_rhs[Dof(k, 1)] = -r.y;
            m_rhs[Dof(k, 2)] = -r.z;
            m_rhs[Dof(k, 3)] = -rt;
        }
        if (m_factorValid)
        {
            m_matrix.Solve(m_rhs.data());
        }
        else
        {
            for (int k = 0; k < n; ++k)
            {
                for (int c = 0; c < 3; ++c)
                {
                    m_rhs[Dof(k, c)] /= m_p.nodeMass * massScale;
                }
                m_rhs[Dof(k, 3)] /= m_p.nodeInertia * massScale;
            }
        }
        for (int k = 0; k < n; ++k)
        {
            m_vTrial[k] += Vec3(m_rhs[Dof(k, 0)], m_rhs[Dof(k, 1)], m_rhs[Dof(k, 2)]);
            m_psiDotTrial[k] += m_rhs[Dof(k, 3)];
        }
    }
    for (int k = 0; k < n; ++k)
    {
        m_xTrial[k] = m_x[k] + h * ((1.0 - beta) * m_v[k] + beta * m_vTrial[k]);
        m_psiTrial[k] = m_psi[k] + h * ((1.0 - beta) * m_psiDot[k] + beta * m_psiDotTrial[k]);
    }
    settings.assemble = false;
    settings.commit = true;
    Evaluate(m_xTrial, m_vTrial, m_psiTrial, m_psiDotTrial, m_stepFrame, settings, massScale);
    m_x.swap(m_xTrial);
    m_v.swap(m_vTrial);
    m_psi.swap(m_psiTrial);
    m_psiDot.swap(m_psiDotTrial);
    m_force0 = m_force;
    m_torque0 = m_torque;
    m_wrench = m_evalWrench;
    m_rim = rimEnd;
    std::swap(m_frame, m_stepFrame);
    m_time += h;
    UpdateLateralBending(h);
}

Wrench FlexRingTyre::Advance(const RimState& rimEnd, double dt)
{
    const auto start = std::chrono::steady_clock::now();
    m_stepStats = StepStats{};
    if (!m_initialized)
    {
        Reset(rimEnd);
        return m_wrench;
    }
    if (!(dt > 0.0))
    {
        return m_wrench;
    }
    const Vec3 axis = glm::normalize(rimEnd.rotation[1]);
    double spin = std::max(std::abs(glm::dot(m_rim.angularVelocity, axis)), std::abs(glm::dot(rimEnd.angularVelocity, axis)));
    if (!std::isfinite(spin))
    {
        spin = 0.0;
    }
    int steps = static_cast<int>(std::ceil(dt / std::max(m_p.maxStep, 1.0e-7) - 1.0e-9));
    steps = std::max(steps, static_cast<int>(std::ceil(spin * dt / std::max(m_p.maxAngleIncrement, 1.0e-6) - 1.0e-9)));
    steps = std::clamp(steps, 1, 2000);
    const double h = dt / steps;
    const RimState from = m_rim;
    Wrench sum;
    for (int i = 1; i <= steps; ++i)
    {
        const RimState r = i == steps ? rimEnd : Interpolate(from, rimEnd, dt, static_cast<double>(i) / steps);
        Step(r, h, m_p.beta, 1.0);
        sum.force += m_wrench.force;
        sum.moment += m_wrench.moment;
    }
    m_stepStats.substeps = steps;
    m_stepStats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Wrench mean;
    mean.force = sum.force / static_cast<double>(steps);
    mean.moment = sum.moment / static_cast<double>(steps);
    return mean;
}

bool FlexRingTyre::SettleStatic(const RimState& rimIn, int maxSteps, double h, double massScale, double speedTolerance)
{
    RimState rim = rimIn;
    rim.velocity = Vec3(0.0);
    rim.angularVelocity = Vec3(0.0);
    if (!m_initialized)
    {
        Reset(rim);
    }
    m_factorValid = false;
    m_iterationsOverride = kStaticIterations;
    const double halfWidth = 0.5 * m_p.beltWidth;
    // At rest when the belt has stopped, or when the rim's force no longer changes (blocks creeping on
    // the road at the edge of sticking keep the belt from ever stopping exactly).
    double lastForce = 0.0;
    int calm = 0;
    for (int step = 0; step < maxSteps; ++step)
    {
        Step(rim, h, kStaticBeta, massScale);
        double fastest = 0.0;
        for (int k = 0; k < m_n; ++k)
        {
            fastest = std::max(fastest, glm::length(m_v[k]));
            fastest = std::max(fastest, std::abs(m_psiDot[k]) * halfWidth);
        }
        if (step > 3 && fastest < speedTolerance)
        {
            m_factorValid = false;
            m_iterationsOverride = 0;
            return true;
        }
        const double force = glm::length(m_wrench.force);
        calm = std::abs(force - lastForce) <= 1.0e-5 * std::max(force, 1.0) && fastest < 100.0 * speedTolerance ? calm + 1 : 0;
        lastForce = force;
        if (calm >= 20)
        {
            m_factorValid = false;
            m_iterationsOverride = 0;
            return true;
        }
    }
    m_factorValid = false;
    m_iterationsOverride = 0;
    return false;
}

Vec3 FlexRingTyre::SurfacePoint(int segment, double sigma, double lateral, bool withTread, Vec3* normalOut) const
{
    const int n = m_n;
    const int k = ((segment % n) + n) % n;
    const int km = (k + n - 1) % n;
    const int k1 = (k + 1) % n;
    const int k2 = (k + 2) % n;
    double w[4];
    double dw[4];
    CatmullRom(sigma, w, dw);
    const Vec3 centre = w[0] * m_x[km] + w[1] * m_x[k] + w[2] * m_x[k1] + w[3] * m_x[k2];
    const Vec3 tc = glm::normalize(dw[0] * m_x[km] + dw[1] * m_x[k] + dw[2] * m_x[k1] + dw[3] * m_x[k2]);
    const Vec3 lat = glm::normalize(m_frame.axis - glm::dot(m_frame.axis, tc) * tc);
    const Vec3 out = glm::cross(lat, tc);
    const double torsion = (1.0 - sigma) * m_psi[k] + sigma * m_psi[k1];
    const Vec3 latR = lat * std::cos(torsion) - out * std::sin(torsion);
    const Vec3 outR = out * std::cos(torsion) + lat * std::sin(torsion);
    const double radius = m_p.latCurvatureRadius;
    double z = 0.0;
    double dz = 0.0;
    if (radius > std::abs(lateral))
    {
        z = -(radius - std::sqrt(radius * radius - lateral * lateral));
        dz = -lateral / std::sqrt(radius * radius - lateral * lateral);
    }
    const int modes = static_cast<int>(m_p.bendShapeRoots.size());
    for (int i = 0; i < modes; ++i)
    {
        double slope = 0.0;
        const double q = BendShape(i, lateral, &slope);
        const double g = (1.0 - sigma) * m_gamma[static_cast<size_t>(i) * n + k] + sigma * m_gamma[static_cast<size_t>(i) * n + k1];
        z += g * q;
        dz += g * slope;
    }
    const Vec3 normal = glm::normalize(outR - dz * latR);
    Vec3 point = centre + lateral * latR + z * outR;
    if (withTread)
    {
        point += (m_treadDepth + m_p.treadBase) * normal;
    }
    if (normalOut != nullptr)
    {
        *normalOut = normal;
    }
    return point;
}

void FlexRingTyre::StructuralForces(const std::vector<Vec3>& x, const std::vector<Vec3>& v, const std::vector<double>& psi, const std::vector<double>& psiDot,
                                    std::vector<Vec3>& force, std::vector<double>& torque)
{
    EvalSettings settings;
    settings.contact = false;
    settings.gravity = false;
    settings.internalElements = false;
    settings.assemble = false;
    settings.commit = false;
    Evaluate(x, v, psi, psiDot, m_frame, settings, 1.0);
    force = m_force;
    torque = m_torque;
}

}
