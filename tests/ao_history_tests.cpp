#include <engine/renderer/ao_history.h>

#include <iostream>
#include <stdexcept>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void FirstFrameIsInvalid()
{
    AoHistory history;
    const AoHistoryFrame frame = history.Advance(true);
    Require(!frame.valid, "the first frame has no history to read");
    Require(frame.readIndex != frame.writeIndex, "a frame must not read the image it writes");
}

void AccumulatingFramesAlternateAndReadLastWrite()
{
    AoHistory history;
    const AoHistoryFrame first = history.Advance(true);
    const AoHistoryFrame second = history.Advance(true);
    const AoHistoryFrame third = history.Advance(true);
    Require(second.valid && third.valid, "consecutive accumulating frames are valid from the second");
    Require(second.readIndex == first.writeIndex, "a frame reads what the previous one wrote");
    Require(third.readIndex == second.writeIndex, "a frame reads what the previous one wrote");
    Require(second.writeIndex != first.writeIndex, "the written image alternates");
    Require(third.writeIndex == first.writeIndex, "the written image alternates");
}

void NonAccumulatingFrameInvalidatesItselfAndTheNext()
{
    AoHistory history;
    history.Advance(true);
    history.Advance(true);
    const AoHistoryFrame paused = history.Advance(false);
    const AoHistoryFrame resumed = history.Advance(true);
    const AoHistoryFrame settled = history.Advance(true);
    Require(!paused.valid, "a frame that does not accumulate reads no history");
    Require(!resumed.valid, "the frame after a pause must not read stale history");
    Require(settled.valid, "accumulation resumes one frame later");
}

void ResetInvalidatesTheNextFrame()
{
    AoHistory history;
    history.Advance(true);
    history.Advance(true);
    history.Reset();
    Require(!history.Advance(true).valid, "the first frame after Reset is invalid");
    Require(history.Advance(true).valid, "the second frame after Reset is valid");
}
}

int main()
{
    try
    {
        FirstFrameIsInvalid();
        AccumulatingFramesAlternateAndReadLastWrite();
        NonAccumulatingFrameInvalidatesItselfAndTheNext();
        ResetInvalidatesTheNextFrame();
    }
    catch (const std::exception& error)
    {
        std::cerr << "AO history tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "AO history tests passed\n";
    return 0;
}
