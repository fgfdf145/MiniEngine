#pragma once

namespace me
{

namespace platform::process
{
// Asks the OS to run this process at full speed. On a hybrid CPU, Windows treats a process whose
// window is not in front as background work (EcoQoS) and moves its threads to the efficiency cores:
// the render thread then ran at half speed and the GPU sat idle half the frame. A no-op elsewhere.
void RequestFullSpeedScheduling();
}
}
