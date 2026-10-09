#pragma once

#include "material_preview_environment.h"
#include "material_preview_geometry.h"
#include "material_preview_shading.h"

#include <glm/glm.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace me
{

// The preview's orbiting camera (Unreal's asset viewports): it looks at target from distance away,
// turned yaw about +Y (0 puts it on +Z looking toward -Z) and raised by pitch.
struct MaterialPreviewCamera
{
    glm::vec3 target{0.0f};
    float yaw = 0.7f;
    float pitch = 0.3f;
    float distance = 3.0f;
    float verticalFov = 0.6f;

    glm::vec3 Position() const;
    bool operator==(const MaterialPreviewCamera&) const = default;
};

struct MaterialPreviewSettings
{
    MaterialPreviewEnvironmentPreset environment = MaterialPreviewEnvironmentPreset::Studio;
    // A grey floor under the object that takes its shadow and occlusion, fading into the sky.
    bool floor = true;
    // Stops added to the exposure.
    float exposureEv = 0.0f;
    // The key light turned about +Y and raised, radians, from where the environment has it.
    float lightYaw = 0.0f;
    float lightPitch = 0.0f;
    // Samples a pixel gathers before the preview stops rendering.
    uint32_t targetSamples = 64;

    bool operator==(const MaterialPreviewSettings&) const = default;
};

// A finished image of the preview.
struct MaterialPreviewFrame
{
    uint32_t width = 0;
    uint32_t height = 0;
    // Tone mapped, sRGB-encoded RGBA8, rows top-down, with the highlighted slot's outline drawn.
    std::vector<uint8_t> rgba;
    // The material slot each pixel's nearest surface draws with, -1 for the sky and the floor.
    std::vector<int32_t> slots;
    uint32_t samples = 0;
    // Counts the frames finished.
    uint64_t serial = 0;
};

// The Material Editor's preview renderer (docs/design/2026-10-09-material-editor-redesign-design.md):
// a progressive ray tracer on the CPU. Each pass traces one sample of every pixel, in 32 x 32 tiles,
// as a low-priority task on the task system that the editor's thread starts and polls but never
// waits for; passes add up until the settings' target, then it stops. Any change of the scene, the
// camera, the size or the settings starts over; while the camera moves (interacting) it renders at
// half the size without adding up, to keep up with the mouse.
class MaterialPreviewRenderer
{
  public:
    MaterialPreviewRenderer();
    MaterialPreviewRenderer(const MaterialPreviewRenderer&) = delete;
    MaterialPreviewRenderer& operator=(const MaterialPreviewRenderer&) = delete;
    // Waits for the pass in flight.
    ~MaterialPreviewRenderer();

    // What is shown: the geometry and one material per geometry mesh. Null geometry shows the sky.
    void SetScene(std::shared_ptr<const MaterialPreviewGeometry> geometry, std::shared_ptr<const std::vector<MaterialPreviewMaterial>> materials);
    void SetMaterials(std::shared_ptr<const std::vector<MaterialPreviewMaterial>> materials);
    // The slot whose outline the frames show (-1 none). Redraws the last frame's outline at once.
    void SetHighlightSlot(int slot);

    // Once a frame on the editor's thread: takes in a finished pass, and starts the next one when
    // there is more to render. width x height: the full image, in pixels.
    void Update(uint32_t width, uint32_t height, const MaterialPreviewCamera& camera, const MaterialPreviewSettings& settings, bool interacting);
    // True once after a pass finished an image: Frame() holds it.
    bool TakeNewFrame();
    const MaterialPreviewFrame& Frame() const
    {
        return m_frame;
    }
    // The slot under a point of the last frame, u and v in [0, 1]; -1 for none.
    int SlotAt(float u, float v) const;

    // Whether a pass is in flight or more passes are due.
    bool IsRendering() const;
    uint32_t Samples() const
    {
        return m_samples;
    }
    // Milliseconds the last finished pass took, start to finish.
    double LastPassMilliseconds() const
    {
        return m_lastPassMilliseconds;
    }
    // Renders until the target samples, waiting (tests and scripted captures).
    void Finish(uint32_t width, uint32_t height, const MaterialPreviewCamera& camera, const MaterialPreviewSettings& settings);

  private:
    struct PassInput;
    class PassTask;

    void FinishPass();
    void StartPass(uint32_t width, uint32_t height, const MaterialPreviewCamera& camera, const MaterialPreviewSettings& settings, bool interacting, bool reset);
    void ComposeFrame();

    std::shared_ptr<const MaterialPreviewGeometry> m_geometry;
    std::shared_ptr<const std::vector<MaterialPreviewMaterial>> m_materials;
    int m_highlightSlot = -1;

    // What the last pass rendered, to tell when to start over.
    struct Key
    {
        const void* geometry = nullptr;
        const void* materials = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        bool interacting = false;
        MaterialPreviewCamera camera;
        MaterialPreviewSettings settings;
        bool operator==(const Key&) const = default;
    };
    Key m_key;
    bool m_hasKey = false;

    std::unique_ptr<PassTask> m_task;
    std::chrono::steady_clock::time_point m_passStart{};
    double m_lastPassMilliseconds = 0.0;
    uint32_t m_samples = 0;
    uint32_t m_targetSamples = 0;
    bool m_newFrame = false;

    // The images the passes write: the running sum, each pixel's slot, and the tone mapped colour.
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    std::vector<glm::vec3> m_accumulation;
    std::vector<int32_t> m_slots;
    std::vector<uint8_t> m_display;
    MaterialPreviewFrame m_frame;
};
}
