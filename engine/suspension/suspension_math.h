#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <vector>

namespace me::suspension
{

using Vec3 = glm::dvec3;
using Mat3 = glm::dmat3;

// A dense row-major matrix for the small systems a suspension corner solves (tens of unknowns).
class DenseMatrix
{
public:
    DenseMatrix() = default;
    DenseMatrix(std::size_t rows, std::size_t cols);

    void Resize(std::size_t rows, std::size_t cols);
    void SetZero();

    double& operator()(std::size_t row, std::size_t col)
    {
        return m_values[row * m_cols + col];
    }
    double operator()(std::size_t row, std::size_t col) const
    {
        return m_values[row * m_cols + col];
    }

    std::size_t Rows() const
    {
        return m_rows;
    }
    std::size_t Cols() const
    {
        return m_cols;
    }

    // Adds a 3x3 block whose top-left corner is (row, col).
    void AddBlock(std::size_t row, std::size_t col, const Mat3& block);

private:
    std::size_t m_rows = 0;
    std::size_t m_cols = 0;
    std::vector<double> m_values;
};

// LU factorisation with partial pivoting of a square matrix, kept so that one factorisation serves
// several right-hand sides and the transposed system (A x = b and A^T y = c).
class DenseLu
{
public:
    // Returns false when a pivot is zero. PivotRatio() then tells how close to singular it was.
    bool Factor(const DenseMatrix& matrix);

    // Smallest over largest absolute pivot: a cheap conditioning indicator (not the condition number).
    double PivotRatio() const
    {
        return m_pivotRatio;
    }

    void Solve(std::vector<double>& rhs) const;
    void SolveTransposed(std::vector<double>& rhs) const;

    std::size_t Size() const
    {
        return m_size;
    }

private:
    std::size_t m_size = 0;
    std::vector<double> m_lu;
    std::vector<std::size_t> m_perm;
    double m_pivotRatio = 0.0;
};

// Rotation about the unit axis `axis` by `angle` (right-handed).
Mat3 AxisAngle(const Vec3& axis, double angle);

// The skew matrix with Skew(a) * b = cross(a, b).
Mat3 Skew(const Vec3& a);
}
