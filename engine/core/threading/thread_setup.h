#pragma once

namespace me
{

// Names the calling thread for debuggers and profilers, and on Windows opts it out of power
// throttling, which would otherwise move it to the efficiency cores while the window is not in
// front (see RequestFullSpeedScheduling). For threads the engine starts: the render thread and the
// task scheduler's workers.
void ConfigureCurrentThread(const char* name);
}
