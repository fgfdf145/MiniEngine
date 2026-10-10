#include "control_ui.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace me
{

using nlohmann::json;

namespace
{
json RectToJson(const ImRect& rect)
{
    return json::array({rect.Min.x, rect.Min.y, rect.Max.x, rect.Max.y});
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return text;
}

// "Ctrl+Shift+S", "Enter", "F5", "Alt+U": an ImGui key chord.
ImGuiKeyChord ParseKeyChord(const std::string& text)
{
    ImGuiKeyChord chord = 0;
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t plus = text.find('+', start + 1);
        const std::string part = Lower(text.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
        start = plus == std::string::npos ? text.size() + 1 : plus + 1;
        if (part == "ctrl" || part == "control")
        {
            chord |= ImGuiMod_Ctrl;
            continue;
        }
        if (part == "shift")
        {
            chord |= ImGuiMod_Shift;
            continue;
        }
        if (part == "alt")
        {
            chord |= ImGuiMod_Alt;
            continue;
        }
        if (part == "super" || part == "win" || part == "cmd")
        {
            chord |= ImGuiMod_Super;
            continue;
        }
        ImGuiKey found = ImGuiKey_None;
        for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END && found == ImGuiKey_None; ++key)
        {
            if (Lower(ImGui::GetKeyName(static_cast<ImGuiKey>(key))) == part)
            {
                found = static_cast<ImGuiKey>(key);
            }
        }
        if (found == ImGuiKey_None)
        {
            throw std::runtime_error("Unknown key '" + part + "' (ImGui's names: Enter, Escape, F5, A, Space, ...)");
        }
        chord |= found;
    }
    return chord;
}

const char* StatusName(ImGuiTestStatus status)
{
    switch (status)
    {
    case ImGuiTestStatus_Success:
        return "success";
    case ImGuiTestStatus_Queued:
        return "queued";
    case ImGuiTestStatus_Running:
        return "running";
    case ImGuiTestStatus_Error:
        return "error";
    case ImGuiTestStatus_Suspended:
        return "suspended";
    default:
        return "unknown";
    }
}

const std::vector<std::string> kOps = {
    "click", "right_click", "double_click", "hold", "check", "uncheck", "open", "close", "input", "menu", "combo", "key",
    "type", "focus", "hover", "drag", "drag_by", "wheel", "wait", "yield", "set_ref", "info", "exists", "read", "list"};
}

struct ControlUiRunner::Run
{
    json steps;
    json outputs = json::array();
    int failedStep = -1;
    std::string stepError;
};

