#pragma once

#include "gl_functions.h"
#include "imgui_layer.h"

#include <editor_backend_base.h>
#include <texture_loader.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class Window;

struct OpenGLTextureBindings
{
    GLuint baseColor = 0;
    GLuint normal = 0;
    GLuint metallic = 0;
    GLuint roughness = 0;
    GLuint occlusion = 0;
    GLuint emissive = 0;
};

struct OpenGLMesh
{
    GLuint vao = 0;
    GLuint vertexBuffer = 0;
    GLuint indexBuffer = 0;
    GLsizei indexCount = 0;
};

struct OpenGLRenderSubmesh
{
    entt::entity entity = entt::null;
    OpenGLMesh mesh;
    OpenGLTextureBindings textures;
    MaterialPushConstants material;
    std::string name;
};

struct OpenGLSceneViewport
{
    GLuint framebuffer = 0;
    GLuint colorTexture = 0;
    GLuint depthStencilRenderbuffer = 0;
    RenderExtent extent{ 1, 1 };
};

class OpenGLRenderer : public EditorRenderBackendBase
{
public:
    OpenGLRenderer(
        Window& window,
        std::shared_ptr<RendererSharedState> sharedState,
        std::optional<std::string> startupModelPath = std::nullopt
    );
    ~OpenGLRenderer();

    OpenGLRenderer(const OpenGLRenderer&) = delete;
    OpenGLRenderer& operator=(const OpenGLRenderer&) = delete;

    void DrawFrame() override;

protected:
    void HandleBackendEvent(const SDL_Event& event) override;
    bool WantsKeyboardCapture() const override;

private:
    struct UniformLocations
    {
        GLint view = -1;
        GLint projection = -1;
        GLint model = -1;
        GLint baseColorFactor = -1;
        GLint emissiveFactor = -1;
        GLint surfaceFactors = -1;
        GLint cameraWorldPosition = -1;
        GLint lightDirectionAndIntensity = -1;
        GLint lightColorAndAmbient = -1;
    };

    void InitializeOpenGLState();
    void DestroyOpenGLState();
    void CreateShaderProgram();
    void DestroyShaderProgram();
    void UploadSceneResources();
    void DestroySceneResources();
    void SyncSceneViewport();
    void RenderScene();
    GLuint CreateTextureFromData(const TextureData& textureData, bool srgb) const;
    GLuint LoadTexture(const std::string& path, bool srgb);
    OpenGLMesh CreateMesh(const MeshData& meshData);
    void DestroyMesh(OpenGLMesh& mesh) const;
    void DestroyViewport();
    void CreateViewport(RenderExtent extent);

    SDL_GLContext m_glContext = nullptr;
    OpenGLFunctions m_gl;
    std::unique_ptr<OpenGLImGuiLayer> m_imguiLayer;
    GLuint m_program = 0;
    UniformLocations m_uniforms{};
    OpenGLSceneViewport m_sceneViewport;
    std::vector<OpenGLRenderSubmesh> m_renderSubmeshes;
    std::unordered_map<std::string, GLuint> m_textureCache;
};
