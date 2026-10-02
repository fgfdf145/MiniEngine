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
    // Compliance (only when enabled): the wheel's attitude with bushings under the same loads.
    bool complianceSolved = false;
    WheelAttitude compliantAttitude;
};

class SuspensionCorner
{
public:
    // `strutElement` names which element of the definition carries the strut unit; `slider` which
    // sliding joint's side load feeds its friction (-1: none, the double wishbone case).
    SuspensionCorner(const SuspensionDefinition& definition, const StrutUnit& strut, int strutElement = 0, int slider = -1, SolverSettings solver = {});

    // Also solve the compliant (bushed) position each step. Costs one KKT solve per step.
    void EnableCompliance(bool enable);

    const CornerOutput& Step(const CornerInput& input);

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
