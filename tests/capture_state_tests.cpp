#include <engine/editor/services/capture_state.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::filesystem::path ScratchFolder()
{
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "miniengine_capture_state_tests";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    return folder;
}

// Everything the editor writes comes back as it was, the scene resolved beside the state file.
void StateRoundTrips()
{
    const std::filesystem::path folder = ScratchFolder();
    CaptureState written;
    written.scenePath = "viewport_1.scene.yaml";
    written.originalScenePath = "C:/Scenes/Sponza.yaml";
    written.viewportExtent = RenderExtent{1333, 777};
    written.camera.position = glm::vec3(1.5f, -2.25f, 30.0f);
    written.camera.yawDegrees = 123.5f;
    written.camera.pitchDegrees = -12.25f;
    written.camera.fovDegrees = 60.0f;
    written.camera.exposureEv100 = 11.5f;
    written.camera.autoExposure.enabled = false;
    written.camera.autoExposure.compensationEv = -0.5f;
    written.camera.autoWhiteBalance.degree = 0.25f;
    written.camera.autoWhiteBalance.targetKelvin = 5200.0f;
    written.renderDebug.gbufferView = GBufferDebugView::DdgiIrradiance;
    written.renderDebug.toneMapper = ToneMapper::PbrNeutral;
    written.renderDebug.dlssMode = DlssMode::Performance;
    written.renderDebug.dlssPreset = DlssPreset::K;
    written.renderDebug.taa = false;
    written.renderDebug.renderScale = 0.5f;
    written.renderDebug.viewportResolution = {true, 2560, 1440};
    written.renderDebug.ssr.maxDistance = 12.0f;
    written.renderDebug.ao.stepCount = 5;
    written.renderDebug.gi.enabled = false;
    written.renderDebug.ddgi.levels = 3;
    written.renderDebug.ddgi.baseSpacing = 0.5f;
    written.renderDebug.ddgi.hysteresis = 0.9f;
    written.renderDebug.display.outputMode = DisplayOutputMode::Sdr;
    written.renderDebug.display.calibrated = true;
    written.renderDebug.display.maxLuminance = 640.0f;
    written.renderDebug.display.minLuminance = 0.025f;
    written.renderDebug.display.exposureEv = 0.5f;
    written.renderDebug.display.sdrWhite = 0.93f;
    const std::filesystem::path path = folder / "viewport_1.state.yaml";
    CaptureStateService::Write(path, written);

    const CaptureState read = CaptureStateService::Read(path);
    Require(read.scenePath == folder / "viewport_1.scene.yaml", "the scene resolves beside the state");
    Require(read.originalScenePath == written.originalScenePath, "original scene");
    Require(read.viewportExtent.width == 1333 && read.viewportExtent.height == 777, "viewport size");
    Require(read.camera.position == written.camera.position, "camera position");
    Require(read.camera.yawDegrees == 123.5f && read.camera.pitchDegrees == -12.25f && read.camera.fovDegrees == 60.0f, "camera angles");
    Require(read.camera.exposureEv100 == 11.5f, "exposure");
    Require(!read.camera.autoExposure.enabled && read.camera.autoExposure.compensationEv == -0.5f, "auto exposure");
    Require(read.camera.autoWhiteBalance.degree == 0.25f, "auto white balance");
    Require(read.camera.autoWhiteBalance.targetKelvin == 5200.0f, "white balance target");
    Require(read.renderDebug.gbufferView == GBufferDebugView::DdgiIrradiance, "debug view");
    Require(read.renderDebug.toneMapper == ToneMapper::PbrNeutral, "tone mapper");
    Require(read.renderDebug.dlssMode == DlssMode::Performance && read.renderDebug.dlssPreset == DlssPreset::K, "DLSS");
    Require(!read.renderDebug.taa && read.renderDebug.renderScale == 0.5f, "top-level switches");
    Require(read.renderDebug.viewportResolution == written.renderDebug.viewportResolution, "viewport resolution");
    Require(read.renderDebug.ssr.maxDistance == 12.0f && read.renderDebug.ao.stepCount == 5 && !read.renderDebug.gi.enabled, "groups");
    Require(read.renderDebug.ddgi.levels == 3 && read.renderDebug.ddgi.baseSpacing == 0.5f && read.renderDebug.ddgi.hysteresis == 0.9f, "DDGI");
    Require(read.renderDebug.display == written.renderDebug.display, "display calibration");
}

// A file with only a scene still loads, every other field at its default.
void SparseFileKeepsDefaults()
{
    const std::filesystem::path folder = ScratchFolder();
    const std::filesystem::path path = folder / "sparse.state.yaml";
    std::ofstream(path) << "scene: scenes/a.yaml\ncamera:\n  position: [1, 2, 3]\n";
    const CaptureState read = CaptureStateService::Read(path);
    Require(read.scenePath == folder / "scenes/a.yaml", "relative scene");
    Require(read.camera.position == glm::vec3(1.0f, 2.0f, 3.0f), "position");
    Require(read.renderDebug.ddgi.enabled && read.renderDebug.ddgi.levels == DdgiSettings{}.levels, "defaults");
    Require(!read.viewportExtent.IsValid(), "no viewport size");

    const std::filesystem::path empty = folder / "empty.state.yaml";
    std::ofstream(empty) << "camera: {}\n";
    bool threw = false;
    try
    {
        CaptureStateService::Read(empty);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    Require(threw, "a state without a scene is refused");
}
}

int main()
{
    try
    {
        StateRoundTrips();
        SparseFileKeepsDefaults();
    }
    catch (const std::exception& error)
    {
        std::cerr << "capture state tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "capture state tests passed\n";
    return 0;
}
