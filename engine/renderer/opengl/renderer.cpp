#include "renderer.h"

#include <imgui.h>
#include <log/log.h>
#include <texture_loader.h>
#include <window/window.h>

#include <algorithm>
#include <cstddef>
#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace
{
TextureData CreateSolidTexture(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha)
{
    TextureData texture{};
    texture.width = 1;
    texture.height = 1;
    texture.channelCount = 4;
    texture.pixels = { red, green, blue, alpha };
    return texture;
}

TextureData CreateFlatNormalTexture()
{
    return CreateSolidTexture(128, 128, 255, 255);
}

const char* kVertexShaderSource = R"(
#version 330 core

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec4 inTangent;

uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uModel;

out vec3 fragColor;
out vec2 fragTexCoord;
out vec3 fragWorldNormal;
out vec4 fragWorldTangent;
out vec3 fragWorldPosition;

void main()
{
    vec4 worldPosition = uModel * vec4(inPosition, 1.0);
    mat3 normalMatrix = transpose(inverse(mat3(uModel)));
    vec3 worldTangent = normalize(normalMatrix * inTangent.xyz);

    gl_Position = uProjection * uView * worldPosition;
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    fragWorldNormal = normalize(normalMatrix * inNormal);
    fragWorldTangent = vec4(worldTangent, inTangent.w);
    fragWorldPosition = worldPosition.xyz;
}
)";

const char* kFragmentShaderSource = R"(
#version 330 core

uniform vec4 uBaseColorFactor;
uniform vec4 uEmissiveFactor;
uniform vec4 uSurfaceFactors;
uniform vec4 uCameraWorldPosition;
uniform vec4 uLightDirectionAndIntensity;
uniform vec4 uLightColorAndAmbient;

uniform sampler2D uBaseColorTexture;
uniform sampler2D uNormalTexture;
uniform sampler2D uMetallicTexture;
uniform sampler2D uRoughnessTexture;
uniform sampler2D uOcclusionTexture;
uniform sampler2D uEmissiveTexture;

in vec3 fragColor;
in vec2 fragTexCoord;
in vec3 fragWorldNormal;
in vec4 fragWorldTangent;
in vec3 fragWorldPosition;

out vec4 outColor;

const float PI = 3.14159265359;

float DistributionGGX(vec3 normal, vec3 halfVector, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float nDotH = max(dot(normal, halfVector), 0.0);
    float nDotHSquared = nDotH * nDotH;
    float denominator = nDotHSquared * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(PI * denominator * denominator, 0.0001);
}

float GeometrySchlickGGX(float nDotV, float roughness)
{
    float remappedRoughness = roughness + 1.0;
    float k = (remappedRoughness * remappedRoughness) / 8.0;
    float denominator = nDotV * (1.0 - k) + k;
    return nDotV / max(denominator, 0.0001);
}

float GeometrySmith(vec3 normal, vec3 viewDirection, vec3 lightDirection, float roughness)
{
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    float ggxView = GeometrySchlickGGX(nDotV, roughness);
    float ggxLight = GeometrySchlickGGX(nDotL, roughness);
    return ggxView * ggxLight;
}

vec3 FresnelSchlick(float cosineTheta, vec3 baseReflectivity)
{
    return baseReflectivity + (1.0 - baseReflectivity) * pow(1.0 - cosineTheta, 5.0);
}

