#pragma once

#include "application.h"

#include <render_backend_type.h>

#include <cstdint>
#include <optional>
#include <string>

struct EditorApplicationOptions
{
    RenderBackendType renderBackend = GetDefaultRenderBackendType();
    std::optional<std::string> startupModelPath;
    uint32_t maxFrames = 0;
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
