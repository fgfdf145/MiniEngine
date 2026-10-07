#include <engine/renderer/path_tracing.h>

#include <array>
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

PathTraceView TestView()
{
    PathTraceView view;
    view.view = glm::mat4(1.0f);
    view.view[3] = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);
    view.projection = glm::mat4(2.0f);
    view.width = 1280;
    view.height = 720;
    return view;
}

const std::array<glm::vec4, 2> kLighting = {glm::vec4(0.0f, -1.0f, 0.0f, 0.0f), glm::vec4(1.0f, 1.0f, 1.0f, 100000.0f)};

void FirstFrameIsNotStill()
{
    PathTraceAccumulation accumulation;
    Require(accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false) == 0, "the first frame has nothing to average with");
}

void StillFramesCount()
{
    PathTraceAccumulation accumulation;
    accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false);
    Require(accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false) == 1, "a repeated frame is still");
    Require(accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false) == 2, "still frames count up");
}

void AnyChangeStartsOver()
{
    PathTraceAccumulation accumulation;
    accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false);
    accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false);

    PathTraceView moved = TestView();
    moved.view[3].x += 1e-4f;
    Require(accumulation.Advance(moved, kLighting, RenderDebugSettings{}, false) == 0, "a moved camera starts over");
    Require(accumulation.Advance(moved, kLighting, RenderDebugSettings{}, false) == 1, "and stands still again");

    PathTraceView resized = moved;
    resized.width = 640;
    Require(accumulation.Advance(resized, kLighting, RenderDebugSettings{}, false) == 0, "a new render size starts over");

    std::array<glm::vec4, 2> sunMoved = kLighting;
    sunMoved[0].x = 1e-5f;
    accumulation.Advance(resized, kLighting, RenderDebugSettings{}, false);
    Require(accumulation.Advance(resized, sunMoved, RenderDebugSettings{}, false) == 0, "any change of the light starts over");
    Require(accumulation.Advance(resized, std::span<const glm::vec4>(sunMoved).first(1), RenderDebugSettings{}, false) == 0,
            "a light fewer starts over");

    accumulation.Advance(resized, kLighting, RenderDebugSettings{}, false);
    RenderDebugSettings settings;
    settings.pathTracing.maxBounces = 1;
    Require(accumulation.Advance(resized, kLighting, settings, false) == 0, "a settings change starts over");
    Require(accumulation.Advance(resized, kLighting, settings, true) == 0, "a scene change starts over");
}

void ResetStartsOver()
{
    PathTraceAccumulation accumulation;
    accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false);
    accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false);
    accumulation.Reset();
    Require(accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false) == 0, "the frame after Reset is not still");
}

void HistoryCapGrowsWhileStill()
{
    PathTracingSettings settings;
    settings.motionFrames = 32;
    settings.maxFrames = 1000;
    Require(PathTraceHistoryCap(settings, 0) == 32, "moving: the motion cap");
    Require(PathTraceHistoryCap(settings, 10) == 42, "one more frame per still frame");
    Require(PathTraceHistoryCap(settings, 5000) == 1000, "no more than maxFrames");

    settings.maxFrames = 100000;
    Require(PathTraceHistoryCap(settings, 100000) == kPathTraceMaxFrames, "no more than a half float counts");
    settings.motionFrames = 0;
    settings.maxFrames = 0;
    Require(PathTraceHistoryCap(settings, 0) == 1, "at least the frame itself");
    settings.motionFrames = 64;
    settings.maxFrames = 16;
    Require(PathTraceHistoryCap(settings, 0) == 16, "the motion cap never exceeds maxFrames");
}
}

int main()
{
    try
    {
        FirstFrameIsNotStill();
        StillFramesCount();
        AnyChangeStartsOver();
        ResetStartsOver();
        HistoryCapGrowsWhileStill();
    }
    catch (const std::exception& error)
    {
        std::cerr << "path_tracing_tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "path_tracing_tests passed\n";
    return 0;
}
