// HDR display output: the PQ encoding and the black level lift. Every display-linear value is
// relative to the UI white, which follows Windows' SDR content brightness (display_calibration.h)
// and reaches the shaders as a constant. Shared by imgui.frag's HDR10 variant and hdr_ui_encode.frag,
// which encode the whole editor frame, tonemap.frag, which scales the scene into UI-white units,
// display_calibration.slang and the tests. Written, like gt7_tonemap.slang, in the subset Slang and
// C++/GLM both accept.

// ITU-R BT.2408's graphics white: where display-linear 1.0 (SDR white, the UI's white) shows on an
// HDR10 display when the OS says nothing better.
static const float kUiWhiteNits = 203.0f;

// SMPTE ST 2084 inverse EOTF: absolute luminance in cd/m^2 to the PQ signal in [0, 1].
float PqEncodeNits(float nits)
{
    float y = clamp(nits / 10000.0f, 0.0f, 1.0f);
    float ym = pow(y, 0.1593017578125f);
    return pow((0.8359375f + 18.8515625f * ym) / (1.0f + 18.6875f * ym), 78.84375f);
}

float3 PqEncodeNits3(float3 nits)
{
    return float3(PqEncodeNits(nits.x), PqEncodeNits(nits.y), PqEncodeNits(nits.z));
}

// The sRGB transfer function, continued past 1 by the same curve so the scene's highlights keep their
// level through HdrUiComposite's layer (fp16), and undone.
float SrgbEncodeExtended(float linear)
{
    float c = max(linear, 0.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
}

float SrgbDecodeExtended(float signal)
{
    float c = max(signal, 0.0f);
    return c <= 0.04045f ? c / 12.92f : pow((c + 0.055f) / 1.055f, 2.4f);
}

float3 SrgbEncodeExtended3(float3 linear)
{
    return float3(SrgbEncodeExtended(linear.x), SrgbEncodeExtended(linear.y), SrgbEncodeExtended(linear.z));
}

float3 SrgbDecodeExtended3(float3 signal)
{
    return float3(SrgbDecodeExtended(signal.x), SrgbDecodeExtended(signal.y), SrgbDecodeExtended(signal.z));
}

// SMPTE ST 2084 EOTF: the PQ signal in [0, 1] to cd/m^2 (PqEncodeNits undone).
float PqDecodeNits(float signal)
{
    float e = pow(clamp(signal, 0.0f, 1.0f), 1.0f / 78.84375f);
    return 10000.0f * pow(max(e - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * e), 1.0f / 0.1593017578125f);
}

// ITU-R BT.2390's black level lift, in PQ: E' = E + b (1 - E)^4 with b = PQ(floor). 0 cd/m^2 goes to
// the display's floor, so the darkest detail lands where the display still shows it; bright values
// barely move. floorPq is b, computed once on the CPU (hdr_ui_encode.frag applies it to the whole
// frame, already in PQ).
float3 LiftPqToBlackFloor(float3 e, float floorPq)
{
    float3 inverse = float3(1.0f) - e;
    return e + floorPq * inverse * inverse * inverse * inverse;
}

// The same lift on absolute luminance (the tests' reference).
float LiftToBlackFloorNits(float nits, float floorNits)
{
    if (floorNits <= 0.0f)
    {
        return nits;
    }
    return PqDecodeNits(LiftPqToBlackFloor(float3(PqEncodeNits(nits)), PqEncodeNits(floorNits)).x);
}
