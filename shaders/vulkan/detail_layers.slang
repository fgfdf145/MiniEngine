#ifndef DETAIL_LAYERS_SLANG
#define DETAIL_LAYERS_SLANG

// MaterialDetailLayers (engine/scene/material_graph.h), Assetto Corsa's multilayer surfaces: the
// mask's channels weigh four tiling detail maps. Set 1 bindings 27 to 31, after the layer maps of
// material_layers.slang. All five are bound as linear data: AC combines the sRGB-encoded values, and
// the sum is decoded once here. Include after material_common.slang.
[[vk::binding(27, 1)]] Sampler2D detailMaskTexture;
[[vk::binding(28, 1)]] Sampler2D detailLayerRTexture;
[[vk::binding(29, 1)]] Sampler2D detailLayerGTexture;
[[vk::binding(30, 1)]] Sampler2D detailLayerBTexture;
[[vk::binding(31, 1)]] Sampler2D detailLayerATexture;

float3 DetailLayersSrgbToLinear(float3 encoded)
{
    return lerp(encoded / 12.92, pow((encoded + 0.055) / 1.055, float3(2.4)), step(float3(0.04045), encoded));
}

// What the base colour is multiplied by: 1 for a material without detail layers. The layers tile by
// the first UV set or by the position's x and z in the model's space (shadingModel.z 1 or 2); the
// mask always reads the first UV set.
float3 DetailLayersFactor(MaterialData material, float2 uv0, float3 objectPosition)
{
    uint mapping = material.shadingModel.z;
    if (mapping == 0u)
    {
        return float3(1.0);
    }
    float2 coordinate = mapping == 2u ? objectPosition.xz : uv0;
    float4 mask = MaterialTexture(detailMaskTexture, uv0);
    float4 scalesRG = material.detailLayerScales[0];
    float4 scalesBA = material.detailLayerScales[1];
    float3 combined = MaterialTexture(detailLayerRTexture, coordinate * scalesRG.xy).rgb * mask.r +
                    MaterialTexture(detailLayerGTexture, coordinate * scalesRG.zw).rgb * mask.g +
                    MaterialTexture(detailLayerBTexture, coordinate * scalesBA.xy).rgb * mask.b +
                    MaterialTexture(detailLayerATexture, coordinate * scalesBA.zw).rgb * mask.a;
    combined *= material.detailLayerParams.x;
    return DetailLayersSrgbToLinear(max(combined, float3(0.0)));
}

#endif
