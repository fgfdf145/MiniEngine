#pragma once

#include "application.h"

#include <engine/core/paths/engine_paths.h>
#include <engine/core/render_backend_type.h>
#include <engine/renderer/render_types.h>

#include <cstdint>
#include <optional>
#include <string>

namespace me
{

struct EditorApplicationOptions
{
    RenderBackendType renderBackend = GetDefaultRenderBackendType();
    std::optional<std::string> startupModelPath;
    // A scene file loaded in place of the two-cube test scene, the way File > Open would load it.
    std::optional<std::string> startupScenePath;
    uint32_t maxFrames = 0;
    // With --frames: the viewport of the last frame is written here as a PNG.
    std::optional<std::string> capturePath;
    // Starts with the Khronos reference view on (Graphics Debug), for comparing captures against the
    // Khronos glTF Sample Viewer.
    bool khronosReference = false;
    // --viewport-size WxH: renders the scene at a fixed size, independent of the editor's layout.
    std::optional<RenderExtent> viewportSize;
    EnginePaths::Overrides paths;
};

class EditorApplication final : public IApplication
{
  public:
    static EditorApplicationOptions ParseArgs(int argc, char** argv);
    static void PrintDependencyLinkStatus();

    explicit EditorApplication(EditorApplicationOptions options);
    int Run() override;

  private:
    EditorApplicationOptions m_options;
};
}
