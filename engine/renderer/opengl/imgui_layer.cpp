#include "imgui_layer.h"

#include "../imgui/imgui_impl_opengl3.h"
#include "../imgui/imgui_impl_sdl3.h"

#include <imgui.h>
#include <filesystem>
#include <stdexcept>

namespace
{
std::string BuildImGuiIniPath()
{
    return (std::filesystem::path(MINIENGINE_PROJECT_DIR) / "imgui.ini").string();
}
}

OpenGLImGuiLayer::OpenGLImGuiLayer(SDL_Window* window, SDL_GLContext glContext)
    : m_window(window),
      m_iniFilePath(BuildImGuiIniPath())
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = m_iniFilePath.c_str();
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForOpenGL(window, glContext))
    {
        throw std::runtime_error("Failed to initialize ImGui SDL3 OpenGL backend");
    }
    if (!ImGui_ImplOpenGL3_Init("#version 330"))
    {
        throw std::runtime_error("Failed to initialize ImGui OpenGL backend");
    }
}

OpenGLImGuiLayer::~OpenGLImGuiLayer()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void OpenGLImGuiLayer::ProcessEvent(const SDL_Event& event)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void OpenGLImGuiLayer::BeginFrame()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void OpenGLImGuiLayer::RenderDrawData() const
{
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool OpenGLImGuiLayer::WantsKeyboardCapture() const
{
    return ImGui::GetIO().WantCaptureKeyboard;
}

bool OpenGLImGuiLayer::WantsMouseCapture() const
{
    return ImGui::GetIO().WantCaptureMouse;
}
