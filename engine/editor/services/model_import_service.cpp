#include "model_import_service.h"

#include "scene_renderables.h"

#include <engine/editor/renderer_shared_state.h>

#include <engine/asset/asset_paths.h>
#include <engine/asset/asset_registry.h>
#include <engine/core/file/atomic_file.h>
#include <engine/core/log/log.h>
#include <engine/asset/material_definition.h>
#include <engine/asset/material_graph_runtime.h>
#include <engine/asset/model_cache.h>
#include <engine/asset/model_import_target.h>
#include <engine/asset/model_loader.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <future>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace me
{

namespace
{
// Writes a single material's YAML to disk atomically, so a failed save keeps
// the previous edits. Returns the output path; throws when it cannot be saved.
std::filesystem::path WriteMaterialYamlFile(
    const std::filesystem::path& modelPath,
    uint32_t materialIndex,
    const ModelImportedMaterialInfo& material)
{
    const std::filesystem::path outPath = BuildMaterialDefinitionPath(modelPath, materialIndex);

    YAML::Node root(YAML::NodeType::Map);
    root["material"] = SerializeMaterialDefinition(material);
    YAML::Emitter emitter;
    emitter << root;

    std::string error;
    if (!AtomicFile::Write(outPath, emitter.c_str(), &error))
    {
        throw std::runtime_error("Material edit applied but not saved: " + error);
    }
    return outPath;
}
}

namespace ModelImportService
{
std::string ImportModelIntoAssetDirectory(
    const std::string& sourcePath,
    const std::string& destinationDirectory,
    ImportConflictPolicy policy)
{
    const std::filesystem::path src = std::filesystem::path(sourcePath);
    if (!std::filesystem::exists(src))
    {
        throw std::runtime_error("Source file does not exist: " + sourcePath);
    }

    // Each import gets its own folder named after the model; a .gltf's
    // companion files are sorted into subfolders (buffers/, textures/) with
    // the glTF's URIs rewritten to match.
    std::filesystem::path modelFolder =
        ModelImportTarget::DefaultFolder(src, std::filesystem::path(destinationDirectory));
    const bool occupied = ModelImportTarget::IsOccupied(modelFolder);
    if (occupied && policy == ImportConflictPolicy::FailIfExists)
    {
        throw std::runtime_error("'" + modelFolder.string() + "' already exists");
    }
    if (occupied && policy == ImportConflictPolicy::KeepBoth)
    {
        modelFolder = ModelImportTarget::NextFreeFolder(modelFolder);
    }
    const bool overwrite = occupied && policy == ImportConflictPolicy::Overwrite;

    // An overwrite imports into a staging sibling first, so a failed import
    // leaves the existing model untouched.
    const std::filesystem::path copyFolder =
        overwrite ? ModelImportTarget::StagingFolderFor(modelFolder) : modelFolder;

    std::error_code mkdirEc;
    std::filesystem::create_directories(copyFolder, mkdirEc);
    if (mkdirEc)
    {
        throw std::runtime_error(
            "Failed to create model folder '" + copyFolder.string() + "': " + mkdirEc.message());
    }

    std::filesystem::path dst;
    try
    {
        dst = ModelLoader::CopyModelWithSortedReferences(src, copyFolder);
    }
    catch (...)
    {
        if (overwrite)
        {
            std::error_code cleanupEc;
            std::filesystem::remove_all(copyFolder, cleanupEc);
        }
        throw;
    }

    if (overwrite)
    {
        ModelImportTarget::ReplaceContents(modelFolder, copyFolder);
        dst = modelFolder / dst.filename();
        // Parsed data of the replaced model is keyed on the same path.
        ModelCache::Invalidate(modelFolder.string());
    }

    // Register the freshly imported bundle (model + copied textures) so it has
    // stable uuids from the very first reference; this also prunes the uuid
    // sidecars of files an overwrite removed.
    AssetRegistry::RescanAssetTree();

    LOG_INFO("Imported model '{}' -> '{}'", src.string(), dst.string());
    return dst.string();
}

void StartAsyncImport(
    RendererSharedState& state,
    const std::string& sourcePath,
    const std::string& destinationDirectory,
    ImportConflictPolicy policy)
{
    if (state.asyncImport.IsLoading())
    {
        throw std::runtime_error("Another asset import is in progress. Please wait.");
    }

    state.asyncImport.sourcePath = sourcePath;
    state.asyncImport.destinationDirectory = destinationDirectory;
    state.asyncImport.future = std::async(std::launch::async, [sourcePath, destinationDirectory, policy]()
                                          {
                                              return ImportModelIntoAssetDirectory(
                                                  sourcePath, destinationDirectory, policy);
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
        const std::string importedPath = state.asyncImport.future.get();
        state.lastModelLoadError.clear();

        // An overwrite replaced a model the scene may already use; placed
        // instances pick up the new mesh and materials. A fresh import has no
        // instances yet, so this does nothing.
        MarkModelRenderablesDirtyForSourcePath(state, importedPath);
        RefreshDirtySceneRenderables(state);
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
    const bool isModelFile =
        !std::filesystem::is_directory(target, ec) && ModelLoader::IsSupportedModelPath(target);
    // Listed before the model goes; the listing only needs the name.
    const std::vector<std::filesystem::path> materialDefinitions =
        isModelFile ? FindMaterialDefinitionFiles(target) : std::vector<std::filesystem::path>{};

    std::filesystem::remove_all(target, ec);
    if (ec)
    {
        // A folder may be partly gone: resync the registry with what is left.
        AssetRegistry::RescanAssetTree();
        throw std::runtime_error("Failed to delete '" + path + "': " + ec.message());
    }

    // The model's material edits go only once the model itself is gone: when
    // its deletion fails (e.g. the file is open elsewhere) the edits survive.
    for (const std::filesystem::path& definition : materialDefinitions)
    {
        std::error_code removeEc;
        std::filesystem::remove(definition, removeEc);
        if (removeEc)
        {
            LOG_WARN("Could not delete material definition '{}': {}", definition.string(), removeEc.message());
        }
        else
        {
            LOG_INFO("Deleted material definition: {}", definition.string());
        }
    }

    // Drop registry entries and the now-orphaned uuid sidecar.
    AssetRegistry::OnAssetRemoved(target);

    LOG_INFO("Deleted asset: {}", path);
}

void PasteAsset(const std::string& sourcePath, const std::string& destinationDirectory)
{
    const std::filesystem::path src(sourcePath);
    const std::filesystem::path destination(destinationDirectory);
    std::error_code ec;
    if (!std::filesystem::exists(src, ec) || ec)
    {
        throw std::runtime_error("Nothing to paste: '" + sourcePath + "' no longer exists");
    }

    std::filesystem::path dst;
    if (std::filesystem::is_directory(src, ec))
    {
        if (AssetPaths::IsSameOrInside(destination, src))
        {
            throw std::runtime_error("Cannot paste folder '" + src.filename().string() + "' into itself");
        }
        dst = destination / src.filename();
        if (std::filesystem::exists(dst, ec))
        {
            dst = AssetPaths::UniqueCopyPath(dst);
        }
        std::filesystem::copy(src, dst, std::filesystem::copy_options::recursive, ec);
        if (ec)
        {
            throw std::runtime_error("Failed to copy '" + sourcePath + "' to '" + dst.string() + "': " + ec.message());
        }

        // Copied uuid sidecars would duplicate their source's identity, and
        // rescan order must not decide who keeps the uuid: strip them from the
        // copy so the originals stay authoritative and the copies get fresh uuids.
        std::error_code sidecarEc;
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
    else if (ModelLoader::IsSupportedModelPath(src))
    {
        // A model is more than its file: buffers, textures (unpacked ones
        // included) and material edits. It is copied the way it is imported,
        // into its own folder, so the copy is complete.
        std::filesystem::path modelFolder = destination / src.stem();
        if (ModelImportTarget::IsOccupied(modelFolder))
        {
            modelFolder = ModelImportTarget::NextFreeFolder(modelFolder);
        }
        std::filesystem::create_directories(modelFolder, ec);
        if (ec)
        {
            throw std::runtime_error("Failed to create '" + modelFolder.string() + "': " + ec.message());
        }
        dst = ModelLoader::CopyModelWithSortedReferences(src, modelFolder);

        for (const std::filesystem::path& definition : FindMaterialDefinitionFiles(src))
        {
            std::error_code copyEc;
            std::filesystem::copy_file(definition, modelFolder / definition.filename(), copyEc);
            if (copyEc)
            {
                LOG_WARN("Could not copy material definition '{}': {}", definition.string(), copyEc.message());
            }
        }
    }
    else
    {
        dst = destination / src.filename();
        if (std::filesystem::exists(dst, ec))
        {
            dst = AssetPaths::UniqueCopyPath(dst);
        }
        std::filesystem::copy_file(src, dst, ec);
        if (ec)
        {
            throw std::runtime_error("Failed to copy '" + sourcePath + "' to '" + dst.string() + "': " + ec.message());
        }
    }

    AssetRegistry::RescanAssetTree();

    LOG_INFO("Copied asset '{}' -> '{}'", sourcePath, dst.string());
}

void OnAssetRenamed(RendererSharedState& state, const std::string& oldPath, const std::string& newPath)
{
    // Entities keep their paths in memory. Left alone, a renamed model's next
    // reload fails and its material edits are saved under the old name.
    IEditorWorld& world = state.GetEditorWorld();
    std::vector<entt::entity> entities;
    for (entt::entity entity : world.Registry().view<const ModelComponent>())
    {
        entities.push_back(entity);
    }

    size_t retargeted = 0;
    for (entt::entity entity : entities)
    {
        ModelComponent& model = world.EditModel(entity);
        if (const std::optional<std::filesystem::path> rebased =
                AssetPaths::Rebase(model.sourcePath, oldPath, newPath))
        {
            const std::string oldFileName = std::filesystem::path(model.sourcePath).filename().string();
            if (model.displayName == oldFileName)
            {
                model.displayName = rebased->filename().string();
            }
            model.sourcePath = rebased->string();
            ++retargeted;
        }
        if (const std::optional<std::filesystem::path> rebased =
                AssetPaths::Rebase(model.baseColorTextureOverridePath, oldPath, newPath))
        {
            model.baseColorTextureOverridePath = rebased->string();
            ++retargeted;
        }
    }

    if (retargeted > 0)
    {
        LOG_INFO("Retargeted {} scene reference(s) from '{}' to '{}'", retargeted, oldPath, newPath);
    }
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
    ModelCache::UpdateMaterial(modelPath, materialIndex, material);
    MarkModelRenderablesDirtyForSourcePath(state, modelPath);
    RefreshDirtySceneRenderables(state);

    // Persisted last: the edit stays visible even when saving it fails.
    WriteMaterialYamlFile(
        std::filesystem::path(modelPath),
        materialIndex,
        material);
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
    ModelCache::UpdateMaterials(modelPathString, materials);
    MarkModelRenderablesDirtyForSourcePath(state, modelPathString);
    RefreshDirtySceneRenderables(state);

    // Persist each material as a sidecar .material.yaml file alongside the
    // model. One failure must not stop the others from being saved.
    std::string failures;
    for (size_t i = 0; i < materials.size(); ++i)
    {
        try
        {
            const std::filesystem::path outPath =
                WriteMaterialYamlFile(modelPath, static_cast<uint32_t>(i), materials[i]);
            LOG_INFO("Saved material '{}' -> '{}'", materials[i].name, outPath.string());
        }
        catch (const std::exception& error)
        {
            failures += failures.empty() ? error.what() : std::string("; ") + error.what();
        }
    }
    if (!failures.empty())
    {
        throw std::runtime_error(failures);
    }
    LOG_INFO(
        "Saved {} material(s) for model '{}'",
        materials.size(),
        modelPathString);
}

}
}
