// GT7 tone mapping, SDR output, ported from Polyphony Digital's reference sample (version 1.0,
// 2025-08-10). The reference is vendored at tests/third_party/gt7/gt7_tone_mapping_reference.h,
// and tests/tonemap_tests.cpp checks this port against it.
//
// This file is compiled twice: by glslc, included from tonemap.frag, and by the C++ unit test,
// included inside a namespace that has `using namespace glm`. It is therefore written in the
// subset both accept: no #version, layouts or includes; every float literal carries the f suffix
// (GLM's templates do not mix float and double); const only for literal initializers; no out or
// inout parameters; matrices multiplied as `vector * matrix`, which GLSL and GLM both read as the
// row-major matrix written in the constructor applied to a column vector.
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
const float kGt7ReferenceLuminance = 100.0f;

// The SDR reference white GT tone maps against, in cd/m^2.
const float kGt7SdrPaperWhite = 250.0f;

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
const float kGt7PqM1 = 0.1593017578125f; // (2610 / 4096) / 4
const float kGt7PqM2 = 78.84375f;        // (2523 / 4096) * 128
const float kGt7PqC1 = 0.8359375f;       // 3424 / 4096
const float kGt7PqC2 = 18.8515625f;      // (2413 / 4096) * 32
const float kGt7PqC3 = 18.6875f;         // (2392 / 4096) * 32
const float kGt7PqMaxLuminance = 10000.0f;

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
vec3 Gt7RgbToICtCp(vec3 rgb)
{
    float l = (rgb.x * 1688.0f + rgb.y * 2146.0f + rgb.z * 262.0f) / 4096.0f;
    float m = (rgb.x * 683.0f + rgb.y * 2951.0f + rgb.z * 462.0f) / 4096.0f;
    float s = (rgb.x * 99.0f + rgb.y * 309.0f + rgb.z * 3688.0f) / 4096.0f;

    float lPQ = Gt7InverseEotfSt2084(l);
    float mPQ = Gt7InverseEotfSt2084(m);
    float sPQ = Gt7InverseEotfSt2084(s);

    return vec3(
        (2048.0f * lPQ + 2048.0f * mPQ) / 4096.0f,
        (6610.0f * lPQ - 13613.0f * mPQ + 7003.0f * sPQ) / 4096.0f,
        (17933.0f * lPQ - 17390.0f * mPQ - 543.0f * sPQ) / 4096.0f);
}

vec3 Gt7ICtCpToRgb(vec3 ictCp)
{
    float l = ictCp.x + 0.00860904f * ictCp.y + 0.11103f * ictCp.z;
    float m = ictCp.x - 0.00860904f * ictCp.y - 0.11103f * ictCp.z;
    float s = ictCp.x + 0.560031f * ictCp.y - 0.320627f * ictCp.z;

    float lLin = Gt7EotfSt2084(l);
    float mLin = Gt7EotfSt2084(m);
    float sLin = Gt7EotfSt2084(s);

    return vec3(
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
    toneMapping.frameBufferLuminanceTargetUcs = Gt7RgbToICtCp(vec3(toneMapping.frameBufferLuminanceTarget)).x;
    return toneMapping;
}

// Input: linear Rec.2020 in GT frame-buffer units. Output: linear Rec.2020 in [0, 1], ready for
// the sRGB OETF.
vec3 Gt7ApplyToneMapping(Gt7ToneMapping toneMapping, vec3 rgb)
{
    // ICtCp separates luminance from chroma.
    vec3 ucs = Gt7RgbToICtCp(rgb);

    // Per-channel tone mapping ("skewed" color).
    vec3 skewedRgb = vec3(
        Gt7EvaluateCurve(toneMapping.curve, rgb.x),
        Gt7EvaluateCurve(toneMapping.curve, rgb.y),
        Gt7EvaluateCurve(toneMapping.curve, rgb.z));
    vec3 skewedUcs = Gt7RgbToICtCp(skewedRgb);

    float chromaScale = Gt7ChromaCurve(
        ucs.x / toneMapping.frameBufferLuminanceTargetUcs,
        toneMapping.fadeStart,
        toneMapping.fadeEnd);

    // Luminance from the skewed color, chroma from the input, faded out toward the peak.
    vec3 scaledUcs = vec3(skewedUcs.x, ucs.y * chromaScale, ucs.z * chromaScale);
    vec3 scaledRgb = Gt7ICtCpToRgb(scaledUcs);

    // Blend between the per-channel and the UCS-scaled results.
    vec3 blended = (1.0f - toneMapping.blendRatio) * skewedRgb + toneMapping.blendRatio * scaledRgb;
    return toneMapping.sdrCorrectionFactor * min(blended, vec3(toneMapping.frameBufferLuminanceTarget));
}

// ---------------------------------------------------------------------------
// Engine glue: the engine renders linear Rec.709, while the reference's ICtCp coefficients take
// linear Rec.2020, so the scene is converted into Rec.2020 for the operator and back afterwards.
// ---------------------------------------------------------------------------

// ITU-R BT.2087 conversion matrices, written row-major and applied as `vector * matrix`.
const mat3 kRec709ToRec2020 = mat3(
    0.6274040f, 0.3292820f, 0.0433136f,
    0.0690970f, 0.9195400f, 0.0113612f,
    0.0163916f, 0.0880132f, 0.8955950f);
const mat3 kRec2020ToRec709 = mat3(
    1.6604910f, -0.5876411f, -0.0728499f,
    -0.1245505f, 1.1328999f, -0.0083494f,
    -0.0181508f, -0.1005789f, 1.1187297f);

// Exposed values put sensor saturation at 1.0 (see ExposureFromEv100). This places that point at
// the SDR paper white, 250 cd/m^2 or 2.5 GT frame-buffer units, so an exposed mid gray lands where
// the previous Reinhard operator put it while highlights now roll off into a real white.
const float kExposedToGt7FrameBuffer = 2.5f;

// Exposed linear Rec.709 in, display-referred linear Rec.709 in [0, 1] out.
vec3 TonemapExposedRec709(vec3 exposedRec709)
{
    vec3 rec2020 = max(exposedRec709, vec3(0.0f)) * kExposedToGt7FrameBuffer * kRec709ToRec2020;
    vec3 mapped = Gt7ApplyToneMapping(Gt7InitializeAsSdr(), rec2020);
    return clamp(mapped * kRec2020ToRec709, vec3(0.0f), vec3(1.0f));
}
