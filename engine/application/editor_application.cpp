#include "editor_application.h"

#include <log/log.h>
#include <renderer_shared_state.h>
#include <rhi/factory.h>
#include <window/window.h>

#include <ImGuizmo.h>
#include <entt/entt.hpp>
#include <yaml-cpp/yaml.h>

#include <charconv>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{
std::string_view ReadRequiredArgument(int& index, int argc, char** argv, std::string_view optionName)
{
    if (index + 1 >= argc)
    {
        throw std::runtime_error(std::string(optionName) + " requires a value");
    }

    return argv[++index];
}

uint32_t ParsePositiveFrameCount(std::string_view value)
{
    uint32_t frameCount = 0;
    const char* const begin = value.data();
    const char* const end = begin + value.size();
    const auto [parsedEnd, errorCode] = std::from_chars(begin, end, frameCount);
    if (errorCode != std::errc{} || parsedEnd != end || frameCount == 0)
    {
        throw std::runtime_error("--frames requires a positive integer");
    }

    return frameCount;
}

RenderBackendType ParseRenderBackend(std::string_view value)
{
    RenderBackendType backendType = GetPreferredRenderBackendType();
    if (!TryParseRenderBackendType(value, backendType))
    {
        throw std::runtime_error(
            "Unknown backend: " + std::string(value) + ". Supported values: vulkan"
        );
    }

    if (const std::optional<std::string> runtimeError = GetRenderBackendRuntimeError(backendType); runtimeError.has_value())
    {
        throw std::runtime_error(
            "Requested backend '" + std::string(value) + "' is unavailable: " +
            *runtimeError
        );
    }

    return backendType;
}
}

EditorApplicationOptions EditorApplication::ParseArgs(int argc, char** argv)
{
    EditorApplicationOptions options{};
    options.renderBackend = GetPreferredRenderBackendType();

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument = argv[i];
        if (argument == "--model")
        {
            options.startupModelPath = ReadRequiredArgument(i, argc, argv, argument);
            continue;
        }

        if (argument == "--frames")
        {
            options.maxFrames = ParsePositiveFrameCount(ReadRequiredArgument(i, argc, argv, argument));
            continue;
        }

        if (argument == "--backend")
        {
            options.renderBackend = ParseRenderBackend(ReadRequiredArgument(i, argc, argv, argument));
            continue;
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
    auto sharedState = std::make_shared<RendererSharedState>();
    LOG_INFO("Using render backend: {}", ToString(m_options.renderBackend));
    Window window(1920, 1080, "MiniEngine", m_options.renderBackend);
    std::unique_ptr<IRenderBackend> renderer = CreateRenderBackend(
        window,
        sharedState,
        m_options.renderBackend,
        m_options.startupModelPath
    );
    uint32_t renderedFrameCount = 0;

    while (!window.ShouldClose())
    {
        window.PollEvents([&renderer](const SDL_Event& event)
        {
            renderer->HandleEvent(event);
        });
        renderer->DrawFrame();

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
