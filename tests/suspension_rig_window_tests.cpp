// The editor's Suspension Rigs window, driven without a GPU: ImGui and ImPlot run headless, the
// window loads the GT-R from the selected entity's model data, runs the rigs and draws every tab.
// With MINIENGINE_UI_SNAPSHOT_DIR set, each tab is also rasterised in software to a PNG there, to
// look at.

#include "ae86_car_spec.h"
#include "gtr_car_spec.h"

#include <engine/asset/model_cache.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/renderer_shared_state.h>
#include <engine/editor/services/vehicle_rig_service.h>
#include <engine/editor/ui/editor_suspension_rigs.h>
#include <engine/logic/editor_scene.h>

#include <imgui.h>
#include <implot.h>
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace me;

namespace
{
void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

constexpr int kWidth = 1400;
// What the window's Live Rig tab is shown.
VehicleRigStatus g_liveStatus;
constexpr int kHeight = 900;

// The textures ImGui asks for (the font atlas): kept where ImGui has them, the id is the texture.
void ServeTextures(ImDrawData& drawData)
{
    if (drawData.Textures == nullptr)
    {
        return;
    }
    for (ImTextureData* texture : *drawData.Textures)
    {
        if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
        {
            texture->SetTexID(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)));
            texture->SetStatus(ImTextureStatus_OK);
        }
        else if (texture->Status == ImTextureStatus_WantDestroy)
        {
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

// ImGui's triangles into an RGBA image: edge functions, colours and UVs interpolated, the texture
// sampled at the nearest texel, blended over.
std::vector<float> Rasterise(const ImDrawData& drawData)
{
    std::vector<float> image(static_cast<size_t>(kWidth) * kHeight * 3, 0.08f);
    const auto colour = [](ImU32 c, int shift) {
        return static_cast<float>((c >> shift) & 0xFF) / 255.0f;
    };
    for (const ImDrawList* list : drawData.CmdLists)
    {
        for (const ImDrawCmd& cmd : list->CmdBuffer)
        {
            if (cmd.UserCallback != nullptr || cmd.ElemCount == 0)
            {
                continue;
            }
            const ImTextureData* texture = reinterpret_cast<const ImTextureData*>(static_cast<std::uintptr_t>(cmd.GetTexID()));
            const int clipX0 = std::max(0, static_cast<int>(cmd.ClipRect.x - drawData.DisplayPos.x));
            const int clipY0 = std::max(0, static_cast<int>(cmd.ClipRect.y - drawData.DisplayPos.y));
            const int clipX1 = std::min(kWidth, static_cast<int>(std::ceil(cmd.ClipRect.z - drawData.DisplayPos.x)));
            const int clipY1 = std::min(kHeight, static_cast<int>(std::ceil(cmd.ClipRect.w - drawData.DisplayPos.y)));
            for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3)
            {
                const ImDrawVert* v[3];
                for (int k = 0; k < 3; ++k)
                {
                    v[k] = &list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i + k]];
                }
                const float area = (v[1]->pos.x - v[0]->pos.x) * (v[2]->pos.y - v[0]->pos.y) - (v[1]->pos.y - v[0]->pos.y) * (v[2]->pos.x - v[0]->pos.x);
                if (std::abs(area) < 1e-8f)
                {
                    continue;
                }
                const int x0 = std::max(clipX0, static_cast<int>(std::floor(std::min({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}))));
                const int y0 = std::max(clipY0, static_cast<int>(std::floor(std::min({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}))));
                const int x1 = std::min(clipX1, static_cast<int>(std::ceil(std::max({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}))));
                const int y1 = std::min(clipY1, static_cast<int>(std::ceil(std::max({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}))));
                for (int y = y0; y < y1; ++y)
                {
                    for (int x = x0; x < x1; ++x)
                    {
                        const float px = x + 0.5f - drawData.DisplayPos.x;
                        const float py = y + 0.5f - drawData.DisplayPos.y;
                        float w[3];
                        for (int k = 0; k < 3; ++k)
                        {
                            const ImDrawVert* a = v[(k + 1) % 3];
                            const ImDrawVert* b = v[(k + 2) % 3];
                            w[k] = ((b->pos.x - a->pos.x) * (py - a->pos.y) - (b->pos.y - a->pos.y) * (px - a->pos.x)) / area;
                        }
                        if (w[0] < 0.0f || w[1] < 0.0f || w[2] < 0.0f)
                        {
                            continue;
                        }
                        float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        float u = 0.0f;
                        float t = 0.0f;
                        for (int k = 0; k < 3; ++k)
                        {
                            rgba[0] += w[k] * colour(v[k]->col, IM_COL32_R_SHIFT);
                            rgba[1] += w[k] * colour(v[k]->col, IM_COL32_G_SHIFT);
                            rgba[2] += w[k] * colour(v[k]->col, IM_COL32_B_SHIFT);
                            rgba[3] += w[k] * colour(v[k]->col, IM_COL32_A_SHIFT);
                            u += w[k] * v[k]->uv.x;
                            t += w[k] * v[k]->uv.y;
                        }
                        if (texture != nullptr && texture->Pixels != nullptr)
                        {
                            const int tx = std::clamp(static_cast<int>(u * texture->Width), 0, texture->Width - 1);
                            const int ty = std::clamp(static_cast<int>(t * texture->Height), 0, texture->Height - 1);
                            const unsigned char* texel = texture->Pixels + (static_cast<size_t>(ty) * texture->Width + tx) * texture->BytesPerPixel;
                            if (texture->Format == ImTextureFormat_RGBA32)
                            {
                                for (int c = 0; c < 4; ++c)
                                {
                                    rgba[c] *= texel[c] / 255.0f;
                                }
                            }
                            else
                            {
                                rgba[3] *= texel[0] / 255.0f;
                            }
                        }
                        float* out = &image[(static_cast<size_t>(y) * kWidth + x) * 3];
                        for (int c = 0; c < 3; ++c)
                        {
                            out[c] = out[c] * (1.0f - rgba[3]) + rgba[c] * rgba[3];
                        }
                    }
                }
            }
        }
    }
    return image;
}

