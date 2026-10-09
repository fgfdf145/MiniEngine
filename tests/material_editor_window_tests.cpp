// The Material Editor window (docs/design/2026-10-09-material-editor-redesign-design.md) driven
// without a GPU: ImGui runs headless on a model written here (a sphere in a paint, a textured cube),
// the mouse and keys go in as ImGui events, and the window's graph, selection, undo and preview are
// checked. With MINIENGINE_UI_SNAPSHOT_DIR set, frames are rasterised in software to PNGs there;
// MINIENGINE_EDITOR_TEST_MODEL=<model.gltf> also snapshots that model.

#include <engine/core/threading/task_system.h>
#include <engine/editor/command_registry.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/material_preview/material_preview_shapes.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui/framework/editor_style.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/windows/material_editor_window.h>
#include <engine/logic/editor_scene.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <stb_image_write.h>

#include "imgui_software_raster.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace me;

namespace
{
constexpr int kWidth = 1700;
constexpr int kHeight = 1000;

void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

class ScopedDirectory
{
  public:
    ScopedDirectory()
        : path(std::filesystem::temp_directory_path() /
               ("miniengine_material_editor_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(path);
    }
    ~ScopedDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    const std::filesystem::path path;
};

// A glTF of two meshes: a sphere in "Paint" (a blue coat) and, beside it, a cube in "Checker" (a
// checker texture), each with normals, tangents and texture coordinates.
std::filesystem::path WriteTestModel(const std::filesystem::path& directory)
{
    struct Part
    {
        MeshData mesh;
        glm::vec3 offset;
    };
    std::vector<Part> parts;
    parts.push_back({BuildMaterialPreviewShapeMesh(MaterialPreviewShape::Sphere), glm::vec3(-0.45f, 0.5f, 0.0f)});
    parts.push_back({BuildMaterialPreviewShapeMesh(MaterialPreviewShape::Cube), glm::vec3(0.55f, 0.4f, 0.0f)});

    std::vector<uint8_t> buffer;
    const auto append = [&buffer](const void* data, size_t bytes)
    {
        const size_t offset = buffer.size();
        buffer.resize(offset + bytes);
        std::memcpy(buffer.data() + offset, data, bytes);
        while (buffer.size() % 4 != 0)
        {
            buffer.push_back(0);
        }
        return offset;
    };
    std::ostringstream views;
    std::ostringstream accessors;
    std::ostringstream meshes;
    int view = 0;
    for (size_t part = 0; part < parts.size(); ++part)
    {
        const MeshData& mesh = parts[part].mesh;
        std::vector<float> positions;
        std::vector<float> normals;
        std::vector<float> tangents;
        std::vector<float> uvs;
        glm::vec3 minimum(1e9f);
        glm::vec3 maximum(-1e9f);
        for (const Vertex& vertex : mesh.vertices)
        {
            const glm::vec3 position = glm::vec3(vertex.position[0], vertex.position[1], vertex.position[2]) + parts[part].offset;
            positions.insert(positions.end(), {position.x, position.y, position.z});
            minimum = glm::min(minimum, position);
            maximum = glm::max(maximum, position);
            normals.insert(normals.end(), {vertex.normal[0], vertex.normal[1], vertex.normal[2]});
            tangents.insert(tangents.end(), {vertex.tangent[0], vertex.tangent[1], vertex.tangent[2], vertex.tangent[3]});
            uvs.insert(uvs.end(), {vertex.texCoord[0], vertex.texCoord[1]});
        }
        const size_t count = mesh.vertices.size();
        const auto addView = [&](const void* data, size_t bytes)
        {
            const size_t offset = append(data, bytes);
            views << (view == 0 ? "" : ",") << "{\"buffer\":0,\"byteOffset\":" << offset << ",\"byteLength\":" << bytes << "}";
            return view++;
        };
        const int base = static_cast<int>(part) * 5;
        const int positionView = addView(positions.data(), positions.size() * 4);
        const int normalView = addView(normals.data(), normals.size() * 4);
        const int tangentView = addView(tangents.data(), tangents.size() * 4);
        const int uvView = addView(uvs.data(), uvs.size() * 4);
        const int indexView = addView(mesh.indices.data(), mesh.indices.size() * 4);
        accessors << (part == 0 ? "" : ",")
                  << "{\"bufferView\":" << positionView << ",\"componentType\":5126,\"count\":" << count << ",\"type\":\"VEC3\",\"min\":[" << minimum.x
                  << "," << minimum.y << "," << minimum.z << "],\"max\":[" << maximum.x << "," << maximum.y << "," << maximum.z << "]},"
                  << "{\"bufferView\":" << normalView << ",\"componentType\":5126,\"count\":" << count << ",\"type\":\"VEC3\"},"
                  << "{\"bufferView\":" << tangentView << ",\"componentType\":5126,\"count\":" << count << ",\"type\":\"VEC4\"},"
                  << "{\"bufferView\":" << uvView << ",\"componentType\":5126,\"count\":" << count << ",\"type\":\"VEC2\"},"
                  << "{\"bufferView\":" << indexView << ",\"componentType\":5125,\"count\":" << mesh.indices.size() << ",\"type\":\"SCALAR\"}";
        meshes << (part == 0 ? "" : ",") << "{\"primitives\":[{\"attributes\":{\"POSITION\":" << base << ",\"NORMAL\":" << base + 1
               << ",\"TANGENT\":" << base + 2 << ",\"TEXCOORD_0\":" << base + 3 << "},\"indices\":" << base + 4 << ",\"material\":" << part << "}]}";
    }
    std::ofstream(directory / "test.bin", std::ios::binary).write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));

