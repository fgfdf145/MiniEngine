#pragma once

#include "backend.h"

#include <optional>
#include <memory>
#include <string>

struct RendererSharedState;
class Window;

RenderBackendDescriptor GetRenderBackendDescriptor(RenderBackendType backendType);
bool IsRenderBackendSupported(RenderBackendType backendType);
std::optional<std::string> GetRenderBackendRuntimeError(RenderBackendType backendType);
RenderBackendType GetPreferredRenderBackendType();

std::unique_ptr<IRenderBackend> CreateRenderBackend(
    Window& window,
    std::shared_ptr<RendererSharedState> sharedState,
    RenderBackendType backendType,
    std::optional<std::string> startupModelPath = std::nullopt
);
