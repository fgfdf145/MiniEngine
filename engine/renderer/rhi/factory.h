#pragma once

#include "backend.h"

#include <memory>
#include <optional>
#include <string>

struct RendererSharedState;
class Window;

std::unique_ptr<IRenderBackend> CreateRenderBackend(
    Window& window,
    std::shared_ptr<RendererSharedState> sharedState,
    std::optional<std::string> startupModelPath = std::nullopt
);