    // An 8 x 8 checker, orange and dark grey.
    std::vector<uint8_t> checker(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            const bool light = ((x / 8) + (y / 8)) % 2 == 0;
            uint8_t* texel = &checker[(y * 64 + x) * 4];
            texel[0] = light ? 240 : 40;
            texel[1] = light ? 150 : 40;
            texel[2] = light ? 40 : 46;
            texel[3] = 255;
        }
    }
    stbi_write_png((directory / "checker.png").string().c_str(), 64, 64, 4, checker.data(), 64 * 4);

    const std::filesystem::path model = directory / "test.gltf";
    std::ofstream(model) << "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"test.bin\",\"byteLength\":" << buffer.size() << "}],"
                         << "\"bufferViews\":[" << views.str() << "],\"accessors\":[" << accessors.str() << "],"
                         << "\"images\":[{\"uri\":\"checker.png\"}],\"textures\":[{\"source\":0}],"
                         << "\"materials\":["
                         << "{\"name\":\"Paint\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.05,0.15,0.6,1],\"metallicFactor\":0.0,\"roughnessFactor\":0.45},"
                         << "\"extensions\":{\"KHR_materials_clearcoat\":{\"clearcoatFactor\":1.0,\"clearcoatRoughnessFactor\":0.05}}},"
                         << "{\"name\":\"Checker\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0.0,\"roughnessFactor\":0.8}}],"
                         << "\"extensionsUsed\":[\"KHR_materials_clearcoat\"],"
                         << "\"meshes\":[" << meshes.str() << "],\"nodes\":[{\"mesh\":0},{\"mesh\":1}],\"scenes\":[{\"nodes\":[0,1]}],\"scene\":0}";
    return model;
}

