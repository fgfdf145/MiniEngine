#include "suspension_math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace me::suspension
{

DenseMatrix::DenseMatrix(std::size_t rows, std::size_t cols)
{
    Resize(rows, cols);
}

void DenseMatrix::Resize(std::size_t rows, std::size_t cols)
{
    m_rows = rows;
    m_cols = cols;
    m_values.assign(rows * cols, 0.0);
}

void DenseMatrix::SetZero()
{
    std::fill(m_values.begin(), m_values.end(), 0.0);
}

void DenseMatrix::AddBlock(std::size_t row, std::size_t col, const Mat3& block)
{
    // glm is column-major: block[c][r].
    for (std::size_t r = 0; r < 3; ++r)
    {
        for (std::size_t c = 0; c < 3; ++c)
        {
            (*this)(row + r, col + c) += block[static_cast<int>(c)][static_cast<int>(r)];
        }
    }
}

bool DenseLu::Factor(const DenseMatrix& matrix)
{
    const std::size_t n = matrix.Rows();
    m_size = n;
    m_lu.resize(n * n);
    m_perm.resize(n);
    for (std::size_t r = 0; r < n; ++r)
    {
        m_perm[r] = r;
        for (std::size_t c = 0; c < n; ++c)
        {
            m_lu[r * n + c] = matrix(r, c);
        }
    }

    double smallest = std::numeric_limits<double>::infinity();
    double largest = 0.0;
    for (std::size_t k = 0; k < n; ++k)
    {
        std::size_t pivotRow = k;
        double pivotSize = std::abs(m_lu[k * n + k]);
        for (std::size_t r = k + 1; r < n; ++r)
        {
            const double size = std::abs(m_lu[r * n + k]);
            if (size > pivotSize)
            {
                pivotSize = size;
                pivotRow = r;
            }
        }
        smallest = std::min(smallest, pivotSize);
        largest = std::max(largest, pivotSize);
        if (pivotSize == 0.0)
        {
            m_pivotRatio = 0.0;
            return false;
        }
        if (pivotRow != k)
        {
            for (std::size_t c = 0; c < n; ++c)
            {
                std::swap(m_lu[k * n + c], m_lu[pivotRow * n + c]);
            }
            std::swap(m_perm[k], m_perm[pivotRow]);
        }
        const double pivot = m_lu[k * n + k];
        for (std::size_t r = k + 1; r < n; ++r)
        {
            const double factor = m_lu[r * n + k] / pivot;
            m_lu[r * n + k] = factor;
            if (factor == 0.0)
            {
                continue;
            }
            for (std::size_t c = k + 1; c < n; ++c)
            {
                m_lu[r * n + c] -= factor * m_lu[k * n + c];
            }
        }
    }
    m_pivotRatio = largest > 0.0 ? smallest / largest : 0.0;
    return true;
}

void DenseLu::Solve(std::vector<double>& rhs) const
{
    const std::size_t n = m_size;
    std::vector<double> x(n);
    for (std::size_t r = 0; r < n; ++r)
    {
        double sum = rhs[m_perm[r]];
        for (std::size_t c = 0; c < r; ++c)
        {
            sum -= m_lu[r * n + c] * x[c];
        }
        x[r] = sum;
    }
    for (std::size_t r = n; r-- > 0;)
    {
        double sum = x[r];
        for (std::size_t c = r + 1; c < n; ++c)
        {
            sum -= m_lu[r * n + c] * x[c];
        }
        x[r] = sum / m_lu[r * n + r];
    }
    rhs.swap(x);
}

void DenseLu::SolveTransposed(std::vector<double>& rhs) const
{
    // P A = L U, so A^T = U^T L^T P: solve U^T w = c, then L^T v = w, then y = P^T v.
    const std::size_t n = m_size;
    std::vector<double> w(rhs.begin(), rhs.end());
    for (std::size_t r = 0; r < n; ++r)
    {
        double sum = w[r];
        for (std::size_t c = 0; c < r; ++c)
        {
            sum -= m_lu[c * n + r] * w[c];
        }
        w[r] = sum / m_lu[r * n + r];
    }
    for (std::size_t r = n; r-- > 0;)
    {
        double sum = w[r];
        for (std::size_t c = r + 1; c < n; ++c)
        {
            sum -= m_lu[c * n + r] * w[c];
        }
        w[r] = sum;
    }
    for (std::size_t r = 0; r < n; ++r)
    {
        rhs[m_perm[r]] = w[r];
    }
}

Mat3 AxisAngle(const Vec3& axis, double angle)
{
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const Mat3 outer = glm::outerProduct(axis, axis);
    return outer + (Mat3(1.0) - outer) * c + Skew(axis) * s;
}

Mat3 Skew(const Vec3& a)
{
    // Columns of the skew matrix [a]x.
    return Mat3(Vec3(0.0, a.z, -a.y), Vec3(-a.z, 0.0, a.x), Vec3(a.y, -a.x, 0.0));
}
}
