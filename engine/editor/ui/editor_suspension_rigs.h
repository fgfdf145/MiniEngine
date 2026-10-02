#pragma once

#include <engine/editor/services/vehicle_rig_service.h>
#include <engine/suspension/suspension_kinematics.h>
#include <engine/suspension/suspension_rig_report.h>

#include <array>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace me
{
class IEditorWorld;
struct EditorUiFrameResult;

// The Suspension Rigs window: the virtual K&C rig and seven-post rig (engine/suspension) run on the
// selected car's own data (its model's MINIENGINE_vehicle), plotted, with a view of the linkage
// that moves with travel, roll and steering.
class SuspensionRigWindow
{
public:
    SuspensionRigWindow() = default;
    ~SuspensionRigWindow();
    SuspensionRigWindow(const SuspensionRigWindow&) = delete;
    SuspensionRigWindow& operator=(const SuspensionRigWindow&) = delete;

    // `live` is the running rig's record; the window asks to start or stop it through `result`,
    // which also takes its live settings.
    void Draw(const IEditorWorld& scene, bool* open, const VehicleRigStatus& live, EditorUiFrameResult& result);

    // What the window's own controls do, for callers that drive it (and its tests).
    enum Tab
    {
        LiveTab,
        LinkageTab,
        SummaryTab,
        KcTab,
        SevenPostTab,
    };
    // What the next run does (the window's own controls set some of it).
    suspension::RigReportOptions& Options()
    {
        return m_options;
    }
    // The live rig's settings as the Live Rig tab has them.
    const VehicleRigExcitation& Excitation() const
    {
        return m_excitation;
    }
    void StartRun();
    bool IsRunning() const
    {
        return m_run != nullptr;
    }
    bool HasReport() const
    {
        return m_report.has_value();
    }
    // Shows `tab` on the next Draw.
    void RequestTab(Tab tab)
    {
        m_requestedTab = tab;
    }
    void SetLinkagePose(int axle, float travelMm, float rollDegrees, float steering)
    {
        m_axle = axle;
        m_heaveMm = travelMm;
        m_rollDegrees = rollDegrees;
        m_steering = steering;
        m_animation = 0;
    }

private:
    // A run of the rigs on a worker thread; the window polls it each frame.
    struct Run
    {
        std::atomic<double> progress{0.0};
        std::atomic<bool> cancel{false};
        std::mutex mutex;
        std::string stage; // guarded by mutex
        std::future<std::optional<suspension::RigReport>> result;
    };

    void DrawCarSource(const IEditorWorld& scene);
    void DrawRunControls();
    void PollRun();
    void LoadCar(const std::string& sourcePath, const std::string& name);
    void DrawSummaryTab();
    void DrawKcTab();
    void DrawSevenPostTab();
    void DrawLinkageTab();
    void DrawLiveTab(const IEditorWorld& scene, const VehicleRigStatus& live, EditorUiFrameResult& result);
    // Moves the linkage view's two corners to the sliders' (or the animation's) travel, roll and rack.
    void PoseLinkage();

    std::string m_sourcePath;
    std::string m_carName;
    std::optional<suspension::CarModel> m_car;
    std::string m_carError;

    suspension::RigReportOptions m_options;
    std::unique_ptr<Run> m_run;
    std::optional<suspension::RigReport> m_report;
    std::string m_runError;

    int m_requestedTab = -1;

    // Live rig tab.
    VehicleRigExcitation m_excitation;
    int m_roadRoughness = 1; // smooth track, bumpy road, rough road

    // Seven-post tab.
    int m_mode = 0; // RigMode order
    bool m_showFriction = false;

    // Linkage tab: the axle shown, the pose and the animation.
    int m_axle = 0;
    float m_heaveMm = 0.0f;
    float m_rollDegrees = 0.0f;
    float m_steering = 0.0f; // -1 full left .. 1 full right
    int m_animation = 0;     // 0 off, 1 bounce, 2 roll, 3 steering
    float m_animationHz = 0.5f;
    double m_animationTime = 0.0;
    bool m_showGeometry = true;
    std::array<std::unique_ptr<suspension::Kinematics>, 2> m_kinematics; // left, right of m_axle
    std::unique_ptr<suspension::SolidAxle> m_solid;                       // m_axle when it is a solid axle
    int m_kinematicsAxle = -1;
    bool m_poseFailed = false;
};
}
