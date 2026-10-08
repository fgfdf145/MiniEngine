#pragma once

struct SDL_Window;

namespace me
{

namespace platform::window
{
void ApplyPlatformWindowHints();

// MINIENGINE_VIRTUAL_DESKTOP picks the Windows virtual desktop the main window opens on:
// "launcher" is the desktop of the nearest ancestor process with a window (the terminal or app
// that started the engine), anything else is a desktop GUID. Unset, the window opens wherever
// Windows puts it: the desktop the user is looking at.
bool HasVirtualDesktopRequest();

// Moves the (still hidden) window to the requested desktop, then shows it without activating it,
// so a run started from another desktop never pulls the user's desktop over or takes the focus.
// keepHidden leaves it hidden after the move.
void ShowOnRequestedVirtualDesktop(SDL_Window* window, bool keepHidden);
}
}
