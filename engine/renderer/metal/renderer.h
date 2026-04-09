#pragma once

#include <editor_backend_base.h>

#include <memory>
#include <optional>
#include <string>

class Window;

class MetalRenderer : public EditorRenderBackendBase
{
public:
    MetalRenderer(
        Window& window,
        std::shared_ptr<RendererSharedState> sharedState,
        std::optional<std::string> startupModelPath = std::nullopt
    );
    ~MetalRenderer() override;

    MetalRenderer(const MetalRenderer&) = delete;
    MetalRenderer& operator=(const MetalRenderer&) = delete;

    void DrawFrame() override;

protected:
    void HandleBackendEvent(const SDL_Event& event) override;
    bool WantsKeyboardCapture() const override;

private:
    void CreateScenePipeline();
    void EnsureSceneViewportResources(RenderExtent extent);
    void UploadSceneResources();
    ImTextureID GetViewportTextureId() const;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
