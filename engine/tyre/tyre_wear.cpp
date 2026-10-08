#include "tyre_wear.h"

#include "tyre_thermal.h"

#include <algorithm>
#include <cmath>

namespace me::tyre
{

namespace
{
// The game holds the slip graining and blistering go by below this many times its peak's.
constexpr double kMostSlip = 2.5;
// Below this speed of the wheel over the road (m/s), and on a road gripping less than these, nothing grains or
// blisters.
constexpr double kSlowest = 2.0;
constexpr double kLeastGripToGrain = 0.95;
constexpr double kLeastGripToBlister = 0.95;
}

TyreWearModel::TyreWearModel(TyreWearParameters parameters)
    : m_p(std::move(parameters))
{
    // The performance curve's window: the first temperature it reaches 1, and the last.
    const std::vector<glm::vec2>& curve = m_p.performanceCurve;
    for (const glm::vec2& point : curve)
    {
        if (point.y >= 1.0f)
        {
            m_grainBelow = point.x;
            break;
        }
    }
    for (size_t index = curve.size(); index-- > 1;)
    {
        if (curve[index].y >= 1.0f)
        {
            m_blisterAbove = curve[index].x;
            break;
        }
    }
}

void TyreWearModel::Reset()
{
    m_virtualKm = 0.0;
    m_grain = 0.0;
    m_blister = 0.0;
}

double TyreWearModel::Grip() const
{
    const double wear = m_p.wearCurve.empty() ? 1.0 : EvaluateThermalCurve(m_p.wearCurve, m_virtualKm) * 0.01;
    return wear / (1.0 + 0.2 * std::clamp(m_blister * 0.01, 0.0, 1.0));
}

void TyreWearModel::Step(const TyreWearInput& in)
{
    if (!(in.dt > 0.0) || !(in.rate > 0.0))
    {
        return;
    }
    // Virtual km: the distance slid, times the load over FZ0 with USE_LOAD.
    const double loadShare = m_p.useLoad && m_p.referenceLoad > 0.0 ? in.load / m_p.referenceLoad : 1.0;
    m_virtualKm += std::max(in.slideSpeed, 0.0) * in.dt * in.rate * loadShare * 0.001;

    if (!in.temperatures || !(in.load > 0.0))
    {
        return;
    }
    const double slip = std::min(std::max(in.slip, 0.0), kMostSlip);
    const double t = in.coreTemperature;
    const double road = in.surfaceGrip * in.contactSpeed;
    // Graining below the window, with the slip to GRAIN_GAMMA, the speed and how far below; and worn off again
    // with driving on.
    if (m_p.grainGain > 0.0 && t < m_grainBelow && in.contactSpeed > kSlowest && in.surfaceGrip >= kLeastGripToGrain)
    {
        m_grain += std::pow(slip, m_p.grainGamma) * in.dt * (road * m_p.grainGain * (m_grainBelow - t) * 0.0001) * in.rate;
    }
    const double recovery = in.contactSpeed * in.surfaceGrip * m_p.grainGain * 0.00005;
    if (recovery > 0.0)
    {
        m_grain -= std::pow(slip, m_p.grainGamma) * in.dt * recovery * in.rate;
    }
    // Blistering above it.
    if (m_p.blisterGain > 0.0 && t > m_blisterAbove && in.contactSpeed > kSlowest && in.surfaceGrip >= kLeastGripToBlister)
    {
        m_blister += std::pow(slip, m_p.blisterGamma) * in.dt * (in.surfaceGrip * in.contactSpeed * m_p.blisterGain * (t - m_blisterAbove) * 0.0001) * in.rate;
    }
    m_grain = std::clamp(m_grain, 0.0, 100.0);
    m_blister = std::clamp(m_blister, 0.0, 100.0);
}
}