void main()
{
    vec4 sampledBaseColor = texture(uBaseColorTexture, fragTexCoord);
    vec4 albedo = sampledBaseColor * vec4(fragColor, 1.0) * uBaseColorFactor;

    vec3 geometricNormal = normalize(fragWorldNormal);
    vec3 tangent = normalize(fragWorldTangent.xyz - geometricNormal * dot(geometricNormal, fragWorldTangent.xyz));
    vec3 bitangent = normalize(cross(geometricNormal, tangent) * fragWorldTangent.w);
    mat3 tbn = mat3(tangent, bitangent, geometricNormal);

    vec3 sampledNormal = texture(uNormalTexture, fragTexCoord).xyz * 2.0 - 1.0;
    sampledNormal.xy *= uSurfaceFactors.z;
    vec3 normal = normalize(tbn * sampledNormal);

    float metallicSample = texture(uMetallicTexture, fragTexCoord).b;
    float roughnessSample = texture(uRoughnessTexture, fragTexCoord).g;
    float ambientOcclusionSample = texture(uOcclusionTexture, fragTexCoord).r;
    vec3 emissiveSample = texture(uEmissiveTexture, fragTexCoord).rgb;

    float metallic = clamp(uSurfaceFactors.x * metallicSample, 0.0, 1.0);
    float roughness = clamp(uSurfaceFactors.y * roughnessSample, 0.04, 1.0);
    float ambientOcclusion = mix(1.0, ambientOcclusionSample, clamp(uSurfaceFactors.w, 0.0, 1.0));

    vec3 viewDirection = normalize(uCameraWorldPosition.xyz - fragWorldPosition);
    vec3 lightDirection = normalize(-uLightDirectionAndIntensity.xyz);
    vec3 halfVector = normalize(viewDirection + lightDirection);
    vec3 radiance = uLightColorAndAmbient.rgb * uLightDirectionAndIntensity.w;

    float nDotL = max(dot(normal, lightDirection), 0.0);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float hDotV = max(dot(halfVector, viewDirection), 0.0);

    vec3 baseReflectivity = mix(vec3(0.04), albedo.rgb, metallic);
    vec3 fresnel = FresnelSchlick(hDotV, baseReflectivity);
    float distribution = DistributionGGX(normal, halfVector, roughness);
    float geometry = GeometrySmith(normal, viewDirection, lightDirection, roughness);

    vec3 numerator = distribution * geometry * fresnel;
    float denominator = max(4.0 * nDotV * nDotL, 0.0001);
    vec3 specular = numerator / denominator;

    vec3 specularRatio = fresnel;
    vec3 diffuseRatio = (vec3(1.0) - specularRatio) * (1.0 - metallic);
    vec3 diffuse = diffuseRatio * albedo.rgb / PI;

    vec3 ambient = albedo.rgb * uLightColorAndAmbient.w * ambientOcclusion;
    vec3 directLighting = (diffuse + specular) * radiance * nDotL;
    vec3 emissive = emissiveSample * uEmissiveFactor.rgb;

    vec3 color = ambient + directLighting + emissive;
    color = color / (color + vec3(1.0));

    outColor = vec4(color, albedo.a);
}
)";

GLuint CompileShader(OpenGLFunctions& gl, GLenum shaderType, const char* source)
{
    const GLuint shader = gl.CreateShader(shaderType);
    gl.ShaderSource(shader, 1, &source, nullptr);
    gl.CompileShader(shader);

    GLint compileStatus = GL_FALSE;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);
    if (compileStatus == GL_FALSE)
    {
        GLint infoLogLength = 0;
        gl.GetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLogLength);
        std::string infoLog(static_cast<size_t>(std::max(infoLogLength, 1)), '\0');
        gl.GetShaderInfoLog(shader, infoLogLength, nullptr, infoLog.data());
        gl.DeleteShader(shader);
        throw std::runtime_error("Failed to compile OpenGL shader: " + infoLog);
    }

    return shader;
}

GLuint LinkProgram(OpenGLFunctions& gl, GLuint vertexShader, GLuint fragmentShader)
{
    const GLuint program = gl.CreateProgram();
    gl.AttachShader(program, vertexShader);
    gl.AttachShader(program, fragmentShader);
    gl.LinkProgram(program);

    GLint linkStatus = GL_FALSE;
    gl.GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    if (linkStatus == GL_FALSE)
    {
        GLint infoLogLength = 0;
        gl.GetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLogLength);
        std::string infoLog(static_cast<size_t>(std::max(infoLogLength, 1)), '\0');
        gl.GetProgramInfoLog(program, infoLogLength, nullptr, infoLog.data());
        gl.DeleteProgram(program);
        throw std::runtime_error("Failed to link OpenGL program: " + infoLog);
    }

    return program;
}
}

