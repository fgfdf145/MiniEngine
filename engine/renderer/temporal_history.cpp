#include "temporal_history.h"

namespace me
{

TemporalHistoryFrame TemporalHistory::Advance(bool accumulating)
{
    TemporalHistoryFrame frame{};
    frame.readIndex = m_lastWriteIndex;
    frame.writeIndex = 1u - m_lastWriteIndex;
    frame.valid = accumulating && m_hasHistory;

    m_hasHistory = accumulating;
    if (accumulating)
    {
        m_lastWriteIndex = frame.writeIndex;
    }
    return frame;
}

void TemporalHistory::Reset()
{
    m_hasHistory = false;
}
}
