#pragma once

#include "renderer_shared_state.h"
#include "services/photo_mode.h"
#include "services/quad_recording.h"

#include <engine/core/video/video_mosaic.h>
#include <engine/core/video/video_recorder.h>
#include <engine/renderer/rhi/backend.h>
#include <engine/renderer/scene_capture_view.h>

#include <array>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace me
{

class Window;

// Photo Mode's view as the frame drawn last left it: its tone mapped picture (RGBA8, rows from the
// top) and the exposure it was drawn at.
struct PhotoViewPicture
{
    std::vector<uint8_t> rgba;
    uint32_t width = 0;
    uint32_t height = 0;
    float exposureEv100 = 0.0f;
};

class EditorRenderBackendBase : public IRenderBackend
{
  public:
    RenderBackendType GetBackendType() const override;
    void HandleEvent(const SDL_Event& event) override;
    bool StartVideoRecording(const VideoRecordingRequest& request, std::string& error) override;
    void StopVideoRecording() override;
    bool StartQuadRecording(const VideoRecordingRequest& request, std::string& error) override;
    void StopQuadRecording() override;
    bool TakePhoto(const PhotoRequest& request, std::string& error) override;
    bool IsTakingPhoto() const override
    {
        return m_photo.has_value();
    }
    void WaitForPhotoWrite() override;

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
    // A quad recording while it runs (docs/design/2026-10-07-quad-vehicle-recording-design.md): its
    // recorder, the canvas its cameras' pictures go into, and the names written on them (empty when
    // the labels are off). Fixed from start to stop, as the settings it started with.
    struct QuadVideoRecording
    {
        std::unique_ptr<VideoRecorder> recorder;
        VideoMosaic mosaic;
        std::array<std::string, kQuadCameraCount> labels;
        QuadRecordingSettings settings;
    };
    const QuadVideoRecording* ActiveQuadRecording() const
    {
        return m_quadRecording.get();
    }
    // As FlushVideoFrames, for the quad recording.
    virtual void FlushQuadVideoFrames()
    {
    }
    // The photo view's picture of the frame drawn last (SceneCaptureView::photo), once the render
    // thread has finished it. Throws when there is none.
    virtual PhotoViewPicture ReadPhotoView()
    {
        throw std::runtime_error("This render backend cannot take photos");
    }
    // This frame's quad cameras, in the canvas's order (UpdateCaptureViews): while a quad recording
    // runs or the Quad Recording window previews them, and there is something to follow; else none.
    const std::vector<SceneCaptureView>& CaptureViews() const
    {
        return m_captureViews;
    }
    // Runs work while the backend's render thread, if it has one, waits with nothing in hand: for
    // what the render thread reads (the video recorder) or uses (the device) while it draws.
    virtual void RunWithRenderIdle(const std::function<void()>& work)
    {
        work();
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
    bool StartVideoRecordingNow(const VideoRecordingRequest& request, std::string& error);
    void StopVideoRecordingNow();
    // Every frame: stops a recording whose file could not be written, and updates what the viewport
    // shows of the one running.
    void UpdateVideoRecording();
    // Tools > Record Quad Cameras: starts a quad recording to captures/quad_<date>_<time>.mp4 (.avi
    // where there is no Media Foundation), or stops the one running.
    void ToggleQuadRecordingFromEditor();
    bool StartQuadRecordingNow(const VideoRecordingRequest& request, std::string& error);
    void StopQuadRecordingNow();
    void UpdateQuadRecording();
    // Tools > Take Photo: a photo at the Photo Mode window's settings to
    // captures/photo_<date>_<time>.png.
    void TakePhotoFromEditor();
    // Before this frame's views are placed: takes the photo view's picture once it has rendered its
    // warm-up frames (into the canvas, a tile at a time) and moves to the next tile; once the last is
    // in, writes the PNG on a worker thread and, when that is done, reports how the photo ended in
    // State().photoStatus.
    void AdvancePhoto();
    // The photo's view for this frame, while it renders.
    std::optional<SceneCaptureView> PlacePhotoView();
    // What the quad cameras follow this frame: the driven car's body, else the selected model; with
    // its name. Nothing when there is neither.
    struct QuadRecordingTarget
    {
        PhysicsPose pose;
        std::string name;
    };
    std::optional<QuadRecordingTarget> FindQuadRecordingTarget();
    // Places the quad cameras for this frame, after the UI (whose settings they take) and the drive
    // (whose pose they follow).
    void UpdateCaptureViews();
    // The Assets window's sound preview: plays the file, or stops it when it is the one playing.
    void PreviewAudio(const std::string& path);

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
    std::unique_ptr<QuadVideoRecording> m_quadRecording;
    std::vector<SceneCaptureView> m_captureViews;
    // The photo being made: what was asked; the viewport's camera and aspect when it was, which every
    // tile keeps; how it is cut into tiles, the tile rendering and how many frames have named its
    // view; the canvas the tiles go into; and, once they are all in, the PNG being written (its
    // error, empty when it was written).
    struct PhotoInProgress
    {
        PhotoRequest request;
        Camera camera;
        float viewportAspect = 1.0f;
        PhotoTiling tiling;
        size_t tile = 0;
        uint32_t tileFrames = 0;
        std::vector<uint8_t> canvas;
        float exposureEv100 = 0.0f;
        std::optional<std::future<std::string>> writing;
    };
    std::optional<PhotoInProgress> m_photo;
    // The viewport's width over its height as the scene last rendered it (UpdateViewportMatrices),
    // which the photo frames inside.
    float m_viewportAspect = 16.0f / 9.0f;
    // The fixed viewport size before the recording fixed it, put back when it stops.
    std::optional<RenderExtent> m_fixedViewportExtentBeforeRecording;

    Window& m_window;
    std::shared_ptr<RendererSharedState> m_sharedState;
    RenderBackendType m_backendType = RenderBackendType::Vulkan;
};
}
