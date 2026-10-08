// GT7 tone mapping, SDR output, ported from Polyphony Digital's reference sample (version 1.0,
// 2025-08-10). The reference is vendored at tests/third_party/gt7/gt7_tone_mapping_reference.h,
// and tests/tonemap_tests.cpp checks this port against it.
//
// This file is compiled twice: by slangc, included from tonemap.frag, and by the C++ unit test,
// included inside a namespace that has `using namespace glm` and engine/renderer/shader_cpp_compat.h.
// It is therefore written in the subset both accept: no layouts or includes; every float literal
// carries the f suffix (GLM's templates do not mix float and double); static const only for literal
// initializers; no out or inout parameters; no matrix constructors (Slang fills them by rows, GLM by
// columns), so the colour space conversions are written as one dot product per output channel.
//
// Differences from the reference: SDR only (initializeAsSDR); ICtCp only, the reference's
// default UCS, so the PQ helpers drop the Jzazbz exponent scale; functions take and return
// values instead of pointers. The arithmetic and every constant are the reference's.
//
// -----
// MIT License
//
// Copyright (c) 2025 Polyphony Digital Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// 1.0 in GT's linear frame-buffer space is this many cd/m^2.
static const float kGt7ReferenceLuminance = 100.0f;

// The SDR reference white GT tone maps against, in cd/m^2.
static const float kGt7SdrPaperWhite = 250.0f;

float Gt7PhysicalToFrameBuffer(float physical)
{
    return physical / kGt7ReferenceLuminance;
}

float Gt7FrameBufferToPhysical(float frameBufferValue)
{
    return frameBufferValue * kGt7ReferenceLuminance;
}

float Gt7SmoothStep(float x, float edge0, float edge1)
{
    float t = (x - edge0) / (edge1 - edge0);

    if (x < edge0)
    {
        return 0.0f;
    }
    if (x > edge1)
    {
        return 1.0f;
    }

    return t * t * (3.0f - 2.0f * t);
}

float Gt7ChromaCurve(float x, float a, float b)
{
    return 1.0f - Gt7SmoothStep(x, a, b);
}

// ---------------------------------------------------------------------------
// "GT Tone Mapping" curve with convergent shoulder.
// ---------------------------------------------------------------------------
struct Gt7Curve
{
    float peakIntensity;
    float alpha;
    float midPoint;
    float linearSection;
    float toeStrength;
    float kA;
    float kB;
    float kC;
};

Gt7Curve Gt7InitializeCurve(float monitorIntensity, float alpha, float grayPoint, float linearSection, float toeStrength)
{
    Gt7Curve curve;
    curve.peakIntensity = monitorIntensity;
    curve.alpha = alpha;
    curve.midPoint = grayPoint;
    curve.linearSection = linearSection;
    curve.toeStrength = toeStrength;

    // Constants for the shoulder region.
    float k = (linearSection - 1.0f) / (alpha - 1.0f);
    curve.kA = monitorIntensity * linearSection + monitorIntensity * k;
    curve.kB = -monitorIntensity * k * exp(linearSection / k);
    curve.kC = -1.0f / (k * monitorIntensity);
    return curve;
}

float Gt7EvaluateCurve(Gt7Curve curve, float x)
{
    if (x < 0.0f)
    {
        return 0.0f;
    }

    float weightLinear = Gt7SmoothStep(x, 0.0f, curve.midPoint);
    float weightToe = 1.0f - weightLinear;

    // Shoulder mapping for highlights.
    float shoulder = curve.kA + curve.kB * exp(x * curve.kC);

    if (x < curve.linearSection * curve.peakIntensity)
    {
        float toeMapped = curve.midPoint * pow(x / curve.midPoint, curve.toeStrength);
        return weightToe * toeMapped + weightLinear * x;
    }
    return shoulder;
}

// ---------------------------------------------------------------------------
// EOTF / inverse EOTF for SMPTE ST 2084 (PQ), in GT frame-buffer units.
// ---------------------------------------------------------------------------
static const float kGt7PqM1 = 0.1593017578125f; // (2610 / 4096) / 4
static const float kGt7PqM2 = 78.84375f;        // (2523 / 4096) * 128
static const float kGt7PqC1 = 0.8359375f;       // 3424 / 4096
static const float kGt7PqC2 = 18.8515625f;      // (2413 / 4096) * 32
static const float kGt7PqC3 = 18.6875f;         // (2392 / 4096) * 32
static const float kGt7PqMaxLuminance = 10000.0f;

