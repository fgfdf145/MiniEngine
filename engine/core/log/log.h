#pragma once

#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace me
{

class Log
{
  public:
    static void Init();

    template <typename... Args>
    static void InputInfo(fmt::format_string<Args...> formatString, Args&&... args)
    {
        WriteInputLine(fmt::format(formatString, std::forward<Args>(args)...));
    }

    // Copies the input messages into `messages` only when they changed since `revision`, then
    // updates `revision`. Pass revision 0 the first time. Returns whether it copied, so a per-frame
    // caller does not copy the whole log every frame.
    static bool RefreshInputMessagesSnapshot(std::vector<std::string>& messages, uint64_t& revision);
    static void ClearInputMessages();

    struct RecentLine
    {
        // Counts every line logged since Init, from 1.
        uint64_t sequence = 0;
        std::string text;
    };
    // The last lines the default logger wrote (at most `maxLines`, of the newest 4096 kept), only those
    // after `afterSequence`, oldest first: what the control channel's log command returns.
    static std::vector<RecentLine> RecentLines(size_t maxLines, uint64_t afterSequence = 0);

  private:
    static void WriteInputLine(const std::string& message);
};

#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_INPUT_INFO(...) Log::InputInfo(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
}
