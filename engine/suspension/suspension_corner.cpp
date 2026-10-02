#include "suspension_corner.h"

#include <stdexcept>

namespace me::suspension
{

SuspensionCorner::SuspensionCorner(const SuspensionDefinition& definition, const StrutUnit& strut, int strutElement, int slider, SolverSettings solver)
    : m_kinematics(Compile(definition), solver),
      m_strut(strut),
      m_strutElement(strutElement),
      m_slider(slider),
      m_definition(definition)
{
    const Model& model = m_kinematics.GetModel();
    if (strutElement != kWheelTravel && (strutElement < 0 || strutElement >= static_cast<int>(model.elementA.size())))
    {
        throw std::invalid_argument("SuspensionCorner: no such strut element");
    }
    if (slider >= static_cast<int>(model.sliderSlot.size()))
    {
        throw std::invalid_argument("SuspensionCorner: no such slider");
    }
    if (strutElement != kWheelTravel)
    {
        m_designLength = glm::length(model.design[model.elementB[strutElement]] - model.design[model.elementA[strutElement]]);
    }
}

void SuspensionCorner::EnableCompliance(bool enable)
{
    if (enable && !m_compliance)
    {
        m_compliance.emplace(Compile(m_definition, ModelMode::Compliant));
    }
    else if (!enable)
    {
        m_compliance.reset();
    }
}

const CornerOutput& SuspensionCorner::Step(const CornerInput& input)
{
    CornerOutput& out = m_out;
    out.solve = m_kinematics.Solve(input.travel, input.rack);
    ComputeOutputs(m_kinematics, out.geometry);

    // The strut's compression and rate, and how it moves per metre of travel.
    const Model& model = m_kinematics.GetModel();
    double perTravel = -1.0; // dl/dz: a unit on the travel shortens one for one
    double perRack = 0.0;
    if (m_strutElement == kWheelTravel)
    {
        out.strutCompression = m_kinematics.Travel();
        out.strutRate = input.travelRate;
    }
    else
    {
        const int a = model.elementA[m_strutElement];
        const int b = model.elementB[m_strutElement];
        const Vec3 d = m_kinematics.Point(b) - m_kinematics.Point(a);
        const double length = glm::length(d);
        const Vec3 e = d / length;
        perTravel = glm::dot(e, m_kinematics.PointDTravel(b) - m_kinematics.PointDTravel(a));
        perRack = glm::dot(e, m_kinematics.PointDRack(b) - m_kinematics.PointDRack(a));
        out.strutCompression = m_designLength - length;
        out.strutRate = -(perTravel * input.travelRate + perRack * input.rackRate);
    }

    out.sideLoad = m_sideLoad;
    out.strutForce = m_strut.Step(out.strutCompression, out.strutRate, m_sideLoad, input.dt);
    // Generalised force along travel: F dl/dz; its slopes from the unit's (compression = -dl).
    out.strutTravelForce = out.strutForce * perTravel;
    out.strutTravelStiffness = -m_strut.StiffnessSlope(out.strutCompression) * perTravel * perTravel;
    out.strutTravelDamping = -m_strut.RateSlope(out.strutRate, m_sideLoad, input.dt) * perTravel * perTravel;

    std::vector<double> elementForces(model.elementA.size(), 0.0);
    if (m_strutElement != kWheelTravel)
    {
        elementForces[m_strutElement] = out.strutForce;
    }
    SolveReactions(m_kinematics, input.load, elementForces, m_reactions);
    out.rackForce = m_reactions.rackForce;
    out.nextSideLoad = m_slider >= 0 ? m_reactions.sliderSideLoad[m_slider] : 0.0;
    m_sideLoad = out.nextSideLoad;
    if (m_strutElement == kWheelTravel)
    {
        // The unit is not in the linkage: its force adds straight onto the travel.
        out.loadTravelForce = m_reactions.travelForce;
        out.travelForce = m_reactions.travelForce + out.strutTravelForce;
    }
    else
    {
        out.travelForce = m_reactions.travelForce;
        out.loadTravelForce = m_reactions.travelForce - out.strutTravelForce;
    }

    const Vec3 omega = KnuckleAngularRate(m_kinematics, m_kinematics.DqDTravel(), 0.0);
    out.wheelCenterPerTravel = m_kinematics.PointDTravel(model.wheelCenter);
    out.contactPerTravel = out.wheelCenterPerTravel + glm::cross(omega, out.geometry.contactPoint - out.geometry.wheelCenter);
    out.normalPerTravel = glm::dot(input.contactNormal, out.contactPerTravel);

    out.complianceSolved = false;
    if (m_compliance)
    {
        const Compliance::Report report = m_compliance->Solve(m_kinematics.Travel(), m_kinematics.Rack(), input.load, elementForces);
        out.complianceSolved = report.converged;
        out.compliantAttitude = m_compliance->Attitude();
    }
    return out;
}
}
