#include "material_pipeline.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace me
{

MaterialPipelineState GetMaterialPipelineState(MaterialPipelineKey key)
{
    MaterialPipelineState state{};
    state.cullBackFaces = !key.doubleSided;
    state.alphaMaskEnabled = key.alphaMode == MaterialAlphaMode::Mask;
    state.blendEnabled = key.alphaMode == MaterialAlphaMode::Blend;
    state.depthWriteEnabled = key.alphaMode != MaterialAlphaMode::Blend;
    return state;
}

size_t GetMaterialPipelineIndex(MaterialPipelineKey key)
{
    return static_cast<size_t>(key.alphaMode) * 2u + (key.doubleSided ? 1u : 0u);
}

std::vector<size_t> BuildMaterialDrawOrder(std::span<const MaterialDrawSortKey> keys)
{
    std::vector<size_t> order(keys.size());
    std::iota(order.begin(), order.end(), size_t{0});
    const auto blendBegin = std::stable_partition(order.begin(), order.end(), [&](size_t index)
                                                  {
                                                      return keys[index].pipeline.alphaMode != MaterialAlphaMode::Blend;
                                                  });
    // Opaque and Mask draws depth-test and depth-write, so their relative order does not change
    // the image: group them by pipeline variant instead, which collapses the redundant
    // vkCmdBindPipeline calls an interleaved submesh list would otherwise produce. Blend draws
    // below keep their back-to-front order — correctness there outranks pipeline batching.
    std::stable_sort(order.begin(), blendBegin, [&](size_t lhs, size_t rhs)
                     {
                         return GetMaterialPipelineIndex(keys[lhs].pipeline) <
                                GetMaterialPipelineIndex(keys[rhs].pipeline);
                     });
    std::stable_sort(blendBegin, order.end(), [&](size_t lhs, size_t rhs)
                     {
                         const bool lhsIsNaN = std::isnan(keys[lhs].viewDepth);
                         const bool rhsIsNaN = std::isnan(keys[rhs].viewDepth);
                         if (lhsIsNaN != rhsIsNaN)
                         {
                             return !lhsIsNaN;
                         }
                         if (lhsIsNaN)
                         {
                             return false;
                         }
                         return keys[lhs].viewDepth > keys[rhs].viewDepth;
                     });
    return order;
}
}
