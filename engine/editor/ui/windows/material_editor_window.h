#pragma once

#include <engine/asset/model_loader.h>
#include <engine/editor/material_preview/material_preview_renderer.h>
#include <engine/editor/material_preview/material_preview_shapes.h>
#include <engine/editor/ui/editor_material_graph.h>
#include <engine/editor/ui/editor_user_texture.h>
#include <engine/editor/ui/framework/editor_window.h>
#include <engine/scene/scene_components.h>

#include <imgui.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace me
{

// The Material Editor (docs/design/2026-10-09-material-editor-redesign-design.md): one model's
// material slots, each a shader graph, laid out as Unreal's material editor is:
//
//   toolbar
//   Viewport  | Graph | Material Slots
//   Details   |       | Palette
//   status bar
//
// The viewport shows the model, or a preview shape in the selected slot's material, through the
// preview renderer (engine/editor/material_preview); edits show there at once and, with Live Preview,
// on the scene's copies of the model; Save writes them beside the model. A floating tool window,
// opened from an asset or the Scene panel's Edit Materials, not from the Window menu.
class MaterialEditorWindow final : public EditorWindow
{
  public:
    MaterialEditorWindow();
    ~MaterialEditorWindow() override;

    // Opens the window on a model, dropping what the scene previews of the one open before; with
    // preselectPaint the slot most like car paint is selected.
    void OpenModel(EditorContext& context, const std::string& modelPath, bool preselectPaint = false);
    // Follows a rename of the model, or of a folder it is in, so edits are saved beside it.
    void OnAssetRenamed(const std::string& oldPath, const std::string& newPath);

    // Closes the window once its model is gone from disk; lets go of retired textures.
    void Tick(EditorContext& context) override;
    // However it is closed, the scene drops the edits that were not saved.
    void OnClose(EditorContext& context) override;

    // For tests: the model's slots as edited, the slot shown, and the window's edits.
    const std::vector<ModelImportedMaterialInfo>& Materials() const
    {
        return m_materials;
    }
    int SelectedSlot() const
    {
        return m_selectedSlot;
    }
    void SelectSlot(int slot);
    bool IsDirty() const
    {
        return !m_editedSlots.empty();
    }
    bool Undo();
    bool Redo();
    // Waits for the preview's hierarchy, textures and samples (tests and scripted captures).
    void FinishPreview();
    const MaterialPreviewFrame& PreviewFrame() const
    {
        return m_preview.Frame();
    }
    const MaterialGraphCanvas& GraphCanvas() const
    {
        return m_graph;
    }
    const MaterialGraphSelection& GraphSelection() const
    {
        return m_slotUi[static_cast<size_t>(m_selectedSlot)].selection;
    }
    // The preview's picture on screen as last drawn.
    ImVec2 ViewportMin() const
    {
        return m_viewportMin;
    }
    ImVec2 ViewportMax() const
    {
        return m_viewportMax;
    }

  protected:
    void OnGui(EditorContext& context) override;
    void PreBegin(EditorContext& context) override;
    ImGuiWindowFlags GetWindowFlags(const EditorContext& context) const override;

  private:
    // A slot's state an undo goes back to.
    struct SlotState
    {
        ModelImportedMaterialInfo material;
        bool asImported = true;
        std::optional<ModelImportedMaterialInfo> stashedEdit;
    };
    // Each slot's graph view and selection, and its undo history.
    struct SlotUi
    {
        MaterialGraphView view;
        MaterialGraphSelection selection;
        std::vector<SlotState> undo;
        std::vector<SlotState> redo;
        // A graph made from the imported material rather than read from a definition: laid out in
        // columns the first time it is shown (where it sits is not an edit).
        bool arrangeOnShow = false;
    };

    void LoadModel(const std::string& modelPath, bool preselectPaint);
    void Reset();
    void RevertScenePreview(EditorContext& context);

    // The layout's parts.
    void DrawToolbar(EditorContext& context);
    void DrawViewport(EditorContext& context);
    void DrawViewportOverlay(const ImVec2& min, const ImVec2& max);
    void DrawDetails(EditorContext& context);
    void DrawGraph(EditorContext& context);
    void DrawSlots(EditorContext& context);
    void DrawPalette(EditorContext& context);
    void DrawStatusBar();
    void HandleShortcuts(EditorContext& context);

    // Details for what the graph has selected; each returns whether the material changed.
    bool DrawMaterialDetails(ModelImportedMaterialInfo& material);
    bool DrawNodeDetails(ModelImportedMaterialInfo& material, MaterialShaderNode& node);
    bool DrawLinkDetails(ModelImportedMaterialInfo& material, const MaterialShaderLink& link);

    // The edit flow: the slot's state before this frame's edits, and an open undo step.
    SlotState CaptureSlot(int slot) const;
    void RestoreSlot(int slot, const SlotState& state);
    void BeginFrameEdits();
    void EndFrameEdits(EditorContext& context, bool changed, bool dragging);
    // A slot's material changed: recompiled, marked edited, previewed here and in the scene.
    void OnMaterialChanged(EditorContext& context, int slot);
    void SetSlotAsImported(EditorContext& context, int slot, bool asImported);
    void Save(EditorContext& context);

    // The preview: its geometry (built in the background), materials and image.
    void UpdatePreview(bool interacting, uint32_t width, uint32_t height);
    void RequestPreviewGeometry();
    void PollPreviewGeometry();
    void RebuildPreviewMaterials();
    MaterialPreviewCamera& ActiveCamera();
    void FramePreview();
    std::string ResolveTexturePath(const std::string& path) const;
    ImTextureRef Thumbnail(const std::string& path);

    std::string m_modelPath;
    std::string m_displayName;
    std::string m_loadError;
    std::shared_ptr<const LoadedModelData> m_model;
    std::vector<ModelImportedMaterialInfo> m_materials;
    // Each slot as the model's import made it, without the definitions saved beside the model.
    std::vector<ModelImportedMaterialInfo> m_importedMaterials;
    // Per slot: it shows its imported material, not an edit; Save removes its definition.
    std::vector<bool> m_slotAsImported;
    // Per slot: the edit set aside while the slot shows its imported material, to switch back to.
    std::vector<std::optional<ModelImportedMaterialInfo>> m_stashedEdits;
    // The slots changed since the last save; previewed in the scene, written by Save.
    std::set<uint32_t> m_editedSlots;
    // Per slot: how many of the model's submeshes and triangles draw with it.
    std::vector<uint32_t> m_slotSubmeshes;
    std::vector<uint64_t> m_slotTriangles;
    std::vector<SlotUi> m_slotUi;
    int m_selectedSlot = 0;
    char m_slotFilter[64] = {};
    char m_paletteFilter[64] = {};

    // Edits show in the scene as they are made.
    bool m_livePreview = true;
    // The scene shows edits that are not saved, to be reverted when they are dropped.
    bool m_scenePreviewed = false;
    // Quick Edit's base colour brightness, kept between frames so dragging it below 1 does not fold
    // it back into the colour.
    float m_quickEditBrightness = 0.0f;
    double m_lastExistsCheckTime = -1.0e9;

    // The undo step this frame's edits go into, and whether one is open (a drag still going on).
    std::optional<SlotState> m_frameStart;
    bool m_undoStepOpen = false;

    // Status line: the last message and when it was set.
    std::string m_status;
    std::chrono::steady_clock::time_point m_statusTime{};
    void SetStatus(std::string message);

    // Panel sizes in points (the columns' widths) and fractions (the rows').
    float m_leftWidth = 0.0f;
    float m_rightWidth = 0.0f;
    float m_viewportFraction = 0.55f;
    float m_slotsFraction = 0.5f;

    MaterialGraphCanvas m_graph;
    bool m_graphFocused = false;
    bool m_viewportHovered = false;
    // A Texture node a file is being picked for.
    uint32_t m_pickTextureNode = 0;
    bool m_pickTextureRequested = false;
    char m_nodeName[128] = {};
    uint32_t m_nodeNameFor = 0;

    // The preview.
    MaterialPreviewRenderer m_preview;
    MaterialPreviewTextureCache m_textures;
    MaterialPreviewShape m_shape = MaterialPreviewShape::Model;
    MaterialPreviewSettings m_previewSettings;
    bool m_modelFloor = true;
    bool m_shapeFloor = false;
    bool m_outline = true;
    MaterialPreviewCamera m_modelCamera;
    MaterialPreviewCamera m_shapeCamera;
    struct GeometryJob;
    std::unique_ptr<GeometryJob> m_geometryJob;
    std::shared_ptr<const MaterialPreviewGeometry> m_modelGeometry;
    std::array<std::shared_ptr<const MaterialPreviewGeometry>, kMaterialPreviewShapeCount> m_shapeGeometry{};
    bool m_previewMaterialsDirty = true;
    EditorUserTexture m_previewImage;
    ImVec2 m_viewportMin{0.0f, 0.0f};
    ImVec2 m_viewportMax{0.0f, 0.0f};
    // The preview's size in pixels as last drawn.
    uint32_t m_previewWidth = 0;
    uint32_t m_previewHeight = 0;
    // Mouse drags in the viewport: which button, and whether it moved (a click picks a slot).
    int m_viewportDragButton = -1;
    bool m_viewportDragMoved = false;
    std::unordered_map<std::string, std::unique_ptr<EditorUserTexture>> m_thumbnails;
};
}
