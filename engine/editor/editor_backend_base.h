#pragma once

#include "renderer_shared_state.h"

#include <engine/core/video/video_recorder.h>
#include <engine/renderer/rhi/backend.h>

#include <memory>
#include <optional>
#include <string>

namespace me
{

class Window;

class EditorRenderBackendBase : public IRenderBackend
{
  public:
    RenderBackendType GetBackendType() const override;
    void HandleEvent(const SDL_Event& event) override;
    bool StartVideoRecording(const VideoRecordingRequest& request, std::string& error) override;
    void StopVideoRecording() override;

  protected:
    EditorRenderBackendBase(
        Window& window,
        std::shared_ptr<RendererSharedState> sharedState,
        RenderBackendType backendType,
        std::optional<std::string> startupModelPath = std::nullopt);

    bool TickSharedFrame();
    bool ProcessPendingOperations();
    void ApplyUiActions(const EditorUiFrameResult& uiFrame);
    void CaptureViewportWithState();
    // The recording the backend reads frames back for, while one runs.
    VideoRecorder* ActiveVideoRecorder()
    {
        return m_videoRecorder.get();
    }
    // Hands the recorder every frame the backend still holds; called before the recording stops.
    virtual void FlushVideoFrames()
    {
    }
    void UpdateViewportMatrices(RenderExtent extent);
    EditorUiFrameResult DrawEditorUi(ImTextureID viewportTextureId, RenderExtent viewportExtent);
    bool HasDrawableArea() const;

    RendererSharedState& State();
    const RendererSharedState& State() const;
    IEditorWorld& EditorWorld();
    RendererWorld& RenderWorld();
    Window& GetWindow() const;

    virtual void HandleBackendEvent(const SDL_Event& event) = 0;
    virtual bool WantsKeyboardCapture() const = 0;

  private:
    // Right mouse looks around; with Alt held it orbits `orbitPivot` instead, when there is one.
    static void UpdateCameraFromInput(
        Camera& camera,
        const InputState& input,
        float deltaTime,
        bool blockKeyboardInput,
        const std::optional<glm::vec3>& orbitPivot);
    // The centre of the selected entity's bounds, or where it is when it has none; nothing without a selection.
    std::optional<glm::vec3> SelectionOrbitPivot();
    void EnsureInitialized(std::optional<std::string> startupModelPath);
    void InitializeEditorScene();
    void SaveEngineSettings();
    // Tools > Record Viewport: starts a recording to captures/recording_<date>_<time>.mp4 (.avi where
    // there is no Media Foundation), or stops
    // the one running.
    void ToggleVideoRecordingFromEditor();
    // Every frame: stops a recording whose file could not be written, and updates what the viewport
    // shows of the one running.
    void UpdateVideoRecording();

    // The bounds and aspect ratio the Khronos reference view last framed; it reframes when either
    // changes (a scene finishing loading, the viewport resizing) and leaves the camera to the user
    // in between.
    struct KhronosReferenceFraming
    {
        glm::vec3 minBounds{0.0f};
        glm::vec3 maxBounds{0.0f};
        float aspectRatio = 1.0f;
        bool operator==(const KhronosReferenceFraming&) const = default;
    };
    void UpdateKhronosReferenceFraming(RenderExtent extent);
    std::optional<KhronosReferenceFraming> m_khronosReferenceFraming;

    std::unique_ptr<VideoRecorder> m_videoRecorder;
    // The fixed viewport size before the recording fixed it, put back when it stops.
    std::optional<RenderExtent> m_fixedViewportExtentBeforeRecording;

    Window& m_window;
    std::shared_ptr<RendererSharedState> m_sharedState;
    RenderBackendType m_backendType = RenderBackendType::Vulkan;
};
}
