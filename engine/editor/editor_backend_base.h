#pragma once

#include "renderer_shared_state.h"

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

  protected:
    EditorRenderBackendBase(
        Window& window,
        std::shared_ptr<RendererSharedState> sharedState,
        RenderBackendType backendType,
        std::optional<std::string> startupModelPath = std::nullopt);

    bool TickSharedFrame();
    bool ProcessPendingOperations();
    void ApplyUiActions(const EditorUiFrameResult& uiFrame);
    void UpdateViewportMatrices(RenderExtent extent);
    EditorUiFrameResult DrawEditorUi(ImTextureID viewportTextureId, RenderExtent viewportExtent);
    bool HasDrawableArea() const;

    RendererSharedState& State();
    const RendererSharedState& State() const;
    IEditorWorld& EditorWorld();
    const IEditorWorld& EditorWorld() const;
    RendererWorld& RenderWorld();
    const RendererWorld& RenderWorld() const;
    Window& GetWindow() const;

    virtual void HandleBackendEvent(const SDL_Event& event) = 0;
    virtual bool WantsKeyboardCapture() const = 0;

  private:
    static void UpdateCameraFromInput(Camera& camera, const InputState& input, float deltaTime, bool blockKeyboardInput);
    void EnsureInitialized(std::optional<std::string> startupModelPath);
    void InitializeEditorScene();
    void SaveEngineSettings();

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

    Window& m_window;
    std::shared_ptr<RendererSharedState> m_sharedState;
    RenderBackendType m_backendType = RenderBackendType::Vulkan;
};
}
