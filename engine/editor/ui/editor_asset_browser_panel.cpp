#include "editor_ui.h"

#include <material_graph_runtime.h>
#include <model_loader.h>
#include <texture_loader.h>

#include <editor_world.h>
#include <file_dialog/file_dialog.h>
#include <log/log.h>
#include <ui/ui_scale.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <yaml-cpp/yaml.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

void EditorUiController::DrawAssetBrowserPanel(EditorUiFrameResult& result)
{
    if (!m_assetManager.has_value())
    {
        m_assetManager.emplace(std::filesystem::path(MINIENGINE_PROJECT_DIR) / "assets");
    }
    if (ImGui::Begin("Assets", &m_showAssetManagerWindow))
    {
        const AssetManagerResult assetResult = m_assetManager->Draw();

        if (assetResult.wantsImportModel)
        {
            if (const std::optional<std::string> sourcePath = OpenModelFileDialog(); sourcePath.has_value())
            {
                result.actions.importedModelRequest = EditorUiActions::ImportedModelRequest{
                    *sourcePath,
                    m_assetManager->GetCurrentDirectory().string()
                };
                m_assetManager->Refresh();
            }
        }
        if (assetResult.selectedModelPath.has_value())
        {
            result.actions.selectedModelPath = assetResult.selectedModelPath;
        }
        if (!assetResult.deleteRequests.empty())
        {
            for (const std::string& path : assetResult.deleteRequests)
            {
                result.actions.deleteAssetPaths.push_back(path);
            }
            m_assetManager->Refresh();
        }
        if (assetResult.pasteRequest.has_value())
        {
            result.actions.pastedAsset = EditorUiActions::AssetPasteRequest{
                assetResult.pasteRequest->sourcePath,
                assetResult.pasteRequest->destinationDirectory
            };
            m_assetManager->Refresh();
        }
    }
    ImGui::End();
}
