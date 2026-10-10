#pragma once

#include <nlohmann/json.hpp>

#include <memory>

struct ImGuiTest;
struct ImGuiTestEngine;

namespace me
{

// The control channel's ui.* commands (docs/design/2026-10-10-engine-control-channel-design.md):
// Dear ImGui Test Engine drives the editor's own windows with simulated mouse and keyboard input
// that goes to ImGui alone, never the real cursor, so it works on a hidden or background window and
// while the user works on the machine. Items are named by ImGui paths: "Window/Label",
// "Window/##id", "**/Label" (anywhere), menus by "File/Open...".
class ControlUiRunner
{
  public:
    explicit ControlUiRunner(ImGuiTestEngine* engine);
    ~ControlUiRunner();
    ControlUiRunner(const ControlUiRunner&) = delete;
    ControlUiRunner& operator=(const ControlUiRunner&) = delete;

    // Runs `steps` (a JSON array, see ControlSession's ui.run) as one test from the next frame on.
    // Throws while another run is still going, or on a step it cannot read.
    void Start(const nlohmann::json& steps);
    // Throws when `steps` has a step Start could not run.
    static void Validate(const nlohmann::json& steps);
    // The run has finished (or failed).
    bool Done() const;
    // {ok, status, failed_step, outputs: [per step that returns something], log}.
    nlohmann::json Result() const;

    // What a run holds while the test engine plays it (control_ui.cpp).
    struct Run;

  private:
    ImGuiTestEngine* m_engine = nullptr;
    ImGuiTest* m_test = nullptr;
    std::unique_ptr<Run> m_run;
};

// Every window ImGui has this frame: name, shown or not, position and size, the dock it is in.
nlohmann::json ListUiWindows();
}
