// The Vehicle panel's Tyres section, driven without a GPU: a car whose wheels name library tyres, a small
// library beside it, the section's pickers clicked, and the choice saved into the car's glTF. With
// MINIENGINE_UI_SNAPSHOT_DIR set, the frames are rasterised to PNGs there.

#include "imgui_software_raster.h"

#include <engine/asset/asset_registry.h>
#include <engine/asset/model_cache.h>
#include <engine/asset/tyre_library.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/editor/command_registry.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/services/vehicle_tyre_fitment.h>
#include <engine/editor/ui/framework/editor_style.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/panels/vehicle_panel.h>
#include <engine/logic/editor_scene.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
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

constexpr int kWidth = 560;
constexpr int kHeight = 900;

struct Editor
{
    EditorScene scene;
    Camera camera;
    ViewportMatrices matrices;
    EditorFrameInput frame;
    EditorUiFrameResult result;
    EditorSharedState state;
    EditorStyle style;
    EditorWindowManager windows;
    CommandRegistry commands;
};

// One frame with the Vehicle panel filling the display.
void Frame(Editor& editor, const char* snapshot = nullptr)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.DeltaTime = 1.0f / 60.0f;
    editor.result = EditorUiFrameResult{};
    EditorContext context{editor.scene, editor.camera, editor.matrices, editor.frame, editor.result, editor.state, editor.style, editor.windows, editor.commands};
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    editor.windows.TickAndDraw(context, false);
    ImGui::Render();
    test::ServeTextures(*ImGui::GetDrawData());
    const char* folder = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR");
    if (snapshot != nullptr && folder != nullptr)
    {
        std::filesystem::create_directories(folder);
        test::WritePng(test::Rasterise(*ImGui::GetDrawData(), kWidth, kHeight), kWidth, kHeight, std::filesystem::path(folder) / snapshot);
    }
}

// A left click at `at`, over a few frames.
void Click(Editor& editor, ImVec2 at, const char* snapshot = nullptr)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at.x, at.y);
    Frame(editor);
    io.AddMouseButtonEvent(0, true);
    Frame(editor);
    io.AddMouseButtonEvent(0, false);
    Frame(editor);
    Frame(editor, snapshot);
}

tyre::TyreSpec MakeTyre(const char* name, float grip, float rate)
{
    tyre::TyreSpec spec;
    spec.name = name;
    spec.shortName = std::string(name).substr(0, 2);
    spec.size.radius = 0.312f;
    spec.size.width = 0.235f;
    spec.grip.referenceLoad = 3000.0f;
    spec.grip.longitudinalReference = grip;
    spec.grip.lateralReference = grip - 0.02f;
    spec.grip.longitudinalLoadExponent = 0.85f;
    spec.slip.frictionLimitAngleDegrees = 7.5f;
    spec.vertical.rate = rate;
    return spec;
}

