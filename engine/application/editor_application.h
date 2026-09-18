#pragma once

#include "application.h"

#include <engine/core/paths/engine_paths.h>
#include <engine/core/render_backend_type.h>

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
