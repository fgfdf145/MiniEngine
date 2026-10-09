#include "flex_ring_modal.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace me::tyre::flexring
{

void SymmetricEigen(int n, std::vector<double> a, std::vector<double>& values, std::vector<double>& vectors)
{
    vectors.assign(static_cast<size_t>(n) * n, 0.0);
    for (int i = 0; i < n; ++i)
    {
        vectors[static_cast<size_t>(i) * n + i] = 1.0;
    }
    const auto A = [&](int i, int j) -> double& {
        return a[static_cast<size_t>(i) * n + j];
    };
    for (int sweep = 0; sweep < 100; ++sweep)
    {
        double off = 0.0;
        double scale = 0.0;
        for (int i = 0; i < n; ++i)
        {
            for (int j = 0; j < n; ++j)
            {
                (i == j ? scale : off) += A(i, j) * A(i, j);
            }
        }
        if (off <= 1.0e-30 * std::max(scale, 1.0e-300))
        {
            break;
        }
        for (int p = 0; p < n - 1; ++p)
        {
            for (int q = p + 1; q < n; ++q)
            {
                const double apq = A(p, q);
                if (std::abs(apq) < 1.0e-300)
                {
                    continue;
                }
                const double theta = 0.5 * (A(q, q) - A(p, p)) / apq;
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (int k = 0; k < n; ++k)
                {
                    const double akp = A(k, p);
                    const double akq = A(k, q);
                    A(k, p) = c * akp - s * akq;
                    A(k, q) = s * akp + c * akq;
                }
                for (int k = 0; k < n; ++k)
                {
                    const double apk = A(p, k);
                    const double aqk = A(q, k);
                    A(p, k) = c * apk - s * aqk;
                    A(q, k) = s * apk + c * aqk;
                }
                for (int k = 0; k < n; ++k)
                {
                    double& vkp = vectors[static_cast<size_t>(k) * n + p];
                    double& vkq = vectors[static_cast<size_t>(k) * n + q];
                    const double x = vkp;
                    const double y = vkq;
                    vkp = c * x - s * y;
                    vkq = s * x + c * y;
                }
            }
        }
    }
    values.resize(n);
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i)
    {
        values[i] = A(i, i);
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](int x, int y) {
        return values[x] < values[y];
    });
    std::vector<double> sortedValues(n);
    std::vector<double> sortedVectors(static_cast<size_t>(n) * n);
    for (int i = 0; i < n; ++i)
    {
        sortedValues[i] = values[order[i]];
        for (int k = 0; k < n; ++k)
        {
            sortedVectors[static_cast<size_t>(k) * n + i] = vectors[static_cast<size_t>(k) * n + order[i]];
        }
    }
    values = sortedValues;
    vectors = sortedVectors;
}

