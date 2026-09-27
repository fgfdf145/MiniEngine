#pragma once

#include <engine/renderer/camera.h>
#include <engine/renderer/render_types.h>

#include <filesystem>
#include <string>

namespace me
{

// What an editor viewport capture records beside its PNG so the frame can be rendered again from the
// command line (miniengine_app --state <file>): the scene as it was (a snapshot saved next to the
// capture, since the open scene may be unsaved or change later), the camera, the viewport's size and
// every Graphics Debug setting. The DDGI probes and the exposure's adaptation are history, not state:
// the replay converges to them again, so compare structure rather than exact EV.
struct CaptureState
{
    // The scene snapshot, relative to the state file's folder when written by WriteCaptureState.
    std::filesystem::path scenePath;
    // The scene file the editor had open, for reference; empty for an unsaved scene.
    std::string originalScenePath;
    RenderExtent viewportExtent{};
    Camera camera;
    RenderDebugSettings renderDebug;
};

namespace CaptureStateService
{
// Writes the state as YAML, with a comment giving the command line that replays it.
void Write(const std::filesystem::path& path, const CaptureState& state);

// Reads a state file; the scene path comes back resolved against the file's folder. Fields the file
// lacks keep their defaults, so older files still load. Throws std::runtime_error on a bad file.
CaptureState Read(const std::filesystem::path& path);
}
}
