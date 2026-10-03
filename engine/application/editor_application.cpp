#include "editor_application.h"

#include <engine/core/log/log.h>
#include <engine/core/version/engine_version.h>
#include <engine/editor/renderer_shared_state.h>
#include <engine/editor/services/capture_state.h>
#include <engine/editor/services/scene_io_service.h>
#include <engine/renderer/rhi/factory.h>
#include <engine/platform/window/window.h>

#include <ImGuizmo.h>
#include <entt/entt.hpp>
#include <yaml-cpp/yaml.h>

#include <charconv>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace me
{

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

RenderExtent ParseViewportSize(std::string_view value)
{
    const size_t separator = value.find('x');
    uint32_t width = 0;
    uint32_t height = 0;
    const bool parsed =
        separator != std::string_view::npos &&
        std::from_chars(value.data(), value.data() + separator, width).ec == std::errc{} &&
        std::from_chars(value.data() + separator + 1, value.data() + value.size(), height).ec == std::errc{};
    if (!parsed || width == 0 || height == 0 || width > 16384 || height > 16384)
    {
        throw std::runtime_error("--viewport-size requires WIDTHxHEIGHT, for example 667x541");
    }
    return RenderExtent{width, height};
}

// Exactly count comma-separated numbers, for example "0,1.5,-20" for three.
template <size_t count>
std::array<float, count> ParseFloatList(std::string_view value, std::string_view optionName)
{
    std::array<float, count> numbers{};
    const char* cursor = value.data();
    const char* const end = value.data() + value.size();
    for (size_t index = 0; index < count; ++index)
    {
        const auto [parsedEnd, errorCode] = std::from_chars(cursor, end, numbers[index]);
        const bool last = index + 1 == count;
        if (errorCode != std::errc{} || (last ? parsedEnd != end : parsedEnd == end || *parsedEnd != ','))
        {
            throw std::runtime_error(std::string(optionName) + " requires " + std::to_string(count) + " comma-separated numbers");
        }
        cursor = parsedEnd + 1;
    }
    return numbers;
}

RenderBackendType ParseRenderBackend(std::string_view value)
{
    RenderBackendType backendType = GetPreferredRenderBackendType();
    if (!TryParseRenderBackendType(value, backendType))
    {
        throw std::runtime_error(
            "Unknown backend: " + std::string(value) + ". Supported values: vulkan");
    }

    if (const std::optional<std::string> runtimeError = GetRenderBackendRuntimeError(backendType); runtimeError.has_value())
    {
        throw std::runtime_error(
            "Requested backend '" + std::string(value) + "' is unavailable: " +
            *runtimeError);
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

        if (argument == "--scene")
        {
            options.startupScenePath = ReadRequiredArgument(i, argc, argv, argument);
            continue;
        }

        if (argument == "--frames")
        {
            options.maxFrames = ParsePositiveFrameCount(ReadRequiredArgument(i, argc, argv, argument));
            continue;
        }

        if (argument == "--capture")
        {
            options.capturePath = ReadRequiredArgument(i, argc, argv, argument);
            continue;
        }

        if (argument == "--viewport-size")
        {
            options.viewportSize = ParseViewportSize(ReadRequiredArgument(i, argc, argv, argument));
            continue;
        }

        if (argument == "--khronos-reference")
        {
            options.khronosReference = true;
            continue;
        }

        if (argument == "--camera")
        {
            options.camera = ParseFloatList<5>(ReadRequiredArgument(i, argc, argv, argument), argument);
            continue;
        }

        if (argument == "--camera-velocity")
        {
            const std::array<float, 3> velocity = ParseFloatList<3>(ReadRequiredArgument(i, argc, argv, argument), argument);
            options.cameraVelocity = glm::vec3(velocity[0], velocity[1], velocity[2]);
            continue;
        }

        if (argument == "--debug-view")
        {
            const std::string_view value = ReadRequiredArgument(i, argc, argv, argument);
            uint32_t view = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), view).ptr != value.data() + value.size() ||
                view > static_cast<uint32_t>(GBufferDebugView::DdgiProbes))
            {
                throw std::runtime_error("--debug-view requires a view number from 0 to " +
                                         std::to_string(static_cast<uint32_t>(GBufferDebugView::DdgiProbes)));
            }
            options.debugView = static_cast<GBufferDebugView>(view);
            continue;
        }

        if (argument == "--state")
        {
            options.statePath = std::filesystem::path(ReadRequiredArgument(i, argc, argv, argument));
            options.waitForScene = true;
            continue;
        }

        if (argument == "--wait-for-scene")
        {
            options.waitForScene = true;
            continue;
        }

        if (argument == "--no-ddgi")
        {
            options.ddgiDisabled = true;
            continue;
        }

        if (argument == "--ddgi-spacing")
        {
            options.ddgiSpacing = ParseFloatList<1>(ReadRequiredArgument(i, argc, argv, argument), argument)[0];
            continue;
        }

        if (argument == "--reference")
        {
            options.referencePrefix = ReadRequiredArgument(i, argc, argv, argument);
            continue;
        }

        if (argument == "--reference-explain")
        {
            const std::array<float, 2> point = ParseFloatList<2>(ReadRequiredArgument(i, argc, argv, argument), argument);
            options.referenceExplainColumn = static_cast<int>(point[0]);
            options.referenceExplainRow = static_cast<int>(point[1]);
            continue;
        }

        if (argument == "--reference-samples" || argument == "--reference-stride")
        {
            const std::string_view value = ReadRequiredArgument(i, argc, argv, argument);
            uint32_t number = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), number).ptr != value.data() + value.size() || number == 0)
            {
                throw std::runtime_error(std::string(argument) + " requires a positive integer");
            }
            (argument == "--reference-samples" ? options.referenceSamples : options.referenceStride) = number;
            continue;
        }

        if (argument == "--backend")
        {
            options.renderBackend = ParseRenderBackend(ReadRequiredArgument(i, argc, argv, argument));
            continue;
        }

        if (argument == "--project")
        {
            options.paths.projectRoot = ReadRequiredArgument(i, argc, argv, argument);
            continue;
        }

        if (argument == "--assets")
        {
            options.paths.assetsRoot = ReadRequiredArgument(i, argc, argv, argument);
            continue;
        }

        if (argument == "--cache")
        {
            options.paths.cacheRoot = ReadRequiredArgument(i, argc, argv, argument);
            continue;
        }

        if (argument == "--shaders")
        {
            options.paths.shaderRoot = ReadRequiredArgument(i, argc, argv, argument);
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
    // Resolve the directory roots before any subsystem touches the filesystem.
    EnginePaths::Initialize(m_options.paths);
    LOG_INFO(
        "Roots: project='{}' assets='{}' cache='{}' shaders='{}'",
        EnginePaths::ProjectRoot().string(),
        EnginePaths::AssetsRoot().string(),
        EnginePaths::CacheRoot().string(),
        EnginePaths::ShaderRoot().string());

    // The assets folder is not version controlled, so a fresh checkout has none. Every asset browser
    // action works inside it; create it up front instead of leaving the browser empty and inert.
    std::error_code assetsRootError;
    std::filesystem::create_directories(EnginePaths::AssetsRoot(), assetsRootError);
    if (assetsRootError)
    {
        LOG_ERROR(
            "Could not create the assets folder '{}': {}",
            EnginePaths::AssetsRoot().string(),
            assetsRootError.message());
    }

    auto sharedState = std::make_shared<RendererSharedState>();
    // A scripted run renders what its options say, not what the editor was last left at, and does
    // not save the camera or render settings its options changed.
    sharedState->viewSettingsFromCommandLine =
        m_options.maxFrames > 0 || m_options.statePath.has_value() || m_options.khronosReference ||
        m_options.debugView.has_value() || m_options.ddgiDisabled || m_options.ddgiSpacing.has_value();
    std::optional<std::string> startupScenePath = m_options.startupScenePath;
    std::optional<RenderExtent> viewportSize = m_options.viewportSize;
    if (m_options.statePath.has_value())
    {
        const CaptureState state = CaptureStateService::Read(*m_options.statePath);
        LOG_INFO("Replaying the capture state '{}'", m_options.statePath->string());
        sharedState->editorUi.EditRenderDebug() = state.renderDebug;
        Camera& camera = sharedState->camera;
        camera.position = state.camera.position;
        camera.yawDegrees = state.camera.yawDegrees;
        camera.pitchDegrees = state.camera.pitchDegrees;
        camera.fovDegrees = state.camera.fovDegrees;
        camera.nearPlane = state.camera.nearPlane;
        camera.farPlane = state.camera.farPlane;
        camera.exposureEv100 = state.camera.exposureEv100;
        camera.autoExposure = state.camera.autoExposure;
        camera.autoWhiteBalance = state.camera.autoWhiteBalance;
        if (!startupScenePath.has_value())
        {
            startupScenePath = state.scenePath.string();
        }
        if (!viewportSize.has_value() && state.viewportExtent.IsValid())
        {
            viewportSize = state.viewportExtent;
        }
    }
    if (m_options.khronosReference)
    {
        sharedState->editorUi.EditRenderDebug().khronosReference = true;
    }
    if (m_options.debugView.has_value())
    {
        sharedState->editorUi.EditRenderDebug().gbufferView = *m_options.debugView;
    }
    if (m_options.ddgiDisabled)
    {
        sharedState->editorUi.EditRenderDebug().ddgi.enabled = false;
    }
    if (m_options.ddgiSpacing.has_value())
    {
        sharedState->editorUi.EditRenderDebug().ddgi.baseSpacing = *m_options.ddgiSpacing;
    }
    if (m_options.camera.has_value())
    {
        const std::array<float, 5>& camera = *m_options.camera;
        sharedState->camera.position = glm::vec3(camera[0], camera[1], camera[2]);
        sharedState->camera.yawDegrees = camera[3];
        sharedState->camera.pitchDegrees = camera[4];
    }
    sharedState->fixedViewportExtent = viewportSize;
    LOG_INFO("Using render backend: {}", ToString(m_options.renderBackend));
    const std::string windowTitle = std::string("MiniEngine v") + EngineVersion::String();
    Window window(1920, 1080, windowTitle.c_str(), m_options.renderBackend);
    std::unique_ptr<IRenderBackend> renderer = CreateRenderBackend(
        window,
        sharedState,
        m_options.renderBackend,
        m_options.startupModelPath);
    if (startupScenePath.has_value())
    {
        // Loaded asynchronously and applied by the frame loop, exactly like a scene opened from the
        // editor, so it replaces the test scene a few frames in.
        SceneIoService::StartAsyncSceneLoad(*sharedState, *startupScenePath);
    }
    uint32_t renderedFrameCount = 0;

    // Keeps the frame coming while a window edge is dragged, so the area the drag exposes is drawn
    // instead of left unpainted until the mouse is released. The handler is removed before the
    // renderer it calls is destroyed.
    window.SetLiveResizeHandler([&renderer]()
                                {
                                    renderer->DrawFrame();
                                });
    struct LiveResizeHandlerReset
    {
        Window& window;
        ~LiveResizeHandlerReset()
        {
            window.SetLiveResizeHandler({});
        }
    } liveResizeHandlerReset{window};

    while (!window.ShouldClose())
    {
        window.PollEvents([&renderer](const SDL_Event& event)
                          {
                              renderer->HandleEvent(event);
                          });
        renderer->DrawFrame();

        // Still loading: the scene file, its models, its textures or its ray scene.
        const bool loading = sharedState->asyncSceneLoad.IsActive() || sharedState->asyncLoad.IsActive() ||
                             !sharedState->pendingModelLoads.empty() || !sharedState->sceneUploadStatus.empty() ||
                             sharedState->rayScenePending;
        const bool waiting = m_options.waitForScene && loading;
        // The camera moves only on the frames that count, so it starts from where it was placed.
        if (!waiting)
        {
            sharedState->camera.position += m_options.cameraVelocity;
        }
        if (m_options.maxFrames > 0 && !waiting)
        {
            ++renderedFrameCount;
            if (renderedFrameCount >= m_options.maxFrames)
            {
                break;
            }
        }
    }

    if (m_options.maxFrames > 0)
    {
        renderer->LogFrameTimings();
    }
    if (m_options.capturePath.has_value())
    {
        renderer->CaptureViewport(*m_options.capturePath);
    }
    if (m_options.referencePrefix.has_value())
    {
        IRenderBackend::DdgiReferenceRequest reference;
        reference.prefix = *m_options.referencePrefix;
        reference.samples = m_options.referenceSamples;
        reference.stride = m_options.referenceStride;
        reference.explainColumn = m_options.referenceExplainColumn;
        reference.explainRow = m_options.referenceExplainRow;
        renderer->CaptureDdgiReference(reference);
    }

    return 0;
}
}
