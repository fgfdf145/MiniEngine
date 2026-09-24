// The HDR target's unit, shared by the shaders that convert to or from it and by
// engine/renderer/exposure.h, which tests/exposure_tests.cpp checks against this file. Like
// gt7_tonemap.glsl it is written in the subset GLSL and C++/GLM both accept.
//
// The HDR target and GB3 hold pre-exposed values in GT7's frame-buffer unit, where 1.0 is
// 100 cd/m^2 as displayed: physical radiance times ExposureFromEv100 (1.0 = sensor saturation)
// times this scale, which places saturation at GT7's 250 cd/m^2 SDR paper white.
const float kFrameBufferUnitsPerExposed = 2.5f;
const float kExposedPerFrameBufferUnit = 0.4f;
