#include "flex_ring_rig.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <cstdio>
#include <cstdlib>

namespace me::tyre::flexring
{

namespace
{
using Clock = std::chrono::steady_clock;

// A free wheel's spin inertia on the rigs (the R34's wheel and tyre in the game's data: 1.62 kg m^2).
constexpr double kWheelInertia = 1.6;

double Seconds(Clock::time_point since)
{
    return std::chrono::duration<double>(Clock::now() - since).count();
}

double BeltWeight(const FlexRingParameters& p)
{
    return p.gravity ? 9.81 * p.nodeMass * p.segments : 0.0;
}

// Keeps a tyre rolling on a flat road from one steady point to the next.
class SteadyRun
{
  public:
    explicit SteadyRun(const FlexRingParameters& parameters)
        : m_tyre(parameters), m_road(0.0)
    {
        m_tyre.SetRoad(&m_road);
        m_kv = 2.5e5;
    }

    FlexRingTyre& Tyre()
    {
        return m_tyre;
    }

    // Static load at a standstill, the start of a run.
    void Load(const SteadySettings& s)
    {
        m_road = FlatRoad(0.0, 0.0, 0.0, s.roadFriction);
        RigPose pose;
        pose.camber = s.camber;
        m_touch = RestTouchHeight(m_tyre, pose);
        const double guess = 2.5e5;
        m_deflection = DeflectionForLoad(m_tyre, m_road, pose, s.load, guess, 0.01);
        if (m_deflection > 1.0e-4)
        {
            m_kv = std::max(s.load / m_deflection, 1.0e4);
        }
        m_x = 0.0;
        m_y = 0.0;
        m_spinAngle = 0.0;
        m_spin = 0.0;
        m_speed = 0.0;
        m_camber = s.camber;
        m_slipAngle = 0.0;
        m_loaded = true;
    }

    SteadyPoint Run(const SteadySettings& s, double ramp)
    {
        const Clock::time_point start = Clock::now();
        if (!m_loaded)
        {
            Load(s);
        }
        const double re = s.effectiveRadius > 0.0 ? s.effectiveRadius : m_tyre.Parameters().beltRadius;
        // From a standstill the rim speeds up over at least 0.2 s with the wheel turning along.
        const double speedFrom = m_speed;
        const double speedRamp = std::abs(s.speed - speedFrom) > 1.0e-6 * std::max(s.speed, 1.0) ? std::max(ramp, 0.2) : 0.0;
        const double spinFrom = m_spin;
        const double camberFrom = m_camber;
        const double angleFrom = m_slipAngle;
        const int steps = static_cast<int>(std::ceil((s.settleTime + s.averageTime + speedRamp) / s.dt));
        const int averageFrom = static_cast<int>(std::ceil((s.settleTime + speedRamp) / s.dt));
        // A free wheel's inertia: small, so it finds its free-rolling speed within the settling.
        const double inertia = kWheelInertia;
        RoadForces sum;
        ContactStats lastContact;
        double spinSum = 0.0;
        int count = 0;
        const double weight = BeltWeight(m_tyre.Parameters());
        double vx = s.speed * std::cos(s.slipAngle);
        for (int i = 0; i < steps; ++i)
        {
            const double t = (i + 1) * s.dt;
            const double blend = ramp > 0.0 ? std::min(t / ramp, 1.0) : 1.0;
            const double speedBlend = speedRamp > 0.0 ? std::min(t / speedRamp, 1.0) : 1.0;
            // Smooth start: (3 - 2 b) b^2.
            const double sb = speedBlend * speedBlend * (3.0 - 2.0 * speedBlend);
            m_speed = speedFrom + (s.speed - speedFrom) * sb;
            m_camber = camberFrom + (s.camber - camberFrom) * blend;
            m_slipAngle = angleFrom + (s.slipAngle - angleFrom) * blend;
            const double cvx = m_speed * std::cos(m_slipAngle);
            const double cvy = -m_speed * std::sin(m_slipAngle);
            vx = cvx;
            m_x += cvx * s.dt;
            m_y += cvy * s.dt;
            RigPose pose;
            pose.x = m_x;
            pose.y = m_y;
            pose.camber = m_camber;
            if (speedBlend < 1.0)
            {
                m_spin = (s.freeRolling ? 1.0 : 1.0 + s.slipRatio) * cvx / re;
            }
            else if (s.freeRolling)
            {
                const Vec3 axis = m_tyre.Rim().rotation[1];
                const double moment = glm::dot(m_tyre.LastWrench().moment, axis);
                m_spin += s.dt * moment / inertia;
                // A free wheel stays near V / R; a runaway (a point past the integrator's reach) is held there.
                m_spin = std::clamp(m_spin, 0.5 * cvx / re, 1.5 * cvx / re);
            }
            else
            {
                const double target = (1.0 + s.slipRatio) * cvx / re;
                m_spin = spinFrom + (target - spinFrom) * blend;
            }
            m_spinAngle += m_spin * s.dt;
            const double height = m_touch - m_deflection;
            const RimState rim = MakeRim(pose, height, m_spinAngle, Vec3(cvx, cvy, 0.0), m_spin);
            const Wrench w = m_tyre.Advance(rim, s.dt);
            // Load control: the road's vertical force, low-pass filtered over 20 ms (the belt's own vertical
            // vibration is in it), regulated by the rim height with a slow integral gain.
            const double load = m_tyre.Contact().normalForce;
            if (std::isfinite(load))
            {
                m_filteredLoad = m_filteredLoad > 0.0 ? m_filteredLoad + (load - m_filteredLoad) * std::min(s.dt / 0.02, 1.0) : load;
                m_deflection += 0.03 * (s.load - m_filteredLoad) / m_kv;
                m_deflection = std::max(m_deflection, 0.0);
            }
            if (i >= averageFrom)
            {
                const RoadForces f = ToRoadAxes(w, 0.0);
                sum.fx += f.fx;
                sum.fy += f.fy;
                sum.fz += f.fz + weight;
                sum.mx += f.mx;
                sum.my += f.my;
                sum.mz += f.mz;
                spinSum += m_spin;
                count += 1;
                lastContact = m_tyre.Contact();
            }
        }
        SteadyPoint point;
        point.settings = s;
        const double inv = count > 0 ? 1.0 / count : 0.0;
        point.forces.fx = sum.fx * inv;
        point.forces.fy = sum.fy * inv;
        point.forces.fz = sum.fz * inv;
        point.forces.mx = sum.mx * inv;
        point.forces.my = sum.my * inv;
        point.forces.mz = sum.mz * inv;
        point.spinRate = spinSum * inv;
        point.effectiveRadius = point.spinRate > 0.0 ? vx / point.spinRate : 0.0;
        point.contact = lastContact.blocks > 0 ? lastContact : m_tyre.Contact();
        point.deflection = m_deflection;
        point.cpuSeconds = Seconds(start);
        return point;
    }

