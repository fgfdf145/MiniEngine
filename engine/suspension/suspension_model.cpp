#include "suspension_model.h"

#include <cmath>
#include <stdexcept>

namespace me::suspension
{

namespace
{
constexpr const char* kWheelAxisPoint = "wheel_axis";
constexpr const char* kStrutAxisPoint = "strut_axis";

Vec3 Mirror(const Vec3& v)
{
    return {v.x, -v.y, v.z};
}

Mat3 Mirror(const Mat3& m)
{
    // Reflect y on both sides: S m S with S = diag(1, -1, 1).
    Mat3 result = m;
    for (int c = 0; c < 3; ++c)
    {
        for (int r = 0; r < 3; ++r)
        {
            const double sign = ((r == 1) != (c == 1)) ? -1.0 : 1.0;
            result[c][r] = m[c][r] * sign;
        }
    }
    return result;
}

double TriangleArea2(const Vec3& a, const Vec3& b, const Vec3& c)
{
    return glm::length(glm::cross(b - a, c - a));
}
}

int Model::Find(const std::string& name) const
{
    for (std::size_t i = 0; i < pointNames.size(); ++i)
    {
        if (pointNames[i] == name)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

SuspensionDefinition MirrorToRight(const SuspensionDefinition& left)
{
    SuspensionDefinition right = left;
    right.side = -left.side;
    for (PointDef& point : right.points)
    {
        point.position = Mirror(point.position);
    }
    for (BushingDef& bushing : right.bushings)
    {
        bushing.axes = Mirror(bushing.axes);
    }
    right.wheelAxis = Mirror(left.wheelAxis);
    right.travelAxis = Mirror(left.travelAxis);
    right.rackAxis = left.rackAxis; // the rack moves both ends the same way
    right.vehicleCenter = Mirror(left.vehicleCenter);
    return right;
}

SuspensionDefinition MakeDoubleWishbone(const DoubleWishboneHardpoints& hp, double tyreRadius, const Vec3& wheelAxis)
{
    SuspensionDefinition def;
    def.name = "double wishbone";
    def.points = {
        {"lower_front", hp.lowerFront, PointRole::Chassis},
        {"lower_rear", hp.lowerRear, PointRole::Chassis},
        {"lower_ball", hp.lowerBall, PointRole::Moving},
        {"upper_front", hp.upperFront, PointRole::Chassis},
        {"upper_rear", hp.upperRear, PointRole::Chassis},
        {"upper_ball", hp.upperBall, PointRole::Moving},
        {"tie_inner", hp.tieInner, PointRole::Rack},
        {"tie_outer", hp.tieOuter, PointRole::Moving},
        {"wheel_center", hp.wheelCenter, PointRole::Moving},
        {"spring_lower", hp.springLower, PointRole::Moving},
        {"spring_upper", hp.springUpper, PointRole::Chassis},
    };
    def.bodies = {
        {"knuckle", {"lower_ball", "upper_ball", "tie_outer", "wheel_center"}, 0.0},
        {"lower_arm", {"lower_front", "lower_rear", "lower_ball", "spring_lower"}, 0.0},
        {"upper_arm", {"upper_front", "upper_rear", "upper_ball"}, 0.0},
        {"tie_rod", {"tie_inner", "tie_outer"}, 0.0},
    };
    def.elements = {{"spring", "spring_lower", "spring_upper"}};
    def.steered = hp.steered;
    def.steeringAxisReference = "lower_ball";
    def.knuckle = "knuckle";
    def.wheelCenter = "wheel_center";
    def.wheelAxis = glm::normalize(wheelAxis);
    def.tyreRadius = tyreRadius;
    return def;
}

SuspensionDefinition MakeMacPherson(const MacPhersonHardpoints& hp, double tyreRadius, const Vec3& wheelAxis)
{
    SuspensionDefinition def;
    def.name = "macpherson";
    // The strut axis is the line from the tube point to the top mount in the design position; a
    // second point of it, fixed to the knuckle, is placed where the top mount is then.
    def.points = {
        {"lower_front", hp.lowerFront, PointRole::Chassis},
        {"lower_rear", hp.lowerRear, PointRole::Chassis},
        {"lower_ball", hp.lowerBall, PointRole::Moving},
        {"strut_top", hp.strutTop, PointRole::Chassis},
        {"strut_lower", hp.strutLower, PointRole::Moving},
        {kStrutAxisPoint, hp.strutTop, PointRole::Moving},
        {"tie_inner", hp.tieInner, PointRole::Rack},
        {"tie_outer", hp.tieOuter, PointRole::Moving},
        {"wheel_center", hp.wheelCenter, PointRole::Moving},
    };
    std::vector<std::string> knuckle = {"lower_ball", "strut_lower", kStrutAxisPoint, "tie_outer", "wheel_center"};
    // When the strut axis runs through the lower ball joint (Assetto Corsa's layout), the tube point
    // is the ball joint itself: keep one point and let the axis start there.
    const bool sharedBall = glm::length(hp.strutLower - hp.lowerBall) < 1e-9;
    if (sharedBall)
    {
        def.points.erase(def.points.begin() + 4);
        knuckle.erase(knuckle.begin() + 1);
    }
    def.bodies = {
        {"knuckle", knuckle, 0.0},
        {"lower_arm", {"lower_front", "lower_rear", "lower_ball"}, 0.0},
        {"tie_rod", {"tie_inner", "tie_outer"}, 0.0},
    };
    def.sliders = {{"strut_top", sharedBall ? "lower_ball" : "strut_lower", kStrutAxisPoint}};
    def.elements = {{"strut", sharedBall ? "lower_ball" : "strut_lower", "strut_top"}};
    def.steered = hp.steered;
    def.steeringAxisReference = "lower_ball";
    def.knuckle = "knuckle";
    def.wheelCenter = "wheel_center";
    def.wheelAxis = glm::normalize(wheelAxis);
    def.tyreRadius = tyreRadius;
    return def;
}

Model Compile(const SuspensionDefinition& definition, ModelMode mode)
{
    Model model;
    model.definition = definition;
    model.mode = mode;

    for (const PointDef& point : definition.points)
    {
        if (model.Find(point.name) >= 0)
        {
            throw std::invalid_argument("suspension point named twice: " + point.name);
        }
        model.pointNames.push_back(point.name);
        model.design.push_back(point.position);
        model.roles.push_back(point.role);
    }
    const auto require = [&model](const std::string& name) {
        const int index = model.Find(name);
        if (index < 0)
        {
            throw std::invalid_argument("suspension point not defined: " + name);
        }
        return index;
    };

    model.wheelCenter = require(definition.wheelCenter);
    const Vec3 axis = glm::normalize(definition.wheelAxis);
    model.pointNames.push_back(kWheelAxisPoint);
    model.design.push_back(model.design[model.wheelCenter] + axis * model.wheelAxisLength);
    model.roles.push_back(PointRole::Moving);
    model.wheelAxisPoint = static_cast<int>(model.pointNames.size()) - 1;

    std::vector<BodyDef> bodies = definition.bodies;
    for (std::size_t b = 0; b < bodies.size(); ++b)
    {
        if (bodies[b].name == definition.knuckle)
        {
            model.knuckle = static_cast<int>(b);
            bodies[b].points.push_back(kWheelAxisPoint);
        }
    }
    if (model.knuckle < 0)
    {
        throw std::invalid_argument("suspension knuckle body not defined: " + definition.knuckle);
    }

    // Unknowns: moving points, and chassis points that a bushing holds in compliance mode.
    std::vector<bool> bushed(model.pointNames.size(), false);
    if (mode == ModelMode::Compliant)
    {
        for (const BushingDef& def : definition.bushings)
        {
            const int point = require(def.point);
            if (model.roles[point] != PointRole::Chassis)
            {
                throw std::invalid_argument("bushing on a point that is not a chassis point: " + def.point);
            }
            bushed[point] = true;
            Bushing bushing;
            bushing.point = point;
            bushing.anchor = model.design[point];
            bushing.axes = def.axes;
            bushing.curves[0] = def.x;
            bushing.curves[1] = def.y;
            bushing.curves[2] = def.z;
            model.bushings.push_back(bushing);
        }
    }
    model.slot.assign(model.pointNames.size(), -1);
    for (std::size_t i = 0; i < model.pointNames.size(); ++i)
    {
        if (model.roles[i] == PointRole::Moving || bushed[i])
        {
            model.slot[i] = model.unknowns;
            model.unknowns += 3;
        }
    }

    const auto addDistance = [&model](int a, int b) {
        if (model.slot[a] < 0 && model.slot[b] < 0)
        {
            return; // both ends on the chassis: nothing to solve
        }
        Constraint c;
        c.kind = ConstraintKind::Distance;
        c.a = a;
        c.b = b;
        c.length = glm::length(model.design[a] - model.design[b]);
        c.row = model.rows;
        model.rows += 1;
        model.constraints.push_back(c);
    };

    model.pointMass.assign(model.pointNames.size(), 0.0);
    for (std::size_t b = 0; b < bodies.size(); ++b)
    {
        const BodyDef& body = bodies[b];
        std::vector<int> points;
        for (const std::string& name : body.points)
        {
            points.push_back(require(name));
        }
        if (static_cast<int>(b) == model.knuckle)
        {
            model.knucklePoints = points;
        }
        int moving = 0;
        for (int p : points)
        {
            moving += model.slot[p] >= 0 ? 1 : 0;
        }
        for (int p : points)
        {
            if (model.slot[p] >= 0 && moving > 0)
            {
                model.pointMass[p] += body.mass / moving;
            }
        }
        if (points.size() < 2)
        {
            throw std::invalid_argument("suspension body with fewer than two points: " + body.name);
        }
        if (points.size() == 2)
        {
            addDistance(points[0], points[1]);
            continue;
        }
        // A base triangle (the first point, the farthest from it, and the one spanning the largest
        // triangle with them), then three distances from every other point to its corners.
        const int p0 = points[0];
        int p1 = points[1];
        for (int p : points)
        {
            if (glm::length(model.design[p] - model.design[p0]) > glm::length(model.design[p1] - model.design[p0]))
            {
                p1 = p;
            }
        }
        int p2 = -1;
        double best = 0.0;
        for (int p : points)
        {
            const double area = TriangleArea2(model.design[p0], model.design[p1], model.design[p]);
            if (area > best)
            {
                best = area;
                p2 = p;
            }
        }
        const double span = glm::length(model.design[p1] - model.design[p0]);
        if (p2 < 0 || best < 1e-6 * span * span)
        {
            throw std::invalid_argument("suspension body with collinear points: " + body.name);
        }
        addDistance(p0, p1);
        addDistance(p0, p2);
        addDistance(p1, p2);
        for (int p : points)
        {
            if (p == p0 || p == p1 || p == p2)
            {
                continue;
            }
            addDistance(p, p0);
            addDistance(p, p1);
            addDistance(p, p2);
        }
    }
    if (model.knucklePoints.size() < 4)
    {
        throw std::invalid_argument("the knuckle needs at least three hardpoints besides the wheel centre's axis point");
    }

    for (const SliderDef& def : definition.sliders)
    {
        Constraint c;
        c.kind = ConstraintKind::Slider;
        c.a = require(def.through);
        c.b = require(def.base);
        c.c = require(def.tip);
        c.length = glm::length(model.design[c.c] - model.design[c.b]);
        if (c.length < 1e-6)
        {
            throw std::invalid_argument("slider axis has no length: " + def.base + " -> " + def.tip);
        }
        const Vec3 direction = (model.design[c.c] - model.design[c.b]) / c.length;
        const double along = glm::dot(model.design[c.a] - model.design[c.b], direction);
        const Vec3 off = model.design[c.a] - (model.design[c.b] + along * direction);
        if (glm::length(off) > 1e-6)
        {
            throw std::invalid_argument("slider point is not on its axis: " + def.through);
        }
        c.slider = static_cast<int>(model.sliderSlot.size());
        model.sliderSlot.push_back(model.unknowns);
        model.sliderDesign.push_back(along);
        model.unknowns += 1;
        c.row = model.rows;
        model.rows += 3;
        model.constraints.push_back(c);
    }

    {
        Constraint c;
        c.kind = ConstraintKind::Travel;
        c.a = model.wheelCenter;
        c.row = model.rows;
        model.rows += 1;
        model.constraints.push_back(c);
    }

    model.steeringReference = definition.steeringAxisReference.empty() ? model.wheelCenter : require(definition.steeringAxisReference);
    if (model.slot[model.steeringReference] < 0)
    {
        throw std::invalid_argument("the steering axis reference must be a moving point: " + definition.steeringAxisReference);
    }

    for (const ElementDef& element : definition.elements)
    {
        model.elementA.push_back(require(element.a));
        model.elementB.push_back(require(element.b));
    }

    if (mode == ModelMode::Kinematic && model.rows != model.unknowns)
    {
        throw std::invalid_argument("suspension '" + definition.name + "' is not determinate: " + std::to_string(model.unknowns) + " unknowns, " + std::to_string(model.rows) + " equations");
    }
    return model;
}
}
