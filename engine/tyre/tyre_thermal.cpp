#include "tyre_thermal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace me::tyre
{

namespace
{
// The tyre's turn is kept within this many radians (as the game keeps it).
constexpr double kAngleWrap = 100000.0;
// The game's cooling per (m/s)^2 of speed, per unit of COOL_FACTOR over 1.
constexpr double kCoolPerFactor = 0.000324;
}

double EvaluateThermalCurve(const std::vector<glm::vec2>& curve, double x)
{
    if (curve.empty())
    {
        return 1.0;
    }
    if (x <= curve.front().x)
    {
        return curve.front().y;
    }
    for (size_t index = 1; index < curve.size(); ++index)
    {
        if (x <= curve[index].x)
        {
            const double span = curve[index].x - curve[index - 1].x;
            const double t = span > 0.0 ? (x - curve[index - 1].x) / span : 1.0;
            return curve[index - 1].y + (curve[index].y - curve[index - 1].y) * t;
        }
    }
    return curve.back().y;
}

TyreThermalModel::TyreThermalModel(TyreThermalParameters parameters, double temperature)
    : m_p(std::move(parameters))
{
    if (!(m_p.staticPressure > 0.0))
    {
        m_p.staticPressure = 26.0;
    }
    Reset(temperature);
}

void TyreThermalModel::Reset(double temperature)
{
    for (auto& lane : m_patches)
    {
        lane.fill(temperature);
    }
    for (auto& lane : m_inputs)
    {
        lane.fill(0.0);
    }
    m_core = temperature;
    m_coreInput = 0.0;
    m_angle = 0.0;
    Settle(0.0);
}

double TyreThermalModel::Pressure() const
{
    return m_p.staticPressure + (m_core - kThermalAmbient) * m_p.temperatureGain;
}

double TyreThermalModel::PressureFactor() const
{
    const double pressure = Pressure();
    if (!(m_p.idealPressure > 0.0))
    {
        return 1.0;
    }
    return pressure >= 0.0 ? 1.0 + (m_p.idealPressure / std::max(pressure, 1e-3) - 1.0) * m_p.rollingResistanceGain : 0.0;
}

int TyreThermalModel::ContactIndex() const
{
    const double turns = m_angle / (2.0 * std::numbers::pi);
    const auto index = static_cast<std::int64_t>(turns * kThermalPatches);
    return static_cast<int>(((index % kThermalPatches) + kThermalPatches) % kThermalPatches);
}

std::array<double, kThermalLanes> TyreThermalModel::ContactTemperatures() const
{
    const int index = ContactIndex();
    return {m_patches[0][static_cast<size_t>(index)], m_patches[1][static_cast<size_t>(index)], m_patches[2][static_cast<size_t>(index)]};
}

double TyreThermalModel::ContactTemperature(double camber) const
{
    const double spread = std::clamp(camber * m_p.camberSpread, -1.0, 1.0);
    const std::array<double, kThermalLanes> t = ContactTemperatures();
    return ((1.0 + spread) * t[0] + t[1] + (1.0 - spread) * t[2]) / 3.0;
}

std::array<double, kThermalLanes> TyreThermalModel::LaneTemperatures() const
{
    std::array<double, kThermalLanes> mean{};
    for (int lane = 0; lane < kThermalLanes; ++lane)
    {
        for (const double t : m_patches[static_cast<size_t>(lane)])
        {
            mean[static_cast<size_t>(lane)] += t / kThermalPatches;
        }
    }
    return mean;
}

// The patch on the road takes the heat level (heat plus the road's temperature), spread across the lanes by
// camber, and by pressure towards the middle (over-inflated) or the shoulders.
void TyreThermalModel::AddSurfaceInput(double camber, double pressureRatio, double heat, double road)
{
    const double level = heat + road;
    const double spread = std::clamp(camber * m_p.camberSpread, -1.0, 1.0);
    const double scale = 1.0 - 0.5 * pressureRatio;
    const double middle = 0.1 * pressureRatio;
    const double shoulders = 0.5 * middle;
    const size_t index = static_cast<size_t>(ContactIndex());
    m_inputs[0][index] += (1.0 + spread - shoulders) * scale * level;
    m_inputs[1][index] += (1.0 + middle) * scale * level;
    m_inputs[2][index] += (1.0 - spread - shoulders) * scale * level;
}

void TyreThermalModel::Settle(double camber)
{
    m_practical = m_core + (ContactTemperature(camber) - m_core) * 0.25;
    m_performance = EvaluateThermalCurve(m_p.performanceCurve, m_practical);
}

void TyreThermalModel::Step(const TyreThermalInput& in)
{
    const double dt = in.dt;
    if (!(dt > 0.0))
    {
        return;
    }
    // The heat: sliding (speed times load times grip) and rolling, on the tread; rolling on the core. Rolling
    // heats the same rolling backwards (the game takes the signed spin).
    const double factor = PressureFactor();
    const double pressureRatio = m_p.idealPressure > 0.0 ? Pressure() / m_p.idealPressure - 1.0 : 0.0;
    const double spin = std::abs(in.wheelSpeed);
    const double load = std::max(in.load, 0.0);
    double heat = std::max(in.slideSpeed, 0.0) * load * std::max(in.grip, 0.0) * m_p.frictionK * in.surfaceGrip;
    heat += factor * m_p.surfaceRollingK * spin * load * 0.001;
    AddSurfaceInput(in.camber, pressureRatio, heat, in.road);
    m_coreInput += m_p.rollingK * factor * spin * load * 0.001;

    m_angle += in.wheelSpeed * dt;
    m_angle = std::fmod(m_angle, kAngleWrap);
    if (m_angle < 0.0)
    {
        m_angle += kAngleWrap;
    }

    // The core towards its rolling heat (never below the air).
    m_core += (std::max(m_coreInput, in.air) - m_core) * dt * m_p.internalCoreTransfer;
    m_coreInput = 0.0;

    const double coolPerSpeed = m_p.coolFactor > 0.0 ? (m_p.coolFactor - 1.0) * kCoolPerFactor : 0.0;
    const double cooling = (1.0 + in.carSpeed * in.carSpeed * coolPerSpeed) * m_p.surfaceTransfer * dt;
    const double patchShare = dt * m_p.patchTransfer;
    const double coreShare = dt * m_p.coreTransfer;
    for (int lane = 0; lane < kThermalLanes; ++lane)
    {
        for (int index = 0; index < kThermalPatches; ++index)
        {
            double& t = m_patches[static_cast<size_t>(lane)][static_cast<size_t>(index)];
            double& input = m_inputs[static_cast<size_t>(lane)][static_cast<size_t>(index)];
            // On the road towards its heat level; elsewhere cooling towards the air, faster with speed.
            if (input > in.air)
            {
                t += (input - t) * dt * m_p.surfaceTransfer;
            }
            else
            {
                t += (in.air - t) * cooling;
            }
            input = 0.0;
            // The neighbours across and round the tyre, in the game's order.
            const auto share = [&](int otherLane, int otherIndex) { t += (m_patches[static_cast<size_t>(otherLane)][static_cast<size_t>(otherIndex)] - t) * patchShare; };
            if (lane > 0)
            {
                share(lane - 1, index);
            }
            share(lane, (index + kThermalPatches - 1) % kThermalPatches);
            if (lane + 1 < kThermalLanes)
            {
                share(lane + 1, index);
            }
            share(lane, (index + 1) % kThermalPatches);
            // And the core, both ways.
            t += (m_core - t) * coreShare;
            m_core += (t - m_core) * coreShare;
        }
    }
    Settle(in.camber);
}
}
