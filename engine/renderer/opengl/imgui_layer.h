#pragma once

#include <SDL3/SDL.h>

struct ImDrawData;

class OpenGLImGuiLayer
{
public:
    OpenGLImGuiLayer(SDL_Window* window, SDL_GLContext glContext);
    ~OpenGLImGuiLayer();

    OpenGLImGuiLayer(const OpenGLImGuiLayer&) = delete;
    OpenGLImGuiLayer& operator=(const OpenGLImGuiLayer&) = delete;

    void ProcessEvent(const SDL_Event& event);
    void BeginFrame();
    void RenderDrawData() const;
    bool WantsKeyboardCapture() const;
    bool WantsMouseCapture() const;

private:
    SDL_Window* m_window = nullptr;
};