struct Fixture
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
    MaterialEditorWindow& window = windows.Register<MaterialEditorWindow>();

    EditorContext Context()
    {
        return EditorContext{scene, camera, matrices, frame, result, state, style, windows, commands};
    }

    void Frame(const char* snapshot = nullptr)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        result = EditorUiFrameResult{};
        EditorContext context = Context();
        ImGui::NewFrame();
        windows.TickAndDraw(context, false);
        ImGui::Render();
        test::ServeTextures(*ImGui::GetDrawData());
        if (snapshot != nullptr)
        {
            if (const char* folder = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR"))
            {
                std::filesystem::create_directories(folder);
                test::WritePng(test::Rasterise(*ImGui::GetDrawData(), kWidth, kHeight), kWidth, kHeight, std::filesystem::path(folder) / snapshot);
            }
        }
    }

    void Frames(int count)
    {
        for (int index = 0; index < count; ++index)
        {
            Frame();
        }
    }

    void MoveMouse(const ImVec2& position)
    {
        ImGui::GetIO().AddMousePosEvent(position.x, position.y);
        Frame();
    }

    void Click(const ImVec2& position, int button = 0)
    {
        MoveMouse(position);
        ImGui::GetIO().AddMouseButtonEvent(button, true);
        Frame();
        ImGui::GetIO().AddMouseButtonEvent(button, false);
        Frame();
    }

    void Drag(const ImVec2& from, const ImVec2& to, int button = 0)
    {
        MoveMouse(from);
        ImGui::GetIO().AddMouseButtonEvent(button, true);
        Frame();
        for (int step = 1; step <= 6; ++step)
        {
            const float t = static_cast<float>(step) / 6.0f;
            MoveMouse(ImVec2(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t));
        }
        ImGui::GetIO().AddMouseButtonEvent(button, false);
        Frame();
    }

    void Key(ImGuiKey key, ImGuiKey modifier = ImGuiKey_None)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (modifier != ImGuiKey_None)
        {
            io.AddKeyEvent(modifier, true);
        }
        io.AddKeyEvent(key, true);
        Frame();
        io.AddKeyEvent(key, false);
        if (modifier != ImGuiKey_None)
        {
            io.AddKeyEvent(modifier, false);
        }
        Frame();
    }

    ImVec2 WindowPosition() const
    {
        const ImGuiWindow* imguiWindow = ImGui::FindWindowByName(window.GetTitle().c_str());
        return imguiWindow != nullptr ? imguiWindow->Pos : ImVec2(-1.0f, -1.0f);
    }

    const MaterialShaderGraph& Graph() const
    {
        return window.Materials()[static_cast<size_t>(window.SelectedSlot())].shaderGraph;
    }

    uint32_t NodeOfType(MaterialShaderNodeType type) const
    {
        for (const MaterialShaderNode& node : Graph().nodes)
        {
            if (node.type == type)
            {
                return node.id;
            }
        }
        return 0;
    }
};

// A point on the graph's background, away from every node.
ImVec2 EmptyGraphPoint(const MaterialEditorWindow& window, const MaterialShaderGraph& graph)
{
    const ImVec2 min = window.GraphCanvas().CanvasMin();
    const ImVec2 max = window.GraphCanvas().CanvasMax();
    for (float y = min.y + 30.0f; y < max.y - 30.0f; y += 23.0f)
    {
        for (float x = min.x + 30.0f; x < max.x - 30.0f; x += 23.0f)
        {
            bool clear = true;
            for (const MaterialShaderNode& node : graph.nodes)
            {
                const std::optional<ImVec2> title = window.GraphCanvas().NodeTitleOnScreen(node.id);
                if (title.has_value() && std::abs(title->x - x) < 200.0f && std::abs(title->y - y) < 260.0f)
                {
                    clear = false;
                    break;
                }
            }
            if (clear)
            {
                return ImVec2(x, y);
            }
        }
    }
    return ImVec2(min.x + 10.0f, max.y - 10.0f);
}

