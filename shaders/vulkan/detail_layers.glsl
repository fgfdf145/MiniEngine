#ifndef DETAIL_LAYERS_GLSL
#define DETAIL_LAYERS_GLSL

// MaterialDetailLayers (engine/scene/material_graph.h), Assetto Corsa's multilayer surfaces: the
// mask's channels weigh four tiling detail maps. Set 1 bindings 27 to 31, after the layer maps of
// material_layers.glsl. All five are bound as linear data: AC combines the sRGB-encoded values, and
// the sum is decoded once here. Include after material_common.glsl.
layout(set = 1, binding = 27) uniform sampler2D detailMaskTexture;
layout(set = 1, binding = 28) uniform sampler2D detailLayerRTexture;
layout(set = 1, binding = 29) uniform sampler2D detailLayerGTexture;
layout(set = 1, binding = 30) uniform sampler2D detailLayerBTexture;
layout(set = 1, binding = 31) uniform sampler2D detailLayerATexture;

vec3 DetailLayersSrgbToLinear(vec3 encoded)
{
    return mix(encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), encoded));
}

// What the base colour is multiplied by: 1 for a material without detail layers. The layers tile by
// the first UV set or by the position's x and z in the model's space (shadingModel.z 1 or 2); the
// mask always reads the first UV set.
vec3 DetailLayersFactor(MaterialData material, vec2 uv0, vec3 objectPosition)
{
    uint mapping = material.shadingModel.z;
    if (mapping == 0u)
    {
        return vec3(1.0);
    }
    vec2 coordinate = mapping == 2u ? objectPosition.xz : uv0;
    vec4 mask = texture(detailMaskTexture, uv0);
    vec4 scalesRG = material.detailLayerScales[0];
    vec4 scalesBA = material.detailLayerScales[1];
    vec3 combined = texture(detailLayerRTexture, coordinate * scalesRG.xy).rgb * mask.r +
                    texture(detailLayerGTexture, coordinate * scalesRG.zw).rgb * mask.g +
                    texture(detailLayerBTexture, coordinate * scalesBA.xy).rgb * mask.b +
                    texture(detailLayerATexture, coordinate * scalesBA.zw).rgb * mask.a;
    combined *= material.detailLayerParams.x;
    return DetailLayersSrgbToLinear(max(combined, vec3(0.0)));
}

#endif