float Gt7EotfSt2084(float n)
{
    n = clamp(n, 0.0f, 1.0f);

    float np = pow(n, 1.0f / kGt7PqM2);
    float l = max(np - kGt7PqC1, 0.0f);

    l = l / (kGt7PqC2 - kGt7PqC3 * np);
    l = pow(l, 1.0f / kGt7PqM1);

    return Gt7PhysicalToFrameBuffer(l * kGt7PqMaxLuminance);
}

float Gt7InverseEotfSt2084(float v)
{
    float y = Gt7FrameBufferToPhysical(v) / kGt7PqMaxLuminance;
    float ym = pow(y, kGt7PqM1);
    return exp2(kGt7PqM2 * (log2(kGt7PqC1 + kGt7PqC2 * ym) - log2(1.0f + kGt7PqC3 * ym)));
}

// ---------------------------------------------------------------------------
// ICtCp (ITU-T T.302), from and to linear Rec.2020.
// ---------------------------------------------------------------------------
float3 Gt7RgbToICtCp(float3 rgb)
{
    float l = (rgb.x * 1688.0f + rgb.y * 2146.0f + rgb.z * 262.0f) / 4096.0f;
    float m = (rgb.x * 683.0f + rgb.y * 2951.0f + rgb.z * 462.0f) / 4096.0f;
    float s = (rgb.x * 99.0f + rgb.y * 309.0f + rgb.z * 3688.0f) / 4096.0f;

    float lPQ = Gt7InverseEotfSt2084(l);
    float mPQ = Gt7InverseEotfSt2084(m);
    float sPQ = Gt7InverseEotfSt2084(s);

    return float3(
        (2048.0f * lPQ + 2048.0f * mPQ) / 4096.0f,
        (6610.0f * lPQ - 13613.0f * mPQ + 7003.0f * sPQ) / 4096.0f,
        (17933.0f * lPQ - 17390.0f * mPQ - 543.0f * sPQ) / 4096.0f);
}

float3 Gt7ICtCpToRgb(float3 ictCp)
{
    float l = ictCp.x + 0.00860904f * ictCp.y + 0.11103f * ictCp.z;
    float m = ictCp.x - 0.00860904f * ictCp.y - 0.11103f * ictCp.z;
    float s = ictCp.x + 0.560031f * ictCp.y - 0.320627f * ictCp.z;

    float lLin = Gt7EotfSt2084(l);
    float mLin = Gt7EotfSt2084(m);
    float sLin = Gt7EotfSt2084(s);

    return float3(
        max(3.43661f * lLin - 2.50645f * mLin + 0.0698454f * sLin, 0.0f),
        max(-0.79133f * lLin + 1.9836f * mLin - 0.192271f * sLin, 0.0f),
        max(-0.0259499f * lLin - 0.0989137f * mLin + 1.12486f * sLin, 0.0f));
}

// ---------------------------------------------------------------------------
// GT7 tone mapping.
// ---------------------------------------------------------------------------
struct Gt7ToneMapping
{
    float sdrCorrectionFactor;
    float frameBufferLuminanceTarget;
    float frameBufferLuminanceTargetUcs; // Target luminance in ICtCp's I
    Gt7Curve curve;
    float blendRatio;
    float fadeStart;
    float fadeEnd;
};

// The reference's initializeAsSDR. A maximum of 1.0 in SDR output stands for the 250 cd/m^2 paper
// white, while sRGB puts 1.0 at 100 cd/m^2, so the result is scaled back into [0, 1].
Gt7ToneMapping Gt7InitializeAsSdr()
{
    Gt7ToneMapping toneMapping;
    toneMapping.sdrCorrectionFactor = 1.0f / Gt7PhysicalToFrameBuffer(kGt7SdrPaperWhite);
    toneMapping.frameBufferLuminanceTarget = Gt7PhysicalToFrameBuffer(kGt7SdrPaperWhite);
    toneMapping.curve = Gt7InitializeCurve(toneMapping.frameBufferLuminanceTarget, 0.25f, 0.538f, 0.444f, 1.280f);
    toneMapping.blendRatio = 0.6f;
    toneMapping.fadeStart = 0.98f;
    toneMapping.fadeEnd = 1.16f;
    toneMapping.frameBufferLuminanceTargetUcs = Gt7RgbToICtCp(float3(toneMapping.frameBufferLuminanceTarget)).x;
    return toneMapping;
}

