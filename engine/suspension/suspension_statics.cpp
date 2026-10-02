#include "suspension_statics.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace me::suspension
{

namespace
{
Vec3 SlotVector(const std::vector<double>& v, int slot)
{
    return {v[slot], v[slot + 1], v[slot + 2]};
}

void AddSlot(std::vector<double>& v, int slot, const Vec3& value)
{
    v[slot] += value.x;
    v[slot + 1] += value.y;
    v[slot + 2] += value.z;
}

double MaxAbs(const std::vector<double>& values)
{
    double result = 0.0;
    for (double v : values)
    {
        result = std::max(result, std::abs(v));
    }
    return result;
}

Vec3 ContactPoint(const Model& model, const Vec3& centre, const Vec3& spin)
{
    const Vec3 up(0.0, 0.0, 1.0);
    const Vec3 radial = glm::normalize(-up - glm::dot(-up, spin) * spin);
    return centre + model.definition.tyreRadius * radial;
}

// -mu dPhi/dp for every point: the force each constraint puts on each point.
void ConstraintForces(const Model& model, const std::vector<double>& q, double rack, const std::vector<double>& mu, std::vector<Vec3>& forces, std::vector<Vec3>* sliderSide)
{
    forces.assign(model.pointNames.size(), Vec3(0.0));
    if (sliderSide)
    {
        sliderSide->assign(model.sliderSlot.size(), Vec3(0.0));
    }
    for (const Constraint& c : model.constraints)
    {
        switch (c.kind)
        {
        case ConstraintKind::Distance:
        {
            const Vec3 d = PointPosition(model, q, rack, c.a) - PointPosition(model, q, rack, c.b);
            forces[c.a] -= mu[c.row] * d;
            forces[c.b] += mu[c.row] * d;
            break;
        }
        case ConstraintKind::Slider:
        {
            const Vec3 m(mu[c.row], mu[c.row + 1], mu[c.row + 2]);
            const double t = q[model.sliderSlot[c.slider]] / c.length;
            forces[c.a] -= m;
            forces[c.b] += (1.0 - t) * m;
            forces[c.c] += t * m;
            if (sliderSide)
            {
                const Vec3 e = glm::normalize(PointPosition(model, q, rack, c.c) - PointPosition(model, q, rack, c.b));
                const Vec3 onThrough = -m;
                (*sliderSide)[c.slider] = onThrough - glm::dot(onThrough, e) * e;
            }
            break;
        }
        case ConstraintKind::Travel:
            forces[c.a] -= mu[c.row] * model.definition.travelAxis;
            break;
        }
    }
}
}

std::vector<Vec3> DistributeLoad(const std::vector<Vec3>& positions, const Vec3& at, const WheelLoad& load)
{
    // Unknowns (lambda, mu): n lambda + mu x sumr = F and (sumr) x lambda + sum(|r|^2 mu - r (r.mu)) = M.
    const double n = static_cast<double>(positions.size());
    Vec3 sum(0.0);
    Mat3 second(0.0);
    for (const Vec3& p : positions)
    {
        const Vec3 r = p - at;
        sum += r;
        second += Mat3(glm::dot(r, r)) - glm::outerProduct(r, r);
    }
    DenseMatrix a(6, 6);
    // Row block 1: n I lambda - Skew(sumr) mu = F  (mu x sumr = -sumr x mu).
    a.AddBlock(0, 0, Mat3(n));
    a.AddBlock(0, 3, -Skew(sum));
    // Row block 2: Skew(sumr) lambda + second mu = M.
    a.AddBlock(3, 0, Skew(sum));
    a.AddBlock(3, 3, second);
    DenseLu lu;
    if (!lu.Factor(a))
    {
        throw std::invalid_argument("DistributeLoad needs at least three non-collinear points");
    }
    std::vector<double> rhs = {load.force.x, load.force.y, load.force.z, load.moment.x, load.moment.y, load.moment.z};
    lu.Solve(rhs);
    const Vec3 lambda(rhs[0], rhs[1], rhs[2]);
    const Vec3 mu(rhs[3], rhs[4], rhs[5]);
    std::vector<Vec3> forces;
    forces.reserve(positions.size());
    for (const Vec3& p : positions)
    {
        forces.push_back(lambda + glm::cross(mu, p - at));
    }
    return forces;
}

void GeneralisedForces(const Model& model, const std::vector<double>& q, double rack, const Vec3& contactPoint, const WheelLoad& load, const std::vector<double>& elementForces, std::vector<double>& forces)
{
    forces.assign(static_cast<std::size_t>(model.unknowns), 0.0);
    std::vector<Vec3> positions;
    for (int index : model.knucklePoints)
    {
        positions.push_back(PointPosition(model, q, rack, index));
    }
    const std::vector<Vec3> shares = DistributeLoad(positions, contactPoint, load);
    for (std::size_t k = 0; k < shares.size(); ++k)
    {
        const int slot = model.slot[model.knucklePoints[k]];
        if (slot >= 0)
        {
            AddSlot(forces, slot, shares[k]);
        }
    }
    for (std::size_t e = 0; e < elementForces.size() && e < model.elementA.size(); ++e)
    {
        const int a = model.elementA[e];
        const int b = model.elementB[e];
        const Vec3 d = glm::normalize(PointPosition(model, q, rack, b) - PointPosition(model, q, rack, a));
        if (model.slot[a] >= 0)
        {
            AddSlot(forces, model.slot[a], -elementForces[e] * d);
        }
        if (model.slot[b] >= 0)
        {
            AddSlot(forces, model.slot[b], elementForces[e] * d);
        }
    }
}

void SolveReactions(const Kinematics& kinematics, const WheelLoad& load, const std::vector<double>& elementForces, Reactions& out)
{
    const Model& model = kinematics.GetModel();
    const std::vector<double>& q = kinematics.Coordinates();
    const double rack = kinematics.Rack();
    const WheelAttitude attitude = ComputeAttitude(model, q, rack);

    std::vector<double> forces;
    GeneralisedForces(model, q, rack, attitude.contactPoint, load, elementForces, forces);

    out.travelForce = 0.0;
    for (std::size_t i = 0; i < forces.size(); ++i)
    {
        out.travelForce += forces[i] * kinematics.DqDTravel()[i];
    }

    out.multipliers = forces;
    kinematics.Factorisation().SolveTransposed(out.multipliers);

    ConstraintForces(model, q, rack, out.multipliers, out.pointForces, &out.sliderSideForce);
    // Element forces acting straight on chassis points.
    for (std::size_t e = 0; e < elementForces.size() && e < model.elementA.size(); ++e)
    {
        const int a = model.elementA[e];
        const int b = model.elementB[e];
        const Vec3 d = glm::normalize(kinematics.Point(b) - kinematics.Point(a));
        if (model.slot[a] < 0)
        {
            out.pointForces[a] -= elementForces[e] * d;
        }
        if (model.slot[b] < 0)
        {
            out.pointForces[b] += elementForces[e] * d;
        }
    }
    out.rackForce = 0.0;
    for (std::size_t i = 0; i < model.roles.size(); ++i)
    {
        if (model.roles[i] == PointRole::Rack)
        {
            out.rackForce += glm::dot(out.pointForces[i], model.definition.rackAxis);
        }
    }
    out.sliderSideLoad.resize(out.sliderSideForce.size());
    for (std::size_t s = 0; s < out.sliderSideForce.size(); ++s)
    {
        out.sliderSideLoad[s] = glm::length(out.sliderSideForce[s]);
    }
}

WheelAttitude ComputeAttitude(const Model& model, const std::vector<double>& q, double rack)
{
    WheelAttitude a;
    a.wheelCenter = PointPosition(model, q, rack, model.wheelCenter);
    a.spinAxis = glm::normalize(PointPosition(model, q, rack, model.wheelAxisPoint) - a.wheelCenter);
    a.contactPoint = ContactPoint(model, a.wheelCenter, a.spinAxis);
    const double side = model.definition.side >= 0 ? 1.0 : -1.0;
    a.camber = -std::asin(std::clamp(a.spinAxis.z, -1.0, 1.0));
    a.toe = std::atan2(a.spinAxis.x, side * a.spinAxis.y);
    return a;
}

Compliance::Compliance(const Model& compliantModel)
    : m_model(compliantModel)
{
    if (m_model.mode != ModelMode::Compliant)
    {
        throw std::invalid_argument("Compliance needs a model compiled in ModelMode::Compliant");
    }
    m_q = DesignCoordinates(m_model);
    m_mu.assign(static_cast<std::size_t>(m_model.rows), 0.0);
}

Vec3 Compliance::Deflection(int bushing) const
{
    const Bushing& b = m_model.bushings[bushing];
    return glm::transpose(b.axes) * (Point(b.point) - b.anchor);
}

double Compliance::SliderSideLoad(int slider) const
{
    std::vector<Vec3> forces;
    std::vector<Vec3> side;
    ConstraintForces(m_model, m_q, m_rack, m_mu, forces, &side);
    return glm::length(side[slider]);
}

Compliance::Report Compliance::Solve(double travel, double rack, const WheelLoad& load, const std::vector<double>& elementForces)
{
    if (!m_model.definition.steered)
    {
        rack = 0.0;
    }
    m_rack = rack;
    const std::size_t n = static_cast<std::size_t>(m_model.unknowns);
    const std::size_t m = static_cast<std::size_t>(m_model.rows);
    Report report;
    DenseMatrix jacobian;
    DenseMatrix kkt(n + m, n + m);
    DenseLu lu;
    std::vector<double> phi;
    std::vector<double> forces;
    std::vector<double> rhs(n + m);

    for (int it = 0; it < 30; ++it)
    {
        EvaluateConstraints(m_model, m_q, travel, rack, phi);
        EvaluateJacobian(m_model, m_q, rack, jacobian, nullptr, nullptr);
        const WheelAttitude attitude = ComputeAttitude(m_model, m_q, rack);
        GeneralisedForces(m_model, m_q, rack, attitude.contactPoint, load, elementForces, forces);

        kkt.SetZero();
        // Residual of the force balance: grad V - Q + Phi_q^T mu.
        std::vector<double> balance(n, 0.0);
        for (std::size_t i = 0; i < n; ++i)
        {
            balance[i] = -forces[i];
        }
        for (const Bushing& b : m_model.bushings)
        {
            const int slot = m_model.slot[b.point];
            const Vec3 local = glm::transpose(b.axes) * (Point(b.point) - b.anchor);
            Vec3 f(0.0);
            Vec3 k(0.0);
            for (int a = 0; a < 3; ++a)
            {
                double slope = 0.0;
                f[a] = b.curves[a].Evaluate(local[a], slope);
                // A bushing axis with no stiffness would leave the point free: keep a token one.
                k[a] = std::max(slope, 1.0);
            }
            AddSlot(balance, slot, b.axes * f);
            const Mat3 stiffness = b.axes * Mat3(Vec3(k.x, 0, 0), Vec3(0, k.y, 0), Vec3(0, 0, k.z)) * glm::transpose(b.axes);
            kkt.AddBlock(slot, slot, stiffness);
        }
        for (std::size_t r = 0; r < m; ++r)
        {
            for (std::size_t c = 0; c < n; ++c)
            {
                const double j = jacobian(r, c);
                if (j != 0.0)
                {
                    balance[c] += j * m_mu[r];
                    kkt(c, n + r) = j;
                    kkt(n + r, c) = j;
                }
            }
        }
        // sum mu grad ^2Phi: distance rows give mu [I -I; -I I]; slider rows couple s with the axis points.
        for (const Constraint& c : m_model.constraints)
        {
            if (c.kind == ConstraintKind::Distance)
            {
                const int sa = m_model.slot[c.a];
                const int sb = m_model.slot[c.b];
                const Mat3 block(m_mu[c.row]);
                if (sa >= 0)
                {
                    kkt.AddBlock(sa, sa, block);
                }
                if (sb >= 0)
                {
                    kkt.AddBlock(sb, sb, block);
                }
                if (sa >= 0 && sb >= 0)
                {
                    kkt.AddBlock(sa, sb, -block);
                    kkt.AddBlock(sb, sa, -block);
                }
            }
            else if (c.kind == ConstraintKind::Slider)
            {
                const int ss = m_model.sliderSlot[c.slider];
                const int sb = m_model.slot[c.b];
                const int sc = m_model.slot[c.c];
                for (int k = 0; k < 3; ++k)
                {
                    const double mu = m_mu[c.row + k];
                    if (sc >= 0)
                    {
                        kkt(ss, sc + k) -= mu / c.length;
                        kkt(sc + k, ss) -= mu / c.length;
                    }
                    if (sb >= 0)
                    {
                        kkt(ss, sb + k) += mu / c.length;
                        kkt(sb + k, ss) += mu / c.length;
                    }
                }
            }
        }

        // Converged when forces balance to a micro-newton and the joints close to a picometre-ish
        // (distance rows are L * error).
        report.residual = MaxAbs(balance);
        report.iterations = it;
        if (report.residual < 1e-6 && MaxAbs(phi) < 1e-13)
        {
            report.converged = true;
            break;
        }
        for (std::size_t i = 0; i < n; ++i)
        {
            rhs[i] = -balance[i];
        }
        for (std::size_t r = 0; r < m; ++r)
        {
            rhs[n + r] = -phi[r];
        }
        if (!lu.Factor(kkt))
        {
            break;
        }
        lu.Solve(rhs);
        for (std::size_t i = 0; i < n; ++i)
        {
            m_q[i] += rhs[i];
        }
        for (std::size_t r = 0; r < m; ++r)
        {
            m_mu[r] += rhs[n + r];
        }
        report.iterations = it + 1;
    }
    return report;
}
}
