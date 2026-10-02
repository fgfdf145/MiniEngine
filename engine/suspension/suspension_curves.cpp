#include "suspension_curves.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace me::suspension
{

Curve Curve::Linear(double slope)
{
    Curve curve;
    curve.m_kind = Kind::Linear;
    curve.m_a = slope;
    return curve;
}

Curve Curve::Progressive(double slope, double endSlope, double range)
{
    if (range <= 0.0)
    {
        throw std::invalid_argument("Curve::Progressive needs a positive range");
    }
    Curve curve;
    curve.m_kind = Kind::Cubic;
    curve.m_a = slope;
    // dF/dx = slope + 3 c x^2 = endSlope at x = range.
    curve.m_c = (endSlope - slope) / (3.0 * range * range);
    return curve;
}

Curve Curve::Stop(double gap, double slope, double quadratic, int sign)
{
    Curve curve;
    curve.m_kind = Kind::Stop;
    curve.m_a = gap;
    curve.m_b = slope;
    curve.m_c = quadratic;
    curve.m_sign = sign;
    return curve;
}

Curve Curve::Table(std::vector<double> x, std::vector<double> f)
{
    if (x.size() != f.size() || x.size() < 2)
    {
        throw std::invalid_argument("Curve::Table needs at least two samples of each");
    }
    for (std::size_t i = 1; i < x.size(); ++i)
    {
        if (!(x[i] > x[i - 1]))
        {
            throw std::invalid_argument("Curve::Table samples must be ascending");
        }
    }
    Curve curve;
    curve.m_kind = Kind::Table;
    const std::size_t n = x.size();
    std::vector<double> delta(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i)
    {
        delta[i] = (f[i + 1] - f[i]) / (x[i + 1] - x[i]);
    }
    std::vector<double> m(n);
    m[0] = delta[0];
    m[n - 1] = delta[n - 2];
    for (std::size_t i = 1; i + 1 < n; ++i)
    {
        m[i] = (delta[i - 1] * delta[i] <= 0.0) ? 0.0 : 0.5 * (delta[i - 1] + delta[i]);
    }
    // Fritsch-Carlson: keep each segment monotone.
    for (std::size_t i = 0; i + 1 < n; ++i)
    {
        if (delta[i] == 0.0)
        {
            m[i] = 0.0;
            m[i + 1] = 0.0;
            continue;
        }
        const double alpha = m[i] / delta[i];
        const double beta = m[i + 1] / delta[i];
        const double length = alpha * alpha + beta * beta;
        if (length > 9.0)
        {
            const double tau = 3.0 / std::sqrt(length);
            m[i] = tau * alpha * delta[i];
            m[i + 1] = tau * beta * delta[i];
        }
    }
    curve.m_x = std::move(x);
    curve.m_f = std::move(f);
    curve.m_m = std::move(m);
    return curve;
}

Curve Curve::Polyline(std::vector<double> x, std::vector<double> f)
{
    if (x.size() != f.size() || x.size() < 2)
    {
        throw std::invalid_argument("Curve::Polyline needs at least two samples of each");
    }
    for (std::size_t i = 1; i < x.size(); ++i)
    {
        if (!(x[i] > x[i - 1]))
        {
            throw std::invalid_argument("Curve::Polyline samples must be ascending");
        }
    }
    Curve curve;
    curve.m_kind = Kind::Polyline;
    curve.m_x = std::move(x);
    curve.m_f = std::move(f);
    return curve;
}

Curve Curve::Polynomial(double a1, double a2, double a3)
{
    Curve curve;
    curve.m_kind = Kind::Cubic;
    curve.m_a = a1;
    curve.m_b = a2;
    curve.m_c = a3;
    return curve;
}

double Curve::Value(double x) const
{
    double slope = 0.0;
    return Evaluate(x, slope);
}

double Curve::Slope(double x) const
{
    double slope = 0.0;
    Evaluate(x, slope);
    return slope;
}

double Curve::Evaluate(double x, double& slope) const
{
    switch (m_kind)
    {
    case Kind::Zero:
        slope = 0.0;
        return 0.0;
    case Kind::Linear:
        slope = m_a;
        return m_a * x;
    case Kind::Cubic:
        slope = m_a + 2.0 * m_b * x + 3.0 * m_c * x * x;
        return m_a * x + m_b * x * x + m_c * x * x * x;
    case Kind::Stop:
    {
        const double sign = x >= 0.0 ? 1.0 : -1.0;
        if ((m_sign > 0 && x < 0.0) || (m_sign < 0 && x > 0.0))
        {
            slope = 0.0;
            return 0.0;
        }
        const double overlap = std::abs(x) - m_a;
        if (overlap <= 0.0)
        {
            slope = 0.0;
            return 0.0;
        }
        slope = m_b + 2.0 * m_c * overlap;
        return sign * (m_b * overlap + m_c * overlap * overlap);
    }
    case Kind::Polyline:
    {
        const std::size_t n = m_x.size();
        std::size_t i = 0;
        if (x >= m_x[n - 1])
        {
            i = n - 2;
        }
        else if (x > m_x[0])
        {
            i = static_cast<std::size_t>(std::upper_bound(m_x.begin(), m_x.end(), x) - m_x.begin()) - 1;
        }
        slope = (m_f[i + 1] - m_f[i]) / (m_x[i + 1] - m_x[i]);
        return m_f[i] + slope * (x - m_x[i]);
    }
    case Kind::Table:
    {
        if (x <= m_x.front())
        {
            slope = m_m.front();
            return m_f.front() + slope * (x - m_x.front());
        }
        if (x >= m_x.back())
        {
            slope = m_m.back();
            return m_f.back() + slope * (x - m_x.back());
        }
        const std::size_t i = static_cast<std::size_t>(std::upper_bound(m_x.begin(), m_x.end(), x) - m_x.begin()) - 1;
        const double h = m_x[i + 1] - m_x[i];
        const double t = (x - m_x[i]) / h;
        const double t2 = t * t;
        const double t3 = t2 * t;
        const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
        const double h10 = t3 - 2.0 * t2 + t;
        const double h01 = -2.0 * t3 + 3.0 * t2;
        const double h11 = t3 - t2;
        const double d00 = (6.0 * t2 - 6.0 * t) / h;
        const double d10 = 3.0 * t2 - 4.0 * t + 1.0;
        const double d01 = (-6.0 * t2 + 6.0 * t) / h;
        const double d11 = 3.0 * t2 - 2.0 * t;
        slope = d00 * m_f[i] + d10 * m_m[i] + d01 * m_f[i + 1] + d11 * m_m[i + 1];
        return h00 * m_f[i] + h10 * h * m_m[i] + h01 * m_f[i + 1] + h11 * h * m_m[i + 1];
    }
    }
    slope = 0.0;
    return 0.0;
}
}
