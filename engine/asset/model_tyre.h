#pragma once

#include "model_loader.h"

namespace me
{

// Finds the tyres among a car's wheel submeshes (ModelWheelPart::Wheel under a model with a wheel
// rig): those reaching the wheel's outer radius with a sidewall below it, where a rim reaches only its
// lip. Each gets its ModelTyreShape, is cut finer around the wheel (RefineTyreMesh) and is marked
// deformable. Does nothing for a model without a wheel rig.
void PrepareModelTyres(LoadedModelData& modelData);

// Cuts every triangle of a mesh lathed about `shape`'s axle into a grid fine enough that no edge spans
// more than maxStepRadians about the axle, its new vertices placed on the surface of revolution
// through the old ones (angle, radius and axial place interpolated, not straight lines), so the tyre
// is rounder and its flattened part bends smoothly into the rest. Edges two triangles share get the
// same vertices, and copies of a vertex at one place (UV seams) the same positions, so nothing
// cracks. Returns how many parts each edge was cut into (1 when nothing changed).
uint32_t RefineTyreMesh(MeshData& mesh, const ModelTyreShape& shape, float maxStepRadians);
}
