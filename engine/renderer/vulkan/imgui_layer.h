#pragma once

#include "common.h"

#include <SDL3/SDL.h>

#include <nvrhi/nvrhi.h>

#include <memory>
#include <string>

// ImGui's own type, declared in the global namespace like the rest of ImGui.
struct ImDrawData;
struct ImGuiTestEngine;

namespace me
{

class ImGuiNvrhiRenderer;

// The ImGui context, its SDL3 platform backend and its renderer (ImGuiNvrhiRenderer).
class VulkanImGuiLayer
{
  public:
    VulkanImGuiLayer(SDL_Window* window, nvrhi::IDevice* device, uint32_t frameSlots);
    ~VulkanImGuiLayer();

    VulkanImGuiLayer(const VulkanImGuiLayer&) = delete;
    VulkanImGuiLayer& operator=(const VulkanImGuiLayer&) = delete;

    // The main thread's side: events and the frame's UI.
    void ProcessEvent(const SDL_Event& event);
    void BeginFrame();
    ImDrawData* GetDrawData() const;
    bool WantsKeyboardCapture() const;
    bool WantsMouseCapture() const;
    // After the frame's ImGui::Render: lets a running UI test (the test engine) go on to its next step.
    void EndFrame();
    // Dear ImGui Test Engine on this context, made the first time it is asked for (the control
    // channel's ui.* commands); null where ImGui was built without it.
    ImGuiTestEngine* GetTestEngine();

    // The render side.
    ImGuiNvrhiRenderer& GetRenderer() const;

  private:
    SDL_Window* m_window = nullptr;
    std::unique_ptr<ImGuiNvrhiRenderer> m_renderer;
    std::string m_iniFilePath;
    ImGuiTestEngine* m_testEngine = nullptr;
};
}
