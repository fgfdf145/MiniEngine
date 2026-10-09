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

SDL_Event MiddleButton(SDL_EventType type, float x, float y)
{
    SDL_Event event{};
    event.type = type;
    event.button.button = SDL_BUTTON_MIDDLE;
    event.button.x = x;
    event.button.y = y;
    return event;
}

SDL_Event Wheel(float x, float y, float amount)
{
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.mouse_x = x;
    event.wheel.mouse_y = y;
    event.wheel.y = amount;
    event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    return event;
}

// A window over the viewport (the Material Editor floating on it) takes its own clicks and wheel:
// inside the viewport's rectangle, but not over its picture, nothing moves the scene camera.
void CoveredViewportLeavesTheCameraAlone()
{
    InputState input;
    input.SetViewportInteractionRegion(SDL_FRect{0.0f, 0.0f, 800.0f, 600.0f}, true, false);
    input.HandleEvent(RightButton(SDL_EVENT_MOUSE_BUTTON_DOWN, 400.0f, 300.0f, 0));
    Require(!input.IsMouseLookActive(), "a right press on a window over the viewport does not look around");
    input.HandleEvent(MiddleButton(SDL_EVENT_MOUSE_BUTTON_DOWN, 400.0f, 300.0f));
    Require(!input.IsMousePanActive(), "a middle press on a window over the viewport does not pan");
    input.HandleEvent(Wheel(400.0f, 300.0f, 1.0f));
    Require(input.GetMouseWheelDelta() == 0.0f, "the wheel over a window on the viewport does not zoom");
    input.HandleEvent(RightButton(SDL_EVENT_MOUSE_BUTTON_UP, 400.0f, 300.0f, 50 * kMillisecondNs));
    Require(!input.ConsumeViewportRightClick(), "nor is its right click the viewport's");
}

// A look that began over the picture goes on when the UI stops reporting the picture as hovered
// (relative mouse mode hides the cursor), and the wheel still changes its speed.
void LookKeepsGoingWhenCovered()
{
    InputState input = ViewportInput();
    input.HandleEvent(RightButton(SDL_EVENT_MOUSE_BUTTON_DOWN, 400.0f, 300.0f, 0));
    Require(input.IsMouseLookActive(), "a right press on the picture looks around");
    input.SetViewportInteractionRegion(SDL_FRect{0.0f, 0.0f, 800.0f, 600.0f}, true, false);
    Require(input.IsMouseLookActive(), "the look goes on while the picture is not hovered");
    input.HandleEvent(Wheel(400.0f, 300.0f, 2.0f));
    Require(input.GetMouseWheelDelta() == 2.0f, "the wheel during a look is the look's");
    input.HandleEvent(RightButton(SDL_EVENT_MOUSE_BUTTON_UP, 400.0f, 300.0f, 500 * kMillisecondNs));
    Require(!input.IsMouseLookActive(), "releasing the button ends the look");
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
        CoveredViewportLeavesTheCameraAlone();
        LookKeepsGoingWhenCovered();
    }
    catch (const std::exception& error)
    {
        std::cerr << "input state test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "input state tests passed\n";
    return 0;
}
