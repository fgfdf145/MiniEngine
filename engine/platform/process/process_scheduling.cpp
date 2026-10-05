#include "process_scheduling.h"

#include <engine/core/log/log.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace me
{

namespace platform::process
{
void RequestFullSpeedScheduling()
{
#if defined(_WIN32)
    // Opting out of execution-speed throttling: the control bit set with the state bit clear.
    PROCESS_POWER_THROTTLING_STATE processState{};
    processState.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    processState.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    processState.StateMask = 0;
    const bool processSet =
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &processState, sizeof(processState)) != FALSE;

    THREAD_POWER_THROTTLING_STATE threadState{};
    threadState.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    threadState.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    threadState.StateMask = 0;
    const bool threadSet = SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &threadState, sizeof(threadState)) != FALSE;
    if (!processSet || !threadSet)
    {
        LOG_WARN("Could not opt out of power throttling (error {}): the render thread may run on efficiency cores", GetLastError());
    }
#endif
}
}
}
