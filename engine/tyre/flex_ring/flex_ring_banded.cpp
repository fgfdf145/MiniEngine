#include "flex_ring_banded.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace me::tyre::flexring
{

void BandMatrix::Resize(int size, int halfBandwidth)
{
    m_n = size;
    m_b = std::min(halfBandwidth, std::max(size - 1, 0));
    m_data.assign(static_cast<size_t>(m_n) * (m_b + 1), 0.0);
}

void BandMatrix::SetZero()
{
    std::fill(m_data.begin(), m_data.end(), 0.0);
}

void BandMatrix::Add(int i, int j, double value)
{
    if (i < j)
    {
        std::swap(i, j);
    }
    if (i - j > m_b)
    {
        return;
    }
    At(i, j) += value;
}

double BandMatrix::Get(int i, int j) const
{
    if (i < j)
    {
        std::swap(i, j);
    }
    return i - j > m_b ? 0.0 : At(i, j);
}

bool BandMatrix::Factor()
{
    for (int i = 0; i < m_n; ++i)
    {
        const int first = std::max(0, i - m_b);
        for (int j = first; j <= i; ++j)
        {
            double sum = At(i, j);
            const int kFirst = std::max(first, j - m_b);
            const double* li = &m_data[static_cast<size_t>(i) * (m_b + 1) + (kFirst - i + m_b)];
            const double* lj = &m_data[static_cast<size_t>(j) * (m_b + 1) + (kFirst - j + m_b)];
            for (int k = 0; k < j - kFirst; ++k)
            {
                sum -= li[k] * lj[k];
            }
            if (j == i)
            {
                if (!(sum > 0.0))
                {
                    return false;
                }
                At(i, i) = std::sqrt(sum);
            }
            else
            {
                At(i, j) = sum / At(j, j);
            }
        }
    }
    return true;
}

void BandMatrix::Solve(double* b) const
{
    // L y = b
    for (int i = 0; i < m_n; ++i)
    {
        double sum = b[i];
        const int first = std::max(0, i - m_b);
        for (int k = first; k < i; ++k)
        {
            sum -= At(i, k) * b[k];
        }
        b[i] = sum / At(i, i);
    }
    // L^T x = y
    for (int i = m_n - 1; i >= 0; --i)
    {
        double sum = b[i];
        const int last = std::min(m_n - 1, i + m_b);
        for (int k = i + 1; k <= last; ++k)
        {
            sum -= At(k, i) * b[k];
        }
        b[i] = sum / At(i, i);
    }
}

}
