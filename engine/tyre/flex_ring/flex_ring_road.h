#pragma once

#include <glm/vec3.hpp>

#include <functional>
#include <vector>

namespace me::tyre::flexring
{

using Vec3 = glm::dvec3;

// The road under a flexible ring tyre: its height z(x, y) in the global frame (z up), the friction scale
// on the tread rubber's friction, and the surface's own velocity (a flat-track belt, a drum). FTire asks
// no more of a road (Gipser 1999, 3.); the tyre finds the gradients itself.
class Road
{
  public:
    virtual ~Road() = default;
    virtual double Height(double x, double y) const = 0;
    virtual double FrictionScale(double /*x*/, double /*y*/) const
    {
        return 1.0;
    }
    virtual Vec3 SurfaceVelocity(double /*x*/, double /*y*/) const
    {
        return Vec3(0.0);
    }
    // Lowest and highest height within a rectangle, for the contact processor's quick test. The default
    // samples a grid; exact roads override it.
    virtual void HeightRange(double x0, double y0, double x1, double y1, double& low, double& high) const;
};

// A plane z = height + slopeX x + slopeY y.
class FlatRoad final : public Road
{
  public:
    explicit FlatRoad(double height = 0.0, double slopeX = 0.0, double slopeY = 0.0, double friction = 1.0);
    double Height(double x, double y) const override;
    double FrictionScale(double x, double y) const override;
    void HeightRange(double x0, double y0, double x1, double y1, double& low, double& high) const override;
    void SetSurfaceVelocity(const Vec3& v)
    {
        m_velocity = v;
    }
    Vec3 SurfaceVelocity(double x, double y) const override;

  private:
    double m_height;
    double m_slopeX;
    double m_slopeY;
    double m_friction;
    Vec3 m_velocity{0.0};
};

// A cleat on a flat road, FTire's test obstacles: a bar of `width` and `height` with bevelled top edges
// (`bevel` wide), a trapezoid or triangle (`topWidth` less than the width: flanks from the base to the
// top), or a half cylinder (semicircular, width = diameter). It crosses the road at `position`
// along `direction` (the unit vector across it, in x-y; the default (1, 0) makes a transversal cleat, a
// longitudinal one is (0, 1)). `length` limits it along its own axis (0: endless).
struct CleatGeometry
{
    double position[2] = {0.0, 0.0};
    double direction[2] = {1.0, 0.0};
    double width = 0.02;
    double height = 0.01;
    double bevel = 0.0;
    double topWidth = -1.0; // < 0: the width (a bar)
    bool semicircular = false;
    double length = 0.0;
};

class CleatRoad final : public Road
{
  public:
    CleatRoad(double baseHeight, const CleatGeometry& cleat, double friction = 1.0);
    double Height(double x, double y) const override;
    double FrictionScale(double x, double y) const override;
    void HeightRange(double x0, double y0, double x1, double y1, double& low, double& high) const override;
    const CleatGeometry& Geometry() const
    {
        return m_cleat;
    }

  private:
    double m_base;
    CleatGeometry m_cleat;
    double m_friction;
};

// A regular height grid (FTire's 'Regular Grid Road'), bilinear between samples, flat outside.
class GridRoad final : public Road
{
  public:
    GridRoad(double x0, double y0, double dx, double dy, int nx, int ny, std::vector<double> heights, double outside = 0.0);
    double Height(double x, double y) const override;

  private:
    double m_x0, m_y0, m_dx, m_dy;
    int m_nx, m_ny;
    std::vector<double> m_heights;
    double m_outside;
};

// A road given by a function (the C API's callback, scripted roads).
class FunctionRoad final : public Road
{
  public:
    explicit FunctionRoad(std::function<double(double, double)> height, std::function<double(double, double)> friction = {});
    double Height(double x, double y) const override;
    double FrictionScale(double x, double y) const override;

  private:
    std::function<double(double, double)> m_height;
    std::function<double(double, double)> m_friction;
};

}
