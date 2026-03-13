#include <log/log.h>
#include <vulkan/renderer.h>
#include <window/window.h>

#include <imgui.h>
#include <ImGuizmo.h>
#include <entt/entt.hpp>
#include <yaml-cpp/yaml.h>

#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace
{
struct AppOptions
{
    std::optional<std::string> startupModelPath;
    uint32_t maxFrames = 0;
};

AppOptions ParseArgs(int argc, char** argv)
{
    AppOptions options{};

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

        throw std::runtime_error("Unknown argument: " + std::string(argument));
    }

    return options;
}

void PrintDependencyLinkStatus()
{
    const auto imguizmoSymbol = &ImGuizmo::SetOrthographic;
    const YAML::Node yamlNode = YAML::Load("linked: true");

    entt::registry registry;
    const entt::entity entity = registry.create();

    std::cout << "[link] ImGuizmo: " << (imguizmoSymbol != nullptr ? "OK" : "FAILED") << '\n';
    std::cout << "[link] yaml-cpp: " << (yamlNode["linked"].as<bool>() ? "OK" : "FAILED") << '\n';
    std::cout << "[link] EnTT: " << (registry.valid(entity) ? "OK" : "FAILED") << '\n';
}
}

int main(int argc, char** argv)
{
    Log::Init();
    LOG_INFO("MiniEngine starting");

    try
    {
        PrintDependencyLinkStatus();
        const AppOptions options = ParseArgs(argc, argv);
        Window window(1920, 1080, "MiniEngine");
        VulkanRenderer renderer(window, options.startupModelPath);
        uint32_t renderedFrameCount = 0;

        while (!window.ShouldClose())
        {
            window.PollEvents([&renderer](const SDL_Event& event)
            {
                renderer.HandleEvent(event);
            });
            renderer.DrawFrame();

            if (options.maxFrames > 0)
            {
                ++renderedFrameCount;
                if (renderedFrameCount >= options.maxFrames)
                {
                    break;
                }
            }
        }
    }
    catch (const std::exception& error)
    {
        LOG_ERROR("Unhandled exception: {}", error.what());
        return 1;
    }

    LOG_INFO("MiniEngine shutting down");

    return 0;
}