OpenGLRenderer::OpenGLRenderer(
    Window& window,
    std::shared_ptr<RendererSharedState> sharedState,
    std::optional<std::string> startupModelPath
)
    : EditorRenderBackendBase(window, std::move(sharedState), RenderBackendType::OpenGL, std::move(startupModelPath))
{
    InitializeOpenGLState();
    UploadSceneResources();
}

OpenGLRenderer::~OpenGLRenderer()
{
    DestroyOpenGLState();
}

void OpenGLRenderer::DrawFrame()
{
    SDL_GL_MakeCurrent(GetWindow().GetSDLWindow(), m_glContext);

    if (!TickSharedFrame())
    {
        return;
    }

    if (ProcessPendingOperations())
    {
        UploadSceneResources();
    }

    SyncSceneViewport();
    UpdateViewportMatrices(m_sceneViewport.extent);
    RenderScene();

    m_imguiLayer->BeginFrame();
    State().editorUi.BeginFrame(GetWindow().GetSDLWindow());
    const EditorUiFrameResult uiFrame = DrawEditorUi(
        (ImTextureID)(intptr_t)m_sceneViewport.colorTexture,
        m_sceneViewport.extent
    );
    ApplyUiActions(uiFrame);
    ImGui::Render();

    int windowWidth = 0;
    int windowHeight = 0;
    if (!SDL_GetWindowSizeInPixels(GetWindow().GetSDLWindow(), &windowWidth, &windowHeight))
    {
        throw std::runtime_error(std::string("SDL_GetWindowSizeInPixels failed: ") + SDL_GetError());
    }

    m_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, windowWidth, windowHeight);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.04f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    m_imguiLayer->RenderDrawData();
    SDL_GL_SwapWindow(GetWindow().GetSDLWindow());
}

void OpenGLRenderer::HandleBackendEvent(const SDL_Event& event)
{
    m_imguiLayer->ProcessEvent(event);
}

bool OpenGLRenderer::WantsKeyboardCapture() const
{
    return m_imguiLayer->WantsKeyboardCapture();
}

void OpenGLRenderer::InitializeOpenGLState()
{
    m_glContext = SDL_GL_CreateContext(GetWindow().GetSDLWindow());
    if (m_glContext == nullptr)
    {
        throw std::runtime_error(std::string("SDL_GL_CreateContext failed: ") + SDL_GetError());
    }

    if (!SDL_GL_MakeCurrent(GetWindow().GetSDLWindow(), m_glContext))
    {
        throw std::runtime_error(std::string("SDL_GL_MakeCurrent failed: ") + SDL_GetError());
    }

    SDL_GL_SetSwapInterval(1);
    m_gl.Load();
    CreateShaderProgram();
    m_imguiLayer = std::make_unique<OpenGLImGuiLayer>(GetWindow().GetSDLWindow(), m_glContext);

    if (!State().requestedViewportExtent.IsValid())
    {
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(GetWindow().GetSDLWindow(), &width, &height))
        {
            throw std::runtime_error(std::string("SDL_GetWindowSizeInPixels failed: ") + SDL_GetError());
        }
        State().requestedViewportExtent = RenderExtent{
            static_cast<uint32_t>(std::max(width, 1)),
            static_cast<uint32_t>(std::max(height, 1))
        };
    }
    CreateViewport(State().requestedViewportExtent);
}

