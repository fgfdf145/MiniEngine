// Polyphony Digital's reference GT7 tone mapping sample, vendored as the oracle that
// tests/tonemap_tests.cpp checks shaders/vulkan/gt7_tonemap.glsl against.
//
// Local changes, and only these: #pragma once; the whole implementation is wrapped in the
// gt7_reference namespace; functions are marked inline so the header can be included from more
// than one translation unit; std::powf/expf/log2f/exp2f are spelled powf/expf/log2f/exp2f from
// <math.h>, which not every standard library also declares in std; and the sample's test harness
// and main() are removed. The algorithm and every constant are unchanged.
//
// Sample implementation of the GT7 Tone Mapping operator.
//
// Version history:
// 1.0    (2025-08-10)    Initial release.
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

#pragma once

// clang-format off

#include <algorithm> // std::min, std::max
#include <math.h>    // powf, expf, log2f, exp2f

namespace gt7_reference
{

// -----------------------------------------------------------------------------
// Mode options.
// -----------------------------------------------------------------------------
#define TONE_MAPPING_UCS_ICTCP  0
#define TONE_MAPPING_UCS_JZAZBZ 1
#define TONE_MAPPING_UCS        TONE_MAPPING_UCS_ICTCP

// -----------------------------------------------------------------------------
// Defines the SDR reference white level used in our tone mapping (typically 250 nits).
// -----------------------------------------------------------------------------
#define GRAN_TURISMO_SDR_PAPER_WHITE 250.0f // cd/m^2

// -----------------------------------------------------------------------------
// Gran Turismo luminance-scale conversion helpers.
// In Gran Turismo, 1.0f in the linear frame-buffer space corresponds to
// REFERENCE_LUMINANCE cd/m^2 of physical luminance (typically 100 cd/m^2).
// -----------------------------------------------------------------------------
#define REFERENCE_LUMINANCE 100.0f // cd/m^2 <-> 1.0f

inline float
frameBufferValueToPhysicalValue(float fbValue)
{
    // Converts linear frame-buffer value to physical luminance (cd/m^2)
    // where 1.0 corresponds to REFERENCE_LUMINANCE (e.g., 100 cd/m^2).
    return fbValue * REFERENCE_LUMINANCE;
}

inline float
physicalValueToFrameBufferValue(float physical)
{
    // Converts physical luminance (cd/m^2) to a linear frame-buffer value,
    // where 1.0 corresponds to REFERENCE_LUMINANCE (e.g., 100 cd/m^2).
    return physical / REFERENCE_LUMINANCE;
}

// -----------------------------------------------------------------------------
// Utility functions.
// -----------------------------------------------------------------------------
inline float
smoothStep(float x, float edge0, float edge1)
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

inline float
chromaCurve(float x, float a, float b)
{
    return 1.0f - smoothStep(x, a, b);
}

// -----------------------------------------------------------------------------
// "GT Tone Mapping" curve with convergent shoulder.
// -----------------------------------------------------------------------------
struct GTToneMappingCurveV2
{
    float peakIntensity_;
    float alpha_;
    float midPoint_;
    float linearSection_;
    float toeStrength_;
    float kA_, kB_, kC_;

    void initializeCurve(float monitorIntensity,
                         float alpha,
                         float grayPoint,
                         float linearSection,
                         float toeStrength)
    {
        peakIntensity_ = monitorIntensity;
        alpha_         = alpha;
        midPoint_      = grayPoint;
        linearSection_ = linearSection;
        toeStrength_   = toeStrength;

        // Pre-compute constants for the shoulder region.
        float k = (linearSection_ - 1.0f) / (alpha_ - 1.0f);
        kA_     = peakIntensity_ * linearSection_ + peakIntensity_ * k;
        kB_     = -peakIntensity_ * k * expf(linearSection_ / k);
        kC_     = -1.0f / (k * peakIntensity_);
    }