  private:
    FlexRingTyre m_tyre;
    FlatRoad m_road;
    bool m_loaded = false;
    double m_touch = 0.0;
    double m_deflection = 0.0;
    double m_kv = 2.5e5;
    double m_x = 0.0;
    double m_y = 0.0;
    double m_spinAngle = 0.0;
    double m_spin = 0.0;
    double m_speed = 0.0;
    double m_camber = 0.0;
    double m_slipAngle = 0.0;
    double m_filteredLoad = 0.0;
};
}

RimState MakeRim(const RigPose& pose, double centreHeight, double spinAngle, const Vec3& velocity, double spinRate)
{
    const double ch = std::cos(pose.heading);
    const double sh = std::sin(pose.heading);
    const double cc = std::cos(pose.camber);
    const double sc = std::sin(pose.camber);
    const double cs = std::cos(spinAngle);
    const double ss = std::sin(spinAngle);
    const Mat3 rz(Vec3(ch, sh, 0.0), Vec3(-sh, ch, 0.0), Vec3(0.0, 0.0, 1.0));
    const Mat3 rx(Vec3(1.0, 0.0, 0.0), Vec3(0.0, cc, sc), Vec3(0.0, -sc, cc));
    const Mat3 ry(Vec3(cs, 0.0, -ss), Vec3(0.0, 1.0, 0.0), Vec3(ss, 0.0, cs));
    RimState rim;
    rim.rotation = rz * rx * ry;
    rim.position = Vec3(pose.x, pose.y, centreHeight);
    rim.velocity = velocity;
    rim.angularVelocity = spinRate * rim.rotation[1];
    return rim;
}

double RestTouchHeight(FlexRingTyre& tyre, const RigPose& pose)
{
    RigPose at = pose;
    at.x = 0.0;
    at.y = 0.0;
    tyre.Reset(MakeRim(at, 0.0, 0.0, Vec3(0.0), 0.0));
    const FlexRingParameters& p = tyre.Parameters();
    double lowest = 0.0;
    const int n = tyre.Segments();
    for (int k = 0; k < n; ++k)
    {
        const Vec3 node = tyre.NodePositions()[k];
        if (node.z > -0.5 * p.beltRadius)
        {
            continue;
        }
        for (int i = 0; i <= 8; ++i)
        {
            const double sigma = i / 8.0;
            for (int j = 0; j <= 20; ++j)
            {
                const double lateral = -0.5 * p.treadWidth + p.treadWidth * j / 20.0;
                const Vec3 point = tyre.SurfacePoint(k, sigma, lateral, true);
                lowest = std::min(lowest, point.z);
            }
        }
    }
    return -lowest;
}

RoadForces ToRoadAxes(const Wrench& w, double heading)
{
    const Vec3 x(std::cos(heading), std::sin(heading), 0.0);
    const Vec3 y(-std::sin(heading), std::cos(heading), 0.0);
    const Vec3 z(0.0, 0.0, 1.0);
    RoadForces f;
    f.fx = glm::dot(w.force, x);
    f.fy = glm::dot(w.force, y);
    f.fz = glm::dot(w.force, z);
    f.mx = glm::dot(w.moment, x);
    f.my = glm::dot(w.moment, y);
    f.mz = glm::dot(w.moment, z);
    return f;
}

std::vector<StaticPoint> StaticDeflection(FlexRingTyre& tyre, const Road& road, const RigPose& pose, const std::vector<double>& deflections, double increment)
{
    std::vector<StaticPoint> points;
    const double touch = RestTouchHeight(tyre, pose);
    const double ground = road.Height(pose.x, pose.y);
    tyre.SetRoad(&road);
    tyre.Reset(MakeRim(pose, ground + touch, 0.0, Vec3(0.0), 0.0));
    double current = 0.0;
    for (const double target : deflections)
    {
        bool converged = true;
        while (current < target - 1.0e-12)
        {
            current = std::min(target, current + std::max(increment, 1.0e-5));
            converged = tyre.SettleStatic(MakeRim(pose, ground + touch - current, 0.0, Vec3(0.0), 0.0)) && converged;
        }
        if (target <= 0.0)
        {
            converged = tyre.SettleStatic(MakeRim(pose, ground + touch - target, 0.0, Vec3(0.0), 0.0));
        }
        StaticPoint point;
        point.deflection = target;
        point.forces = ToRoadAxes(tyre.LastWrench(), pose.heading);
        point.contact = tyre.Contact();
        point.converged = converged;
        points.push_back(point);
    }
    return points;
}

double DeflectionForLoad(FlexRingTyre& tyre, const Road& road, const RigPose& pose, double load, double stiffnessGuess, double tolerance)
{
    // Bracket the load, then regula falsi (Illinois): each static solve loads the tyre afresh, so the
    // wheel load is a slightly noisy function of the deflection and a plain secant can run away.
    const auto loadAt = [&](double d) {
        return StaticDeflection(tyre, road, pose, {d}).back().contact.normalForce;
    };
    double lo = 0.0;
    double fLo = 0.0;
    double hi = std::clamp(load / std::max(stiffnessGuess, 1.0e3), 0.001, 0.1);
    double fHi = loadAt(hi);
    for (int grow = 0; grow < 20 && fHi < load; ++grow)
    {
        lo = hi;
        fLo = fHi;
        hi *= 1.6;
        fHi = loadAt(hi);
    }
    double d = hi;
    double f = fHi;
    int side = 0;
    for (int it = 0; it < 30 && std::abs(f - load) > tolerance * load; ++it)
    {
        d = (lo * (fHi - load) - hi * (fLo - load)) / std::max(fHi - fLo, 1.0e-9);
        d = std::clamp(d, lo + 0.02 * (hi - lo), hi - 0.02 * (hi - lo));
        f = loadAt(d);
        if (f < load)
        {
            lo = d;
            fLo = f;
            if (side == -1)
            {
                fHi = load + 0.5 * (fHi - load);
            }
            side = -1;
        }
        else
        {
            hi = d;
            fHi = f;
            if (side == 1)
            {
                fLo = load + 0.5 * (fLo - load);
            }
            side = 1;
        }
    }
    if (std::abs(f - load) > tolerance * load || d != hi)
    {
        // Leave the tyre at the returned deflection.
        f = loadAt(d);
    }
    return d;
}

StaticStiffness MeasureStaticStiffness(const FlexRingParameters& parameters, double deflection)
{
    StaticStiffness result;
    FlatRoad road(0.0);
    FlexRingTyre base(parameters);
    RigPose pose;
    const std::vector<StaticPoint> loaded = StaticDeflection(base, road, pose, {deflection});
    const double touch = RestTouchHeight(base, pose);
    // RestTouchHeight resets: load again.
    StaticDeflection(base, road, pose, {deflection});
    result.load = loaded.back().contact.normalForce;
    result.vertical = deflection > 0.0 ? result.load / deflection : 0.0;
    const double height = touch - deflection;
    const double step = 0.0005;
    const double turn = 0.002;
    {
        FlexRingTyre t = base;
        t.SetRoad(&road);
        RigPose moved = pose;
        moved.x = step;
        t.SettleStatic(MakeRim(moved, height, 0.0, Vec3(0.0), 0.0));
        const double f = ToRoadAxes(t.LastWrench(), 0.0).fx - ToRoadAxes(base.LastWrench(), 0.0).fx;
        result.longitudinal = -f / step;
    }
    {
        FlexRingTyre t = base;
        t.SetRoad(&road);
        RigPose moved = pose;
        moved.y = step;
        t.SettleStatic(MakeRim(moved, height, 0.0, Vec3(0.0), 0.0));
        const double f = ToRoadAxes(t.LastWrench(), 0.0).fy - ToRoadAxes(base.LastWrench(), 0.0).fy;
        result.lateral = -f / step;
    }
    {
        FlexRingTyre t = base;
        t.SetRoad(&road);
        RigPose moved = pose;
        moved.heading = turn;
        t.SettleStatic(MakeRim(moved, height, 0.0, Vec3(0.0), 0.0));
        const double m = ToRoadAxes(t.LastWrench(), 0.0).mz - ToRoadAxes(base.LastWrench(), 0.0).mz;
        result.torsional = -m / turn;
    }
    return result;
}

SteadyPoint RunSteady(const FlexRingParameters& parameters, const SteadySettings& settings, FlexRingTyre* warmStart)
{
    (void)warmStart;
    SteadySettings s = settings;
    SteadyRun run(parameters);
    if (!s.freeRolling && s.effectiveRadius <= 0.0)
    {
        SteadySettings free = s;
        free.freeRolling = true;
        free.slipAngle = 0.0;
        const SteadyPoint p = run.Run(free, 0.0);
        s.effectiveRadius = p.effectiveRadius;
        return run.Run(s, 0.05);
    }
    return run.Run(s, 0.0);
}

std::vector<SteadyPoint> RunSweep(const FlexRingParameters& parameters, SweepKind kind, const std::vector<double>& values, SteadySettings base,
                                  const std::atomic<bool>* cancel)
{
    std::vector<SteadyPoint> points;
    SteadyRun run(parameters);
    // Free rolling first: the effective rolling radius, and a warm tyre.
    SteadySettings free = base;
    free.freeRolling = true;
    free.slipAngle = kind == SweepKind::SlipAngle ? 0.0 : base.slipAngle;
    free.camber = kind == SweepKind::Camber ? 0.0 : base.camber;
    const SteadyPoint rolling = run.Run(free, 0.0);
    if (base.effectiveRadius <= 0.0)
    {
        base.effectiveRadius = rolling.effectiveRadius;
    }
    for (const double value : values)
    {
        if (cancel != nullptr && cancel->load())
        {
            break;
        }
        SteadySettings s = base;
        switch (kind)
        {
        case SweepKind::SlipAngle:
            s.slipAngle = value;
            break;
        case SweepKind::SlipRatio:
            s.slipRatio = value;
            s.freeRolling = false;
            break;
        case SweepKind::Camber:
            s.camber = value;
            break;
        }
        points.push_back(run.Run(s, 0.05));
    }
    return points;
}

std::vector<CleatSample> RunCleat(const FlexRingParameters& parameters, const CleatSettings& c, const std::atomic<bool>* cancel)
{
    std::vector<CleatSample> samples;
    // Loaded at a standstill on the flat road ahead of the cleat (the spindle height for the load), then
    // rolled up to speed over 0.2 s and settled for 0.15 s before the run-up; the spindle height stays.
    const double ramp = 0.2;
    const double settle = 0.15;
    const double startX = -(c.runUp + c.speed * (0.5 * ramp + settle));
    CleatRoad road(0.0, c.cleat);
    FlexRingTyre tyre(parameters);
    RigPose pose;
    pose.x = startX;
    const double touch = RestTouchHeight(tyre, pose);
    const double deflection = DeflectionForLoad(tyre, road, pose, c.load, 2.5e5, 1.0e-3);
    const double height = touch - deflection;
    const double radius = parameters.beltRadius;
    double spin = 0.0;
    double angle = 0.0;
    double x = startX;
    double t = 0.0;
    double time = -(ramp + settle) - c.runUp / c.speed;
    const double inertia = kWheelInertia;
    while (x < c.runOut)
    {
        if (cancel != nullptr && cancel->load())
        {
            break;
        }
        t += c.dt;
        time += c.dt;
        const double b = std::min(t / ramp, 1.0);
        const double speed = c.speed * b * b * (3.0 - 2.0 * b);
        x += speed * c.dt;
        if (b < 1.0)
        {
            spin = speed / radius;
        }
        else
        {
            const Vec3 axis = tyre.Rim().rotation[1];
            spin += c.dt * glm::dot(tyre.LastWrench().moment, axis) / inertia;
            spin = std::clamp(spin, 0.5 * c.speed / radius, 1.5 * c.speed / radius);
        }
        angle += spin * c.dt;
        pose.x = x;
        const Wrench w = tyre.Advance(MakeRim(pose, height, angle, Vec3(speed, 0.0, 0.0), spin), c.dt);
        if (x >= -c.runUp)
        {
            CleatSample sample;
            sample.time = time;
            sample.position = x;
            sample.forces = ToRoadAxes(w, 0.0);
            sample.spinRate = spin;
            samples.push_back(sample);
        }
    }
    return samples;
}

BenchmarkResult RunBenchmark(const FlexRingParameters& parameters, double simulatedSeconds, bool fourInParallel)
{
    BenchmarkResult result;
    result.segments = parameters.segments;
    result.blocksPerSegment = static_cast<int>(parameters.blocks.size());
    result.maxStep = parameters.maxStep;
    result.maxAngle = parameters.maxAngleIncrement;
    SteadySettings s;
    s.load = 4000.0;
    s.slipAngle = 2.0 * 3.14159265358979 / 180.0;
    s.settleTime = 0.1;
    s.averageTime = 0.0;
    SteadySettings timed = s;
    timed.settleTime = simulatedSeconds;
    {
        SteadyRun run(parameters);
        run.Run(s, 0.0);
        const SteadyPoint p = run.Run(timed, 0.0);
        result.secondsPerSimulatedSecond = p.cpuSeconds / simulatedSeconds;
        const double spin = s.speed / parameters.beltRadius;
        const double steps = std::max(std::ceil(s.dt / parameters.maxStep - 1.0e-9), std::ceil(spin * s.dt / parameters.maxAngleIncrement - 1.0e-9)) / s.dt;
        result.stepsPerSecond = steps;
        result.microsecondsPerStep = p.cpuSeconds / (steps * simulatedSeconds) * 1.0e6;
        result.blocksInContact = run.Tyre().Contact().blocks;
    }
    if (fourInParallel)
    {
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::array<double, 4> seconds{};
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
        {
            threads.emplace_back([&, i] {
                SteadyRun run(parameters);
                run.Run(s, 0.0);
                ready++;
                while (!go.load())
                {
                    std::this_thread::yield();
                }
                seconds[i] = run.Run(timed, 0.0).cpuSeconds;
            });
        }
        while (ready.load() < 4)
        {
            std::this_thread::yield();
        }
        go = true;
        for (std::thread& t : threads)
        {
            t.join();
        }
        result.parallelFourTyres = *std::max_element(seconds.begin(), seconds.end()) / simulatedSeconds;
    }
    return result;
}

void ParallelFor(int count, const std::function<void(int)>& body, int threads)
{
    if (count <= 0)
    {
        return;
    }
    int workers = threads > 0 ? threads : static_cast<int>(std::thread::hardware_concurrency());
    workers = std::clamp(workers, 1, count);
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    for (int w = 0; w < workers; ++w)
    {
        pool.emplace_back([&] {
            for (int i = next++; i < count; i = next++)
            {
                body(i);
            }
        });
    }
    for (std::thread& t : pool)
    {
        t.join();
    }
}

}