// Input: linear Rec.2020 in GT frame-buffer units. Output: linear Rec.2020 in [0, 1], ready for
// the sRGB OETF.
float3 Gt7ApplyToneMapping(Gt7ToneMapping toneMapping, float3 rgb)
{
    // ICtCp separates luminance from chroma.
    float3 ucs = Gt7RgbToICtCp(rgb);

    // Per-channel tone mapping ("skewed" color).
    float3 skewedRgb = float3(
        Gt7EvaluateCurve(toneMapping.curve, rgb.x),
        Gt7EvaluateCurve(toneMapping.curve, rgb.y),
        Gt7EvaluateCurve(toneMapping.curve, rgb.z));
    float3 skewedUcs = Gt7RgbToICtCp(skewedRgb);

    float chromaScale = Gt7ChromaCurve(
        ucs.x / toneMapping.frameBufferLuminanceTargetUcs,
        toneMapping.fadeStart,
        toneMapping.fadeEnd);

    // Luminance from the skewed color, chroma from the input, faded out toward the peak.
    float3 scaledUcs = float3(skewedUcs.x, ucs.y * chromaScale, ucs.z * chromaScale);
    float3 scaledRgb = Gt7ICtCpToRgb(scaledUcs);

    // Blend between the per-channel and the UCS-scaled results.
    float3 blended = (1.0f - toneMapping.blendRatio) * skewedRgb + toneMapping.blendRatio * scaledRgb;
    return toneMapping.sdrCorrectionFactor * min(blended, float3(toneMapping.frameBufferLuminanceTarget));
}

// The reference's initializeAsHDR: the same curve built for a display peak in cd/m^2 (250 to
// 10 000). The output is in frame-buffer units up to that peak (1.0 = 100 cd/m^2), ready for PQ.
Gt7ToneMapping Gt7InitializeAsHdr(float physicalTargetLuminance)
{
    Gt7ToneMapping toneMapping;
    toneMapping.sdrCorrectionFactor = 1.0f;
    toneMapping.frameBufferLuminanceTarget = Gt7PhysicalToFrameBuffer(physicalTargetLuminance);
    toneMapping.curve = Gt7InitializeCurve(toneMapping.frameBufferLuminanceTarget, 0.25f, 0.538f, 0.444f, 1.280f);
    toneMapping.blendRatio = 0.6f;
    toneMapping.fadeStart = 0.98f;
    toneMapping.fadeEnd = 1.16f;
    toneMapping.frameBufferLuminanceTargetUcs = Gt7RgbToICtCp(float3(toneMapping.frameBufferLuminanceTarget)).x;
    return toneMapping;
}

// ---------------------------------------------------------------------------
// Engine glue: the engine renders linear Rec.709, while the reference's ICtCp coefficients take
// linear Rec.2020, so the scene is converted into Rec.2020 for the operator and back afterwards.
// ---------------------------------------------------------------------------

// ITU-R BT.2087 conversion matrices, one row per output channel.
float3 Rec709ToRec2020(float3 c)
{
    return float3(
        dot(float3(0.6274040f, 0.3292820f, 0.0433136f), c),
        dot(float3(0.0690970f, 0.9195400f, 0.0113612f), c),
        dot(float3(0.0163916f, 0.0880132f, 0.8955950f), c));
}
float3 Rec2020ToRec709(float3 c)
{
    return float3(
        dot(float3(1.6604910f, -0.5876411f, -0.0728499f), c),
        dot(float3(-0.1245505f, 1.1328999f, -0.0083494f), c),
        dot(float3(-0.0181508f, -0.1005789f, 1.1187297f), c));
}

// HDR target values in, GT7's HDR curve for a display peak in cd/m^2: linear Rec.709 in
// frame-buffer units up to that peak (out-of-gamut channels clamped at 0).
float3 TonemapFrameBufferRec709Hdr(float3 frameBufferRec709, float peakNits)
{
    float3 rec2020 = Rec709ToRec2020(max(frameBufferRec709, float3(0.0f)));
    float3 mapped = Gt7ApplyToneMapping(Gt7InitializeAsHdr(peakNits), rec2020);
    return max(Rec2020ToRec709(mapped), float3(0.0f));
}

// HDR target values (GT7 frame-buffer units, linear Rec.709; see pre_exposure.slang) in,
// display-referred linear Rec.709 in [0, 1] out.
float3 TonemapFrameBufferRec709(float3 frameBufferRec709)
{
    float3 rec2020 = Rec709ToRec2020(max(frameBufferRec709, float3(0.0f)));
    float3 mapped = Gt7ApplyToneMapping(Gt7InitializeAsSdr(), rec2020);
    return clamp(Rec2020ToRec709(mapped), float3(0.0f), float3(1.0f));
}
