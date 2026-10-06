#include <engine/core/input/input.h>

#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
constexpr Uint64 kMillisecondNs = 1'000'000;

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

InputState ViewportInput()
{
    InputState input;
    input.SetViewportInteractionRegion(SDL_FRect{0.0f, 0.0f, 800.0f, 600.0f}, true);
    return input;
}

SDL_Event RightButton(SDL_EventType type, float x, float y, Uint64 timestampNs)
{
    SDL_Event event{};
    event.type = type;
    event.button.button = SDL_BUTTON_RIGHT;
    event.button.x = x;
    event.button.y = y;
    event.button.timestamp = timestampNs;
    return event;
}

SDL_Event MouseMotion(float xrel, float yrel)
{
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.xrel = xrel;
    event.motion.yrel = yrel;
    return event;
}

void QuickStillPressIsAClick()
{
    InputState input = ViewportInput();
    input.HandleEvent(RightButton(SDL_EVENT_MOUSE_BUTTON_DOWN, 400.0f, 300.0f, 0));
    input.HandleEvent(MouseMotion(1.0f, 1.0f));
    Require(!input.ConsumeViewportRightClick(), "no click while the button is held");
    input.HandleEvent(RightButton(SDL_EVENT_MOUSE_BUTTON_UP, 400.0f, 300.0f, 100 * kMillisecondNs));
    Require(input.ConsumeViewportRightClick(), "a quick, still right press is a click");
    Require(!input.ConsumeViewportRightClick(), "the click is consumed once");
}

void DraggingIsAMouseLook()
{
    InputState input = ViewportInput();
    input.HandleEvent(RightButton(SDL_EVENT_MOUSE_BUTTON_DOWN, 400.0f, 300.0f, 0));
    input.HandleEvent(MouseMotion(20.0f, -5.0f));
    input.HandleEvent(RightButton(SDL_EVENT_MOUSE_BUTTON_UP, 400.0f, 300.0f, 100 * kMillisecondNs));
    Require(!input.ConsumeViewportRightClick(), "a drag looks around, it does not click");
}

void HoldingIsAMouseLook()
{
    // Held to fly with WASD without moving the mouse.
    InputState input = ViewportInput();
    input.HandleEvent(RightButton(SDL_EVENT_MOUSE_BUTTON_DOWN, 400.0f, 300.0f, 0));
    input.HandleEvent(RightButton(SDL_EVENT_MOUSE_BUTTON_UP, 400.0f, 300.0f, 2000 * kMillisecondNs));
    Require(!input.ConsumeViewportRightClick(), "a long hold does not click");
}

void OutsideTheViewportIsNotAClick()
{
    InputState input = ViewportInput();
    input.HandleEvent(RightButton(SDL_EVENT_MOUSE_BUTTON_DOWN, 900.0f, 300.0f, 0));
    input.HandleEvent(RightButton(SDL_EVENT_MOUSE_BUTTON_UP, 900.0f, 300.0f, 50 * kMillisecondNs));
    Require(!input.ConsumeViewportRightClick(), "a right click outside the viewport is not the viewport's");
}
}

int main()
{
    try
    {
        QuickStillPressIsAClick();
        DraggingIsAMouseLook();
        HoldingIsAMouseLook();
        OutsideTheViewportIsNotAClick();
    }
    catch (const std::exception& error)
    {
        std::cerr << "input state test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "input state tests passed\n";
    return 0;
}
