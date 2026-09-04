#include <engine/editor/editor_ui.h>

#include <engine/asset/material_graph_runtime.h>
#include <engine/asset/model_loader.h>
#include <engine/asset/texture_loader.h>

#include <engine/logic/editor_world.h>
#include <engine/platform/file_dialog/file_dialog.h>
#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/platform/ui/ui_scale.h>
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

namespace me
{

void EditorUiController::DrawAssetBrowserPanel(EditorUiFrameResult& result)
{
    if (!m_assetManager.has_value())
    {
        m_assetManager.emplace(EnginePaths::AssetsRoot());
    }
    if (ImGui::Begin("Assets", &m_showAssetManagerWindow))
    {
        const AssetManagerResult assetResult = m_assetManager->Draw();

        if (assetResult.wantsImportModel)
        {
            if (const std::optional<std::string> sourcePath = OpenModelFileDialog(); sourcePath.has_value())
            {
                // The import runs on a background thread; the backend calls
                // RequestAssetBrowserRefresh() once the files are on disk.
                result.actions.importedModelRequest = EditorUiActions::ImportedModelRequest{
                    *sourcePath,
                    m_assetManager->GetCurrentDirectory().string()};
            }
        }
        if (assetResult.selectedModelPath.has_value())
        {
            result.actions.selectedModelPath = assetResult.selectedModelPath;
        }
        for (const std::string& path : assetResult.batchLoadModelPaths)
        {
            result.actions.batchLoadModelPaths.push_back(path);
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
                assetResult.pasteRequest->destinationDirectory};
            m_assetManager->Refresh();
        }
    }
    ImGui::End();
}
}
