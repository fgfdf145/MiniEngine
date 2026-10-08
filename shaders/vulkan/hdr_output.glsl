// HDR display output: the encodings hdr_composite.frag writes the swapchain with. Display-linear
// values here are Rec.709 relative to SDR white (1.0 = Windows' SDR content brightness, where the
// editor UI shows in SDR). Shared by hdr_composite.frag and tests/tonemap_tests.cpp, so written, like
// gt7_tonemap.glsl, in the subset GLSL and C++/GLM both accept.

// SMPTE ST 2084 inverse EOTF: absolute luminance in cd/m^2 to the PQ signal in [0, 1].
float PqEncodeNits(float nits)
{
    float y = clamp(nits / 10000.0f, 0.0f, 1.0f);
    float ym = pow(y, 0.1593017578125f);
    return pow((0.8359375f + 18.8515625f * ym) / (1.0f + 18.6875f * ym), 78.84375f);
}

vec3 PqEncodeNits3(vec3 nits)
{
    return vec3(PqEncodeNits(nits.x), PqEncodeNits(nits.y), PqEncodeNits(nits.z));
}

// The sRGB curve, its upper segment carried on above 1.0 so a highlight brighter than SDR white
// encodes (and decodes back) to itself. Negative values clamp to 0.
float SrgbEncodeExtended(float linear)
{
    float x = max(linear, 0.0f);
    return x <= 0.0031308f ? x * 12.92f : 1.055f * pow(x, 1.0f / 2.4f) - 0.055f;
}

float SrgbDecodeExtended(float encoded)
{
    float x = max(encoded, 0.0f);
    return x <= 0.04045f ? x / 12.92f : pow((x + 0.055f) / 1.055f, 2.4f);
}

vec3 SrgbEncodeExtended3(vec3 linear)
{
    return vec3(SrgbEncodeExtended(linear.x), SrgbEncodeExtended(linear.y), SrgbEncodeExtended(linear.z));
}

vec3 SrgbDecodeExtended3(vec3 encoded)
{
    return vec3(SrgbDecodeExtended(encoded.x), SrgbDecodeExtended(encoded.y), SrgbDecodeExtended(encoded.z));
}

// One pixel of the HDR frame, display-linear relative to SDR white: the SDR UI layer (premultiplied
// sRGB-encoded colour, alpha the overlays' coverage where the viewport was cut out) over the scene.
// Blended in sRGB-encoded space, as the SDR swapchain blends ImGui over the viewport image, so wherever
// the scene is no brighter than SDR white the result is the SDR frame's pixel exactly.
vec3 CompositeUiOverScene(vec4 ui, vec3 scene)
{
    vec3 encoded = vec3(ui.x, ui.y, ui.z) + SrgbEncodeExtended3(scene) * (1.0f - ui.w);
    return SrgbDecodeExtended3(encoded);
}
