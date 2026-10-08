#pragma once

#include <engine/asset/tyre_library.h>
#include <engine/editor/ui/framework/editor_panel.h>

#include <string>
#include <vector>

namespace me
{

class IEditorWorld;
struct EditorVehicleSettings;

// Play mode's car: drive the selected model, its telemetry, controls, camera, gamepad feedback,
// physics overlay and tuning (EditorSharedState::vehicle).
class VehiclePanel final : public EditorPanel
{
  public:
    VehiclePanel();

  protected:
    void OnGui(EditorContext& context) override;

  private:
    // The selected car's tyres by wheel, and library tyres to fit in their place.
    void DrawTyres(const IEditorWorld& scene, EditorVehicleSettings& vehicle, bool driving);

    std::vector<TyreLibrary::Entry> m_tyreLibrary;
    bool m_tyreLibraryLoaded = false;
    bool m_pairAxles = true;
    std::string m_tyreStatus;
};
}
