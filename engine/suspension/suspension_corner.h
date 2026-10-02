#pragma once

#include "suspension_kinematics.h"
#include "suspension_model.h"
#include "suspension_statics.h"
#include "suspension_strut.h"

#include <memory>
#include <optional>

namespace me::suspension
{

// One suspension corner stepped at the physics rate: kinematics for the wheel and rack travel the
// vehicle model hands over, the strut's force (with friction fed by last step's side load), and the
// reactions that give the generalised travel force, the rack force and the new side load.
//
// The vehicle model owns the travel coordinate's dynamics: it integrates the unsprung motion with
// travelForce (plus its own tyre and anti-roll bar terms) and EffectiveMass().
struct CornerInput
{
    double travel = 0.0;
    double travelRate = 0.0;
    double rack = 0.0;
    double rackRate = 0.0;
    WheelLoad load;   // the road on the tyre, chassis frame, at the contact point
    // The road's normal at the contact point (chassis frame), for normalPerTravel.
    Vec3 contactNormal{0.0, 0.0, 1.0};
    double dt = 1e-3;
};

struct CornerOutput
{
    SolveReport solve;
    KinematicOutputs geometry;
    double strutCompression = 0.0;
    double strutRate = 0.0;
    double strutForce = 0.0;
    double sideLoad = 0.0;      // normal load the friction used this step (last step's side load)
    double nextSideLoad = 0.0;  // side load from this step's reactions
    double travelForce = 0.0;   // generalised force along travel from load + strut, N
    double rackForce = 0.0;
    // travelForce split into the strut's part and the load's (its virtual work per metre of travel),
    // with the strut part's slopes against travel and travel rate (negative: it resists).
    double strutTravelForce = 0.0;
    double strutTravelStiffness = 0.0;
    double strutTravelDamping = 0.0;
    double loadTravelForce = 0.0;
    // How far the contact point moves along the road's normal per metre of travel (about 1), and its
    // velocity per metre of travel (the knuckle-fixed point under the tyre).
    double normalPerTravel = 1.0;
    Vec3 contactPerTravel{0.0, 0.0, 1.0};
    // The wheel centre's velocity per metre of travel: the hub's mass moves with it.
    Vec3 wheelCenterPerTravel{0.0, 0.0, 1.0};
    // Compliance (only when enabled): the wheel's attitude with bushings under the same loads.
    bool complianceSolved = false;
    WheelAttitude compliantAttitude;
};

class SuspensionCorner
{
public:
    // `strutElement` names which element of the definition carries the strut unit, or kWheelTravel
    // for a unit acting straight on the wheel travel (rates given at the wheel, as a game's data
    // has them: compression is the travel itself); `slider` which sliding joint's side load feeds
    // its friction (-1: none, the double wishbone case).
    static constexpr int kWheelTravel = -1;
    SuspensionCorner(const SuspensionDefinition& definition, const StrutUnit& strut, int strutElement = 0, int slider = -1, SolverSettings solver = {});

    // Also solve the compliant (bushed) position each step. Costs one KKT solve per step.
    void EnableCompliance(bool enable);

    const CornerOutput& Step(const CornerInput& input);
    // What the last Step gave.
    const CornerOutput& Output() const
    {
        return m_out;
    }

    const Kinematics& GetKinematics() const
    {
        return m_kinematics;
    }
    const StrutUnit& Strut() const
    {
        return m_strut;
    }

private:
    Kinematics m_kinematics;
    StrutUnit m_strut;
    int m_strutElement;
    int m_slider;
    double m_designLength = 0.0;
    double m_sideLoad = 0.0;
    std::optional<Compliance> m_compliance;
    SuspensionDefinition m_definition;
    CornerOutput m_out;
    Reactions m_reactions;
};
}
