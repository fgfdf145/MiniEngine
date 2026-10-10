#include "log.h"

#include <spdlog/sinks/base_sink.h>
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

constexpr size_t kMaxRecentLines = 4096;

// Keeps the newest lines, formatted as the console shows them, for Log::RecentLines.
class RecentLinesSink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    std::vector<Log::RecentLine> Lines(size_t maxLines, uint64_t afterSequence)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Log::RecentLine> lines;
        size_t first = m_lines.size();
        while (first > 0 && m_lines.size() - first < maxLines && m_lines[first - 1].sequence > afterSequence)
        {
            --first;
        }
        lines.assign(m_lines.begin() + static_cast<std::ptrdiff_t>(first), m_lines.end());
        return lines;
    }

  protected:
    void sink_it_(const spdlog::details::log_msg& message) override
    {
        spdlog::memory_buf_t formatted;
        formatter_->format(message, formatted);
        std::string text(formatted.data(), formatted.size());
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        {
            text.pop_back();
        }
        m_lines.push_back({++m_sequence, std::move(text)});
        if (m_lines.size() > kMaxRecentLines)
        {
            m_lines.pop_front();
        }
    }
    void flush_() override
    {
    }

  private:
    std::deque<Log::RecentLine> m_lines;
    uint64_t m_sequence = 0;
};

std::shared_ptr<RecentLinesSink> g_recentLines;
}

void Log::Init()
{
#ifdef _WIN32
    // Log text is UTF-8 (paths included); the console defaults to the OEM code
    // page and would print it as mojibake.
    SetConsoleOutputCP(CP_UTF8);
#endif
    auto logger = spdlog::stdout_color_mt("MiniEngine");
    g_recentLines = std::make_shared<RecentLinesSink>();
    logger->sinks().push_back(g_recentLines);
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

std::vector<Log::RecentLine> Log::RecentLines(size_t maxLines, uint64_t afterSequence)
{
    return g_recentLines ? g_recentLines->Lines(maxLines, afterSequence) : std::vector<RecentLine>{};
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