void OpenGLRenderer::DestroyOpenGLState()
{
    if (m_glContext != nullptr)
    {
        SDL_GL_MakeCurrent(GetWindow().GetSDLWindow(), m_glContext);
    }

    m_imguiLayer.reset();
    DestroySceneResources();
    DestroyViewport();
    DestroyShaderProgram();

    if (m_glContext != nullptr)
    {
        SDL_GL_DestroyContext(m_glContext);
        m_glContext = nullptr;
    }
}

void OpenGLRenderer::CreateShaderProgram()
{
    const GLuint vertexShader = CompileShader(m_gl, GL_VERTEX_SHADER, kVertexShaderSource);
    const GLuint fragmentShader = CompileShader(m_gl, GL_FRAGMENT_SHADER, kFragmentShaderSource);
    m_program = LinkProgram(m_gl, vertexShader, fragmentShader);
    m_gl.DeleteShader(vertexShader);
    m_gl.DeleteShader(fragmentShader);

    m_uniforms.view = m_gl.GetUniformLocation(m_program, "uView");
    m_uniforms.projection = m_gl.GetUniformLocation(m_program, "uProjection");
    m_uniforms.model = m_gl.GetUniformLocation(m_program, "uModel");
    m_uniforms.baseColorFactor = m_gl.GetUniformLocation(m_program, "uBaseColorFactor");
    m_uniforms.emissiveFactor = m_gl.GetUniformLocation(m_program, "uEmissiveFactor");
    m_uniforms.surfaceFactors = m_gl.GetUniformLocation(m_program, "uSurfaceFactors");
    m_uniforms.cameraWorldPosition = m_gl.GetUniformLocation(m_program, "uCameraWorldPosition");
    m_uniforms.lightDirectionAndIntensity = m_gl.GetUniformLocation(m_program, "uLightDirectionAndIntensity");
    m_uniforms.lightColorAndAmbient = m_gl.GetUniformLocation(m_program, "uLightColorAndAmbient");

    m_gl.UseProgram(m_program);
    m_gl.Uniform1i(m_gl.GetUniformLocation(m_program, "uBaseColorTexture"), 0);
    m_gl.Uniform1i(m_gl.GetUniformLocation(m_program, "uNormalTexture"), 1);
    m_gl.Uniform1i(m_gl.GetUniformLocation(m_program, "uMetallicTexture"), 2);
    m_gl.Uniform1i(m_gl.GetUniformLocation(m_program, "uRoughnessTexture"), 3);
    m_gl.Uniform1i(m_gl.GetUniformLocation(m_program, "uOcclusionTexture"), 4);
    m_gl.Uniform1i(m_gl.GetUniformLocation(m_program, "uEmissiveTexture"), 5);
    m_gl.UseProgram(0);
}

void OpenGLRenderer::DestroyShaderProgram()
{
    if (m_program != 0)
    {
        m_gl.DeleteProgram(m_program);
        m_program = 0;
    }
}

