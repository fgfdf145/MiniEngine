#include "suspension_axle.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace me::suspension
{

namespace
{
constexpr double kDelta = 1e-5;        // m of travel for the rates
constexpr double kJacobianStep = 1e-7; // for the solver's numerical Jacobian
constexpr double kRegularisation = 1.0; // N/m (N m/rad) on each free coordinate: keeps an unlocated one at rest

Vec3 Vee(const Mat3& skew)
{
    // glm is column-major: m[col][row].
    return Vec3(skew[1][2], skew[2][0], skew[0][1]);
}

// Solves the small symmetric system a x = b in place (Gaussian elimination with partial pivoting).
bool SolveSmall(std::array<std::array<double, 4>, 4> a, std::array<double, 4>& b)
{
    for (int c = 0; c < 4; ++c)
    {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
        {
            if (std::abs(a[r][c]) > std::abs(a[pivot][c]))
            {
                pivot = r;
            }
        }
        if (std::abs(a[pivot][c]) < 1e-300)
        {
            return false;
        }
        std::swap(a[c], a[pivot]);
        std::swap(b[c], b[pivot]);
        for (int r = c + 1; r < 4; ++r)
        {
            const double f = a[r][c] / a[c][c];
            for (int k = c; k < 4; ++k)
            {
                a[r][k] -= f * a[c][k];
            }
            b[r] -= f * b[c];
        }
    }
    for (int c = 3; c >= 0; --c)
    {
        for (int k = c + 1; k < 4; ++k)
        {
            b[c] -= a[c][k] * b[k];
        }
        b[c] /= a[c][c];
    }
    return true;
}
}

SolidAxle::SolidAxle(SolidAxleDefinition definition, StrutUnit left, StrutUnit right)
    : m_definition(std::move(definition)), m_units{{std::move(left), std::move(right)}}
{
    if (m_definition.track <= 0.0 || m_definition.tyreRadius <= 0.0)
    {
        throw std::invalid_argument("SolidAxle: the track and the tyre radius must be positive");
    }
    if (m_definition.links.size() + (m_definition.lateralStiffness > 0.0 ? 1 : 0) < 4)
    {
        throw std::invalid_argument("SolidAxle: the axle needs at least four links (or three and a lateral spring)");
    }
    for (const AxleLinkDef& link : m_definition.links)
    {
        m_length.push_back(glm::length(link.chassis - link.axle));
    }
    Solve(0.0, 0.0);
}

Vec3 SolidAxle::Point(const Vec3& design) const
{
    return m_pose.center + m_pose.rotation * design;
}

Vec3 SolidAxle::WheelCenterAt(int side) const
{
    const double outward = side == 0 ? 1.0 : -1.0;
    return Point(Vec3(0.0, outward * 0.5 * m_definition.track, 0.0));
}

Vec3 SolidAxle::SpinAxisAt(int side) const
{
    const double outward = side == 0 ? 1.0 : -1.0;
    return m_pose.rotation * Vec3(0.0, outward, 0.0);
}

Vec3 SolidAxle::ContactAt(int side) const
{
    const Vec3 spin = SpinAxisAt(side);
    const Vec3 down(0.0, 0.0, -1.0);
    const Vec3 radial = glm::normalize(down - glm::dot(down, spin) * spin);
    return WheelCenterAt(side) + m_definition.tyreRadius * radial;
}

SolidAxlePose SolidAxle::PoseAt(const std::array<double, 4>& u, double leftTravel, double rightTravel) const
{
    const double half = 0.5 * m_definition.track;
    SolidAxlePose p;
    p.yaw = u[2];
    p.pitch = u[3];
    // The wheel centres rise by their travel: Rz leaves heights alone, so the roll follows from the
    // travel difference and the pitch.
    const double s = (leftTravel - rightTravel) / (2.0 * half * std::max(std::cos(p.pitch), 0.1));
    p.roll = std::asin(std::clamp(s, -0.99, 0.99));
    p.center = Vec3(u[0], u[1], 0.5 * (leftTravel + rightTravel));
    p.rotation = AxisAngle(Vec3(0.0, 0.0, 1.0), p.yaw) * AxisAngle(Vec3(0.0, 1.0, 0.0), p.pitch) * AxisAngle(Vec3(1.0, 0.0, 0.0), p.roll);
    return p;
}

void SolidAxle::Residuals(const std::array<double, 4>& u, double leftTravel, double rightTravel, std::vector<double>& r) const
{
    const SolidAxlePose p = PoseAt(u, leftTravel, rightTravel);
    const double linkWeight = std::sqrt(m_definition.linkStiffness);
    const double lateralWeight = std::sqrt(std::max(m_definition.lateralStiffness, 0.0));
    const double regularisation = std::sqrt(kRegularisation);
    r.clear();
    for (std::size_t i = 0; i < m_definition.links.size(); ++i)
    {
        const AxleLinkDef& link = m_definition.links[i];
        r.push_back(linkWeight * (glm::length(link.chassis - (p.center + p.rotation * link.axle)) - m_length[i]));
    }
    r.push_back(lateralWeight * u[1]);
    for (int j = 0; j < 4; ++j)
    {
        r.push_back(regularisation * u[j]);
    }
}

void SolidAxle::Solve(double leftTravel, double rightTravel)
{
    m_travel = {leftTravel, rightTravel};
    // Gauss-Newton on the free coordinates from where the axle was, the Jacobian by differences.
    std::array<double, 4> u = m_free;
    std::vector<double> r;
    std::vector<double> rStep;
    std::array<std::vector<double>, 4> columns;
    std::array<std::array<double, 4>, 4> normal{};
    const auto jacobian = [&](const std::array<double, 4>& at) {
        Residuals(at, leftTravel, rightTravel, r);
        for (int j = 0; j < 4; ++j)
        {
            std::array<double, 4> v = at;
            v[j] += kJacobianStep;
            Residuals(v, leftTravel, rightTravel, rStep);
            columns[j].resize(r.size());
            for (std::size_t i = 0; i < r.size(); ++i)
            {
                columns[j][i] = (rStep[i] - r[i]) / kJacobianStep;
            }
        }
        for (int a = 0; a < 4; ++a)
        {
            for (int b = 0; b < 4; ++b)
            {
                normal[a][b] = 0.0;
                for (std::size_t i = 0; i < r.size(); ++i)
                {
                    normal[a][b] += columns[a][i] * columns[b][i];
                }
            }
        }
    };
    for (int iteration = 0; iteration < 30; ++iteration)
    {
        jacobian(u);
        std::array<double, 4> step{};
        for (int a = 0; a < 4; ++a)
        {
            for (std::size_t i = 0; i < r.size(); ++i)
            {
                step[a] -= columns[a][i] * r[i];
            }
        }
        if (!SolveSmall(normal, step))
        {
            break;
        }
        double size = 0.0;
        for (int j = 0; j < 4; ++j)
        {
            u[j] += step[j];
            size = std::max(size, std::abs(step[j]));
        }
        if (size < 1e-12)
        {
            break;
        }
    }
    m_free = u;
    m_pose = PoseAt(u, leftTravel, rightTravel);
    m_stretch = 0.0;
    for (std::size_t i = 0; i < m_definition.links.size(); ++i)
    {
        const AxleLinkDef& link = m_definition.links[i];
        m_stretch = std::max(m_stretch, std::abs(glm::length(link.chassis - Point(link.axle)) - m_length[i]));
    }

    // How the free coordinates follow each wheel's travel at the solution (the least squares'
    // implicit derivative): du/dz = -(J^T J)^-1 J^T dr/dz.
    jacobian(u);
    for (int side = 0; side < 2; ++side)
    {
        const double h = 1e-7;
        Residuals(u, leftTravel + (side == 0 ? h : 0.0), rightTravel + (side == 1 ? h : 0.0), rStep);
        std::array<double, 4> rhs{};
        for (int a = 0; a < 4; ++a)
        {
            for (std::size_t i = 0; i < r.size(); ++i)
            {
                rhs[a] -= columns[a][i] * (rStep[i] - r[i]) / h;
            }
        }
        if (!SolveSmall(normal, rhs))
        {
            rhs = {};
        }
        m_sensitivity[side] = rhs;
    }
}

SolidAxle::Rates SolidAxle::RatesFor(double dLeft, double dRight) const
{
    // The axle-fixed points under the contact points now, then the pose a little either way along
    // the travel with the free coordinates following their sensitivities.
    std::array<Vec3, 2> contactLocal{};
    for (int side = 0; side < 2; ++side)
    {
        contactLocal[side] = glm::transpose(m_pose.rotation) * (ContactAt(side) - m_pose.center);
    }
    const auto sample = [&](double sign, SolidAxlePose& pose, std::array<Vec3, 2>& wheel, std::array<Vec3, 2>& contact) {
        std::array<double, 4> u = m_free;
        for (int j = 0; j < 4; ++j)
        {
            u[j] += sign * kDelta * (dLeft * m_sensitivity[0][j] + dRight * m_sensitivity[1][j]);
        }
        pose = PoseAt(u, m_travel[0] + sign * kDelta * dLeft, m_travel[1] + sign * kDelta * dRight);
        for (int side = 0; side < 2; ++side)
        {
            const double outward = side == 0 ? 1.0 : -1.0;
            wheel[side] = pose.center + pose.rotation * Vec3(0.0, outward * 0.5 * m_definition.track, 0.0);
            contact[side] = pose.center + pose.rotation * contactLocal[side];
        }
    };
    SolidAxlePose up;
    SolidAxlePose down;
    std::array<Vec3, 2> wheelUp{};
    std::array<Vec3, 2> wheelDown{};
    std::array<Vec3, 2> contactUp{};
    std::array<Vec3, 2> contactDown{};
    sample(1.0, up, wheelUp, contactUp);
    sample(-1.0, down, wheelDown, contactDown);
    Rates rates;
    const double span = 2.0 * kDelta;
    rates.omega = Vee((up.rotation - down.rotation) * glm::transpose(m_pose.rotation) / span);
    for (int side = 0; side < 2; ++side)
    {
        rates.wheel[side] = (wheelUp[side] - wheelDown[side]) / span;
        rates.contact[side] = (contactUp[side] - contactDown[side]) / span;
    }
    return rates;
}

void SolidAxle::ComputeOutputs(int side, KinematicOutputs& out) const
{
    FillOutputs(side, RatesFor(side == 0 ? 1.0 : 0.0, side == 1 ? 1.0 : 0.0), RatesFor(1.0, -1.0), RatesFor(1.0, 1.0), out);
}

void SolidAxle::FillOutputs(int side, const Rates& own, const Rates& roll, const Rates& heave, KinematicOutputs& out) const
{
    const double outwardSign = side == 0 ? 1.0 : -1.0;
    const Vec3 origin(0.0, outwardSign * 0.5 * m_definition.track, 0.0);
    const Vec3 outward(0.0, outwardSign, 0.0);

    const Vec3 w = WheelCenterAt(side);
    const Vec3 axis = SpinAxisAt(side);
    const Vec3 p = ContactAt(side);
    out = KinematicOutputs{};
    out.wheelCenter = w - origin;
    out.spinAxis = axis;
    out.contactPoint = p - origin;
    out.camber = -std::asin(std::clamp(axis.z, -1.0, 1.0));
    out.toe = std::atan2(axis.x, outwardSign * axis.y);
    const Vec3 axisRate = glm::cross(own.omega, axis);
    const double horizontal = axis.x * axis.x + axis.y * axis.y;
    out.camberPerTravel = -axisRate.z / std::sqrt(std::max(1e-300, 1.0 - axis.z * axis.z));
    out.toePerTravel = outwardSign * (axis.y * axisRate.x - axis.x * axisRate.y) / horizontal;
    out.kingpinValid = false;

    const Vec3 p0 = origin + Vec3(0.0, 0.0, -m_definition.tyreRadius);
    out.wheelCenterChange = w - origin;
    out.halfTrackChange = glm::dot(p - p0, outward);
    out.wheelbaseChange = p.x - p0.x;

    // The roll centre: on the car's centre plane, the height whose point of the axle does not move
    // sideways as the axle rolls.
    const Vec3 centerRate = 0.5 * (roll.wheel[0] + roll.wheel[1]);
    if (std::abs(roll.omega.x) > 1e-9)
    {
        const double height = m_pose.center.z + centerRate.y / roll.omega.x;
        out.frontInstantCenterValid = true;
        out.frontInstantCenter = Vec3(w.x, 0.0, height) - origin;
        out.rollCenterHeight = height - p.z;
    }
    // Anti-squat and anti-lift: the paths in side view as both wheels rise together.
    out.contactPathAngle = std::atan2(heave.contact[side].x, heave.contact[side].z);
    out.wheelCenterPathAngle = std::atan2(heave.wheel[side].x, heave.wheel[side].z);
    out.sideInstantCenterValid = false;
}

void SolidAxle::Step(const std::array<CornerInput, 2>& in)
{
    Solve(in[0].travel, in[1].travel);
    const std::array<Rates, 2> own = {RatesFor(1.0, 0.0), RatesFor(0.0, 1.0)};
    // Roll (left up, right down) and bounce (both up), per metre of either wheel's travel.
    const Rates roll = RatesFor(1.0, -1.0);
    const Rates heave = RatesFor(1.0, 1.0);

    // The units on the axle at springPosition: each compresses by the axle's rise there.
    const double a = std::clamp(m_definition.springPosition, 0.0, 1.0);
    const double near = 0.5 * (1.0 + a); // a unit's compression per metre of its own wheel's travel
    const double far = 0.5 * (1.0 - a);  // and of the other wheel's
    std::array<double, 2> compression{};
    std::array<double, 2> rate{};
    std::array<double, 2> force{};
    std::array<double, 2> stiffness{};
    std::array<double, 2> damping{};
    for (int side = 0; side < 2; ++side)
    {
        const int other = 1 - side;
        compression[side] = near * in[side].travel + far * in[other].travel;
        rate[side] = near * in[side].travelRate + far * in[other].travelRate;
        force[side] = m_units[side].Step(compression[side], rate[side], 0.0, in[side].dt);
        stiffness[side] = m_units[side].StiffnessSlope(compression[side]);
        damping[side] = m_units[side].RateSlope(rate[side], 0.0, in[side].dt);
    }
    for (int side = 0; side < 2; ++side)
    {
        const int other = 1 - side;
        CornerOutput& out = m_out[side];
        FillOutputs(side, own[side], roll, heave, out.geometry);
        out.solve.status = SolveStatus::Converged;
        out.solve.travel = in[side].travel;
        out.strutCompression = compression[side];
        out.strutRate = rate[side];
        out.strutForce = force[side];
        out.sideLoad = 0.0;
        out.nextSideLoad = 0.0;
        out.rackForce = 0.0;
        // Both units' forces on this wheel's travel (each resists its compression).
        out.strutTravelForce = -(near * force[side] + far * force[other]);
        out.strutTravelStiffness = -(near * near * stiffness[side] + far * far * stiffness[other]);
        out.strutTravelDamping = -(near * near * damping[side] + far * far * damping[other]);
        // Both tyres' loads through the axle: a push on one wheel moves the other too.
        double load = glm::dot(m_housingMoment, own[side].omega);
        for (int w = 0; w < 2; ++w)
        {
            load += glm::dot(in[w].load.force, own[side].contact[w]) + glm::dot(in[w].load.moment, own[side].omega);
        }
        out.loadTravelForce = load;
        out.travelForce = out.strutTravelForce + load;
        out.contactPerTravel = own[side].contact[side];
        out.wheelCenterPerTravel = own[side].wheel[side];
        out.normalPerTravel = glm::dot(in[side].contactNormal, out.contactPerTravel);
        out.complianceSolved = false;
    }
}

AxleSuspension::AxleSuspension(std::unique_ptr<SuspensionCorner> left, std::unique_ptr<SuspensionCorner> right)
    : m_corners{{std::move(left), std::move(right)}}
{
}

AxleSuspension::AxleSuspension(std::unique_ptr<SolidAxle> axle)
    : m_solid(std::move(axle))
{
}

void AxleSuspension::Step(const std::array<CornerInput, 2>& in)
{
    if (m_solid)
    {
        m_solid->Step(in);
        return;
    }
    for (int side = 0; side < 2; ++side)
    {
        m_corners[side]->Step(in[side]);
    }
}

void AxleSuspension::Pose(double leftTravel, double rightTravel, double rack)
{
    if (m_solid)
    {
        m_solid->Solve(leftTravel, rightTravel);
        return;
    }
    const std::array<double, 2> travel = {leftTravel, rightTravel};
    for (int side = 0; side < 2; ++side)
    {
        Kinematics& kinematics = m_corners[side]->MutableKinematics();
        // In small steps: the solver starts from where the linkage is.
        const double fromTravel = kinematics.Travel();
        const double fromRack = kinematics.Rack();
        const int steps = std::clamp(static_cast<int>(std::max(std::abs(travel[side] - fromTravel) / 0.005, std::abs(rack - fromRack) / 0.002)) + 1, 1, 50);
        for (int i = 1; i <= steps; ++i)
        {
            const double t = static_cast<double>(i) / steps;
            kinematics.Solve(fromTravel + (travel[side] - fromTravel) * t, fromRack + (rack - fromRack) * t);
        }
    }
}

void AxleSuspension::Sketch(double halfTrack, LinkageSketch& out) const
{
    out = LinkageSketch{};
    if (m_solid)
    {
        const SolidAxleDefinition& def = m_solid->Definition();
        for (const AxleLinkDef& link : def.links)
        {
            const Vec3 end = m_solid->Point(link.axle);
            out.links.push_back({link.chassis, end});
            out.chassis.push_back(link.chassis);
            out.joints.push_back(end);
        }
        const double half = 0.5 * def.track;
        out.wheelCenters = {m_solid->Point(Vec3(0.0, half, 0.0)), m_solid->Point(Vec3(0.0, -half, 0.0))};
        out.carriers.push_back(out.wheelCenters);
        return;
    }
    for (int side = 0; side < 2; ++side)
    {
        const Kinematics& kinematics = m_corners[side]->GetKinematics();
        const Model& model = kinematics.GetModel();
        const SuspensionDefinition& def = model.definition;
        const Vec3 offset(0.0, side == 0 ? halfTrack : -halfTrack, 0.0);
        // Game data gives its springs at the wheel: their seats are made up, so they are left out.
        const auto drawn = [](const std::string& name) {
            return name.rfind("spring_", 0) != 0 && name.find("axis") == std::string::npos;
        };
        const auto at = [&](const std::string& name) {
            return kinematics.Point(model.Find(name)) + offset;
        };
        for (const BodyDef& body : def.bodies)
        {
            std::vector<std::string> names;
            for (const std::string& name : body.points)
            {
                if (drawn(name) && model.Find(name) >= 0)
                {
                    names.push_back(name);
                }
            }
            if (body.name == def.knuckle)
            {
                for (const std::string& name : names)
                {
                    if (name != def.wheelCenter)
                    {
                        out.carriers.push_back({at(def.wheelCenter), at(name)});
                    }
                }
                continue;
            }
            // A rod between its two ends; an arm (three or more points) as its outline.
            if (names.size() == 2)
            {
                out.links.push_back({at(names[0]), at(names[1])});
            }
            else if (names.size() > 2)
            {
                for (std::size_t i = 0; i < names.size(); ++i)
                {
                    out.links.push_back({at(names[i]), at(names[(i + 1) % names.size()])});
                }
            }
        }
        for (const SliderDef& slider : def.sliders)
        {
            out.links.push_back({at(slider.through), at(slider.base)});
        }
        for (std::size_t i = 0; i < model.pointNames.size(); ++i)
        {
            if (!drawn(model.pointNames[i]) || model.pointNames[i] == def.wheelCenter)
            {
                continue;
            }
            const Vec3 p = kinematics.Point(static_cast<int>(i)) + offset;
            (model.roles[i] == PointRole::Moving ? out.joints : out.chassis).push_back(p);
        }
        out.wheelCenters[side] = at(def.wheelCenter);
    }
}

const CornerOutput& AxleSuspension::Output(int side) const
{
    return m_solid ? m_solid->Output(side) : m_corners[side]->Output();
}
}
