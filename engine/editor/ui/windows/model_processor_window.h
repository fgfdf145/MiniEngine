#pragma once

#include <engine/asset/model_loader.h>
#include <engine/editor/ui/framework/editor_window.h>
#include <engine/scene/scene_components.h>

#include <imgui.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace me
{

// The Model Preview window: one imported model's materials, edited as shader graphs, previewed in
// the window and (live) in the scene, and saved next to the model. A floating tool window, opened
// from an asset or the Scene panel's Edit Materials, not from the Window menu.
class ModelProcessorWindow final : public EditorWindow
{
  public:
    ModelProcessorWindow();

    // Opens the window on a model, dropping what the scene previews of the one open before; with
    // preselectPaint the slot most like car paint is selected.
    void OpenModel(EditorContext& context, const std::string& modelPath, bool preselectPaint = false);
    // Follows a rename of the model, or of a folder it is in, so edits are saved beside it.
    void OnAssetRenamed(const std::string& oldPath, const std::string& newPath);

    // Closes the window once its model is gone from disk.
    void Tick(EditorContext& context) override;
    // However it is closed, the scene drops the edits that were not saved.
    void OnClose(EditorContext& context) override;

  protected:
    void OnGui(EditorContext& context) override;
    void PreBegin(EditorContext& context) override;

  private:
    // Loads the model and its materials; the window's state starts over.
    void LoadModel(const std::string& modelPath, bool preselectPaint);
    void Reset();
    // The previewed edits back to what is on disk, when there are any.
    void RevertScenePreview(EditorContext& context);

    // The graph section for one material: toolbar, selection and canvas. Each returns whether the
    // graph changed.
    bool DrawMaterialGraphEditor(ModelImportedMaterialInfo& material, size_t materialIndex);
    bool DrawMaterialGraphCanvas(ModelImportedMaterialInfo& material, size_t materialIndex);
    bool DrawMaterialGraphAddNodePopup(
        ModelImportedMaterialInfo& material,
        bool canPasteClipboardNode,
        std::optional<MaterialGraphNodePosition>& pendingPasteNodePosition);
    void UpdateMaterialGraphView(const ImVec2& canvasOrigin, bool canvasBackgroundHovered);

    std::string m_modelPath;
    std::string m_displayName;
    std::string m_statusMessage;
    LoadedModelData m_loadedModel;
    std::vector<ModelImportedMaterialInfo> m_materials;
    int m_selectedMaterialIndex = 0;
    int m_selectedUvSubmeshIndex = 0;
    bool m_dirty = false;
    // The slots changed since the last save; previewed in the scene, written by Save.
    std::set<uint32_t> m_editedSlots;
    // Edits show in the scene as they are made.
    bool m_livePreview = true;
    // The scene shows edits that are not saved, to be reverted when they are dropped.
    bool m_scenePreviewed = false;
    // Quick Edit's base colour brightness, kept between frames so dragging it below 1 does not
    // fold it back into the colour.
    float m_quickEditBrightness = 1.0f;
    double m_lastExistsCheckTime = -1.0e9;
    // Its preview camera, orbiting the model.
    struct PreviewCamera
    {
        float yaw = 0.55f;
        float pitch = 0.35f;
        float distance = 3.0f;
        bool autoFramePending = false;
    };
    PreviewCamera m_modelPreview;
    // Its material graph canvas: selection, drags and view. Reset whenever a model is opened or
    // closed, by assigning a default one.
    struct MaterialGraphCanvas
    {
        uint32_t selectedNodeId = 0;
        uint32_t selectedLinkId = 0;
        MaterialGraphNodePosition viewOrigin{};
        float zoom = 1.0f;
        bool panningActive = false;
        // Where a node added from the context menu goes.
        MaterialGraphNodePosition contextSpawnPosition{};
        bool openAddNodePopup = false;
        bool linkDragActive = false;
        uint32_t linkDragFromNodeId = 0;
        std::string linkDragFromSlot;
        bool nodeResizeActive = false;
        uint32_t resizeNodeId = 0;
        uint8_t resizeEdges = 0;
        MaterialGraphNodePosition resizeStartPosition{};
        ImVec2 resizeStartMouse{0.0f, 0.0f};
        ImVec2 resizeStartSize{0.0f, 0.0f};

        void CancelLinkDrag()
        {
            linkDragActive = false;
            linkDragFromNodeId = 0;
            linkDragFromSlot.clear();
        }
        void CancelResize()
        {
            nodeResizeActive = false;
            resizeNodeId = 0;
            resizeEdges = 0;
        }
    };
    MaterialGraphCanvas m_materialGraph;
    // A copied node. Kept across models, so it can be pasted into another one.
    std::optional<MaterialShaderNode> m_materialGraphClipboardNode;
};
}
