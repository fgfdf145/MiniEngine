#pragma once

#include "common.h"

#include <SDL3/SDL.h>

#include <nvrhi/nvrhi.h>

#include <memory>
#include <string>

// ImGui's own type, declared in the global namespace like the rest of ImGui.
struct ImDrawData;

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

    // The render side.
    ImGuiNvrhiRenderer& GetRenderer() const;

  private:
    SDL_Window* m_window = nullptr;
    std::unique_ptr<ImGuiNvrhiRenderer> m_renderer;
    std::string m_iniFilePath;
};
}
