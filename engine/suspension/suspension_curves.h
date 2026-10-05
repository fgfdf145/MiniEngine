#pragma once

#include <vector>

namespace me::suspension
{

// A one-dimensional force law F(x) with its slope, for springs, bushings, bump stops (x is a
// deflection) and dampers (x is a velocity).
//
// Samples are joined by a monotone cubic (Fritsch-Carlson), so a monotone table stays monotone and
// the slope is continuous, which a Newton solve needs; beyond the table the last segment's slope
// is extended linearly. A curve with no samples is zero; a constant slope is the linear case.
class Curve
{
public:
    Curve() = default;

    static Curve Linear(double slope);
    // F = slope * x for x inside [-range, range], stiffening to `endSlope` at the ends with a cubic
    // term: F = slope * x + c * x^3 with c chosen so dF/dx(range) = endSlope. Odd in x.
    static Curve Progressive(double slope, double endSlope, double range);
    // Zero until |x| passes `gap` on the side `sign` (+1 compression only, -1 tension only, 0 both),
    // then grows as `slope * d + quadratic * d^2` with d the overlap. A one-sided stop's gap may be
    // negative: it then already presses at x = 0 (sign * x > gap).
    static Curve Stop(double gap, double slope, double quadratic, int sign);
    // Samples (x ascending) joined by a monotone cubic.
    static Curve Table(std::vector<double> x, std::vector<double> f);
    // Samples (x ascending) joined by straight lines (a slope that jumps at the samples), extended
    // linearly: a damper's slow and fast ranges as a game's data gives them.
    static Curve Polyline(std::vector<double> x, std::vector<double> f);
    // F = a1 x + a2 x^2 + a3 x^3 (a2 > 0: a rate that rises with compression, not odd in x).
    static Curve Polynomial(double a1, double a2, double a3);

    double Value(double x) const;
    double Slope(double x) const;
    // Value and slope together.
    double Evaluate(double x, double& slope) const;

    bool IsZero() const
    {
        return m_kind == Kind::Zero;
    }

private:
    enum class Kind
    {
        Zero,
        Linear,
        Cubic,
        Stop,
        Table,
        Polyline,
    };

    Kind m_kind = Kind::Zero;
    double m_a = 0.0;
    double m_b = 0.0;
    double m_c = 0.0;
    int m_sign = 0;
    std::vector<double> m_x;
    std::vector<double> m_f;
    std::vector<double> m_m;
};
}