    float evaluateCurve(float x) const
    {
        if (x < 0.0f)
        {
            return 0.0f;
        }

        float weightLinear = smoothStep(x, 0.0f, midPoint_);
        float weightToe    = 1.0f - weightLinear;

        // Shoulder mapping for highlights.
        float shoulder = kA_ + kB_ * expf(x * kC_);

        if (x < linearSection_ * peakIntensity_)
        {
            float toeMapped = midPoint_ * powf(x / midPoint_, toeStrength_);
            return weightToe * toeMapped + weightLinear * x;
        }
        else
        {
            return shoulder;
        }
    }
};

// -----------------------------------------------------------------------------
// EOTF / inverse-EOTF for ST-2084 (PQ).
// Note: Introduce exponentScaleFactor to allow scaling of the exponent in the EOTF for Jzazbz.
// -----------------------------------------------------------------------------
inline float
eotfSt2084(float n, float exponentScaleFactor = 1.0f)
{
    if (n < 0.0f)
    {
        n = 0.0f;
    }
    if (n > 1.0f)
    {
        n = 1.0f;
    }

    // Base functions from SMPTE ST 2084:2014
    // Converts from normalized PQ (0-1) to absolute luminance in cd/m^2 (linear light)
    // Assumes float input; does not handle integer encoding (Annex)
    // Assumes full-range signal (0-1)
    const float m1  = 0.1593017578125f;                // (2610 / 4096) / 4
    const float m2  = 78.84375f * exponentScaleFactor; // (2523 / 4096) * 128
    const float c1  = 0.8359375f;                      // 3424 / 4096
    const float c2  = 18.8515625f;                     // (2413 / 4096) * 32
    const float c3  = 18.6875f;                        // (2392 / 4096) * 32
    const float pqC = 10000.0f;                        // Maximum luminance supported by PQ (cd/m^2)

    // Does not handle signal range from 2084 - assumes full range (0-1)
    float np = powf(n, 1.0f / m2);
    float l  = np - c1;

    if (l < 0.0f)
    {
        l = 0.0f;
    }

    l = l / (c2 - c3 * np);
    l = powf(l, 1.0f / m1);

    // Convert absolute luminance (cd/m^2) into the frame-buffer linear scale.
    return physicalValueToFrameBufferValue(l * pqC);
}

inline float
inverseEotfSt2084(float v, float exponentScaleFactor = 1.0f)
{
    const float m1  = 0.1593017578125f;
    const float m2  = 78.84375f * exponentScaleFactor;
    const float c1  = 0.8359375f;
    const float c2  = 18.8515625f;
    const float c3  = 18.6875f;
    const float pqC = 10000.0f;

    // Convert the frame-buffer linear scale into absolute luminance (cd/m^2).
    float physical = frameBufferValueToPhysicalValue(v);
    float y        = physical / pqC; // Normalize for the ST-2084 curve

    float ym = powf(y, m1);
    return exp2f(m2 * (log2f(c1 + c2 * ym) - log2f(1.0f + c3 * ym)));
}

// -----------------------------------------------------------------------------
// ICtCp conversion.
// Reference: ITU-T T.302 (https://www.itu.int/rec/T-REC-T.302/en)
// -----------------------------------------------------------------------------
inline void
rgbToICtCp(const float* rgb, float* ictCp) // Input: linear Rec.2020
{
    float l = (rgb[0] * 1688.0f + rgb[1] * 2146.0f + rgb[2] * 262.0f) / 4096.0f;
    float m = (rgb[0] * 683.0f + rgb[1] * 2951.0f + rgb[2] * 462.0f) / 4096.0f;
    float s = (rgb[0] * 99.0f + rgb[1] * 309.0f + rgb[2] * 3688.0f) / 4096.0f;

    float lPQ = inverseEotfSt2084(l);
    float mPQ = inverseEotfSt2084(m);
    float sPQ = inverseEotfSt2084(s);

    ictCp[0] = (2048.0f * lPQ + 2048.0f * mPQ) / 4096.0f;
    ictCp[1] = (6610.0f * lPQ - 13613.0f * mPQ + 7003.0f * sPQ) / 4096.0f;
    ictCp[2] = (17933.0f * lPQ - 17390.0f * mPQ - 543.0f * sPQ) / 4096.0f;
}

inline void
iCtCpToRgb(const float* ictCp, float* rgb) // Output: linear Rec.2020
{
    float l = ictCp[0] + 0.00860904f * ictCp[1] + 0.11103f * ictCp[2];
    float m = ictCp[0] - 0.00860904f * ictCp[1] - 0.11103f * ictCp[2];
    float s = ictCp[0] + 0.560031f * ictCp[1] - 0.320627f * ictCp[2];

    float lLin = eotfSt2084(l);
    float mLin = eotfSt2084(m);
    float sLin = eotfSt2084(s);

    rgb[0] = std::max(3.43661f * lLin - 2.50645f * mLin + 0.0698454f * sLin, 0.0f);
    rgb[1] = std::max(-0.79133f * lLin + 1.9836f * mLin - 0.192271f * sLin, 0.0f);
    rgb[2] = std::max(-0.0259499f * lLin - 0.0989137f * mLin + 1.12486f * sLin, 0.0f);
}

// -----------------------------------------------------------------------------
// Unified color space (UCS): ICtCp. The sample's Jzazbz alternative is not vendored; the
// engine uses the sample's default, ICtCp.
// -----------------------------------------------------------------------------
inline void
rgbToUcs(const float* rgb, float* ucs)
{
    rgbToICtCp(rgb, ucs);
}
inline void
ucsToRgb(const float* ucs, float* rgb)
{
    iCtCpToRgb(ucs, rgb);
}

// -----------------------------------------------------------------------------
// GT7 Tone Mapping class.
// -----------------------------------------------------------------------------
struct GT7ToneMapping
{
    float sdrCorrectionFactor_;

    float framebufferLuminanceTarget_;
    float framebufferLuminanceTargetUcs_; // Target luminance in UCS space
    GTToneMappingCurveV2 curve_;

