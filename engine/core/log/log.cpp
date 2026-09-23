#include "log.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <deque>
#include <mutex>
#include <vector>

namespace me
{

namespace
{
constexpr size_t kMaxInputMessages = 256;

std::mutex g_inputMessagesMutex;
std::deque<std::string> g_inputMessages;
// Bumped on every change; starts at 1 so a caller's initial revision 0 always copies.
uint64_t g_inputMessagesRevision = 1;
}

void Log::Init()
{
#ifdef _WIN32
    // Log text is UTF-8 (paths included); the console defaults to the OEM code
    // page and would print it as mojibake.
    SetConsoleOutputCP(CP_UTF8);
#endif
    auto logger = spdlog::stdout_color_mt("MiniEngine");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%T] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::trace);
}

bool Log::RefreshInputMessagesSnapshot(std::vector<std::string>& messages, uint64_t& revision)
{
    std::lock_guard<std::mutex> lock(g_inputMessagesMutex);
    if (revision == g_inputMessagesRevision)
    {
        return false;
    }

    messages.assign(g_inputMessages.begin(), g_inputMessages.end());
    revision = g_inputMessagesRevision;
    return true;
}

void Log::ClearInputMessages()
{
    std::lock_guard<std::mutex> lock(g_inputMessagesMutex);
    g_inputMessages.clear();
    ++g_inputMessagesRevision;
}

void Log::WriteInputLine(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_inputMessagesMutex);
    g_inputMessages.push_back(message);
    while (g_inputMessages.size() > kMaxInputMessages)
    {
        g_inputMessages.pop_front();
    }
    ++g_inputMessagesRevision;
}
}
