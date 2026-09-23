#include "ao_history.h"

namespace me
{

AoHistoryFrame AoHistory::Advance(bool accumulating)
{
    AoHistoryFrame frame{};
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

void AoHistory::Reset()
{
    m_hasHistory = false;
}
}
