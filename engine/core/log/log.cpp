#include "log.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <deque>
#include <mutex>
#include <vector>

namespace
{
constexpr size_t kMaxInputMessages = 256;

std::mutex g_inputMessagesMutex;
std::deque<std::string> g_inputMessages;
}

void Log::Init()
{
    auto logger = spdlog::stdout_color_mt("MiniEngine");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%T] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::trace);
}

std::vector<std::string> Log::GetInputMessagesSnapshot()
{
    std::lock_guard<std::mutex> lock(g_inputMessagesMutex);
    return std::vector<std::string>(g_inputMessages.begin(), g_inputMessages.end());
}

void Log::ClearInputMessages()
{
    std::lock_guard<std::mutex> lock(g_inputMessagesMutex);
    g_inputMessages.clear();
}

void Log::WriteInputLine(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_inputMessagesMutex);
    g_inputMessages.push_back(message);
    while (g_inputMessages.size() > kMaxInputMessages)
    {
        g_inputMessages.pop_front();
    }
}