void WritePng(const std::vector<float>& image, const std::filesystem::path& path)
{
    std::vector<unsigned char> bytes(image.size());
    for (size_t i = 0; i < image.size(); ++i)
    {
        bytes[i] = static_cast<unsigned char>(std::clamp(image[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    Require(stbi_write_png(path.string().c_str(), kWidth, kHeight, 3, bytes.data(), kWidth * 3) != 0, "writes " + path.string());
}

// One editor frame with only the rigs window in it, the window filling the display.
void Frame(SuspensionRigWindow& window, const EditorScene& scene, const char* snapshot = nullptr)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    bool open = true;
    EditorUiFrameResult result;
    window.Draw(scene, &open, g_liveStatus, result);
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    ServeTextures(*drawData);
    const char* folder = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR");
    if (snapshot != nullptr && folder != nullptr)
    {
        std::filesystem::create_directories(folder);
        WritePng(Rasterise(*drawData), std::filesystem::path(folder) / snapshot);
    }
}

void Frames(SuspensionRigWindow& window, const EditorScene& scene, int count, const char* snapshot = nullptr)
{
    for (int i = 0; i < count; ++i)
    {
        Frame(window, scene, i + 1 == count ? snapshot : nullptr);
    }
}

// The GT-R as its imported model carries it: the car's data, its wheel nodes (facing -Z, left -X) and
// one submesh per wheel.
std::shared_ptr<LoadedModelData> MakeGtrModel()
{
    VehicleCarSpec spec = test::MakeGtrSpec();
    spec.wheelbase = 2.78f;
    spec.frontWeightShare = 0.555f;
    spec.frontSuspension->centerOfMassAboveWheel = 0.075f;
    spec.rearSuspension->centerOfMassAboveWheel = 0.075f;
    auto model = std::make_shared<LoadedModelData>();
    model->carSpec = spec;
    ModelWheelRig rig;
    rig.corners[0].center = glm::vec3(-0.8583f, 0.2919f, -1.432f);
    rig.corners[1].center = glm::vec3(0.8583f, 0.2919f, -1.432f);
    rig.corners[2].center = glm::vec3(-0.869f, 0.2919f, 1.3454f);
    rig.corners[3].center = glm::vec3(0.869f, 0.2919f, 1.3454f);
    model->wheelRig = rig;
    model->submeshes.resize(5);
    for (uint8_t corner = 0; corner < 4; ++corner)
    {
        model->submeshes[corner].wheelPart = ModelWheelPart::Wheel;
        model->submeshes[corner].wheelCorner = corner;
    }
    return model;
}

void TestWindowRunsTheRigsOnTheSelectedCar()
{
    std::shared_ptr<LoadedModelData> model = MakeGtrModel();
    const std::string path = "suspension_rig_window_tests/nissan_gtr_gt3.gltf";
    ModelCache::Store(path, model);

    EditorScene scene;
    SerializedEntityData car;
    car.tagName = "Nissan GT-R GT3";
    car.modelSourcePath = path;
    scene.SetSelectedEntity(scene.CreateEntity(car));

    SuspensionRigWindow window;
    // The selected car loads by itself; the linkage view draws at rest and posed.
    Frames(window, scene, 3, "rigs_linkage_rest.png");
    window.SetLinkagePose(0, 35.0f, 2.0f, 0.6f);
    Frames(window, scene, 3, "rigs_linkage_front_posed.png");
    window.SetLinkagePose(1, -30.0f, -2.5f, 0.0f);
    Frames(window, scene, 3, "rigs_linkage_rear_posed.png");

    // A quick run (short sweeps and roads, no friction comparison) on the worker thread.
    suspension::RigReportOptions& options = window.Options();
    options.sweepCycles = 30;
    options.roadSeconds = 3.0;
    options.frictionComparison = false;
    window.StartRun();
    Require(window.IsRunning(), "the run starts");
    const auto start = std::chrono::steady_clock::now();
    bool snappedProgress = false;
    while (window.IsRunning())
    {
        Frame(window, scene, snappedProgress ? nullptr : "rigs_running.png");
        snappedProgress = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(120), "the run finishes");
    }
    Require(window.HasReport(), "the run leaves its report");

    window.RequestTab(SuspensionRigWindow::SummaryTab);
    Frames(window, scene, 3, "rigs_summary.png");
    window.RequestTab(SuspensionRigWindow::KcTab);
    Frames(window, scene, 3, "rigs_kc.png");
    window.RequestTab(SuspensionRigWindow::SevenPostTab);
    Frames(window, scene, 3, "rigs_seven_post.png");
    window.RequestTab(SuspensionRigWindow::LiveTab);
    Frames(window, scene, 3, "rigs_live.png");
    window.RequestTab(SuspensionRigWindow::LinkageTab);
    window.SetLinkagePose(0, 0.0f, 0.0f, 0.0f);
    Frames(window, scene, 3);
    ModelCache::Invalidate(path);
}

// The AE86's live rear axle in the window: the linkage view draws the axle on its links, posed by travel
// and roll, and the rigs run on it.
void TestWindowDrawsTheLiveAxle()
{
    auto model = std::make_shared<LoadedModelData>();
    model->carSpec = test::MakeAe86Spec();
    const std::string path = "suspension_rig_window_tests/ae86/toyota_ae86.gltf";
    ModelCache::Store(path, model);
    EditorScene scene;
    SerializedEntityData car;
    car.tagName = "Toyota AE86";
    car.modelSourcePath = path;
    scene.SetSelectedEntity(scene.CreateEntity(car));

    SuspensionRigWindow window;
    window.RequestTab(SuspensionRigWindow::LinkageTab);
    Frames(window, scene, 2);
    window.SetLinkagePose(1, 0.0f, 0.0f, 0.0f);
    Frames(window, scene, 3, "rigs_ae86_axle_rest.png");
    window.SetLinkagePose(1, 25.0f, 3.0f, 0.0f);
    Frames(window, scene, 3, "rigs_ae86_axle_rolled.png");

    suspension::RigReportOptions& options = window.Options();
    options.sweepCycles = 30;
    options.roadSeconds = 3.0;
    options.frictionComparison = false;
    window.StartRun();
    const auto start = std::chrono::steady_clock::now();
    while (window.IsRunning())
    {
        Frame(window, scene);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(120), "the AE86's run finishes");
    }
    Require(window.HasReport(), "the rigs run on a live axle");
    window.RequestTab(SuspensionRigWindow::KcTab);
    Frames(window, scene, 3, "rigs_ae86_kc.png");
    ModelCache::Invalidate(path);
}

// The live rig moves the selected car, its wheels and the scene's pads and loaders: the pads follow the
// input, roll lifts the body's left side when the left pads rise, the loaders stretch with the body,
// and stopping puts everything back.
void TestLiveRigMovesTheCarAndThePads()
{
    const std::string path = "suspension_rig_window_tests/live/nissan_gtr_gt3.gltf";
    ModelCache::Store(path, MakeGtrModel());
    RendererSharedState state;
    state.editorWorld = CreateEditorWorld();
    IEditorWorld& world = state.GetEditorWorld();
    SerializedEntityData carData;
    carData.tagName = "Nissan GT-R GT3";
    carData.modelSourcePath = path;
    carData.transform.translation = glm::vec3(0.0f, 0.713f, 0.0f);
    const entt::entity car = world.CreateEntity(carData);
    const auto prop = [&](const char* tag, glm::vec3 position, glm::vec3 scale) {
        SerializedEntityData data;
        data.tagName = tag;
        data.transform.translation = position;
        data.transform.scale = scale;
        return world.CreateEntity(data);
    };
    const entt::entity padFL = prop("Wheel pad FL", glm::vec3(-0.86f, 0.475f, -1.43f), glm::vec3(0.5f, 0.35f, 0.6f));
    const entt::entity padFR = prop("Wheel pad FR", glm::vec3(0.86f, 0.475f, -1.43f), glm::vec3(0.5f, 0.35f, 0.6f));
    const entt::entity loader = prop("Aero loader rear", glm::vec3(0.0f, 0.5f, 0.9f), glm::vec3(0.12f, 0.4f, 0.12f));
    const TransformComponent carStart = world.GetTransform(car);
    const TransformComponent padStart = world.GetTransform(padFL);
    const TransformComponent loaderStart = world.GetTransform(loader);

    // Roll at 1.5 Hz, 20 mm at the pads, drawn true size, in real time.
    VehicleRigExcitation excitation;
    excitation.mode = suspension::RigMode::Roll;
    excitation.waveform = VehicleRigWaveform::Sine;
    excitation.frequency = 1.5f;
    excitation.amplitude = 0.02f;
    excitation.playbackRate = 1.0f;
    excitation.exaggeration = 1.0f;
    VehicleRigService::Start(state, car, excitation);
    Require(state.vehicleRig.session != nullptr && state.vehicleRig.session->props.size() == 3, "the rig finds the pads and the loader");

    double mostLeftUp = 0.0;
    double padMoved = 0.0;
    double loaderStretched = 0.0;
    bool sideAgrees = true;
    for (int frame = 0; frame < 40; ++frame)
    {
        Require(VehicleRigService::Tick(state, 0.1f), "the rig runs");
        const VehicleRigSession& session = *state.vehicleRig.session;
        // The pads where the input has them.
        const double pad = world.GetTransform(padFL).translation.y - padStart.translation.y;
        Require(std::abs(pad - session.pads[0]) < 1e-5, "the FL pad follows the input");
        Require(std::abs((world.GetTransform(padFR).translation.y - padStart.translation.y) + pad) < 1e-5, "in roll the FR pad moves the other way");
        padMoved = std::max(padMoved, std::abs(pad));
        loaderStretched = std::max(loaderStretched, static_cast<double>(std::abs(world.GetTransform(loader).scale.y - loaderStart.scale.y)));
        // The model's left front wheel centre against its right front's, in the world.
        const glm::mat4 m = world.GetModelMatrix(car);
        const float leftY = (m * glm::vec4(-0.8583f, 0.2919f, -1.432f, 1.0f)).y;
        const float rightY = (m * glm::vec4(0.8583f, 0.2919f, -1.432f, 1.0f)).y;
        const double leftUp = leftY - rightY;
        if (session.time > 1.5 && std::abs(session.rig->Roll()) > 1e-3)
        {
            // Roll is right side down: the left side up.
            sideAgrees = sideAgrees && (leftUp > 0.0) == (session.rig->Roll() > 0.0);
        }
        mostLeftUp = std::max(mostLeftUp, leftUp);
    }
    std::cout << "live rig, roll 20 mm at 1.5 Hz: pads up to " << padMoved * 1000.0 << " mm, the left front up to " << mostLeftUp * 1000.0
              << " mm over the right, the rear loader stretched up to " << loaderStretched * 1000.0 << " mm\n";
    Require(padMoved > 0.015, "the pads move by about the amplitude");
    Require(sideAgrees, "the drawn body rolls the way the rig does");
    Require(mostLeftUp > 0.005, "the body rolls visibly");
    const VehicleRigStatus status = VehicleRigService::GetStatus(state);
    // The linkage to draw rides with the car: every joint near one of its wheels where the model is now.
    Require(!status.linkage.links.empty() && !status.linkage.joints.empty(), "the rig's linkage to draw");
    const glm::mat4 carMatrix = world.GetModelMatrix(car);
    for (const glm::vec3& joint : status.linkage.joints)
    {
        float nearest = 1e9f;
        for (const ModelWheelRig::Corner& wheel : MakeGtrModel()->wheelRig->corners)
        {
            nearest = std::min(nearest, glm::length(joint - glm::vec3(carMatrix * glm::vec4(wheel.center, 1.0f))));
        }
        Require(nearest < 0.5f, "a joint at its wheel, " + std::to_string(nearest) + " m away");
    }
    Require(status.active && status.sampleTime.size() > 500 && status.tyreLoad[0].size() == status.sampleTime.size(), "the status records the run");

    // The body loaders instead: a cornering roll moment of 0.5 g at 1 Hz with the pads level. The body
    // rolls on its springs, the suspension working: left and right travel opposite. The GT3 car is stiff:
    // 1147 kg x 9.81 x 0.5 x 0.43 m = 2.4 kNm on about 10.6 kNm/deg is 0.23 deg, 3.3 mm at the wheels.
    excitation.waveform = VehicleRigWaveform::BodyLoads;
    excitation.bodyLoad = 0.5f;
    excitation.frequency = 1.0f;
    VehicleRigService::SetExcitation(state, excitation);
    double mostTravel = 0.0;
    bool opposite = true;
    for (int frame = 0; frame < 30; ++frame)
    {
        VehicleRigService::Tick(state, 0.1f);
        const VehicleRigSession& session = *state.vehicleRig.session;
        Require(session.pads[0] == 0.0 && session.pads[1] == 0.0, "the pads stay level under the body loads");
        const double left = session.rig->Travel(0);
        const double right = session.rig->Travel(1);
        mostTravel = std::max(mostTravel, std::abs(left));
        if (std::abs(left) > 0.002)
        {
            opposite = opposite && left * right < 0.0;
        }
    }
    std::cout << "live rig, body roll moment of 0.5 g: front travel up to " << mostTravel * 1000.0 << " mm\n";
    Require(mostTravel > 0.0025 && mostTravel < 0.0045, "the body rolls on its springs as stiffly as the analysis says");
    Require(opposite, "the left and right wheels' travel opposite");

    // Saving the scene sees the car and the props where they were.
    VehicleRigService::RunWithRigAtStart(state, [&] {
        Require(world.GetTransform(car).translation == carStart.translation && world.GetTransform(padFL).translation == padStart.translation,
                "the scene saves its own transforms, not the rig's");
    });

    VehicleRigService::Stop(state);
    Require(world.GetTransform(car).translation == carStart.translation && world.GetTransform(padFL).translation == padStart.translation &&
                world.GetTransform(loader).scale == loaderStart.scale,
            "stopping puts everything back");
    ModelCache::Invalidate(path);

    // The window's Live Rig tab on that record.
    g_liveStatus = status;
}
}

int main()
{
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();
    ImGui::StyleColorsDark();
    int result = 0;
    try
    {
        TestLiveRigMovesTheCarAndThePads();
        TestWindowRunsTheRigsOnTheSelectedCar();
        TestWindowDrawsTheLiveAxle();
        std::cout << "suspension rig window tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "suspension rig window tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return result;
}