void TestEditing(const std::filesystem::path& model)
{
    Fixture fixture;
    EditorContext context = fixture.Context();
    fixture.window.OpenModel(context, model.string());
    fixture.Frames(3);
    fixture.window.FinishPreview();
    fixture.Frames(3);
    Require(fixture.window.Materials().size() == 2, "the test model has two slots");
    Require(fixture.window.PreviewFrame().width > 0, "the preview has drawn a frame");
    fixture.Frame("material_editor_window.png");

    // The preview shows both parts: picking the cube's pixels selects its slot.
    const MaterialPreviewFrame& preview = fixture.window.PreviewFrame();
    int cubeX = -1;
    int cubeY = -1;
    for (uint32_t y = 0; y < preview.height && cubeX < 0; y += 2)
    {
        for (uint32_t x = 0; x < preview.width; x += 2)
        {
            if (preview.slots[static_cast<size_t>(y) * preview.width + x] == 1)
            {
                cubeX = static_cast<int>(x);
                cubeY = static_cast<int>(y);
                break;
            }
        }
    }
    Require(cubeX >= 0, "the cube is in the preview");
    const ImVec2 viewportMin = fixture.window.ViewportMin();
    const ImVec2 viewportMax = fixture.window.ViewportMax();
    const ImVec2 cubeOnScreen(
        viewportMin.x + (static_cast<float>(cubeX) + 0.5f) / static_cast<float>(preview.width) * (viewportMax.x - viewportMin.x),
        viewportMin.y + (static_cast<float>(cubeY) + 0.5f) / static_cast<float>(preview.height) * (viewportMax.y - viewportMin.y));
    const ImVec2 windowBefore = fixture.WindowPosition();
    fixture.Click(cubeOnScreen);
    Require(fixture.window.SelectedSlot() == 1, "a click on the cube in the viewport selects its slot");
    Require(fixture.WindowPosition().x == windowBefore.x && fixture.WindowPosition().y == windowBefore.y, "clicking the viewport leaves the window where it is");
    fixture.window.FinishPreview();
    fixture.Frames(2);
    fixture.Frame("material_editor_checker_slot.png");

    // A drag on the graph's background box-selects and never moves the window.
    fixture.window.SelectSlot(0);
    fixture.Frames(2);
    const MaterialShaderGraph& graph = fixture.Graph();
    const ImVec2 empty = EmptyGraphPoint(fixture.window, graph);
    fixture.Drag(empty, ImVec2(empty.x + 30.0f, empty.y + 30.0f));
    Require(fixture.WindowPosition().x == windowBefore.x && fixture.WindowPosition().y == windowBefore.y, "dragging the graph's background leaves the window where it is");

    // Clicking a node's title selects it; dragging it moves it, as one undo step.
    const uint32_t output = fixture.NodeOfType(MaterialShaderNodeType::Output);
    Require(output != 0, "the graph has an Output node");
    const std::optional<ImVec2> title = fixture.window.GraphCanvas().NodeTitleOnScreen(output);
    Require(title.has_value(), "the Output node is drawn");
    fixture.Click(*title);
    Require(fixture.window.GraphSelection().nodes == std::set<uint32_t>{output}, "clicking the Output node selects it");
    fixture.Frame("material_editor_output_selected.png");
    const MaterialGraphNodePosition before = FindMaterialGraphNode(fixture.Graph(), output)->position;
    fixture.Drag(*title, ImVec2(title->x + 90.0f, title->y + 45.0f));
    const MaterialGraphNodePosition after = FindMaterialGraphNode(fixture.Graph(), output)->position;
    Require(after.x > before.x + 10.0f && after.y > before.y + 5.0f, "dragging the node moves it");
    Require(fixture.window.IsDirty(), "moving a node is an edit");
    Require(fixture.window.Undo(), "the move can be undone");
    const MaterialGraphNodePosition undone = FindMaterialGraphNode(fixture.Graph(), output)->position;
    Require(undone.x == before.x && undone.y == before.y, "one undo puts the whole drag back");
    Require(fixture.window.Redo(), "and redone");

    // A right click on the background opens the add-node menu; typing and Enter adds that node.
    const size_t nodes = fixture.Graph().nodes.size();
    const ImVec2 menuPoint = EmptyGraphPoint(fixture.window, fixture.Graph());
    fixture.Click(menuPoint, 1);
    Require(ImGui::GetCurrentContext()->OpenPopupStack.Size > 0, "a right click on the graph opens its menu");
    fixture.Frame("material_editor_add_menu.png");
    ImGui::GetIO().AddInputCharactersUTF8("scalar");
    fixture.Frame();
    fixture.Key(ImGuiKey_Enter);
    Require(fixture.Graph().nodes.size() == nodes + 1, "Enter adds the node the search found");
    const uint32_t scalar = fixture.Graph().nodes.back().id;
    Require(fixture.Graph().nodes.back().type == MaterialShaderNodeType::Scalar, "a Scalar node");
    Require(fixture.window.GraphSelection().nodes == std::set<uint32_t>{scalar}, "the new node is selected");

    // Delete, with the graph focused, deletes the selected node (not the scene's selection).
    fixture.Frames(2);
    const std::optional<ImVec2> scalarTitle = fixture.window.GraphCanvas().NodeTitleOnScreen(scalar);
    Require(scalarTitle.has_value(), "the new node is drawn");
    fixture.Click(*scalarTitle);
    fixture.Key(ImGuiKey_Delete);
    Require(fixture.Graph().nodes.size() == nodes, "Delete removes the selected node");
    Require(!fixture.result.actions.deleteSelectedSceneEntity, "and asks the scene for nothing");

    // Ctrl+Z brings it back.
    fixture.Key(ImGuiKey_Z, ImGuiMod_Ctrl);
    Require(fixture.Graph().nodes.size() == nodes + 1, "Ctrl+Z undoes the delete");

    // The wheel over the graph zooms it and scrolls nothing.
    fixture.MoveMouse(menuPoint);
    const std::optional<ImVec2> outputBefore = fixture.window.GraphCanvas().NodeTitleOnScreen(output);
    ImGui::GetIO().AddMouseWheelEvent(0.0f, 2.0f);
    fixture.Frames(2);
    const std::optional<ImVec2> outputAfter = fixture.window.GraphCanvas().NodeTitleOnScreen(output);
    Require(outputBefore.has_value() && outputAfter.has_value() &&
                (std::abs(outputAfter->x - outputBefore->x) > 1.0f || std::abs(outputAfter->y - outputBefore->y) > 1.0f),
            "the wheel zooms the graph");
    fixture.Frame("material_editor_zoomed.png");
}

