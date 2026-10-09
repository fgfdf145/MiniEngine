#include "editor_application.h"

#include <engine/asset/compressed_texture_cache.h>
#include <engine/asset/tyre_library.h>
#include <engine/audio/audio_engine.h>
#include <engine/core/log/log.h>
#include <engine/core/threading/task_future.h>
#include <engine/core/threading/task_system.h>
#include <engine/core/version/engine_version.h>
#include <engine/editor/engine_settings.h>
#include <engine/editor/renderer_shared_state.h>
#include <engine/editor/services/capture_state.h>
#include <engine/editor/services/scene_io_service.h>
#include <engine/editor/services/vehicle_drive_service.h>
#include <engine/renderer/rhi/factory.h>
#include <engine/platform/process/process_allocation.h>
#include <engine/platform/process/process_scheduling.h>
#include <engine/platform/window/window.h>

#include <ImGuizmo.h>
#include <entt/entt.hpp>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
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

RenderExtent ParseViewportSize(std::string_view value, std::string_view argument = "--viewport-size")
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
        throw std::runtime_error(std::string(argument) + " requires WIDTHxHEIGHT, for example 667x541");
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
            "Unknown backend: " + std::string(value) + ". Supported values: vulkan, d3d12");
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

        if (argument == "--drive")
        {
            options.driveEntity = ReadRequiredArgument(i, argc, argv, argument);
            continue;
        }

        if (argument == "--drive-controls")
        {
            options.driveControls = ParseFloatList<2>(ReadRequiredArgument(i, argc, argv, argument), argument);
            continue;
        }

        if (argument == "--drive-view")
        {
            const std::string_view value = ReadRequiredArgument(i, argc, argv, argument);
            if (value == "fixed")
            {
                options.driveCameraFixed = true;
                continue;
            }
            for (size_t view = 0; view < kVehicleCameraViewCount && !options.driveView.has_value(); ++view)
            {
                std::string name = VehicleCameraViewName(static_cast<VehicleCameraView>(view));
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c)
                               {
                                   return static_cast<char>(std::tolower(c));
                               });
                if (value == name)
                {
                    options.driveView = static_cast<VehicleCameraView>(view);
                }
            }
            if (!options.driveView.has_value())
            {
                throw std::runtime_error("--drive-view takes chase, cockpit, bonnet, bumper or fixed");
            }
            continue;
        }

        if (argument == "--capture-at")
        {
            const std::string_view value = ReadRequiredArgument(i, argc, argv, argument);
            size_t start = 0;
            while (start <= value.size())
            {
                const size_t end = std::min(value.find(',', start), value.size());
                options.captureFrames.push_back(ParsePositiveFrameCount(value.substr(start, end - start)));
                start = end + 1;
            }
            continue;
        }

        if (argument == "--turn-sun")
        {
            options.sunTurn = ParseFloatList<4>(ReadRequiredArgument(i, argc, argv, argument), argument);
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
                view > static_cast<uint32_t>(kLastGBufferDebugView))
            {
                throw std::runtime_error("--debug-view requires a view number from 0 to " +
                                         std::to_string(static_cast<uint32_t>(kLastGBufferDebugView)));
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

        if (argument == "--record")
        {
            options.recordPath = std::filesystem::path(std::string(ReadRequiredArgument(i, argc, argv, argument)));
            continue;
        }

        if (argument == "--photo")
        {
            options.photoPath = std::filesystem::path(std::string(ReadRequiredArgument(i, argc, argv, argument)));
            continue;
        }

        if (argument == "--photo-size")
        {
            options.photoSize = ParseViewportSize(ReadRequiredArgument(i, argc, argv, argument), argument);
            continue;
        }

        if (argument == "--photo-at")
        {
            options.photoAtFrame = ParsePositiveFrameCount(ReadRequiredArgument(i, argc, argv, argument));
            continue;
        }

        if (argument == "--photo-max-view-pixels")
        {
            options.photoMaxViewPixels = ParsePositiveFrameCount(ReadRequiredArgument(i, argc, argv, argument));
            continue;
        }

        if (argument == "--photo-warmup")
        {
            options.photoWarmupFrames = ParsePositiveFrameCount(ReadRequiredArgument(i, argc, argv, argument));
            continue;
        }

        if (argument == "--photo-path-tracing" || argument == "--photo-rr")
        {
            const std::string_view value = ReadRequiredArgument(i, argc, argv, argument);
            if (value != "on" && value != "off")
            {
                throw std::runtime_error(std::string(argument) + " takes on or off");
            }
            (argument == "--photo-rr" ? options.photoRayReconstruction : options.photoPathTracing) = value == "on";
            continue;
        }

        if (argument == "--photo-spp")
        {
            options.photoSamplesPerPixel = ParsePositiveFrameCount(ReadRequiredArgument(i, argc, argv, argument));
            continue;
        }

        if (argument == "--photo-samples")
        {
            options.photoTargetSamples = ParsePositiveFrameCount(ReadRequiredArgument(i, argc, argv, argument));
            continue;
        }

        if (argument == "--photo-dlss")
        {
            const std::string_view value = ReadRequiredArgument(i, argc, argv, argument);
            static constexpr std::pair<std::string_view, DlssMode> kModes[] = {
                {"off", DlssMode::Off},
                {"dlaa", DlssMode::Dlaa},
                {"quality", DlssMode::Quality},
                {"balanced", DlssMode::Balanced},
                {"performance", DlssMode::Performance},
                {"ultra-performance", DlssMode::UltraPerformance}};
            const auto mode = std::find_if(std::begin(kModes), std::end(kModes), [&](const auto& entry) { return entry.first == value; });
            if (mode == std::end(kModes))
            {
                throw std::runtime_error("--photo-dlss takes off, dlaa, quality, balanced, performance or ultra-performance");
            }
            options.photoDlssMode = mode->second;
            continue;
        }

        if (argument == "--quad-record")
        {
            options.quadRecordPath = std::filesystem::path(std::string(ReadRequiredArgument(i, argc, argv, argument)));
            continue;
        }

        if (argument == "--record-fps")
        {
            const std::string_view value = ReadRequiredArgument(i, argc, argv, argument);
            uint32_t number = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), number).ptr != value.data() + value.size() ||
                number == 0 || number > 240)
            {
                throw std::runtime_error("--record-fps requires an integer from 1 to 240");
            }
            options.recordFramesPerSecond = number;
            continue;
        }

        if (argument == "--no-render-thread")
        {
            options.renderThread = false;
            continue;
        }

        if (argument == "--no-parallel-recording")
        {
            options.parallelRecording = false;
            continue;
        }

        if (argument == "--no-ray-query")
        {
            options.rayQuery = false;
            continue;
        }

        if (argument == "--adopt-car-tyres")
        {
            const std::string_view gltf = ReadRequiredArgument(i, argc, argv, argument);
            const std::string_view folder = ReadRequiredArgument(i, argc, argv, argument);
            options.adoptCarTyres.emplace_back(std::string(gltf), std::string(folder));
            continue;
        }

        if (argument == "--follow-path")
        {
            options.followPath = std::string(ReadRequiredArgument(i, argc, argv, argument));
            continue;
        }

        if (argument == "--path-speed-scale")
        {
            options.pathSpeedScale = ParseFloatList<1>(ReadRequiredArgument(i, argc, argv, argument), argument)[0];
            if (!(options.pathSpeedScale > 0.0f))
            {
                throw std::runtime_error("--path-speed-scale requires a number above 0");
            }
            continue;
        }

        if (argument == "--replay-drive")
        {
            options.replayDrive = std::filesystem::path(std::string(ReadRequiredArgument(i, argc, argv, argument)));
            continue;
        }

        if (argument == "--drive-log")
        {
            options.driveLog = std::filesystem::path(std::string(ReadRequiredArgument(i, argc, argv, argument)));
            continue;
        }

        if (argument == "--physics-rate")
        {
            const std::string_view value = ReadRequiredArgument(i, argc, argv, argument);
            int number = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), number).ptr != value.data() + value.size() ||
                number < 60 || number > 4000)
            {
                throw std::runtime_error("--physics-rate requires an integer from 60 to 4000");
            }
            options.physicsRateHz = number;
            continue;
        }

        if (argument == "--task-threads")
        {
            const std::string_view value = ReadRequiredArgument(i, argc, argv, argument);
            uint32_t number = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), number).ptr != value.data() + value.size() ||
                number == 0 || number > 256)
            {
                throw std::runtime_error("--task-threads requires an integer from 1 to 256");
            }
            options.taskThreads = number;
            continue;
        }

        if (argument == "--priority")
        {
            const std::string_view value = ReadRequiredArgument(i, argc, argv, argument);
            options.processPriority = platform::process::ParseProcessPriority(value);
            if (!options.processPriority.has_value())
            {
                throw std::runtime_error("--priority requires below-normal, normal, above-normal or high");
            }
            continue;
        }

        if (argument == "--cpus")
        {
            const std::string_view value = ReadRequiredArgument(i, argc, argv, argument);
            options.cpuSelection = platform::process::ParseCpuSelection(value);
            if (!options.cpuSelection.has_value() || *options.cpuSelection == platform::process::CpuSelection::Custom)
            {
                const std::optional<std::vector<uint32_t>> cpus = platform::process::ParseCpuList(value);
                if (!cpus.has_value())
                {
                    throw std::runtime_error("--cpus requires all, performance or a list of CPUs such as 0,2,4-7");
                }
                options.cpuSelection = platform::process::CpuSelection::Custom;
                options.customCpus = *cpus;
            }
            continue;
        }

        if (argument == "--wait-for-scene")
        {
            options.waitForScene = true;
            continue;
        }

        if (argument == "--no-audio")
        {
            options.audioDisabled = true;
            continue;
        }

        if (argument == "--no-ddgi")
        {
            options.ddgiDisabled = true;
            continue;
        }

        if (argument == "--software-rays")
        {
            options.softwareRays = true;
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

    if ((options.followPath.has_value() || options.replayDrive.has_value() || options.driveLog.has_value()) && !options.driveEntity.has_value())
    {
        throw std::runtime_error("--follow-path, --replay-drive and --drive-log need --drive");
    }
    if (options.followPath.has_value() && options.replayDrive.has_value())
    {
        throw std::runtime_error("--follow-path and --replay-drive cannot go together");
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

std::vector<uint32_t> EditorApplication::ApplyStartupProcessAllocation()
{
    EngineSettings settings;
    std::string settingsError;
    if (!LoadEngineSettings(BuildEngineSettingsPath(), settings, settingsError))
    {
        // The editor reports the broken file when it loads it; run at the defaults meanwhile.
        settings = EngineSettings{};
    }
    platform::process::ProcessAllocation allocation = settings.process;
    if (m_options.processPriority.has_value())
    {
        allocation.priority = *m_options.processPriority;
    }
    if (m_options.cpuSelection.has_value())
    {
        allocation.cpus = *m_options.cpuSelection;
        allocation.customCpus = m_options.customCpus;
    }

    const platform::process::ProcessorTopology topology = platform::process::QueryProcessorTopology();
    const platform::process::ProcessAllocationResult result = platform::process::ApplyProcessAllocation(allocation, topology);
    m_processStatus = platform::process::DescribeProcessAllocation(allocation, result, topology);
    if (m_options.processPriority.has_value() || m_options.cpuSelection.has_value())
    {
        m_processStatus += " (command line)";
    }
    if (result.error.empty())
    {
        LOG_INFO("Process: {}", m_processStatus);
    }
    else
    {
        LOG_WARN("Process: {}", m_processStatus);
    }
    return result.cpus;
}

int EditorApplication::Run()
{
    // Before any thread starts, so the workers inherit it.
    platform::process::RequestFullSpeedScheduling();
    // Resolve the directory roots before any subsystem touches the filesystem.
    EnginePaths::Initialize(m_options.paths);
    if (!m_options.adoptCarTyres.empty())
    {
        for (const auto& [gltf, folder] : m_options.adoptCarTyres)
        {
            const size_t tyres = TyreLibrary::AdoptGltfCarTyres(EnginePaths::ResolveProjectPath(gltf), folder);
            LOG_INFO("'{}': its {} compound tyre(s) are in the tyre library under '{}'", gltf, tyres, folder);
        }
        return 0;
    }
    // The priority and CPUs, before the task system sizes its workers to those CPUs. The editor
    // loads the rest of the settings later; a change in the Preferences window applies at once.
    const std::vector<uint32_t> processCpus = ApplyStartupProcessAllocation();
    // Before anything runs tasks; stopped after everything that does (the renderer) is gone.
    // Workers for every CPU the process could be given, those past the CPUs it has now parked, so a
    // change in the Preferences window resizes the parallel work (physics steps included) at once.
    TaskSystem::Settings taskSettings;
    const auto machineCpus = static_cast<uint32_t>(platform::process::QueryProcessorTopology().processors.size());
    taskSettings.workerThreads = m_options.taskThreads > 0 ? m_options.taskThreads : TaskSystem::WorkersForCpus(machineCpus);
    taskSettings.activeWorkerThreads = TaskSystem::WorkersForCpus(static_cast<uint32_t>(processCpus.size()));
    TaskSystem::Initialize(taskSettings);
    struct TaskSystemShutdown
    {
        ~TaskSystemShutdown()
        {
            TaskSystem::Shutdown();
        }
    } taskSystemShutdown;
    LOG_INFO(
        "Roots: project='{}' assets='{}' cache='{}' shaders='{}'",
        EnginePaths::ProjectRoot().string(),
        EnginePaths::AssetsRoot().string(),
        EnginePaths::CacheRoot().string(),
        EnginePaths::ShaderRoot().string());

    // The texture cache is shared by every checkout on the machine and only grows; trim it in the
    // background. Declared after the task system's guard, so it finishes before the workers stop.
    TaskFuture<void> textureCacheTrim = RunAsync(TaskPriority::Low, []()
    {
        const std::filesystem::path directory = DefaultTextureCacheDirectory();
        const TextureCacheTrim trim = TrimCompressedTextureCache(directory, kTextureCacheBudgetBytes);
        if (trim.filesDeleted > 0)
        {
            LOG_INFO(
                "Texture cache '{}': {:.1f} GB, deleted {} least recently used files ({:.1f} GB)",
                directory.string(),
                static_cast<double>(trim.bytesBefore) / 1e9,
                trim.filesDeleted,
                static_cast<double>(trim.bytesDeleted) / 1e9);
        }
    });

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
    sharedState->processStatus = m_processStatus;
    sharedState->renderThread = m_options.renderThread;
    sharedState->parallelRecording = m_options.parallelRecording;
    sharedState->rayQuery = m_options.rayQuery;
    // A scripted run renders what its options say, not what the editor was last left at, and does
    // not save the camera or render settings its options changed.
    sharedState->viewSettingsFromCommandLine =
        m_options.maxFrames > 0 || m_options.statePath.has_value() || m_options.khronosReference ||
        m_options.debugView.has_value() || m_options.ddgiDisabled || m_options.ddgiSpacing.has_value() ||
        m_options.softwareRays;
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
    if (m_options.softwareRays)
    {
        sharedState->editorUi.EditRenderDebug().hardwareRayTracing = false;
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
    if (m_options.audioDisabled)
    {
        sharedState->audioStatus = "Off (--no-audio)";
    }
    else
    {
        // A scripted run stays silent: its sounds go to no device.
        AudioEngineOptions audioOptions;
        audioOptions.output = m_options.maxFrames > 0 ? AudioOutput::None : AudioOutput::Device;
        std::string audioError;
        sharedState->audio = AudioEngine::Create(audioOptions, audioError);
        if (sharedState->audio)
        {
            const AudioEngine& audio = *sharedState->audio;
            sharedState->audioStatus = audio.Output() == AudioOutput::None
                                           ? std::string("None (scripted run)")
                                           : audio.DeviceName() + ", " + std::to_string(audio.SampleRate()) + " Hz, " +
                                                 std::to_string(audio.Channels()) + " channels";
        }
        else
        {
            // The editor works without sound.
            LOG_WARN("No audio output: {}", audioError);
            sharedState->audioStatus = "None: " + audioError;
        }
    }
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
    bool countingStarted = false;
    bool recordingStarted = false;
    bool quadRecordingStarted = false;
    bool photoStarted = false;
    bool driveStarted = false;
    const bool automatedDrive = m_options.followPath.has_value() || m_options.replayDrive.has_value();
    int exitCode = 0;
    if (m_options.driveControls.has_value())
    {
        VehicleControls controls;
        controls.throttle = std::clamp((*m_options.driveControls)[0], -1.0f, 1.0f);
        controls.steering = std::clamp((*m_options.driveControls)[1], -1.0f, 1.0f);
        sharedState->vehicleDrive.scriptedControls = controls;
    }
    if (m_options.driveView.has_value())
    {
        sharedState->vehicleDrive.cameraView = *m_options.driveView;
    }
    if (m_options.physicsRateHz.has_value())
    {
        sharedState->editorUi.EditVehiclePhysicsRate() = *m_options.physicsRateHz;
    }
    if (m_options.driveCameraFixed)
    {
        sharedState->editorUi.EditVehicleCamera().follow = false;
    }

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
    window.SetQuitGuard([&renderer]()
                        {
                            return renderer->AllowQuit();
                        });
    struct QuitGuardReset
    {
        Window& window;
        ~QuitGuardReset()
        {
            window.SetQuitGuard({});
        }
    } quitGuardReset{window};

    while (!window.ShouldClose())
    {
        window.PollEvents([&renderer](const SDL_Event& event)
                          {
                              renderer->HandleEvent(event);
                          });
        renderer->DrawFrame();
        // A minimized window draws nothing (DrawFrame returns at once), so the loop would spin a
        // core and run through --frames in a moment: it idles instead, and its frames do not count.
        const bool minimized = (SDL_GetWindowFlags(window.GetSDLWindow()) & SDL_WINDOW_MINIMIZED) != 0;
        if (minimized)
        {
            SDL_Delay(10);
        }

        const bool loading = sharedState->IsSceneLoading();
        const bool waiting = m_options.waitForScene && loading;
        // The next frame built draws with every temporal effect started over.
        if (m_options.waitForScene && !waiting && !countingStarted)
        {
            countingStarted = true;
            ++sharedState->temporalRestart;
        }
        if (m_options.driveEntity.has_value() && !driveStarted && !loading)
        {
            driveStarted = true;
            const IEditorWorld& world = sharedState->GetEditorWorld();
            const std::vector<entt::entity>& order = world.GetSceneOrder();
            const auto found = std::find_if(order.begin(), order.end(), [&](entt::entity entity)
                                            {
                                                return world.GetTag(entity).name == *m_options.driveEntity;
                                            });
            if (found == order.end())
            {
                std::string names;
                for (const entt::entity entity : order)
                {
                    names += (names.empty() ? "'" : ", '") + world.GetTag(entity).name + "'";
                }
                throw std::runtime_error("--drive: the scene has no entity named '" + *m_options.driveEntity + "' (it has " + names + ")");
            }
            VehicleDriveService::Start(*sharedState, *found, VehicleDriveService::DefaultTuning());
            if (m_options.followPath.has_value())
            {
                DrivePathTrackSettings track;
                track.speedScale = m_options.pathSpeedScale;
                VehicleDriveService::StartPathFollow(*sharedState, *m_options.followPath, track);
            }
            else if (m_options.replayDrive.has_value())
            {
                VehicleDriveService::StartReplay(*sharedState, *m_options.replayDrive);
            }
            sharedState->vehicleDrive.fixedFrameStep = automatedDrive;
            if (m_options.driveLog.has_value())
            {
                VehicleDriveService::StartDriveLog(*sharedState, *m_options.driveLog, !automatedDrive);
            }
        }
        // A path run or replay ends the run when it ends.
        if (automatedDrive && driveStarted && sharedState->vehicleDrive.automationResult.has_value())
        {
            exitCode = *sharedState->vehicleDrive.automationResult ? 0 : 3;
            break;
        }
        // The recording starts once a frame counts, with the frame after it: the viewport has its
        // size only once a frame has been drawn.
        if (m_options.recordPath.has_value() && !waiting && !recordingStarted)
        {
            recordingStarted = true;
            IRenderBackend::VideoRecordingRequest request;
            request.path = *m_options.recordPath;
            request.framesPerSecond = m_options.recordFramesPerSecond;
            request.everyFrame = true;
            std::string error;
            if (!renderer->StartVideoRecording(request, error))
            {
                throw std::runtime_error("Cannot record to '" + m_options.recordPath->string() + "': " + error);
            }
        }
        // Once the car it films is driven (when --drive asks for one).
        if (m_options.quadRecordPath.has_value() && !waiting && !quadRecordingStarted && (!m_options.driveEntity.has_value() || driveStarted))
        {
            quadRecordingStarted = true;
            IRenderBackend::VideoRecordingRequest request;
            request.path = *m_options.quadRecordPath;
            request.framesPerSecond = m_options.recordFramesPerSecond;
            request.everyFrame = true;
            std::string error;
            if (!renderer->StartQuadRecording(request, error))
            {
                throw std::runtime_error("Cannot record to '" + m_options.quadRecordPath->string() + "': " + error);
            }
        }
        if (m_options.photoPath.has_value() && !waiting && !photoStarted && renderedFrameCount + 1 >= m_options.photoAtFrame)
        {
            photoStarted = true;
            const PhotoModeSettings saved = ClampPhotoModeSettings(sharedState->engineSettings.photoMode);
            IRenderBackend::PhotoRequest request = PhotoRequestFromSettings(saved);
            request.path = *m_options.photoPath;
            request.width = m_options.photoSize.has_value() ? m_options.photoSize->width : saved.width;
            request.height = m_options.photoSize.has_value() ? m_options.photoSize->height : saved.height;
            request.warmupFrames = m_options.photoWarmupFrames.value_or(saved.warmupFrames);
            request.maxViewPixels = m_options.photoMaxViewPixels.value_or(0);
            request.offlinePathTracing = m_options.photoPathTracing.value_or(request.offlinePathTracing);
            request.samplesPerPixel = m_options.photoSamplesPerPixel.value_or(request.samplesPerPixel);
            request.targetSamples = m_options.photoTargetSamples.value_or(request.targetSamples);
            request.dlssMode = m_options.photoDlssMode.value_or(request.dlssMode);
            request.dlssRayReconstruction = m_options.photoRayReconstruction.value_or(request.dlssRayReconstruction);
            std::string error;
            if (!renderer->TakePhoto(request, error))
            {
                throw std::runtime_error("Cannot take the photo '" + m_options.photoPath->string() + "': " + error);
            }
        }
        // The camera moves only on the frames that count, so it starts from where it was placed.
        if (!waiting && !minimized)
        {
            sharedState->camera.position += m_options.cameraVelocity;
        }
        if (m_options.maxFrames > 0 && !waiting && !minimized)
        {
            ++renderedFrameCount;
            if (m_options.capturePath.has_value() &&
                std::find(m_options.captureFrames.begin(), m_options.captureFrames.end(), renderedFrameCount) != m_options.captureFrames.end())
            {
                const std::filesystem::path path(*m_options.capturePath);
                renderer->CaptureViewport(path.parent_path() / (path.stem().string() + "_" + std::to_string(renderedFrameCount) + path.extension().string()));
            }
            if (m_options.sunTurn.has_value() && renderedFrameCount == static_cast<uint32_t>((*m_options.sunTurn)[0]))
            {
                IEditorWorld& world = sharedState->GetEditorWorld();
                std::vector<entt::entity> suns;
                world.ForEachLight([&](entt::entity entity, const TagComponent&, const TransformComponent&, const LightComponent& light)
                                   {
                                       if (light.type == LightType::Directional)
                                       {
                                           suns.push_back(entity);
                                       }
                                   });
                for (const entt::entity sun : suns)
                {
                    world.EditTransform(sun).rotationDegrees += glm::vec3((*m_options.sunTurn)[1], (*m_options.sunTurn)[2], (*m_options.sunTurn)[3]);
                }
                LOG_INFO("--turn-sun: turned {} directional light(s) after frame {}", suns.size(), renderedFrameCount);
            }
            if (renderedFrameCount >= m_options.maxFrames)
            {
                break;
            }
        }
    }

    VehicleDriveService::StopDriveLog(*sharedState);
    // Also when the window was closed before the last frame: the file is finished either way.
    renderer->StopVideoRecording();
    renderer->StopQuadRecording();
    renderer->WaitForPhotoWrite();
    if (m_options.photoPath.has_value() && (!photoStarted || renderer->IsTakingPhoto()))
    {
        LOG_ERROR("--photo: the run ended before the photo was saved; give --frames more than its warm-up frames");
        exitCode = exitCode == 0 ? 4 : exitCode;
    }
    else if (m_options.photoPath.has_value() && !std::filesystem::exists(*m_options.photoPath))
    {
        // Its view could not be made or read back; the log says why.
        exitCode = exitCode == 0 ? 4 : exitCode;
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

    return exitCode;
}
}
