#pragma once

#include <engine/asset/mesh.h>

#include <cstdint>

namespace me
{

// What the Material Editor's preview shows (docs/design/2026-10-09-material-editor-redesign-design.md):
// the whole model, or one of Unreal's preview shapes in the selected slot's material.
enum class MaterialPreviewShape : uint32_t
{
    Model = 0,
    Sphere = 1,
    Cube = 2,
    Plane = 3,
    Cylinder = 4
};

inline constexpr uint32_t kMaterialPreviewShapeCount = 5;
const char* ToString(MaterialPreviewShape shape);

// The shape about the origin, about a metre across, Y up, with normals, tangents (w the bitangent's
// sign, as glTF has it) and texture coordinates in [0, 1] that wrap the shape once; white vertex
// colours. Model has no mesh of its own: an empty one.
MeshData BuildMaterialPreviewShapeMesh(MaterialPreviewShape shape);
}
