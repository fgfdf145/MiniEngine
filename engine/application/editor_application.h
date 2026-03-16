#pragma once

#include "application.h"

#include <rhi/backend.h>

#include <cstdint>
#include <optional>
#include <string>

struct EditorApplicationOptions
{
    std::optional<std::string> startupModelPath;
    RenderBackendType initialBackendType = RenderBackendType::Vulkan;
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