ModalResult AnalyzeUnloadedModes(FlexRingTyre& tyre, int maxWaveNumber)
{
    ModalResult result;
    RimState rim;
    tyre.Reset(rim);
    const int n = tyre.Segments();
    const int dofs = 4 * n;
    std::vector<Vec3> x0 = tyre.NodePositions();
    std::vector<Vec3> v0(n, Vec3(0.0));
    std::vector<double> psi0(n, 0.0);
    std::vector<double> psiDot0(n, 0.0);

    // Dense K and D by central differences (forces are -K dq - D dv).
    std::vector<double> kMat(static_cast<size_t>(dofs) * dofs, 0.0);
    std::vector<double> dMat(static_cast<size_t>(dofs) * dofs, 0.0);
    std::vector<Vec3> fPlus, fMinus;
    std::vector<double> tPlus, tMinus;
    const double radius = tyre.Parameters().beltRadius;
    const double epsX = 1.0e-7 * radius;
    const double epsPsi = 1.0e-6;
    const double epsV = 1.0e-4;
    const auto column = [&](std::vector<double>& m, int j, double eps) {
        for (int k = 0; k < n; ++k)
        {
            for (int c = 0; c < 3; ++c)
            {
                m[static_cast<size_t>(4 * k + c) * dofs + j] = -(fPlus[k][c] - fMinus[k][c]) / (2.0 * eps);
            }
            m[static_cast<size_t>(4 * k + 3) * dofs + j] = -(tPlus[k] - tMinus[k]) / (2.0 * eps);
        }
    };
    for (int j = 0; j < dofs; ++j)
    {
        const int node = j / 4;
        const int c = j % 4;
        std::vector<Vec3> x = x0;
        std::vector<double> psi = psi0;
        const double eps = c < 3 ? epsX : epsPsi;
        if (c < 3)
        {
            x[node][c] += eps;
        }
        else
        {
            psi[node] += eps;
        }
        tyre.StructuralForces(x, v0, psi, psiDot0, fPlus, tPlus);
        if (c < 3)
        {
            x[node][c] -= 2.0 * eps;
        }
        else
        {
            psi[node] -= 2.0 * eps;
        }
        tyre.StructuralForces(x, v0, psi, psiDot0, fMinus, tMinus);
        column(kMat, j, eps);

        std::vector<Vec3> v = v0;
        std::vector<double> psiDot = psiDot0;
        if (c < 3)
        {
            v[node][c] += epsV;
        }
        else
        {
            psiDot[node] += epsV;
        }
        tyre.StructuralForces(x0, v, psi0, psiDot, fPlus, tPlus);
        if (c < 3)
        {
            v[node][c] -= 2.0 * epsV;
        }
        else
        {
            psiDot[node] -= 2.0 * epsV;
        }
        tyre.StructuralForces(x0, v, psi0, psiDot, fMinus, tMinus);
        column(dMat, j, epsV);
    }
    double kMax = 0.0;
    double kAsym = 0.0;
    for (int i = 0; i < dofs; ++i)
    {
        for (int j = i; j < dofs; ++j)
        {
            const double a = kMat[static_cast<size_t>(i) * dofs + j];
            const double b = kMat[static_cast<size_t>(j) * dofs + i];
            kMax = std::max(kMax, std::abs(a));
            kAsym = std::max(kAsym, std::abs(a - b));
            const double s = 0.5 * (a + b);
            kMat[static_cast<size_t>(i) * dofs + j] = s;
            kMat[static_cast<size_t>(j) * dofs + i] = s;
            const double da = dMat[static_cast<size_t>(i) * dofs + j];
            const double db = dMat[static_cast<size_t>(j) * dofs + i];
            dMat[static_cast<size_t>(i) * dofs + j] = 0.5 * (da + db);
            dMat[static_cast<size_t>(j) * dofs + i] = 0.5 * (da + db);
        }
    }
    result.asymmetry = kMax > 0.0 ? kAsym / kMax : 0.0;
    std::vector<double> mass(static_cast<size_t>(dofs));
    for (int k = 0; k < n; ++k)
    {
        mass[4 * k + 0] = mass[4 * k + 1] = mass[4 * k + 2] = tyre.NodeMass();
        mass[4 * k + 3] = std::max(tyre.NodeInertia(), 1.0e-12);
    }

    const Vec3 axis(0.0, 1.0, 0.0);
    for (int w = 0; w <= maxWaveNumber; ++w)
    {
        // Basis of the wave number's (cos) family: radial cos, tangential sin (cos at w = 0), lateral cos,
        // torsion cos.
        std::vector<std::vector<double>> basis(4, std::vector<double>(static_cast<size_t>(dofs), 0.0));
        for (int k = 0; k < n; ++k)
        {
            const double phi = 2.0 * std::numbers::pi * k / n;
            const double cs = std::cos(w * phi);
            const double sn = w == 0 ? 1.0 : std::sin(w * phi);
            const Vec3 radial = glm::normalize(x0[k]);
            const Vec3 tangential = glm::cross(radial, axis);
            for (int c = 0; c < 3; ++c)
            {
                basis[0][4 * k + c] = radial[c] * cs;
                basis[1][4 * k + c] = tangential[c] * sn;
                basis[2][4 * k + c] = axis[c] * cs;
            }
            basis[3][4 * k + 3] = cs;
        }
        for (auto& b : basis)
        {
            double norm = 0.0;
            for (double e : b)
            {
                norm += e * e;
            }
            norm = std::sqrt(norm);
            for (double& e : b)
            {
                e /= norm;
            }
        }
        std::vector<std::vector<double>> kb(4, std::vector<double>(static_cast<size_t>(dofs), 0.0));
        std::vector<std::vector<double>> db(4, std::vector<double>(static_cast<size_t>(dofs), 0.0));
        for (int a = 0; a < 4; ++a)
        {
            for (int i = 0; i < dofs; ++i)
            {
                double sk = 0.0;
                double sd = 0.0;
                const double* rowK = &kMat[static_cast<size_t>(i) * dofs];
                const double* rowD = &dMat[static_cast<size_t>(i) * dofs];
                for (int j = 0; j < dofs; ++j)
                {
                    sk += rowK[j] * basis[a][j];
                    sd += rowD[j] * basis[a][j];
                }
                kb[a][i] = sk;
                db[a][i] = sd;
            }
        }
        double kr[4][4];
        double dr[4][4];
        double mr[4];
        for (int a = 0; a < 4; ++a)
        {
            double m = 0.0;
            for (int i = 0; i < dofs; ++i)
            {
                m += basis[a][i] * mass[i] * basis[a][i];
            }
            mr[a] = m;
            for (int b = 0; b < 4; ++b)
            {
                double sk = 0.0;
                double sd = 0.0;
                for (int i = 0; i < dofs; ++i)
                {
                    sk += basis[b][i] * kb[a][i];
                    sd += basis[b][i] * db[a][i];
                }
                kr[b][a] = sk;
                dr[b][a] = sd;
            }
            // Residual of K b out of the subspace.
            double outside = 0.0;
            double total = 0.0;
            for (int i = 0; i < dofs; ++i)
            {
                double in = 0.0;
                for (int b = 0; b < 4; ++b)
                {
                    double proj = 0.0;
                    for (int j = 0; j < dofs; ++j)
                    {
                        proj += basis[b][j] * kb[a][j];
                    }
                    in += proj * basis[b][i];
                }
                outside += (kb[a][i] - in) * (kb[a][i] - in);
                total += kb[a][i] * kb[a][i];
            }
            if (total > 0.0)
            {
                result.subspaceResidual = std::max(result.subspaceResidual, std::sqrt(outside / total));
            }
        }
        // The w = 0 tangential and torsion basis coincide with... nothing; at w = 0 all four are distinct.
        // M is diagonal in this basis: scale to a standard problem.
        std::vector<double> a(16);
        for (int i = 0; i < 4; ++i)
        {
            for (int j = 0; j < 4; ++j)
            {
                a[static_cast<size_t>(i) * 4 + j] = kr[i][j] / std::sqrt(mr[i] * mr[j]);
            }
        }
        std::vector<double> values, vectors;
        SymmetricEigen(4, a, values, vectors);
        for (int m = 0; m < 4; ++m)
        {
            ModeInfo mode;
            mode.waveNumber = w;
            mode.frequency = std::sqrt(std::max(values[m], 0.0)) / (2.0 * std::numbers::pi);
            double phi[4];
            double total = 0.0;
            for (int i = 0; i < 4; ++i)
            {
                phi[i] = vectors[static_cast<size_t>(i) * 4 + m] / std::sqrt(mr[i]);
                total += phi[i] * phi[i] * mr[i];
            }
            for (int i = 0; i < 4; ++i)
            {
                mode.share[i] = total > 0.0 ? phi[i] * phi[i] * mr[i] / total : 0.0;
            }
            double damp = 0.0;
            for (int i = 0; i < 4; ++i)
            {
                for (int j = 0; j < 4; ++j)
                {
                    damp += phi[i] * dr[i][j] * phi[j];
                }
            }
            const double omega = 2.0 * std::numbers::pi * mode.frequency;
            mode.damping = omega > 0.0 && total > 0.0 ? damp / (2.0 * omega * total) : 0.0;
            result.modes.push_back(mode);
        }
    }

    // FTire's reference modes.
    const auto pick = [&](int w, auto predicate, double& f, double& d) {
        double best = 1.0e300;
        for (const ModeInfo& m : result.modes)
        {
            if (m.waveNumber == w && predicate(m) && m.frequency < best)
            {
                best = m.frequency;
                f = m.frequency;
                d = m.damping;
            }
        }
    };
    pick(0, [](const ModeInfo& m) { return m.share[1] > 0.5; }, result.f1, result.d1);
    pick(1, [](const ModeInfo& m) { return m.InPlane(); }, result.f2, result.d2);
    pick(0, [](const ModeInfo& m) { return m.share[2] > 0.5; }, result.f3, result.d3);
    pick(1, [](const ModeInfo& m) { return !m.InPlane(); }, result.f4, result.d4);
    pick(2, [](const ModeInfo& m) { return m.InPlane(); }, result.f5, result.d5);
    pick(2, [](const ModeInfo& m) { return !m.InPlane(); }, result.f6, result.d6);
    return result;
}

}
