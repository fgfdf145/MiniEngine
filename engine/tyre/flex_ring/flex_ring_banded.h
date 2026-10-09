#pragma once

#include <vector>

namespace me::tyre::flexring
{

// A symmetric positive definite band matrix, lower half stored row by row, and its Cholesky factor in
// place. The flexible ring's system matrix is a cyclic band matrix (Gipser 2004, fig. 13); numbering the
// nodes 0, n-1, 1, n-2, 2, ... turns it into an ordinary band of twice the width.
class BandMatrix
{
  public:
    void Resize(int size, int halfBandwidth);
    void SetZero();
    int Size() const
    {
        return m_n;
    }
    int HalfBandwidth() const
    {
        return m_b;
    }
    // A(i, j) for |i - j| <= halfBandwidth; adds to the lower half (i >= j).
    void Add(int i, int j, double value);
    double Get(int i, int j) const;

    // Factors A = L L^T in place; false (and nothing usable) when A is not positive definite.
    bool Factor();
    // Solves A x = b with the factor; b is overwritten with x.
    void Solve(double* b) const;

  private:
    double& At(int i, int j)
    {
        return m_data[static_cast<size_t>(i) * (m_b + 1) + (j - i + m_b)];
    }
    double At(int i, int j) const
    {
        return m_data[static_cast<size_t>(i) * (m_b + 1) + (j - i + m_b)];
    }

    int m_n = 0;
    int m_b = 0;
    std::vector<double> m_data;
};

}