namespace
{
// One step, on the test engine's coroutine (in step with the frames). Returns what it reads, if anything.
json RunStep(ImGuiTestContext* ctx, const json& step)
{
    const std::string op = step.at("op").get<std::string>();
    const std::string refText = step.value("ref", std::string());
    const ImGuiTestRef ref(refText.c_str());
    if (op == "click")
    {
        ctx->ItemClick(ref);
    }
    else if (op == "right_click")
    {
        ctx->ItemClick(ref, ImGuiMouseButton_Right);
    }
    else if (op == "double_click")
    {
        ctx->ItemDoubleClick(ref);
    }
    else if (op == "hold")
    {
        ctx->ItemHold(ref, step.value("seconds", 0.5f));
    }
    else if (op == "check")
    {
        ctx->ItemCheck(ref);
    }
    else if (op == "uncheck")
    {
        ctx->ItemUncheck(ref);
    }
    else if (op == "open")
    {
        ctx->ItemOpen(ref);
    }
    else if (op == "close")
    {
        ctx->ItemClose(ref);
    }
    else if (op == "input")
    {
        const json& value = step.at("value");
        if (value.is_string())
        {
            ctx->ItemInputValue(ref, value.get<std::string>().c_str());
        }
        else if (value.is_number_integer())
        {
            ctx->ItemInputValue(ref, value.get<int>());
        }
        else
        {
            ctx->ItemInputValue(ref, value.get<float>());
        }
    }
    else if (op == "menu")
    {
        ctx->MenuClick(ref);
    }
    else if (op == "combo")
    {
        ctx->ComboClick(ref);
    }
    else if (op == "key")
    {
        ctx->KeyPress(ParseKeyChord(step.at("keys").get<std::string>()), step.value("count", 1));
    }
    else if (op == "type")
    {
        ctx->KeyChars(step.at("text").get<std::string>().c_str());
    }
    else if (op == "focus")
    {
        ctx->WindowFocus(ref);
    }
    else if (op == "hover")
    {
        ctx->MouseMove(ref);
    }
    else if (op == "drag")
    {
        const std::string to = step.at("to").get<std::string>();
        ctx->ItemDragAndDrop(ref, ImGuiTestRef(to.c_str()));
    }
    else if (op == "drag_by")
    {
        const json& delta = step.at("delta");
        ctx->ItemDragWithDelta(ref, ImVec2(delta.at(0).get<float>(), delta.at(1).get<float>()));
    }
    else if (op == "wheel")
    {
        if (!refText.empty())
        {
            ctx->MouseMove(ref);
        }
        const json& delta = step.at("delta");
        ctx->MouseWheel(ImVec2(delta.at(0).get<float>(), delta.at(1).get<float>()));
    }
    else if (op == "wait")
    {
        ctx->SleepNoSkip(step.value("seconds", 0.25f), 1.0f / 60.0f);
    }
    else if (op == "yield")
    {
        ctx->Yield(step.value("frames", 1));
    }
    else if (op == "set_ref")
    {
        ctx->SetRef(ref);
    }
    else if (op == "info" || op == "exists")
    {
        const ImGuiTestItemInfo info = ctx->ItemInfo(ref, ImGuiTestOpFlags_NoError);
        if (info.ID == 0)
        {
            return {{"ref", refText}, {"found", false}};
        }
        return {
            {"ref", refText},
            {"found", true},
            {"id", info.ID},
            {"label", info.DebugLabel},
            {"window", info.Window != nullptr ? info.Window->Name : ""},
            {"rect", RectToJson(info.RectFull)},
            {"hovered", (info.StatusFlags & ImGuiItemStatusFlags_HoveredRect) != 0},
            {"checked", (info.StatusFlags & ImGuiItemStatusFlags_Checked) != 0},
            {"opened", (info.StatusFlags & ImGuiItemStatusFlags_Opened) != 0},
            {"disabled", (info.ItemFlags & ImGuiItemFlags_Disabled) != 0}};
    }
    else if (op == "read")
    {
        const char* text = ctx->ItemReadAsString(ref);
        return {{"ref", refText}, {"value", text != nullptr ? text : ""}};
    }
    else if (op == "list")
    {
        ImGuiTestItemList items;
        ctx->GatherItems(&items, ref, step.value("depth", 1));
        json list = json::array();
        for (int index = 0; index < items.GetSize(); ++index)
        {
            const ImGuiTestItemInfo* item = items.GetByIndex(index);
            list.push_back({{"id", item->ID}, {"label", item->DebugLabel}, {"depth", item->Depth}, {"rect", RectToJson(item->RectFull)}});
        }
        return {{"ref", refText}, {"items", list}};
    }
    return nullptr;
}

void RunTest(ImGuiTestContext* ctx)
{
    auto* run = static_cast<ControlUiRunner::Run*>(ctx->Test->UserData);
    if (run == nullptr)
    {
        return;
    }
    for (size_t index = 0; index < run->steps.size(); ++index)
    {
        try
        {
            json output = RunStep(ctx, run->steps[index]);
            if (!output.is_null())
            {
                output["step"] = index;
                run->outputs.push_back(std::move(output));
            }
        }
        catch (const std::exception& error)
        {
            run->stepError = error.what();
            run->failedStep = static_cast<int>(index);
            IM_ERRORF_NOHDR("step %d: %s", static_cast<int>(index), error.what());
            return;
        }
        if (ctx->IsError())
        {
            run->failedStep = static_cast<int>(index);
            return;
        }
    }
}
}