    float blendRatio_;
    float fadeStart_;
    float fadeEnd_;

    // Initializes the tone mapping curve and related parameters based on the target display luminance.
    // This method should not be called directly. Use initializeAsHDR() or initializeAsSDR() instead.
    void initializeParameters(float physicalTargetLuminance)
    {
        framebufferLuminanceTarget_ = physicalValueToFrameBufferValue(physicalTargetLuminance);

        // Initialize the curve (slightly different parameters from GT Sport).
        curve_.initializeCurve(framebufferLuminanceTarget_, 0.25f, 0.538f, 0.444f, 1.280f);

        // Default parameters.
        blendRatio_ = 0.6f;
        fadeStart_  = 0.98f;
        fadeEnd_    = 1.16f;

        float ucs[3];
        float rgb[3] = { framebufferLuminanceTarget_,
                         framebufferLuminanceTarget_,
                         framebufferLuminanceTarget_ };
        rgbToUcs(rgb, ucs);
        framebufferLuminanceTargetUcs_ =
            ucs[0]; // Use the first UCS component (I or Jz) as luminance
    }

    // Initialize for HDR (High Dynamic Range) display.
    // Input: target display peak luminance in nits (range: 250 to 10,000)
    // Note: The lower limit is 250 because the parameters for GTToneMappingCurveV2
    //       were determined based on an SDR paper white assumption of 250 nits (GRAN_TURISMO_SDR_PAPER_WHITE).
    void initializeAsHDR(float physicalTargetLuminance)
    {
        sdrCorrectionFactor_ = 1.0f;
        initializeParameters(physicalTargetLuminance);
    }

    // Initialize for SDR (Standard Dynamic Range) display.
    void initializeAsSDR()
    {
        // Regarding SDR output:
        // First, in GT (Gran Turismo), it is assumed that a maximum value of 1.0 in SDR output
        // corresponds to GRAN_TURISMO_SDR_PAPER_WHITE (typically 250 nits).
        // Therefore, tone mapping for SDR output is performed based on GRAN_TURISMO_SDR_PAPER_WHITE.
        // However, in the sRGB standard, 1.0f corresponds to 100 nits,
        // so we need to "undo" the tone-mapped values accordingly.
        // To match the sRGB range, the tone-mapped values are scaled using sdrCorrectionFactor_.
        //
        // * These adjustments ensure that the visual appearance (in terms of brightness)
        //   stays generally consistent across both HDR and SDR outputs for the same rendered content.
        sdrCorrectionFactor_ = 1.0f / physicalValueToFrameBufferValue(GRAN_TURISMO_SDR_PAPER_WHITE);
        initializeParameters(GRAN_TURISMO_SDR_PAPER_WHITE);
    }

    // Input:  linear Rec.2020 RGB (frame buffer values)
    // Output: tone-mapped RGB (frame buffer values);
    //         - in SDR mode: mapped to [0, 1], ready for sRGB OETF
    //         - in HDR mode: mapped to [0, framebufferLuminanceTarget_], ready for PQ inverse-EOTF
    // Note: framebufferLuminanceTarget_ represents the display's target peak luminance converted to a frame buffer value.
    //       The returned values are suitable for applying the appropriate OETF to generate final output signal.
    void applyToneMapping(const float* rgb, float* out) const
    {
        // Convert to UCS to separate luminance and chroma.
        float ucs[3];
        rgbToUcs(rgb, ucs);

        // Per-channel tone mapping ("skewed" color).
        float skewedRgb[3] = { curve_.evaluateCurve(rgb[0]),
                               curve_.evaluateCurve(rgb[1]),
                               curve_.evaluateCurve(rgb[2]) };

        float skewedUcs[3];
        rgbToUcs(skewedRgb, skewedUcs);

        float chromaScale =
            chromaCurve(ucs[0] / framebufferLuminanceTargetUcs_, fadeStart_, fadeEnd_);

        const float scaledUcs[3] = { skewedUcs[0],         // Luminance from skewed color
                                     ucs[1] * chromaScale, // Scaled chroma components
                                     ucs[2] * chromaScale };

        // Convert back to RGB.
        float scaledRgb[3];
        ucsToRgb(scaledUcs, scaledRgb);

        // Final blend between per-channel and UCS-scaled results.
        for (int i = 0; i < 3; ++i)
        {
            float blended = (1.0f - blendRatio_) * skewedRgb[i] + blendRatio_ * scaledRgb[i];
            // When using SDR, apply the correction factor.
            // When using HDR, sdrCorrectionFactor_ is 1.0f, so it has no effect.
            out[i] = sdrCorrectionFactor_ * std::min(blended, framebufferLuminanceTarget_);
        }
    }
};

#undef TONE_MAPPING_UCS_ICTCP
#undef TONE_MAPPING_UCS_JZAZBZ
#undef TONE_MAPPING_UCS
#undef GRAN_TURISMO_SDR_PAPER_WHITE
#undef REFERENCE_LUMINANCE

} // namespace gt7_reference

// clang-format on