void OpenGLRenderer::UploadSceneResources()
{
    DestroySceneResources();

    const GLuint defaultBaseColor = CreateTextureFromData(CreateSolidTexture(255, 255, 255, 255), true);
    const GLuint defaultNormal = CreateTextureFromData(CreateFlatNormalTexture(), false);
    const GLuint defaultLinear = CreateTextureFromData(CreateSolidTexture(255, 255, 255, 255), false);
    const GLuint defaultEmissive = CreateTextureFromData(CreateSolidTexture(255, 255, 255, 255), true);
    m_textureCache.emplace("__default_base_color__", defaultBaseColor);
    m_textureCache.emplace("__default_normal__", defaultNormal);
    m_textureCache.emplace("__default_linear__", defaultLinear);
    m_textureCache.emplace("__default_emissive__", defaultEmissive);

    for (const CpuRenderSubmesh& cpuRenderSubmesh : State().renderSubmeshes)
    {
        OpenGLRenderSubmesh renderSubmesh{};
        renderSubmesh.entity = cpuRenderSubmesh.entity;
        renderSubmesh.mesh = CreateMesh(cpuRenderSubmesh.mesh);
        renderSubmesh.material = cpuRenderSubmesh.material;
        renderSubmesh.name = cpuRenderSubmesh.name;

        renderSubmesh.textures.baseColor = defaultBaseColor;
        renderSubmesh.textures.normal = defaultNormal;
        renderSubmesh.textures.metallic = defaultLinear;
        renderSubmesh.textures.roughness = defaultLinear;
        renderSubmesh.textures.occlusion = defaultLinear;
        renderSubmesh.textures.emissive = defaultEmissive;

        if (cpuRenderSubmesh.hasTexCoords)
        {
            try
            {
                if (!cpuRenderSubmesh.textures.baseColor.empty())
                {
                    renderSubmesh.textures.baseColor = LoadTexture(cpuRenderSubmesh.textures.baseColor, true);
                }
                if (!cpuRenderSubmesh.textures.normal.empty())
                {
                    renderSubmesh.textures.normal = LoadTexture(cpuRenderSubmesh.textures.normal, false);
                }
                if (!cpuRenderSubmesh.textures.metallic.empty())
                {
                    renderSubmesh.textures.metallic = LoadTexture(cpuRenderSubmesh.textures.metallic, false);
                }
                if (!cpuRenderSubmesh.textures.roughness.empty())
                {
                    renderSubmesh.textures.roughness = LoadTexture(cpuRenderSubmesh.textures.roughness, false);
                }
                if (!cpuRenderSubmesh.textures.occlusion.empty())
                {
                    renderSubmesh.textures.occlusion = LoadTexture(cpuRenderSubmesh.textures.occlusion, false);
                }
                if (!cpuRenderSubmesh.textures.emissive.empty())
                {
                    renderSubmesh.textures.emissive = LoadTexture(cpuRenderSubmesh.textures.emissive, true);
                }
            }
            catch (const std::exception& error)
            {
                LOG_ERROR("Failed to upload OpenGL texture for '{}': {}", cpuRenderSubmesh.name, error.what());
            }
        }

        m_renderSubmeshes.push_back(std::move(renderSubmesh));
    }
}

void OpenGLRenderer::DestroySceneResources()
{
    for (OpenGLRenderSubmesh& renderSubmesh : m_renderSubmeshes)
    {
        DestroyMesh(renderSubmesh.mesh);
    }
    m_renderSubmeshes.clear();

    for (const auto& [_, textureId] : m_textureCache)
    {
        if (textureId != 0)
        {
            glDeleteTextures(1, &textureId);
        }
    }
    m_textureCache.clear();
}

void OpenGLRenderer::SyncSceneViewport()
{
    if (!State().requestedViewportExtent.IsValid())
    {
        State().requestedViewportExtent = m_sceneViewport.extent;
    }

    if (m_sceneViewport.framebuffer != 0 &&
        m_sceneViewport.extent.width == State().requestedViewportExtent.width &&
        m_sceneViewport.extent.height == State().requestedViewportExtent.height)
    {
        return;
    }

    DestroyViewport();
    CreateViewport(State().requestedViewportExtent);
}

