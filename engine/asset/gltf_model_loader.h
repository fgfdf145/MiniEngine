#pragma once

#include "model_loader.h"

#include <string>

namespace me
{

class GltfModelLoader
{
  public:
    static LoadedModelData LoadModel(const std::string& path, const ModelLoadProgressCallback& progress = {});

    // Copies an ASCII .gltf into targetDirectory together with the external
    // files it references, sorting them into subfolders by kind (buffers/,
    // textures/) and rewriting the copied glTF's URIs to match. Existing files
    // at the destination are kept, never overwritten. Returns the path of the
    // glTF written inside targetDirectory. Throws on unreadable or malformed
    // JSON.
    static std::filesystem::path CopyWithSortedReferences(
        const std::filesystem::path& gltfPath,
        const std::filesystem::path& targetDirectory);
};
}
