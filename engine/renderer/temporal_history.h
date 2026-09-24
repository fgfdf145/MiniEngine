#pragma once

#include <cstdint>

namespace me
{

// Which of a temporal effect's two history images a frame's resolve samples and which it stores to,
// and whether the sampled one holds last frame's accumulation.
struct TemporalHistoryFrame
{
    uint32_t readIndex = 0;
    uint32_t writeIndex = 1;
    bool valid = false;
};

// Ping-pong bookkeeping for a temporal filter (the AO resolve and TAA each own one). The images
// themselves live in the resolve pass; this unit only decides how a frame uses them, so the pass
// keeps no per-frame state.
class TemporalHistory
{
  public:
    // Called once per recorded frame. accumulating is whether this frame's resolve feeds history
    // (for AO: AO and its temporal filter both on). A frame that does not accumulate is invalid and makes
    // the next one invalid too, so history paused and resumed is never stale.
    TemporalHistoryFrame Advance(bool accumulating);

    // The next Advance is invalid. Called whenever the history images are recreated.
    void Reset();

  private:
    bool m_hasHistory = false;
    uint32_t m_lastWriteIndex = 0;
};
}