void OpenGLRenderer::RenderScene()
{
    m_gl.BindFramebuffer(GL_FRAMEBUFFER, m_sceneViewport.framebuffer);
    glViewport(0, 0, static_cast<GLsizei>(m_sceneViewport.extent.width), static_cast<GLsizei>(m_sceneViewport.extent.height));
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glClearColor(0.08f, 0.1f, 0.16f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    m_gl.UseProgram(m_program);
    m_gl.UniformMatrix4fv(m_uniforms.view, 1, GL_FALSE, glm::value_ptr(State().viewportMatrices.view));
    m_gl.UniformMatrix4fv(m_uniforms.projection, 1, GL_FALSE, glm::value_ptr(State().viewportMatrices.renderProjection));
    const glm::vec3 lightDirection = glm::normalize(glm::vec3(-0.6f, -1.0f, -0.35f));
    const glm::vec4 cameraWorldPosition(State().camera.position, 1.0f);
    const glm::vec4 lightDirectionAndIntensity(lightDirection, 2.25f);
    const glm::vec4 lightColorAndAmbient(1.0f, 0.98f, 0.95f, 0.2f);
    m_gl.Uniform4fv(m_uniforms.cameraWorldPosition, 1, glm::value_ptr(cameraWorldPosition));
    m_gl.Uniform4fv(m_uniforms.lightDirectionAndIntensity, 1, glm::value_ptr(lightDirectionAndIntensity));
    m_gl.Uniform4fv(m_uniforms.lightColorAndAmbient, 1, glm::value_ptr(lightColorAndAmbient));

    for (const OpenGLRenderSubmesh& renderSubmesh : m_renderSubmeshes)
    {
        const glm::mat4 modelMatrix = State().editorScene.GetModelMatrix(renderSubmesh.entity);
        m_gl.UniformMatrix4fv(m_uniforms.model, 1, GL_FALSE, glm::value_ptr(modelMatrix));
        m_gl.Uniform4fv(m_uniforms.baseColorFactor, 1, renderSubmesh.material.baseColorFactor);
        m_gl.Uniform4fv(m_uniforms.emissiveFactor, 1, renderSubmesh.material.emissiveFactor);
        m_gl.Uniform4fv(m_uniforms.surfaceFactors, 1, renderSubmesh.material.surfaceFactors);

        m_gl.ActiveTexture(GL_TEXTURE0 + 0);
        glBindTexture(GL_TEXTURE_2D, renderSubmesh.textures.baseColor);
        m_gl.ActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, renderSubmesh.textures.normal);
        m_gl.ActiveTexture(GL_TEXTURE0 + 2);
        glBindTexture(GL_TEXTURE_2D, renderSubmesh.textures.metallic);
        m_gl.ActiveTexture(GL_TEXTURE0 + 3);
        glBindTexture(GL_TEXTURE_2D, renderSubmesh.textures.roughness);
        m_gl.ActiveTexture(GL_TEXTURE0 + 4);
        glBindTexture(GL_TEXTURE_2D, renderSubmesh.textures.occlusion);
        m_gl.ActiveTexture(GL_TEXTURE0 + 5);
        glBindTexture(GL_TEXTURE_2D, renderSubmesh.textures.emissive);

        m_gl.BindVertexArray(renderSubmesh.mesh.vao);
        glDrawElements(GL_TRIANGLES, renderSubmesh.mesh.indexCount, GL_UNSIGNED_INT, nullptr);
    }

    m_gl.BindVertexArray(0);
    m_gl.UseProgram(0);
    m_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
}

GLuint OpenGLRenderer::CreateTextureFromData(const TextureData& textureData, bool srgb) const
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
        textureData.width,
        textureData.height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        textureData.pixels.data()
    );
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

GLuint OpenGLRenderer::LoadTexture(const std::string& path, bool srgb)
{
    const std::string cacheKey = path + (srgb ? "|srgb" : "|linear");
    if (const auto iterator = m_textureCache.find(cacheKey); iterator != m_textureCache.end())
    {
        return iterator->second;
    }

    const GLuint texture = CreateTextureFromData(TextureLoader::LoadRGBA8(path), srgb);
    m_textureCache.emplace(cacheKey, texture);
    return texture;
}