// A car with two compounds of its own in the library and a tyre from another car beside them.
void TestTyresSectionFitsAndSaves(const std::filesystem::path& root)
{
    VehicleCarSpec spec;
    spec.massKg = 1300.0f;
    spec.frontWeightShare = 0.5f;
    VehicleTyreCompound street;
    street.front.name = street.rear.name = "Street";
    street.front.shortName = street.rear.shortName = "ST";
    street.front.values = {{"DX_REF", 1.2f}, {"FZ0", 3000.0f}, {"RATE", 290000.0f}};
    street.rear.values = {{"DX_REF", 1.21f}, {"FZ0", 3100.0f}, {"RATE", 295000.0f}};
    VehicleTyreCompound semis = street;
    semis.front.name = semis.rear.name = "Semislicks";
    semis.front.shortName = semis.rear.shortName = "SM";
    semis.front.values["DX_REF"] = 1.3f;
    semis.rear.values["DX_REF"] = 1.31f;
    spec.tyreCompounds = {street, semis};
    spec.defaultTyreCompound = 1;
    TyreLibrary::AdoptCarTyres(spec, "ks_test_car");
    TyreLibrary::Store(MakeTyre("Slick Medium", 1.6f, 300000.0f), "ks_other_car", "slick_medium_front");
    TyreLibrary::ResolveWheelTyres(spec);
    Require(spec.wheelTyres[0]->name == "Semislicks", "the car starts on its default compound");

    // The car's glTF, with its data, so that Save to Car has a file to write.
    const std::filesystem::path gltf = root / "cars" / "test_car.gltf";
    std::filesystem::create_directories(gltf.parent_path());
    {
        std::ofstream file(gltf, std::ios::binary);
        file << R"({"asset":{"version":"2.0"},"extensions":{"MINIENGINE_vehicle":{"massKg":1300}}})";
    }
    TyreLibrary::WriteCarTyres(gltf, spec);
    auto model = std::make_shared<LoadedModelData>();
    model->carSpec = spec;
    ModelCache::Store(gltf.string(), model);

    Editor editor;
    SerializedEntityData car;
    car.tagName = "Test Car";
    car.modelSourcePath = gltf.string();
    editor.scene.SetSelectedEntity(editor.scene.CreateEntity(car));
    VehiclePanel& panel = editor.windows.Register<VehiclePanel>();
    panel.SetOpen(true);
    for (int i = 0; i < 3; ++i)
    {
        Frame(editor, i == 2 ? "tyres_section.png" : nullptr);
    }
    Require(editor.state.vehicle.tyreFitmentModel == gltf.string(), "the section follows the selected car");

    // Front Left's picker open (the click lands on it: see the snapshot), then the other car's slick fitted
    // to the front axle, and saved into the car.
    if (const char* x = std::getenv("MINIENGINE_TYRES_CLICK_X"))
    {
        Click(editor, ImVec2(static_cast<float>(std::atof(x)), static_cast<float>(std::atof(std::getenv("MINIENGINE_TYRES_CLICK_Y")))), "tyres_picker_open.png");
        // Closed again by a click on the window's title bar.
        Click(editor, ImVec2(280.0f, 9.0f));
    }
    const std::vector<TyreLibrary::Entry> library = TyreLibrary::List();
    const auto slick = std::find_if(library.begin(), library.end(), [](const TyreLibrary::Entry& entry) { return entry.spec.name == "Slick Medium"; });
    Require(library.size() == 5 && slick != library.end(), "the car's four tyres and the other car's slick");
    VehicleTyreFitment::Fitment& fitment = editor.state.vehicle.tuning.tyreFitment;
    VehicleTyreFitment::Fit(fitment, spec, 0, slick->ref, true);
    Require(fitment[0] == slick->ref && fitment[1] == slick->ref && fitment[2].Empty() && fitment[3].Empty(), "both front wheels, the rear left alone");
    VehicleTyreFitment::Fit(fitment, spec, 3, spec.libraryCompounds[0].rear, false);
    Require(fitment[3] == spec.libraryCompounds[0].rear && fitment[2].Empty(), "one rear wheel alone on the street tyre");
    VehicleTyreFitment::Fit(fitment, spec, 3, spec.wheelTyreRefs[3], false);
    Require(fitment[3].Empty(), "the car's own tyre back on a wheel is no fitment");
    for (int i = 0; i < 3; ++i)
    {
        Frame(editor, i == 2 ? "tyres_section_refitted.png" : nullptr);
    }
    Require(editor.state.vehicle.tuning.tyreFitment[0] == slick->ref, "drawing keeps the fitment");

    const VehicleCarSpec saved = VehicleTyreFitment::Save(gltf, spec, fitment);
    Require(saved.wheelTyreRefs[0] == slick->ref && saved.wheelTyres[1]->name == "Slick Medium" && saved.wheelTyres[2]->name == "Semislicks",
            "the saved car names the slick on the front");
    Require(ModelCache::Get(gltf.string())->carSpec->wheelTyres[0]->name == "Slick Medium", "and the cached car has it");
    std::string text;
    {
        std::ifstream written(gltf, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(written), std::istreambuf_iterator<char>());
    }
    Require(text.find("\"massKg\":1300") != std::string::npos && text.find(slick->ref.uuid) != std::string::npos, "the glTF keeps its data and names the slick");
    fitment = {};
    for (int i = 0; i < 3; ++i)
    {
        Frame(editor, i == 2 ? "tyres_section_saved.png" : nullptr);
    }

    // Another car's model: the fitment is not carried over.
    fitment[0] = slick->ref;
    editor.state.vehicle.tyreFitmentModel = "another.gltf";
    Frame(editor);
    Require(!VehicleTyreFitment::Any(editor.state.vehicle.tuning.tyreFitment), "a fitment chosen for another car is dropped");

    // A car imported before the library: adopted.
    VehicleCarSpec old = spec;
    old.libraryCompounds.clear();
    old.wheelTyreRefs = {};
    const VehicleCarSpec adopted = VehicleTyreFitment::Adopt(gltf, old, "ks_test_car");
    Require(adopted.libraryCompounds.size() == 2 && adopted.wheelTyreRefs[0] == spec.libraryCompounds[1].front, "its tyres found in the library again");
    ModelCache::Invalidate(gltf.string());
}
}

int main()
{
    std::random_device random;
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("miniengine_vehicle_tyres_panel_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(random()));
    std::filesystem::create_directories(root / "assets");
    EnginePaths::Overrides paths;
    paths.assetsRoot = root / "assets";
    EnginePaths::Initialize(paths);
    AssetRegistry::Initialize(root / "assets");

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.Fonts->AddFontDefault();
    ImGui::StyleColorsDark();
    int result = 0;
    try
    {
        TestTyresSectionFitsAndSaves(root);
        std::cout << "vehicle tyres panel tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "vehicle tyres panel tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return result;
}
