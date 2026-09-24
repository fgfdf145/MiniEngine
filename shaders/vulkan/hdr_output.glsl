// HDR display output: the PQ encoding and the UI white every display-linear value is relative to.
// Shared by imgui_hdr10.frag, which encodes the whole editor frame, tonemap.frag, which scales the
// scene into UI-white units, and tests/tonemap_tests.cpp. Written, like gt7_tonemap.glsl, in the
// subset GLSL and C++/GLM both accept.

// ITU-R BT.2408's graphics white: display-linear 1.0 (SDR white, the UI's white) shows at this many
// cd/m^2 on an HDR10 display.
const float kUiWhiteNits = 203.0f;

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