OpenGLMesh OpenGLRenderer::CreateMesh(const MeshData& meshData)
{
    OpenGLMesh mesh{};
    mesh.indexCount = static_cast<GLsizei>(meshData.indices.size());

    m_gl.GenVertexArrays(1, &mesh.vao);
    m_gl.GenBuffers(1, &mesh.vertexBuffer);
    m_gl.GenBuffers(1, &mesh.indexBuffer);

    m_gl.BindVertexArray(mesh.vao);
    m_gl.BindBuffer(GL_ARRAY_BUFFER, mesh.vertexBuffer);
    m_gl.BufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(sizeof(Vertex) * meshData.vertices.size()),
        meshData.vertices.data(),
        GL_STATIC_DRAW
    );
    m_gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.indexBuffer);
    m_gl.BufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(sizeof(uint32_t) * meshData.indices.size()),
        meshData.indices.data(),
        GL_STATIC_DRAW
    );

    m_gl.EnableVertexAttribArray(0);
    m_gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, position)));
    m_gl.EnableVertexAttribArray(1);
    m_gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, color)));
    m_gl.EnableVertexAttribArray(2);
    m_gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, texCoord)));
    m_gl.EnableVertexAttribArray(3);
    m_gl.VertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, normal)));
    m_gl.EnableVertexAttribArray(4);
    m_gl.VertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, tangent)));

    m_gl.BindVertexArray(0);
    return mesh;
}

void OpenGLRenderer::DestroyMesh(OpenGLMesh& mesh) const
{
    if (mesh.indexBuffer != 0)
    {
        m_gl.DeleteBuffers(1, &mesh.indexBuffer);
        mesh.indexBuffer = 0;
    }
    if (mesh.vertexBuffer != 0)
    {
        m_gl.DeleteBuffers(1, &mesh.vertexBuffer);
        mesh.vertexBuffer = 0;
    }
    if (mesh.vao != 0)
    {
        m_gl.DeleteVertexArrays(1, &mesh.vao);
        mesh.vao = 0;
    }
    mesh.indexCount = 0;
}

void OpenGLRenderer::DestroyViewport()
{
    if (m_sceneViewport.depthStencilRenderbuffer != 0)
    {
        m_gl.DeleteRenderbuffers(1, &m_sceneViewport.depthStencilRenderbuffer);
        m_sceneViewport.depthStencilRenderbuffer = 0;
    }
    if (m_sceneViewport.colorTexture != 0)
    {
        glDeleteTextures(1, &m_sceneViewport.colorTexture);
        m_sceneViewport.colorTexture = 0;
    }
    if (m_sceneViewport.framebuffer != 0)
    {
        m_gl.DeleteFramebuffers(1, &m_sceneViewport.framebuffer);
        m_sceneViewport.framebuffer = 0;
    }
}

void OpenGLRenderer::CreateViewport(RenderExtent extent)
{
    m_sceneViewport.extent = RenderExtent{
        std::max(extent.width, 1u),
        std::max(extent.height, 1u)
    };

    m_gl.GenFramebuffers(1, &m_sceneViewport.framebuffer);
    m_gl.BindFramebuffer(GL_FRAMEBUFFER, m_sceneViewport.framebuffer);

    glGenTextures(1, &m_sceneViewport.colorTexture);
    glBindTexture(GL_TEXTURE_2D, m_sceneViewport.colorTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        static_cast<GLsizei>(m_sceneViewport.extent.width),
        static_cast<GLsizei>(m_sceneViewport.extent.height),
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        nullptr
    );
    m_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_sceneViewport.colorTexture, 0);

    m_gl.GenRenderbuffers(1, &m_sceneViewport.depthStencilRenderbuffer);
    m_gl.BindRenderbuffer(GL_RENDERBUFFER, m_sceneViewport.depthStencilRenderbuffer);
    m_gl.RenderbufferStorage(
        GL_RENDERBUFFER,
        GL_DEPTH24_STENCIL8,
        static_cast<GLsizei>(m_sceneViewport.extent.width),
        static_cast<GLsizei>(m_sceneViewport.extent.height)
    );
    m_gl.FramebufferRenderbuffer(
        GL_FRAMEBUFFER,
        GL_DEPTH_STENCIL_ATTACHMENT,
        GL_RENDERBUFFER,
        m_sceneViewport.depthStencilRenderbuffer
    );

    if (m_gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        throw std::runtime_error("Failed to create OpenGL scene framebuffer");
    }

    m_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
}