void TestSnapshotModel()
{
    const char* path = std::getenv("MINIENGINE_EDITOR_TEST_MODEL");
    if (path == nullptr)
    {
        return;
    }
    Fixture fixture;
    EditorContext context = fixture.Context();
    fixture.window.OpenModel(context, path, true);
    fixture.Frames(3);
    fixture.window.FinishPreview();
    fixture.Frames(3);
    fixture.Frame("material_editor_model.png");
    // Selecting a node shows its details.
    const MaterialShaderGraph& graph = fixture.Graph();
    for (const MaterialShaderNode& node : graph.nodes)
    {
        if (node.type == MaterialShaderNodeType::Texture)
        {
            if (const std::optional<ImVec2> title = fixture.window.GraphCanvas().NodeTitleOnScreen(node.id))
            {
                fixture.Click(*title);
                fixture.window.FinishPreview();
                fixture.Frames(2);
                fixture.Frame("material_editor_model_texture_node.png");
            }
            break;
        }
    }
}
}

int main()
{
    TaskSystem::Initialize();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    int result = 0;
    try
    {
        const ScopedDirectory directory;
        TestEditing(WriteTestModel(directory.path));
        TestSnapshotModel();
        std::cout << "material_editor_window_tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "material_editor_window_tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    TaskSystem::Shutdown();
    return result;
}
