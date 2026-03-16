#pragma once

#include "model_loader.h"
#include "renderer_shared_state.h"

#include <render_backend.h>

#include <memory>
#include <optional>
#include <string>

class Window;

class EditorRenderBackendBase : public IRenderBackend
{
public:
    RenderBackendType GetBackendType() const override;
    void HandleEvent(const SDL_Event& event) override;
    std::optional<RenderBackendType> ConsumeBackendSwitchRequest() override;

protected:
    EditorRenderBackendBase(
        Window& window,
        std::shared_ptr<RendererSharedState> sharedState,
        RenderBackendType backendType,
        std::optional<std::string> startupModelPath = std::nullopt
    );

    bool TickSharedFrame();
    bool ProcessPendingOperations();
    void ApplyUiActions(const EditorUiFrameResult& uiFrame);
    void UpdateViewportMatrices(RenderExtent extent);
    EditorUiFrameResult DrawEditorUi(ImTextureID viewportTextureId, RenderExtent viewportExtent);
    bool HasDrawableArea() const;

    RendererSharedState& State();
    const RendererSharedState& State() const;
    Window& GetWindow() const;

    virtual void HandleBackendEvent(const SDL_Event& event) = 0;
    virtual bool WantsKeyboardCapture() const = 0;

private:
    static void UpdateCameraFromInput(Camera& camera, const InputState& input, float deltaTime, bool blockKeyboardInput);
    void EnsureInitialized(std::optional<std::string> startupModelPath);
    void InitializeEditorScene();
    void LoadSelectedModel(const std::string& path, bool resetTransform = true);
    void LoadScene(const std::string& path);
    void RebuildSceneRenderables();

    Window& m_window;
    std::shared_ptr<RendererSharedState> m_sharedState;
    RenderBackendType m_backendType = RenderBackendType::Vulkan;
};
