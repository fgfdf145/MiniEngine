#pragma once

#include <engine/core/threading/task_future.h>
#include <engine/editor/ui/framework/editor_panel.h>
#include <engine/tyre/flex_ring/flex_ring_data.h>
#include <engine/tyre/flex_ring/flex_ring_preprocess.h>
#include <engine/tyre/flex_ring/flex_ring_report.h>
#include <engine/tyre/flex_ring/flex_ring_road.h>
#include <engine/tyre/flex_ring/flex_ring_tyre.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace me
{
namespace flexring = tyre::flexring;

// The Flex Ring Tyre panel: the flexible ring (FTire-type) tyre of engine/tyre/flex_ring on its own,
// not on a car. Its basic data edited item by item, pre-processing with the fit's targets, a live rig
// (static load or rolling at a slip angle, slip ratio and camber over flat road or cleats) drawn from the
// side, the front and the footprint with the deformation magnified, the unloaded modes, and the test
// programs with their curves.
class FlexRingTyrePanel final : public EditorPanel
{
  public:
    FlexRingTyrePanel();
    ~FlexRingTyrePanel() override;

    // The panel's contents in the current window (the tests drive it without an editor).
    void DrawContents(double frameSeconds);

    enum Tab
    {
        LiveTab,
        ParametersTab,
        ModesTab,
        TestsTab,
    };
    void RequestTab(Tab tab)
    {
        m_requestedTab = tab;
    }

    // What the window's own controls do, for its callers and tests.
    flexring::FlexRingData& Data()
    {
        return m_data;
    }
    void StartPreprocess();
    bool IsPreprocessing() const
    {
        return m_pre != nullptr;
    }
    bool HasModel() const
    {
        return m_model.has_value();
    }
    void StartTests(bool quick);
    bool IsTesting() const
    {
        return m_tests != nullptr;
    }
    bool HasTestResults() const
    {
        return m_report.has_value();
    }
    // Waits for running work to finish (tests).
    void WaitForWork();

    struct LiveSettings
    {
        int mode = 1;               // 0 standing, 1 rolling
        float load = 3000.0f;       // N
        float speedKmh = 60.0f;
        float slipAngleDeg = 0.0f;
        float slipRatio = 0.0f;
        bool freeRolling = true;
        float camberDeg = 0.0f;
        int road = 0;               // 0 flat, 1 transversal cleats, 2 longitudinal cleat, 3 waves
        float cleatHeightMm = 10.0f;
        float cleatWidthMm = 20.0f;
        float pressureBar = 0.0f;   // 0: the data's
        float frictionScale = 1.0f;
        float timeScale = 1.0f;     // simulated over real time asked for
        float magnification = 3.0f; // of the drawn deformation
        bool running = true;
    };
    LiveSettings& Live()
    {
        return m_live;
    }
    // Advances the live rig by the simulated time the frame asks for (within a budget).
    void StepLive(double frameSeconds);
    double LiveTime() const
    {
        return m_liveTime;
    }

  protected:
    void OnGui(EditorContext& context) override;
    void PreBegin(EditorContext& context) override;

  private:
    struct PreRun
    {
        std::atomic<double> progress{0.0};
        std::atomic<bool> cancel{false};
        std::mutex mutex;
        std::string stage;
        TaskFuture<flexring::PreprocessResult> result;
    };
    struct TestRun
    {
        std::atomic<bool> cancel{false};
        std::mutex mutex;
        std::vector<std::string> log;
        TaskFuture<flexring::ReportResult> result;
    };
    struct History
    {
        std::deque<double> t, fx, fy, fz, mz;
    };

    void PollWork();
    void DrawHeader();
    void DrawLiveTab(double frameSeconds);
    void DrawParametersTab();
    void DrawModesTab();
    void DrawTestsTab();
    void ResetLive();
    void BuildLiveRoad();
    void DrawSideView(const ImVec2& size);
    void DrawFrontView(const ImVec2& size);
    void DrawFootprint(const ImVec2& size);

    flexring::FlexRingData m_data;
    std::string m_dataPath;
    std::string m_dataMessage;
    bool m_fit = true;
    bool m_dataChanged = false;
    std::unique_ptr<PreRun> m_pre;
    std::optional<flexring::PreprocessResult> m_model;
    std::string m_error;

    // Live rig.
    LiveSettings m_live;
    std::unique_ptr<flexring::FlexRingTyre> m_tyre;
    std::unique_ptr<flexring::Road> m_road;
    int m_roadBuilt = -1;
    double m_liveTime = 0.0;
    double m_x = 0.0;
    double m_y = 0.0;
    double m_spin = 0.0;
    double m_spinAngle = 0.0;
    double m_touch = 0.0;
    double m_deflection = 0.0;
    double m_kv = 2.5e5;
    double m_filteredLoad = 0.0;
    double m_speed = 0.0;
    double m_cpuPerSecond = 0.0; // CPU seconds per simulated second, smoothed
    History m_history;

    // Tests.
    std::unique_ptr<TestRun> m_tests;
    std::optional<flexring::ReportResult> m_report;
    std::vector<std::string> m_testLog;
    std::string m_outputDirectory = "out/flex_ring";

    int m_requestedTab = -1;
};

}
