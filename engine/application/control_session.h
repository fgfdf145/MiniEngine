#pragma once

#include <engine/application/control_server.h>
#include <engine/application/control_ui.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace me
{

class IRenderBackend;
struct RendererSharedState;

// The engine's side of the control channel (docs/design/2026-10-10-engine-control-channel-design.md):
// runs the requests ControlServer received on the main thread, between frames, where the editor's
// own panels change the same state. A request is one JSON object a line,
//   {"id": 7, "cmd": "camera.set", "args": {"position": [0, 2, 5]}}
// and its answer is one line with the same id,
//   {"id": 7, "ok": true, "result": {...}}   or   {"id": 7, "ok": false, "error": "..."}
// A command that takes frames to finish (frames, wait_scene, photo) answers when it has; the frame
// loop keeps running meanwhile, and other requests are answered in between. "help" lists them all.
class ControlSession
{
  public:
    ControlSession(ControlServer& server, RendererSharedState& state, IRenderBackend& renderer);

    // Once a frame, after the frame is drawn: counts it, runs the requests that came in, and answers
    // the waiting ones that are done. `frameDrawn` is false for a frame the loop skipped (minimized).
    void Update(bool frameDrawn);
    // A client asked the engine to quit.
    bool QuitRequested() const
    {
        return m_quitRequested;
    }

    // Runs one command now, outside the socket (tests): its result, or the error it threw. A command
    // that waits for frames returns only what it answers at once.
    nlohmann::json Execute(const std::string& command, const nlohmann::json& args);

  private:
    using Clock = std::chrono::steady_clock;
    // Returns the answer once done, else nothing; throws to answer with an error.
    using WaitPoll = std::function<std::optional<nlohmann::json>()>;
    struct Waiter
    {
        uint64_t connection = 0;
        nlohmann::json id;
        std::string command;
        WaitPoll poll;
        Clock::time_point deadline;
    };
    // What a command returns: an answer now, or a poll answered later.
    struct Outcome
    {
        nlohmann::json result;
        WaitPoll wait;
        double timeoutSeconds = 0.0;
    };
    using Handler = std::function<Outcome(const nlohmann::json& args)>;
    struct CommandInfo
    {
        std::string summary;
        Handler handler;
    };

    void Register(const std::string& name, std::string summary, Handler handler);
    void RegisterCommands();
    void Handle(const ControlServer::Request& request);
    void Answer(uint64_t connection, const nlohmann::json& id, const nlohmann::json& result);
    void Fail(uint64_t connection, const nlohmann::json& id, const std::string& error);

    ControlServer& m_server;
    RendererSharedState& m_state;
    IRenderBackend& m_renderer;
    std::map<std::string, CommandInfo> m_commands;
    std::vector<Waiter> m_waiters;
    uint64_t m_framesDrawn = 0;
    bool m_quitRequested = false;
    // The camera's adaptation before deterministic turned it off, given back when it is turned off.
    std::optional<std::pair<bool, bool>> m_adaptationBeforeDeterministic;
    // Made with the first ui.run.
    std::unique_ptr<ControlUiRunner> m_uiRunner;
};
}
