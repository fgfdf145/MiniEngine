#pragma once

#include "model_loader.h"
#include "renderer_shared_state.h"

#include <rhi/backend.h>

#include <entt/entt.hpp>
#include <glm/vec3.hpp>

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
    std::string ImportModelIntoAssetDirectory(const std::string& sourcePath);
    void DeleteAssetPath(const std::string& path);
    void LoadSelectedModel(const std::string& path, bool resetTransform = true);
    void PlaceModelIntoScene(const std::string& path, const glm::vec3& worldPosition);
    void UpdateViewportModelPreview(const EditorUiActions::ViewportModelPlacement& placement);
    void CommitViewportModelPreview(const EditorUiActions::ViewportModelPlacement& placement);
    void ClearViewportModelPreview(bool restoreSelection = true);
    void UpdateImportedMaterialDefinition(const EditorUiActions::ImportedMaterialUpdate& update);
    void UpdateImportedModelMaterialDefinitions(const EditorUiActions::ImportedModelMaterialsUpdate& update);
    void ApplySelectedModelBaseColorTexture(const std::string& path);
    void ClearSelectedModelBaseColorTexture();
    void CreateSceneEntity();
    void DeleteSelectedSceneEntity();
    void LoadScene(const std::string& path);
    void PasteAssetPath(const EditorUiActions::AssetPasteRequest& request);
    void SaveEngineSettings();
    void RebuildSceneRenderables();

    Window& m_window;
    std::shared_ptr<RendererSharedState> m_sharedState;
    RenderBackendType m_backendType = RenderBackendType::Vulkan;
};