ControlUiRunner::ControlUiRunner(ImGuiTestEngine* engine)
    : m_engine(engine)
{
    ImGuiTestEngineIO& io = ImGuiTestEngine_GetIO(engine);
    // As fast as the frames come; the simulated cursor still travels to each item.
    io.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
    io.ConfigSavedSettings = false;
    io.ConfigCaptureEnabled = false;
    io.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
    io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    m_test = ImGuiTestEngine_RegisterTest(engine, "control", "ui.run", __FILE__, __LINE__);
    m_test->TestFunc = RunTest;
}

ControlUiRunner::~ControlUiRunner()
{
    if (m_test != nullptr)
    {
        m_test->UserData = nullptr;
    }
}

void ControlUiRunner::Validate(const json& steps)
{
    if (!steps.is_array() || steps.empty())
    {
        throw std::runtime_error("'steps' is an array of {op, ...}");
    }
    for (size_t index = 0; index < steps.size(); ++index)
    {
        const json& step = steps[index];
        if (!step.is_object() || !step.contains("op") || !step["op"].is_string() ||
            std::find(kOps.begin(), kOps.end(), step["op"].get<std::string>()) == kOps.end())
        {
            std::string ops;
            for (const std::string& op : kOps)
            {
                ops += (ops.empty() ? "" : ", ") + op;
            }
            throw std::runtime_error("Step " + std::to_string(index) + " needs an op: " + ops);
        }
        if (step["op"] == "key")
        {
            ParseKeyChord(step.value("keys", std::string()));
        }
    }
}

void ControlUiRunner::Start(const json& steps)
{
    if (m_run && !Done())
    {
        throw std::runtime_error("A ui.run is still going");
    }
    Validate(steps);
    m_run = std::make_unique<Run>();
    m_run->steps = steps;
    m_test->UserData = m_run.get();
    m_test->Output.Log.Clear();
    ImGuiTestEngine_QueueTest(m_engine, m_test);
}

bool ControlUiRunner::Done() const
{
    if (!m_run)
    {
        return true;
    }
    const ImGuiTestStatus status = m_test->Output.Status;
    return ImGuiTestEngine_IsTestQueueEmpty(m_engine) && status != ImGuiTestStatus_Queued && status != ImGuiTestStatus_Running;
}

json ControlUiRunner::Result() const
{
    if (!m_run)
    {
        return json::object();
    }
    const ImGuiTestStatus status = m_test->Output.Status;
    json result = {
        {"ok", status == ImGuiTestStatus_Success && m_run->failedStep < 0},
        {"status", StatusName(status)},
        {"outputs", m_run->outputs},
        {"log", std::string(m_test->Output.Log.Buffer.c_str())}};
    if (m_run->failedStep >= 0)
    {
        result["failed_step"] = m_run->failedStep;
    }
    if (!m_run->stepError.empty())
    {
        result["error"] = m_run->stepError;
    }
    return result;
}

json ListUiWindows()
{
    ImGuiContext* context = ImGui::GetCurrentContext();
    json list = json::array();
    if (context == nullptr)
    {
        return list;
    }
    for (ImGuiWindow* window : context->Windows)
    {
        // A docked window is flagged a child of its dock node's host; other children are parts of windows.
        const bool child = (window->Flags & ImGuiWindowFlags_ChildWindow) != 0 && !window->DockIsActive;
        if (child || (window->Flags & ImGuiWindowFlags_Tooltip) != 0)
        {
            continue;
        }
        json entry = {
            {"name", window->Name},
            {"shown", static_cast<bool>(window->WasActive) && !static_cast<bool>(window->Hidden)},
            {"rect", json::array({window->Pos.x, window->Pos.y, window->Pos.x + window->Size.x, window->Pos.y + window->Size.y})},
            {"docked", static_cast<bool>(window->DockIsActive)},
            // A docked window behind another tab is not drawn: its items cannot be found until focus selects it.
            {"tab_selected", window->DockNode == nullptr || window->DockNode->TabBar == nullptr ||
                                 window->DockNode->TabBar->VisibleTabId == window->TabId},
            {"focused", context->NavWindow == window}};
        list.push_back(std::move(entry));
    }
    return list;
}
}
