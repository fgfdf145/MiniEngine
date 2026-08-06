#include "model_import_service.h"

#include "scene_renderables.h"

#include <renderer_shared_state.h>

#include <asset_registry.h>
#include <log/log.h>
#include <material_definition.h>
#include <material_graph_runtime.h>
#include <model_cache.h>
#include <model_loader.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace
{
// Writes a single material's YAML to disk. Returns the output path on success.
std::optional<std::filesystem::path> WriteMaterialYamlFile(
    const std::filesystem::path& modelPath,
    uint32_t materialIndex,
    const ModelImportedMaterialInfo& material)
{
    const std::filesystem::path outPath = BuildMaterialDefinitionPath(modelPath, materialIndex);

    YAML::Node root(YAML::NodeType::Map);
    root["material"] = SerializeMaterialDefinition(material);

    std::ofstream outFile(outPath);
    if (!outFile)
    {
        LOG_ERROR("Failed to open material file for writing: '{}'", outPath.string());
        return std::nullopt;
    }
    outFile << root;
    return outPath;
}
}

namespace ModelImportService
{
std::string ImportModelIntoAssetDirectory(const std::string& sourcePath, const std::string& destinationDirectory)
{
    const std::filesystem::path src = std::filesystem::path(sourcePath);
    if (!std::filesystem::exists(src))
    {
        throw std::runtime_error("Source file does not exist: " + sourcePath);
    }

    // Each import gets its own folder named after the model; a .gltf's
    // companion files are sorted into subfolders (buffers/, textures/) with
    // the glTF's URIs rewritten to match. Importing directly into a folder
    // that already carries the model's name reuses it instead of nesting
    // another level.
    const std::filesystem::path dstDir = std::filesystem::path(destinationDirectory);
    const std::filesystem::path modelFolder =
        dstDir.filename() == src.stem() ? dstDir : dstDir / src.stem();

    std::error_code mkdirEc;
    std::filesystem::create_directories(modelFolder, mkdirEc);
    if (mkdirEc)
    {
        throw std::runtime_error(
            "Failed to create model folder '" + modelFolder.string() + "': " + mkdirEc.message());
    }

    const std::filesystem::path dst = ModelLoader::CopyModelWithSortedReferences(src, modelFolder);

    // Register the freshly imported bundle (model + copied textures) so it has
    // stable uuids from the very first reference.
    AssetRegistry::RescanAssetTree();

    LOG_INFO("Imported model '{}' -> '{}'", src.string(), dst.string());
    return dst.string();
}

void StartAsyncImport(RendererSharedState& state, const std::string& sourcePath, const std::string& destinationDirectory)
{
    if (state.asyncImport.IsLoading())
    {
        throw std::runtime_error("Another asset import is in progress. Please wait.");
    }

    state.asyncImport.sourcePath = sourcePath;
    state.asyncImport.destinationDirectory = destinationDirectory;
    state.asyncImport.future = std::async(std::launch::async, [sourcePath, destinationDirectory]()
                                          {
                                              return ImportModelIntoAssetDirectory(sourcePath, destinationDirectory);
                                          });

    LOG_INFO("Started async import: {} -> {}", sourcePath, destinationDirectory);
}

void PumpAsyncImport(RendererSharedState& state)
{
    if (!state.asyncImport.IsActive() || state.asyncImport.IsLoading())
    {
        return;
    }

    try
    {
        state.asyncImport.future.get();
        state.lastModelLoadError.clear();
    }
    catch (const std::exception& error)
    {
        state.lastModelLoadError = error.what();
        LOG_ERROR(
            "Failed to import model '{}' into '{}': {}",
            state.asyncImport.sourcePath,
            state.asyncImport.destinationDirectory,
            error.what());
    }

    // Whether it succeeded or failed, rescan so the browser reflects whatever
    // ended up on disk.
    state.editorUi.RequestAssetBrowserRefresh();
}

void DeleteAssetPath(const std::string& path)
{
    // Drop cached model data first, while the on-disk path still exists and
    // canonicalizes to the same key the cache was populated with.
    ModelCache::Invalidate(path);

    const std::filesystem::path target(path);
    std::error_code ec;

    // Deleting a model also removes its "<stem>_<index>.material.yaml"
    // sidecars: they are meaningless without the model and would otherwise be
    // left behind as orphans.
    if (!std::filesystem::is_directory(target, ec) && ModelLoader::IsSupportedModelPath(target))
    {
        const std::string sidecarPrefix = target.stem().string() + "_";
        constexpr std::string_view kSidecarSuffix = ".material.yaml";

        std::error_code iterEc;
        for (const auto& item : std::filesystem::directory_iterator(target.parent_path(), iterEc))
        {
            if (iterEc)
            {
                break;
            }
            const std::string name = item.path().filename().string();
            if (!name.starts_with(sidecarPrefix) || !name.ends_with(kSidecarSuffix))
            {
                continue;
            }
            const std::string indexPart =
                name.substr(sidecarPrefix.size(), name.size() - sidecarPrefix.size() - kSidecarSuffix.size());
            const bool isMaterialIndex =
                !indexPart.empty() &&
                std::all_of(indexPart.begin(), indexPart.end(), [](unsigned char c)
                            {
                                return std::isdigit(c) != 0;
                            });
            if (!isMaterialIndex)
            {
                continue;
            }

            std::error_code removeEc;
            std::filesystem::remove(item.path(), removeEc);
            if (removeEc)
            {
                LOG_WARN("Could not delete material sidecar '{}': {}", item.path().string(), removeEc.message());
            }
            else
            {
                LOG_INFO("Deleted material sidecar: {}", item.path().string());
            }
        }
    }

    std::filesystem::remove_all(target, ec);
    if (ec)
    {
        throw std::runtime_error("Failed to delete '" + path + "': " + ec.message());
    }

    // Drop registry entries and the now-orphaned uuid sidecar.
    AssetRegistry::OnAssetRemoved(target);

    LOG_INFO("Deleted asset: {}", path);
}

void PasteAsset(const std::string& sourcePath, const std::string& destinationDirectory)
{
    const std::filesystem::path src = std::filesystem::path(sourcePath);
    const std::filesystem::path dst = std::filesystem::path(destinationDirectory) / src.filename();
    std::error_code eqEc;
    if (std::filesystem::equivalent(src, dst, eqEc) && !eqEc)
    {
        return;
    }
    std::error_code ec;
    std::filesystem::copy(src, dst,
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing,
                          ec);
    if (ec)
    {
        throw std::runtime_error(
            "Failed to copy '" + sourcePath + "' to '" + destinationDirectory + "': " + ec.message());
    }

    // Copied uuid sidecars would duplicate their source's identity, and rescan
    // order must not decide who keeps the uuid: strip the sidecars from the
    // copy so the originals stay authoritative and the copies get fresh uuids.
    std::error_code sidecarEc;
    if (std::filesystem::is_directory(dst, sidecarEc))
    {
        for (std::filesystem::recursive_directory_iterator
                 it(dst, std::filesystem::directory_options::skip_permission_denied, sidecarEc),
             end;
             !sidecarEc && it != end;
             it.increment(sidecarEc))
        {
            std::error_code fileEc;
            if (it->is_regular_file(fileEc) &&
                it->path().filename().string().ends_with(".miniengine_asset.yaml"))
            {
                std::error_code removeEc;
                std::filesystem::remove(it->path(), removeEc);
            }
        }
    }
    AssetRegistry::RescanAssetTree();

    LOG_INFO("Copied asset '{}' -> '{}'", sourcePath, dst.string());
}

void UpdateImportedMaterialDefinition(
    RendererSharedState& state,
    const std::string& modelPath,
    uint32_t materialIndex,
    const ModelImportedMaterialInfo& material)
{
    if (modelPath.empty())
    {
        return;
    }

    // Update the single material at the given index in the model cache.
    std::shared_ptr<LoadedModelData> cached = ModelCache::Get(modelPath);
    if (cached && materialIndex < cached->materials.size())
    {
        ApplyImportedMaterialInfo(material, cached->materials[materialIndex]);
    }

    WriteMaterialYamlFile(
        std::filesystem::path(modelPath),
        materialIndex,
        material);

    MarkModelRenderablesDirtyForSourcePath(state, modelPath);
    RefreshDirtySceneRenderables(state);
    LOG_INFO(
        "Updated material {} for model '{}'",
        materialIndex,
        modelPath);
}

void UpdateImportedModelMaterialDefinitions(
    RendererSharedState& state,
    const std::string& modelPathString,
    const std::vector<ModelImportedMaterialInfo>& materials)
{
    if (modelPathString.empty() || materials.empty())
    {
        return;
    }

    const std::filesystem::path modelPath(modelPathString);

    // Propagate user edits into the cached raw model data so that
    // Dirty renderable refresh picks up the new blend graphs and PBR factors.
    std::shared_ptr<LoadedModelData> cached = ModelCache::Get(modelPathString);
    if (cached)
    {
        const size_t count = std::min(materials.size(), cached->materials.size());
        for (size_t i = 0; i < count; ++i)
        {
            ApplyImportedMaterialInfo(materials[i], cached->materials[i]);
        }
    }

    // Persist each material as a sidecar .material.yaml file alongside the model.
    for (size_t i = 0; i < materials.size(); ++i)
    {
        const auto outPath = WriteMaterialYamlFile(modelPath, static_cast<uint32_t>(i), materials[i]);
        if (outPath.has_value())
        {
            LOG_INFO("Saved material '{}' -> '{}'", materials[i].name, outPath->string());
        }
    }

    MarkModelRenderablesDirtyForSourcePath(state, modelPathString);
    RefreshDirtySceneRenderables(state);
    LOG_INFO(
        "Saved {} material(s) for model '{}'",
        materials.size(),
        modelPathString);
}

}
