#include "editor_application.h"

#include <log/log.h>
#include <renderer_shared_state.h>
#include <rhi/factory.h>
#include <window/window.h>

#include <imgui.h>
#include <ImGuizmo.h>
#include <entt/entt.hpp>
#include <yaml-cpp/yaml.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{
WindowGraphicsApi ToWindowGraphicsApi(RenderBackendType backendType)
{
    switch (backendType)
    {
    case RenderBackendType::Vulkan:
        return WindowGraphicsApi::Vulkan;
    case RenderBackendType::OpenGL:
        return WindowGraphicsApi::OpenGL;
    default:
        return WindowGraphicsApi::Vulkan;
    }
}
}

EditorApplicationOptions EditorApplication::ParseArgs(int argc, char** argv)
{
    EditorApplicationOptions options{};

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument = argv[i];
        if (argument == "--model")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--model requires a file path");
            }

            options.startupModelPath = argv[++i];
            continue;
        }

        if (argument == "--frames")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--frames requires a positive integer");
            }

            const int parsedFrameCount = std::stoi(argv[++i]);
            if (parsedFrameCount <= 0)
            {
                throw std::runtime_error("--frames requires a positive integer");
            }

            options.maxFrames = static_cast<uint32_t>(parsedFrameCount);
            continue;
        }

        if (argument == "--backend")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--backend requires 'vulkan' or 'opengl'");
            }

            const std::string_view backendName = argv[++i];
            if (backendName == "vulkan")
            {
                options.initialBackendType = RenderBackendType::Vulkan;
                continue;
            }
            if (backendName == "opengl")
            {
                options.initialBackendType = RenderBackendType::OpenGL;
                continue;
            }

            throw std::runtime_error("--backend requires 'vulkan' or 'opengl'");
        }

        throw std::runtime_error("Unknown argument: " + std::string(argument));
    }

    return options;
}

void EditorApplication::PrintDependencyLinkStatus()
{
    const auto imguizmoSymbol = &ImGuizmo::SetOrthographic;
    const YAML::Node yamlNode = YAML::Load("linked: true");

    entt::registry registry;
    const entt::entity entity = registry.create();

    std::cout << "[link] ImGuizmo: " << (imguizmoSymbol != nullptr ? "OK" : "FAILED") << '\n';
    std::cout << "[link] yaml-cpp: " << (yamlNode["linked"].as<bool>() ? "OK" : "FAILED") << '\n';
    std::cout << "[link] EnTT: " << (registry.valid(entity) ? "OK" : "FAILED") << '\n';
}

EditorApplication::EditorApplication(EditorApplicationOptions options)
    : m_options(std::move(options))
{
}

int EditorApplication::Run()
{
    RenderBackendType backendType = m_options.initialBackendType;
    auto sharedState = std::make_shared<RendererSharedState>();
    Window window(1920, 1080, "MiniEngine", ToWindowGraphicsApi(backendType));
    std::unique_ptr<IRenderBackend> renderer = CreateRenderBackend(backendType, window, sharedState, m_options.startupModelPath);
    uint32_t renderedFrameCount = 0;

    while (!window.ShouldClose())
    {
        window.PollEvents([&renderer](const SDL_Event& event)
        {
            renderer->HandleEvent(event);
        });
        renderer->DrawFrame();
        if (const std::optional<RenderBackendType> requestedBackend = renderer->ConsumeBackendSwitchRequest();
            requestedBackend.has_value() && *requestedBackend != backendType)
        {
            backendType = *requestedBackend;
            renderer.reset();
            window.Recreate(ToWindowGraphicsApi(backendType));
            renderer = CreateRenderBackend(backendType, window, sharedState);
            continue;
        }

        if (m_options.maxFrames > 0)
        {
            ++renderedFrameCount;
            if (renderedFrameCount >= m_options.maxFrames)
            {
                break;
            }
        }
    }

    return 0;
}
