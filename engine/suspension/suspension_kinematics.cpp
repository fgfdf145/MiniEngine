#include "suspension_kinematics.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace me::suspension
{

namespace
{
double MaxAbs(const std::vector<double>& values)
{
    double result = 0.0;
    for (double v : values)
    {
        result = std::max(result, std::abs(v));
    }
    return result;
}

Vec3 SlotVector(const std::vector<double>& v, int slot)
{
    return {v[slot], v[slot + 1], v[slot + 2]};
}

double TetraVolume(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d)
{
    return glm::dot(glm::cross(b - a, c - a), d - a);
}
}

Vec3 PointPosition(const Model& model, const std::vector<double>& q, double rack, int index)
{
    const int slot = model.slot[index];
    if (slot >= 0)
    {
        return SlotVector(q, slot);
    }
    if (model.roles[index] == PointRole::Rack)
    {
        return model.design[index] + model.definition.rackAxis * rack;
    }
    return model.design[index];
}

std::vector<double> DesignCoordinates(const Model& model)
{
    std::vector<double> q(static_cast<std::size_t>(model.unknowns), 0.0);
    for (std::size_t i = 0; i < model.slot.size(); ++i)
    {
        const int slot = model.slot[i];
        if (slot >= 0)
        {
            q[slot] = model.design[i].x;
            q[slot + 1] = model.design[i].y;
            q[slot + 2] = model.design[i].z;
        }
    }
    for (std::size_t s = 0; s < model.sliderSlot.size(); ++s)
    {
        q[model.sliderSlot[s]] = model.sliderDesign[s];
    }
    return q;
}

void EvaluateConstraints(const Model& model, const std::vector<double>& q, double travel, double rack, std::vector<double>& phi)
{
    phi.assign(static_cast<std::size_t>(model.rows), 0.0);
    for (const Constraint& c : model.constraints)
    {
        switch (c.kind)
        {
        case ConstraintKind::Distance:
        {
            const Vec3 d = PointPosition(model, q, rack, c.a) - PointPosition(model, q, rack, c.b);
            phi[c.row] = 0.5 * (glm::dot(d, d) - c.length * c.length);
            break;
        }
        case ConstraintKind::Slider:
        {
            const Vec3 pa = PointPosition(model, q, rack, c.a);
            const Vec3 pb = PointPosition(model, q, rack, c.b);
            const Vec3 pc = PointPosition(model, q, rack, c.c);
            const double s = q[model.sliderSlot[c.slider]];
            const Vec3 r = pa - pb - s * (pc - pb) / c.length;
            phi[c.row] = r.x;
            phi[c.row + 1] = r.y;
            phi[c.row + 2] = r.z;
            break;
        }
        case ConstraintKind::Travel:
        {
            const Vec3 p = PointPosition(model, q, rack, c.a);
            phi[c.row] = glm::dot(model.definition.travelAxis, p - model.design[c.a]) - travel;
            break;
        }
        }
    }
}

void EvaluateJacobian(const Model& model, const std::vector<double>& q, double rack, DenseMatrix& jacobian, std::vector<double>* dPhiDTravel, std::vector<double>* dPhiDRack)
{
    jacobian.Resize(static_cast<std::size_t>(model.rows), static_cast<std::size_t>(model.unknowns));
    if (dPhiDTravel)
    {
        dPhiDTravel->assign(static_cast<std::size_t>(model.rows), 0.0);
    }
    if (dPhiDRack)
    {
        dPhiDRack->assign(static_cast<std::size_t>(model.rows), 0.0);
    }
    const Vec3& rackAxis = model.definition.rackAxis;

    // A row's gradient with respect to one point: into Phi_q when the point is an unknown, into dPhi/du
    // when it rides on the rack.
    const auto addRow = [&](int row, int point, const Vec3& gradient) {
        const int slot = model.slot[point];
        if (slot >= 0)
        {
            jacobian(row, slot) += gradient.x;
            jacobian(row, slot + 1) += gradient.y;
            jacobian(row, slot + 2) += gradient.z;
        }
        else if (model.roles[point] == PointRole::Rack && dPhiDRack)
        {
            (*dPhiDRack)[row] += glm::dot(gradient, rackAxis);
        }
    };
    // Three rows whose gradient with respect to one point is scale * I.
    const auto addIdentity = [&](int row, int point, double scale) {
        const int slot = model.slot[point];
        for (int k = 0; k < 3; ++k)
        {
            if (slot >= 0)
            {
                jacobian(row + k, slot + k) += scale;
            }
            else if (model.roles[point] == PointRole::Rack && dPhiDRack)
            {
                (*dPhiDRack)[row + k] += scale * rackAxis[k];
            }
        }
    };

    for (const Constraint& c : model.constraints)
    {
        switch (c.kind)
        {
        case ConstraintKind::Distance:
        {
            const Vec3 d = PointPosition(model, q, rack, c.a) - PointPosition(model, q, rack, c.b);
            addRow(c.row, c.a, d);
            addRow(c.row, c.b, -d);
            break;
        }
        case ConstraintKind::Slider:
        {
            const Vec3 pb = PointPosition(model, q, rack, c.b);
            const Vec3 pc = PointPosition(model, q, rack, c.c);
            const int sSlot = model.sliderSlot[c.slider];
            const double t = q[sSlot] / c.length;
            addIdentity(c.row, c.a, 1.0);
            addIdentity(c.row, c.b, -(1.0 - t));
            addIdentity(c.row, c.c, -t);
            const Vec3 e = (pc - pb) / c.length;
            for (int k = 0; k < 3; ++k)
            {
                jacobian(c.row + k, sSlot) -= e[k];
            }
            break;
        }
        case ConstraintKind::Travel:
            addRow(c.row, c.a, model.definition.travelAxis);
            if (dPhiDTravel)
            {
                (*dPhiDTravel)[c.row] = -1.0;
            }
            break;
        }
    }
}

Kinematics::Kinematics(const Model& model, SolverSettings settings)
    : m_model(model), m_settings(settings)
{
    if (m_model.rows != m_model.unknowns)
    {
        throw std::invalid_argument("Kinematics needs a kinematic (determinate) model");
    }
    m_q = DesignCoordinates(m_model);

    // The knuckle's tetrahedron for the mirror-assembly check.
    const std::vector<int>& k = m_model.knucklePoints;
    double best = 0.0;
    for (std::size_t a = 0; a < k.size(); ++a)
    {
        for (std::size_t b = a + 1; b < k.size(); ++b)
        {
            for (std::size_t c = b + 1; c < k.size(); ++c)
            {
                for (std::size_t d = c + 1; d < k.size(); ++d)
                {
                    const double v = TetraVolume(m_model.design[k[a]], m_model.design[k[b]], m_model.design[k[c]], m_model.design[k[d]]);
                    if (std::abs(v) > best)
                    {
                        best = std::abs(v);
                        m_tetra[0] = k[a];
                        m_tetra[1] = k[b];
                        m_tetra[2] = k[c];
                        m_tetra[3] = k[d];
                        m_tetraSign = v > 0.0 ? 1.0 : -1.0;
                    }
                }
            }
        }
    }
    if (best <= 1e-12)
    {
        throw std::invalid_argument("the knuckle's points are coplanar: add a point off their plane");
    }

    if (!Refactor() || m_report.nearSingular)
    {
        throw std::invalid_argument("the suspension is singular in its design position");
    }
    UpdateSensitivities();
    // A design that does not close (an adjusted link) is assembled at zero travel and rack.
    std::vector<double> phi;
    EvaluateConstraints(m_model, m_q, 0.0, 0.0, phi);
    double residual = 0.0;
    for (double v : phi)
    {
        residual = std::max(residual, std::abs(v));
    }
    if (residual > m_settings.residualTolerance)
    {
        int iterations = 0;
        int factorisations = 0;
        if (!Attempt(0.0, 0.0, iterations, factorisations))
        {
            throw std::invalid_argument("the suspension cannot be assembled at its design position");
        }
    }
    m_report.travel = 0.0;
    m_report.rack = 0.0;
}

bool Kinematics::Refactor()
{
    EvaluateJacobian(m_model, m_q, m_rack, m_jacobian, &m_phiZ, &m_phiU);
    const bool ok = m_lu.Factor(m_jacobian);
    m_report.pivotRatio = m_lu.PivotRatio();
    m_report.nearSingular = !ok || m_lu.PivotRatio() < m_settings.singularPivotRatio;
    return ok;
}

void Kinematics::UpdateSensitivities()
{
    m_dqDz = m_phiZ;
    m_dqDu = m_phiU;
    for (double& v : m_dqDz)
    {
        v = -v;
    }
    for (double& v : m_dqDu)
    {
        v = -v;
    }
    m_lu.Solve(m_dqDz);
    m_lu.Solve(m_dqDu);
}

bool Kinematics::OrientationKept() const
{
    const double v = TetraVolume(Point(m_tetra[0]), Point(m_tetra[1]), Point(m_tetra[2]), Point(m_tetra[3]));
    return v * m_tetraSign > 0.0;
}

bool Kinematics::Attempt(double travel, double rack, int& iterations, int& factorisations)
{
    const std::vector<double> saved = m_q;
    const double savedTravel = m_travel;
    const double savedRack = m_rack;
    const auto restore = [&]() {
        m_q = saved;
        m_travel = savedTravel;
        m_rack = savedRack;
    };

    // First-order predictor from the sensitivities at the current position.
    const double dz = travel - m_travel;
    const double du = rack - m_rack;
    std::vector<double> q = m_q;
    for (std::size_t i = 0; i < q.size(); ++i)
    {
        q[i] += m_dqDz[i] * dz + m_dqDu[i] * du;
    }

    std::vector<double> phi;
    std::vector<double> step;
    DenseMatrix jacobian;
    DenseLu lu;
    bool converged = false;
    // Chord steps reuse the factorisation at the current position (no new LU); they stay while
    // they shrink the residual tenfold, then full Newton takes over.
    bool chord = true;
    EvaluateConstraints(m_model, q, travel, rack, phi);
    double residual = MaxAbs(phi);
    std::vector<double> trial(q.size());
    for (int it = 0; it < m_settings.maxIterations; ++it)
    {
        if (residual < m_settings.residualTolerance)
        {
            converged = true;
            break;
        }
        step = phi;
        if (chord)
        {
            m_lu.Solve(step);
        }
        else
        {
            EvaluateJacobian(m_model, q, rack, jacobian, nullptr, nullptr);
            ++factorisations;
            if (!lu.Factor(jacobian) || lu.PivotRatio() < m_settings.singularPivotRatio)
            {
                break;
            }
            lu.Solve(step);
        }
        ++iterations;
        // Backtracking: halve the correction while it makes the residual worse.
        double alpha = 1.0;
        double trialResidual = 0.0;
        for (int halving = 0; halving < 6; ++halving)
        {
            for (std::size_t i = 0; i < q.size(); ++i)
            {
                trial[i] = q[i] - alpha * step[i];
            }
            EvaluateConstraints(m_model, trial, travel, rack, phi);
            trialResidual = MaxAbs(phi);
            if (trialResidual < residual)
            {
                break;
            }
            alpha *= 0.5;
        }
        if (!(trialResidual < residual))
        {
            if (chord)
            {
                chord = false; // the old Jacobian does not help here: retry with a fresh one
                EvaluateConstraints(m_model, q, travel, rack, phi);
                continue;
            }
            break;
        }
        if (chord && trialResidual > 0.1 * residual)
        {
            chord = false;
        }
        q.swap(trial);
        residual = trialResidual;
    }
    converged = converged || residual < m_settings.residualTolerance;
    if (!converged)
    {
        restore();
        return false;
    }

    m_q = q;
    m_travel = travel;
    m_rack = rack;
    ++factorisations;
    if (!Refactor() || m_report.nearSingular || !OrientationKept())
    {
        restore();
        Refactor();
        return false;
    }
    UpdateSensitivities();
    m_report.residual = residual;
    return true;
}

const SolveReport& Kinematics::Solve(double travel, double rack)
{
    if (!m_model.definition.steered)
    {
        rack = 0.0;
    }
    SolveReport report;
    int iterations = 0;
    int factorisations = 0;
    const double startTravel = m_travel;
    const double startRack = m_rack;

    // Go as far toward the request as the mechanism allows, halving the remaining input step when
    // an attempt fails (singular position, the mirror assembly, no convergence).
    for (int round = 0; round < 4 * m_settings.maxBisections; ++round)
    {
        const double remainingTravel = travel - m_travel;
        const double remainingRack = rack - m_rack;
        if (remainingTravel == 0.0 && remainingRack == 0.0)
        {
            break;
        }
        bool moved = false;
        double fraction = 1.0;
        for (int b = 0; b <= m_settings.maxBisections; ++b)
        {
            if (Attempt(m_travel + fraction * remainingTravel, m_rack + fraction * remainingRack, iterations, factorisations))
            {
                moved = true;
                break;
            }
            fraction *= 0.5;
        }
        if (!moved)
        {
            break;
        }
    }

    const double residualKept = m_report.residual;
    report.iterations = iterations;
    report.factorisations = factorisations;
    report.travel = m_travel;
    report.rack = m_rack;
    report.residual = residualKept;
    report.pivotRatio = m_lu.PivotRatio();
    report.nearSingular = m_report.nearSingular;
    if (m_travel == travel && m_rack == rack)
    {
        report.status = SolveStatus::Converged;
    }
    else if (m_travel != startTravel || m_rack != startRack)
    {
        report.status = SolveStatus::Clamped;
    }
    else
    {
        report.status = SolveStatus::Failed;
    }
    m_report = report;
    return m_report;
}

Vec3 Kinematics::PointDTravel(int index) const
{
    const int slot = m_model.slot[index];
    return slot >= 0 ? SlotVector(m_dqDz, slot) : Vec3(0.0);
}

Vec3 Kinematics::PointDRack(int index) const
{
    const int slot = m_model.slot[index];
    if (slot >= 0)
    {
        return SlotVector(m_dqDu, slot);
    }
    return m_model.roles[index] == PointRole::Rack ? m_model.definition.rackAxis : Vec3(0.0);
}

void Kinematics::Accelerations(double travelRate, double rackRate, double travelAccel, double rackAccel, std::vector<double>& qdd) const
{
    const Model& model = m_model;
    std::vector<double> qd(m_q.size());
    for (std::size_t i = 0; i < qd.size(); ++i)
    {
        qd[i] = m_dqDz[i] * travelRate + m_dqDu[i] * rackRate;
    }
    const Vec3& rackAxis = model.definition.rackAxis;
    const auto velocity = [&](int point) {
        const int slot = model.slot[point];
        if (slot >= 0)
        {
            return SlotVector(qd, slot);
        }
        return model.roles[point] == PointRole::Rack ? rackAxis * rackRate : Vec3(0.0);
    };
    // Accelerations of points that are not unknowns (only rack points move).
    const auto knownAccel = [&](int point) {
        return (model.slot[point] < 0 && model.roles[point] == PointRole::Rack) ? rackAxis * rackAccel : Vec3(0.0);
    };

    std::vector<double> gamma(static_cast<std::size_t>(model.rows), 0.0);
    for (const Constraint& c : model.constraints)
    {
        switch (c.kind)
        {
        case ConstraintKind::Distance:
        {
            const Vec3 d = Point(c.a) - Point(c.b);
            const Vec3 dv = velocity(c.a) - velocity(c.b);
            gamma[c.row] = -glm::dot(dv, dv) - glm::dot(d, knownAccel(c.a) - knownAccel(c.b));
            break;
        }
        case ConstraintKind::Slider:
        {
            const double t = m_q[model.sliderSlot[c.slider]] / c.length;
            const double sRate = qd[model.sliderSlot[c.slider]];
            const Vec3 known = knownAccel(c.a) - (1.0 - t) * knownAccel(c.b) - t * knownAccel(c.c);
            const Vec3 g = 2.0 * sRate * (velocity(c.c) - velocity(c.b)) / c.length - known;
            gamma[c.row] = g.x;
            gamma[c.row + 1] = g.y;
            gamma[c.row + 2] = g.z;
            break;
        }
        case ConstraintKind::Travel:
            gamma[c.row] = travelAccel;
            break;
        }
    }
    m_lu.Solve(gamma);
    qdd.swap(gamma);
}

Vec3 KnuckleAngularRate(const Kinematics& kinematics, const std::vector<double>& dq, double rackRate)
{
    const Model& model = kinematics.GetModel();
    const auto rate = [&](int index) {
        const int slot = model.slot[index];
        if (slot >= 0)
        {
            return SlotVector(dq, slot);
        }
        return model.roles[index] == PointRole::Rack ? model.definition.rackAxis * rackRate : Vec3(0.0);
    };
    const int origin = model.wheelCenter;
    const Vec3 p0 = kinematics.Point(origin);
    const Vec3 v0 = rate(origin);
    // Least squares over the knuckle's points: v_k - v_0 = omega x r_k.
    Mat3 a(0.0);
    Vec3 b(0.0);
    for (int index : model.knucklePoints)
    {
        if (index == origin)
        {
            continue;
        }
        const Vec3 r = kinematics.Point(index) - p0;
        a += Mat3(glm::dot(r, r)) - glm::outerProduct(r, r);
        b += glm::cross(r, rate(index) - v0);
    }
    return glm::inverse(a) * b;
}

std::vector<double> SteeringSensitivity(const Kinematics& kinematics)
{
    const Model& model = kinematics.GetModel();
    if (model.steeringReference == model.wheelCenter)
    {
        return kinematics.DqDRack();
    }
    DenseMatrix jacobian;
    std::vector<double> dPhiDRack;
    EvaluateJacobian(model, kinematics.Coordinates(), kinematics.Rack(), jacobian, nullptr, &dPhiDRack);
    // Swap the travel row's gradient from the wheel centre to the reference point.
    for (const Constraint& c : model.constraints)
    {
        if (c.kind != ConstraintKind::Travel)
        {
            continue;
        }
        for (std::size_t col = 0; col < jacobian.Cols(); ++col)
        {
            jacobian(c.row, col) = 0.0;
        }
        const int slot = model.slot[model.steeringReference];
        for (int k = 0; k < 3; ++k)
        {
            jacobian(c.row, slot + k) = model.definition.travelAxis[k];
        }
    }
    DenseLu lu;
    if (!lu.Factor(jacobian))
    {
        return kinematics.DqDRack();
    }
    for (double& v : dPhiDRack)
    {
        v = -v;
    }
    lu.Solve(dPhiDRack);
    return dPhiDRack;
}

namespace
{
// Where two lines in a plane meet: each through `p` perpendicular to its velocity `v`, using the
// coordinates (i, j) of the plane. Returns false for parallel lines.
bool PlanarInstantCenter(const Vec3& p1, const Vec3& v1, const Vec3& p2, const Vec3& v2, int i, int j, Vec3& center)
{
    // Line k: points x with (x - p_k) . v_k = 0 in the (i, j) plane.
    const double a11 = v1[i];
    const double a12 = v1[j];
    const double a21 = v2[i];
    const double a22 = v2[j];
    const double det = a11 * a22 - a12 * a21;
    const double scale = std::max(std::hypot(a11, a12) * std::hypot(a21, a22), 1e-300);
    if (std::abs(det) < 1e-9 * scale)
    {
        return false;
    }
    const double b1 = a11 * p1[i] + a12 * p1[j];
    const double b2 = a21 * p2[i] + a22 * p2[j];
    center = Vec3(0.0);
    center[i] = (b1 * a22 - a12 * b2) / det;
    center[j] = (a11 * b2 - b1 * a21) / det;
    return true;
}
}

void ComputeOutputs(const Kinematics& kinematics, KinematicOutputs& out)
{
    const Model& model = kinematics.GetModel();
    const SuspensionDefinition& def = model.definition;
    const double side = def.side >= 0 ? 1.0 : -1.0;
    const Vec3 outward(0.0, side, 0.0);
    const Vec3 forward(1.0, 0.0, 0.0);
    const Vec3 up(0.0, 0.0, 1.0);

    const Vec3 w = kinematics.Point(model.wheelCenter);
    const Vec3 axis = glm::normalize(kinematics.Point(model.wheelAxisPoint) - w);
    out.wheelCenter = w;
    out.spinAxis = axis;
    const auto contactOf = [&](const Vec3& centre, const Vec3& spin) {
        const Vec3 radial = glm::normalize(-up - glm::dot(-up, spin) * spin);
        return centre + def.tyreRadius * radial;
    };
    out.contactPoint = contactOf(w, axis);

    // Camber (top outward +) and toe (toe-in +), the same formulas on both sides because `axis`
    // points outward on both.
    out.camber = -std::asin(std::clamp(axis.z, -1.0, 1.0));
    const auto toeOf = [&](const Vec3& a) {
        return std::atan2(a.x, side * a.y);
    };
    out.toe = toeOf(axis);

    // Rates: adot = omega x a for travel and rack.
    const Vec3 omegaZ = KnuckleAngularRate(kinematics, kinematics.DqDTravel(), 0.0);
    const Vec3 omegaU = KnuckleAngularRate(kinematics, kinematics.DqDRack(), 1.0);
    const Vec3 axisZ = glm::cross(omegaZ, axis);
    const Vec3 axisU = glm::cross(omegaU, axis);
    const double horizontal = axis.x * axis.x + axis.y * axis.y;
    const auto toeRate = [&](const Vec3& rate) {
        return side * (axis.y * rate.x - axis.x * rate.y) / horizontal;
    };
    out.camberPerTravel = -axisZ.z / std::sqrt(std::max(1e-300, 1.0 - axis.z * axis.z));
    out.toePerTravel = toeRate(axisZ);
    out.toePerRack = toeRate(axisU);

    // Steering axis: the screw axis of the knuckle's motion under the rack, with the steering
    // reference point's height held.
    const std::vector<double> steer = SteeringSensitivity(kinematics);
    const Vec3 omegaS = KnuckleAngularRate(kinematics, steer, 1.0);
    const double omegaLength = glm::length(omegaS);
    out.kingpinValid = omegaLength > 1e-9;
    if (out.kingpinValid)
    {
        const Vec3 v0 = SlotVector(steer, model.slot[model.wheelCenter]);
        Vec3 e = omegaS / omegaLength;
        if (e.z < 0.0)
        {
            e = -e;
        }
        out.kingpinAxis = e;
        out.kingpinPoint = w + glm::cross(omegaS, v0) / (omegaLength * omegaLength);
        out.kingpinInclination = std::atan2(-side * e.y, e.z);
        out.caster = std::atan2(-e.x, e.z);
        const Vec3 p = out.contactPoint;
        const double t = (p.z - out.kingpinPoint.z) / e.z;
        const Vec3 s = out.kingpinPoint + t * e;
        const Vec3 sp = p - s;
        out.casterTrail = -glm::dot(sp, forward);
        out.scrubRadius = glm::dot(sp, outward);
        out.kingpinOffset = glm::length(glm::cross(w - out.kingpinPoint, e));
    }

    // Displacements from the design position.
    const Vec3 w0 = model.design[model.wheelCenter];
    const Vec3 axis0 = glm::normalize(model.design[model.wheelAxisPoint] - w0);
    const Vec3 p0 = contactOf(w0, axis0);
    out.wheelCenterChange = w - w0;
    out.halfTrackChange = glm::dot(out.contactPoint - p0, outward);
    out.wheelbaseChange = glm::dot(out.contactPoint - p0, forward);

    // Velocities per unit travel of the wheel centre and of the knuckle-fixed point under the
    // contact point.
    const Vec3 vw = kinematics.PointDTravel(model.wheelCenter);
    const Vec3 vp = vw + glm::cross(omegaZ, out.contactPoint - w);
    out.frontInstantCenterValid = PlanarInstantCenter(w, vw, out.contactPoint, vp, 1, 2, out.frontInstantCenter);
    out.frontInstantCenter.x = w.x;
    out.rollCenterHeight = 0.0;
    if (out.frontInstantCenterValid)
    {
        // The line from the contact point through the instant centre, at the vehicle's centre plane.
        const Vec3& ic = out.frontInstantCenter;
        const Vec3& p = out.contactPoint;
        const double dy = ic.y - p.y;
        if (std::abs(dy) > 1e-12)
        {
            // Height above the road (the contact point's level), not the frame's z.
            out.rollCenterHeight = (ic.z - p.z) * (def.vehicleCenter.y - p.y) / dy;
        }
        else
        {
            out.frontInstantCenterValid = false;
        }
    }
    out.contactPathAngle = std::atan2(vp.x, vp.z);
    out.wheelCenterPathAngle = std::atan2(vw.x, vw.z);
    out.sideInstantCenterValid = PlanarInstantCenter(w, vw, out.contactPoint, vp, 0, 2, out.sideInstantCenter);
    out.sideInstantCenter.y = w.y;

    out.effectiveMass = 0.0;
    for (std::size_t i = 0; i < model.pointMass.size(); ++i)
    {
        if (model.pointMass[i] > 0.0)
        {
            const Vec3 v = kinematics.PointDTravel(static_cast<int>(i));
            out.effectiveMass += model.pointMass[i] * glm::dot(v, v);
        }
    }

    out.elements.resize(model.elementA.size());
    for (std::size_t e = 0; e < model.elementA.size(); ++e)
    {
        const Vec3 a = kinematics.Point(model.elementA[e]);
        const Vec3 b = kinematics.Point(model.elementB[e]);
        const Vec3 d = b - a;
        const double length = glm::length(d);
        const double rate = glm::dot(d / length, kinematics.PointDTravel(model.elementB[e]) - kinematics.PointDTravel(model.elementA[e]));
        out.elements[e].length = length;
        out.elements[e].lengthPerTravel = rate;
        out.elements[e].motionRatio = -rate;
    }
}
}
