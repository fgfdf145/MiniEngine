#include "flex_ring_road.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace me::tyre::flexring
{

void Road::HeightRange(double x0, double y0, double x1, double y1, double& low, double& high) const
{
    low = 1.0e300;
    high = -1.0e300;
    constexpr int kSamples = 5;
    for (int i = 0; i < kSamples; ++i)
    {
        for (int j = 0; j < kSamples; ++j)
        {
            const double h = Height(x0 + (x1 - x0) * i / (kSamples - 1), y0 + (y1 - y0) * j / (kSamples - 1));
            low = std::min(low, h);
            high = std::max(high, h);
        }
    }
}

FlatRoad::FlatRoad(double height, double slopeX, double slopeY, double friction)
    : m_height(height), m_slopeX(slopeX), m_slopeY(slopeY), m_friction(friction)
{
}

double FlatRoad::Height(double x, double y) const
{
    return m_height + m_slopeX * x + m_slopeY * y;
}

double FlatRoad::FrictionScale(double, double) const
{
    return m_friction;
}

void FlatRoad::HeightRange(double x0, double y0, double x1, double y1, double& low, double& high) const
{
    const double a = Height(x0, y0);
    const double b = Height(x1, y0);
    const double c = Height(x0, y1);
    const double d = Height(x1, y1);
    low = std::min({a, b, c, d});
    high = std::max({a, b, c, d});
}

Vec3 FlatRoad::SurfaceVelocity(double, double) const
{
    return m_velocity;
}

CleatRoad::CleatRoad(double baseHeight, const CleatGeometry& cleat, double friction)
    : m_base(baseHeight), m_cleat(cleat), m_friction(friction)
{
    const double n = std::hypot(m_cleat.direction[0], m_cleat.direction[1]);
    if (n > 0.0)
    {
        m_cleat.direction[0] /= n;
        m_cleat.direction[1] /= n;
    }
    else
    {
        m_cleat.direction[0] = 1.0;
        m_cleat.direction[1] = 0.0;
    }
}

double CleatRoad::Height(double x, double y) const
{
    const double dx = x - m_cleat.position[0];
    const double dy = y - m_cleat.position[1];
    const double across = dx * m_cleat.direction[0] + dy * m_cleat.direction[1];
    const double along = -dx * m_cleat.direction[1] + dy * m_cleat.direction[0];
    if (m_cleat.length > 0.0 && std::abs(along) > 0.5 * m_cleat.length)
    {
        return m_base;
    }
    const double half = 0.5 * m_cleat.width;
    const double a = std::abs(across);
    if (a >= half)
    {
        return m_base;
    }
    if (m_cleat.semicircular)
    {
        return m_base + std::sqrt(std::max(half * half - a * a, 0.0));
    }
    if (m_cleat.topWidth >= 0.0 && m_cleat.topWidth < m_cleat.width)
    {
        const double top = 0.5 * m_cleat.topWidth;
        if (a <= top)
        {
            return m_base + m_cleat.height;
        }
        return m_base + m_cleat.height * (half - a) / std::max(half - top, 1.0e-12);
    }
    const double bevel = std::clamp(m_cleat.bevel, 0.0, std::min(half, m_cleat.height));
    const double flat = half - bevel;
    if (a <= flat)
    {
        return m_base + m_cleat.height;
    }
    return m_base + m_cleat.height - (a - flat);
}

double CleatRoad::FrictionScale(double, double) const
{
    return m_friction;
}

void CleatRoad::HeightRange(double x0, double y0, double x1, double y1, double& low, double& high) const
{
    low = m_base;
    high = m_base;
    double acrossMin = 1.0e300, acrossMax = -1.0e300, alongMin = 1.0e300, alongMax = -1.0e300;
    for (const double x : {x0, x1})
    {
        for (const double y : {y0, y1})
        {
            const double dx = x - m_cleat.position[0];
            const double dy = y - m_cleat.position[1];
            const double across = dx * m_cleat.direction[0] + dy * m_cleat.direction[1];
            const double along = -dx * m_cleat.direction[1] + dy * m_cleat.direction[0];
            acrossMin = std::min(acrossMin, across);
            acrossMax = std::max(acrossMax, across);
            alongMin = std::min(alongMin, along);
            alongMax = std::max(alongMax, along);
        }
    }
    const double half = 0.5 * m_cleat.width;
    const bool crosses = acrossMax > -half && acrossMin < half;
    const bool within = m_cleat.length <= 0.0 || (alongMax > -0.5 * m_cleat.length && alongMin < 0.5 * m_cleat.length);
    if (crosses && within)
    {
        high = m_base + (m_cleat.semicircular ? half : m_cleat.height);
    }
}

GridRoad::GridRoad(double x0, double y0, double dx, double dy, int nx, int ny, std::vector<double> heights, double outside)
    : m_x0(x0), m_y0(y0), m_dx(dx), m_dy(dy), m_nx(nx), m_ny(ny), m_heights(std::move(heights)), m_outside(outside)
{
    m_heights.resize(static_cast<size_t>(std::max(nx, 0)) * static_cast<size_t>(std::max(ny, 0)), outside);
}

double GridRoad::Height(double x, double y) const
{
    if (m_nx < 2 || m_ny < 2)
    {
        return m_outside;
    }
    const double u = (x - m_x0) / m_dx;
    const double v = (y - m_y0) / m_dy;
    if (u < 0.0 || v < 0.0 || u > m_nx - 1 || v > m_ny - 1)
    {
        return m_outside;
    }
    const int i = std::min(static_cast<int>(u), m_nx - 2);
    const int j = std::min(static_cast<int>(v), m_ny - 2);
    const double fu = u - i;
    const double fv = v - j;
    const auto at = [&](int a, int b) {
        return m_heights[static_cast<size_t>(b) * m_nx + a];
    };
    return (1.0 - fv) * ((1.0 - fu) * at(i, j) + fu * at(i + 1, j)) + fv * ((1.0 - fu) * at(i, j + 1) + fu * at(i + 1, j + 1));
}

FunctionRoad::FunctionRoad(std::function<double(double, double)> height, std::function<double(double, double)> friction)
    : m_height(std::move(height)), m_friction(std::move(friction))
{
}

double FunctionRoad::Height(double x, double y) const
{
    return m_height ? m_height(x, y) : 0.0;
}

double FunctionRoad::FrictionScale(double x, double y) const
{
    return m_friction ? m_friction(x, y) : 1.0;
}

}
