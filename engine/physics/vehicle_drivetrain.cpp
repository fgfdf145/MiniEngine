#include "vehicle_drivetrain.h"

#include <algorithm>
#include <cmath>

namespace me
{
namespace
{
// The unknowns, the parts' speeds at the step's end: the engine, the rims, the belts.
constexpr size_t kUnknowns = 1 + 2 * kDrivetrainWheels;
constexpr size_t kEngine = 0;

constexpr size_t Rim(size_t wheel)
{
    return 1 + wheel;
}

constexpr size_t Belt(size_t wheel)
{
    return 1 + kDrivetrainWheels + wheel;
}

// The frictions: the clutch, the brakes and the rolling resistances.
constexpr size_t kMaxFrictions = 1 + 2 * kDrivetrainWheels;
// Pivots of the active set before the projected Gauss-Seidel takes over; a few suffice in practice.
constexpr int kMaxPivots = 32;
constexpr int kGaussSeidelSweeps = 200;
// How far a held friction's slip may be past zero (rad/s) before it counts as slipping the wrong way.
constexpr double kSlipTolerance = 1e-9;

using Vector = std::array<double, kUnknowns>;
using Matrix = std::array<Vector, kUnknowns>;

// The step's matrix, factored once for the free motion and each friction's direction.
struct Factored
{
    Matrix lu{};
    std::array<size_t, kUnknowns> order{};
};

Factored Factor(Matrix a)
{
    Factored f;
    for (size_t row = 0; row < kUnknowns; ++row)
    {
        f.order[row] = row;
    }
    for (size_t col = 0; col < kUnknowns; ++col)
    {
        size_t pivot = col;
        for (size_t row = col + 1; row < kUnknowns; ++row)
        {
            if (std::abs(a[row][col]) > std::abs(a[pivot][col]))
            {
                pivot = row;
            }
        }
        std::swap(a[col], a[pivot]);
        std::swap(f.order[col], f.order[pivot]);
        for (size_t row = col + 1; row < kUnknowns; ++row)
        {
            a[row][col] /= a[col][col];
            for (size_t k = col + 1; k < kUnknowns; ++k)
            {
                a[row][k] -= a[row][col] * a[col][k];
            }
        }
    }
    f.lu = a;
    return f;
}

Vector SolveFactored(const Factored& f, const Vector& b)
{
    Vector x{};
    for (size_t row = 0; row < kUnknowns; ++row)
    {
        double value = b[f.order[row]];
        for (size_t k = 0; k < row; ++k)
        {
            value -= f.lu[row][k] * x[k];
        }
        x[row] = value;
    }
    for (size_t row = kUnknowns; row-- > 0;)
    {
        double value = x[row];
        for (size_t k = row + 1; k < kUnknowns; ++k)
        {
            value -= f.lu[row][k] * x[k];
        }
        x[row] = value / f.lu[row][row];
    }
    return x;
}

double Dot(const Vector& a, const Vector& b)
{
    double sum = 0.0;
    for (size_t k = 0; k < kUnknowns; ++k)
    {
        sum += a[k] * b[k];
    }
    return sum;
}

// Solves the first n rows and columns of m x = rhs (partial pivoting); false when singular.
bool SolveSmall(std::array<std::array<double, kMaxFrictions + 1>, kMaxFrictions> m, size_t n, std::array<double, kMaxFrictions>& x)
{
    for (size_t col = 0; col < n; ++col)
    {
        size_t pivot = col;
        for (size_t row = col + 1; row < n; ++row)
        {
            if (std::abs(m[row][col]) > std::abs(m[pivot][col]))
            {
                pivot = row;
            }
        }
        std::swap(m[col], m[pivot]);
        if (!(std::abs(m[col][col]) > 1e-300))
        {
            return false;
        }
        for (size_t row = col + 1; row < n; ++row)
        {
            const double factor = m[row][col] / m[col][col];
            for (size_t k = col; k <= n; ++k)
            {
                m[row][k] -= factor * m[col][k];
            }
        }
    }
    for (size_t row = n; row-- > 0;)
    {
        double value = m[row][n];
        for (size_t k = row + 1; k < n; ++k)
        {
            value -= m[row][k] * x[k];
        }
        x[row] = value / m[row][row];
    }
    return true;
}

enum class Bound
{
    Held,  // inside its capacity, its two sides together
    Lower, // slipping forward, at its capacity against it
    Upper, // slipping backward
};

struct Friction
{
    Vector direction{}; // its slip is direction . speeds
    double limit = 0.0; // the most impulse it passes in the step
    Vector response{};  // the speeds a unit impulse along it adds
};
}

DrivetrainResult StepDrivetrain(const std::array<DrivetrainWheel, kDrivetrainWheels>& wheels, DrivetrainState& state, const DrivetrainStep& step)
{
    DrivetrainResult result;
    const double dt = step.dt;
    if (!(dt > 0.0))
    {
        return result;
    }

    // (M + dt D) w' = M w + dt f + J^T impulses, with the sidewalls' spring by the theta method: its force
    // over the step is k (twist + theta dt ((1 - theta) gap + theta gap')), gap the rim's speed less the
    // belt's, and the twist moves by dt ((1 - theta) gap + theta gap'). Theta is a half (the trapezoidal
    // rule, which keeps a lightly damped ring) while the step resolves the twist's ring, omega dt up to 1
    // (every wheel at the 1 ms step), and rises to 1 (backward Euler) by omega dt = 3: the trapezoidal rule
    // keeps a ring it cannot resolve, flipping from step to step, and at 60 Hz that shook the rims the
    // anti-lock brakes read.
    Matrix a{};
    Vector b{};
    const double engineInertia = std::max(step.engineInertia, 1e-4);
    a[kEngine][kEngine] = engineInertia * (1.0 + dt * std::max(step.engineDamping, 0.0));
    b[kEngine] = engineInertia * state.engine + dt * step.engineTorque;
    std::array<double, kDrivetrainWheels> springNow{};
    std::array<double, kDrivetrainWheels> springRate{};
    std::array<double, kDrivetrainWheels> theta{};
    for (size_t wheel = 0; wheel < kDrivetrainWheels; ++wheel)
    {
        const DrivetrainWheel& w = wheels[wheel];
        const double rimInertia = std::max(w.rimInertia, 1e-4);
        const double beltInertia = std::max(w.beltInertia, 1e-4);
        const double stiffness = std::max(w.sidewallStiffness, 0.0);
        const double gap = state.rim[wheel] - state.belt[wheel];
        const double ring = std::sqrt(stiffness * (1.0 / rimInertia + 1.0 / beltInertia)) * dt;
        const double t = theta[wheel] = 0.5 + 0.5 * std::clamp((ring - 1.0) / 2.0, 0.0, 1.0);
        springNow[wheel] = stiffness * (state.twist[wheel] + t * (1.0 - t) * dt * gap);
        springRate[wheel] = std::max(w.sidewallDamping, 0.0) + t * t * dt * stiffness;
        const size_t r = Rim(wheel);
        const size_t s = Belt(wheel);
        const double road = std::max(step.beltDamping[wheel], 0.0);
        a[r][r] = rimInertia + dt * springRate[wheel];
        a[s][s] = beltInertia + dt * (springRate[wheel] + road);
        a[r][s] = -dt * springRate[wheel];
        a[s][r] = -dt * springRate[wheel];
        b[r] = rimInertia * state.rim[wheel] + dt * (step.rimTorque[wheel] - springNow[wheel]);
        b[s] = beltInertia * state.belt[wheel] + dt * (step.beltTorque[wheel] + road * state.belt[wheel] + springNow[wheel]);
    }
    const Factored factored = Factor(a);
    const Vector free = SolveFactored(factored, b);

    // The frictions that act this step.
    std::array<Friction, kMaxFrictions> frictions{};
    std::array<int, kMaxFrictions> brakeOf{};
    std::array<int, kMaxFrictions> rollingOf{};
    brakeOf.fill(-1);
    rollingOf.fill(-1);
    int clutch = -1;
    size_t count = 0;
    bool geared = false;
    for (const double weight : step.clutchWeights)
    {
        geared = geared || weight != 0.0;
    }
    if (step.clutchCapacity > 0.0 && geared)
    {
        Friction& f = frictions[count];
        f.direction[kEngine] = 1.0;
        for (size_t wheel = 0; wheel < kDrivetrainWheels; ++wheel)
        {
            f.direction[Rim(wheel)] = -step.clutchWeights[wheel];
        }
        f.limit = step.clutchCapacity * dt;
        clutch = static_cast<int>(count++);
    }
    for (size_t wheel = 0; wheel < kDrivetrainWheels; ++wheel)
    {
        if (step.brakeCapacity[wheel] > 0.0)
        {
            Friction& f = frictions[count];
            f.direction[Rim(wheel)] = 1.0;
            f.limit = step.brakeCapacity[wheel] * dt;
            brakeOf[count++] = static_cast<int>(wheel);
        }
    }
    for (size_t wheel = 0; wheel < kDrivetrainWheels; ++wheel)
    {
        if (step.rollingCapacity[wheel] > 0.0)
        {
            Friction& f = frictions[count];
            f.direction[Belt(wheel)] = 1.0;
            f.limit = step.rollingCapacity[wheel] * dt;
            rollingOf[count++] = static_cast<int>(wheel);
        }
    }

    // The slips are g = g0 + W impulses, with W the frictions' coupling through the parts.
    std::array<double, kMaxFrictions> slip0{};
    std::array<std::array<double, kMaxFrictions>, kMaxFrictions> coupling{};
    for (size_t e = 0; e < count; ++e)
    {
        frictions[e].response = SolveFactored(factored, frictions[e].direction);
        slip0[e] = Dot(frictions[e].direction, free);
    }
    for (size_t e = 0; e < count; ++e)
    {
        for (size_t f = 0; f < count; ++f)
        {
            coupling[e][f] = Dot(frictions[e].direction, frictions[f].response);
        }
    }
    const auto slipOf = [&](size_t e, const std::array<double, kMaxFrictions>& impulse)
    {
        double g = slip0[e];
        for (size_t f = 0; f < count; ++f)
        {
            g += coupling[e][f] * impulse[f];
        }
        return g;
    };

    // The active set: all held at first; a held one past its capacity slips at it, a slipping one whose
    // slip turns against its push is held again.
    std::array<Bound, kMaxFrictions> bound{};
    std::array<double, kMaxFrictions> impulse{};
    bool solved = false;
    for (int pivot = 0; pivot < kMaxPivots && count > 0; ++pivot)
    {
        result.pivots = pivot + 1;
        std::array<size_t, kMaxFrictions> held{};
        size_t n = 0;
        for (size_t e = 0; e < count; ++e)
        {
            if (bound[e] == Bound::Held)
            {
                held[n++] = e;
            }
            else
            {
                impulse[e] = bound[e] == Bound::Lower ? -frictions[e].limit : frictions[e].limit;
            }
        }
        if (n > 0)
        {
            std::array<std::array<double, kMaxFrictions + 1>, kMaxFrictions> m{};
            for (size_t r = 0; r < n; ++r)
            {
                double right = -slip0[held[r]];
                for (size_t e = 0; e < count; ++e)
                {
                    if (bound[e] != Bound::Held)
                    {
                        right -= coupling[held[r]][e] * impulse[e];
                    }
                }
                for (size_t c = 0; c < n; ++c)
                {
                    m[r][c] = coupling[held[r]][held[c]];
                }
                m[r][n] = right;
            }
            std::array<double, kMaxFrictions> x{};
            if (!SolveSmall(m, n, x))
            {
                break;
            }
            for (size_t r = 0; r < n; ++r)
            {
                impulse[held[r]] = x[r];
            }
        }
        bool changed = false;
        for (size_t e = 0; e < count; ++e)
        {
            if (bound[e] == Bound::Held && std::abs(impulse[e]) > frictions[e].limit)
            {
                bound[e] = impulse[e] > 0.0 ? Bound::Upper : Bound::Lower;
                changed = true;
            }
        }
        if (!changed)
        {
            for (size_t e = 0; e < count; ++e)
            {
                const double g = slipOf(e, impulse);
                if ((bound[e] == Bound::Lower && g < -kSlipTolerance) || (bound[e] == Bound::Upper && g > kSlipTolerance))
                {
                    bound[e] = Bound::Held;
                    changed = true;
                }
            }
        }
        if (!changed)
        {
            solved = true;
            break;
        }
    }
    if (count > 0 && !solved)
    {
        // Projected Gauss-Seidel from where the pivots left off.
        result.converged = false;
        for (int sweep = 0; sweep < kGaussSeidelSweeps; ++sweep)
        {
            for (size_t e = 0; e < count; ++e)
            {
                const double g = slipOf(e, impulse);
                impulse[e] = std::clamp(impulse[e] - g / coupling[e][e], -frictions[e].limit, frictions[e].limit);
            }
        }
        for (size_t e = 0; e < count; ++e)
        {
            const bool atLimit = std::abs(impulse[e]) >= frictions[e].limit * (1.0 - 1e-9);
            bound[e] = !atLimit ? Bound::Held : (impulse[e] > 0.0 ? Bound::Upper : Bound::Lower);
        }
    }

    Vector speeds = free;
    for (size_t e = 0; e < count; ++e)
    {
        for (size_t k = 0; k < kUnknowns; ++k)
        {
            speeds[k] += frictions[e].response[k] * impulse[e];
        }
        const double torque = impulse[e] / dt;
        if (static_cast<int>(e) == clutch)
        {
            result.clutchTorque = -torque;
            result.clutchLocked = bound[e] == Bound::Held;
        }
        else if (brakeOf[e] >= 0)
        {
            result.brakeTorque[static_cast<size_t>(brakeOf[e])] = torque;
            result.brakeLocked[static_cast<size_t>(brakeOf[e])] = bound[e] == Bound::Held;
        }
        else if (rollingOf[e] >= 0)
        {
            result.rollingTorque[static_cast<size_t>(rollingOf[e])] = torque;
        }
    }

    state.engine = speeds[kEngine];
    for (size_t wheel = 0; wheel < kDrivetrainWheels; ++wheel)
    {
        const double gap = state.rim[wheel] - state.belt[wheel];
        const double gapAfter = speeds[Rim(wheel)] - speeds[Belt(wheel)];
        result.sidewallTorque[wheel] = springNow[wheel] + springRate[wheel] * gapAfter;
        state.twist[wheel] += dt * ((1.0 - theta[wheel]) * gap + theta[wheel] * gapAfter);
        state.rim[wheel] = speeds[Rim(wheel)];
        state.belt[wheel] = speeds[Belt(wheel)];
    }
    return result;
}
}
