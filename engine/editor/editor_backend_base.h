#pragma once

#include "renderer_shared_state.h"

#include <rhi/backend.h>

#include <memory>
#include <optional>
#include <string>

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

    Window& m_window;
    std::shared_ptr<RendererSharedState> m_sharedState;
    RenderBackendType m_backendType = RenderBackendType::Vulkan;
};
