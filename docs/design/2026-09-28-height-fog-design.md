# Exponential Height Fog

## Goal

Make the view read like an Unreal scene with SkyAtmosphere plus Exponential Height Fog: distant
geometry and the ground below the horizon fade into a haze that meets the sky at the horizon without
a seam, and a warm glow builds up toward the sun.

Now, with the atmosphere alone (Hillaire 2020, the same model as Unreal's SkyAtmosphere), the ground
below the horizon is the planet sphere shaded with `ground_albedo` and seen through ~20 km of air
(`GroundLuminance`, `shaders/vulkan/atmosphere_sampling.glsl`). A kn5 track such as Spa ends a few
hundred metres out, so the view shows a flat band of planet ground with a hard edge at the horizon.
Unreal scenes hide the same thing with Exponential Height Fog, a non-physical artistic fog that
nearly every outdoor Unreal level has on. This design adds its counterpart.

Scope: the Atmosphere environment mode only. HDRI and None are left as they are.

## The model

A grey medium (the same extinction in r, g, b) whose extinction falls off exponentially with world
height:

```
sigma(y) = density * exp(-(y - fogHeight) * falloff)          per metre
```

- `density`: the extinction per metre at `fogHeight`.
- `falloff`: per metre. 1 / falloff is the scale height.
- `fogHeight`: world y of the reference height.

Along a camera ray `p(t) = c + t * d`, from `startDistance` to the surface's distance `s`, the
optical depth has a closed form. With `y0 = c.y + startDistance * d.y`, `L = s - startDistance`
and `k = falloff * d.y`:

```
tau = density * exp(-(y0 - fogHeight) * falloff) * L * (1 - exp(-k L)) / (k L)
```

When `|k L|` is below ~1e-4, the last factor is 1 - k L / 2 (the limit as the ray turns
horizontal). Clamp the exponent `-(y0 - fogHeight) * falloff` to at most ~80, so a camera far below
the fog height cannot overflow. `L <= 0` (the surface is closer than `startDistance`) gives tau = 0.

For sky pixels the ray runs to infinity:
- `d.y > 0`: tau = density * exp(-(y0 - fogHeight) * falloff) / (falloff * d.y), finite.
- `d.y <= 0`: tau is infinite and the fog is opaque.

So just above the horizon tau grows without bound, and below it the fog is opaque. The horizon is
seamless by construction.

Opacity is `min(1 - exp(-tau), maxOpacity)`, and the fog is applied as

```
color' = color * (1 - opacity) + inscatter * opacity
```

### In-scattered light

The fog colour is not a picked colour. It comes from the light the atmosphere already computes, so
it follows the time of day and the atmosphere settings on its own:

```
inscatter = albedo * ( skyAverageRadiance
                     + sunIlluminanceAtCamera * HenyeyGreenstein(anisotropy, dot(d, toSun)) )
```

- `skyAverageRadiance`: the atmosphere's radiance SH L0 times Y00 = 0.282095, which is the
  radiance averaged over the sphere, including the lit ground below the horizon. It is
  `skyIrradiance.coefficients[0].rgb * 0.282095` (set 0 binding 7), the same convention as
  `VulkanAtmosphere::GetSkyAverageRadiance`. An isotropic phase function over that light gives
  exactly this average.
- `sunIlluminanceAtCamera`: `ubo.sunIlluminance.rgb` times
  `SampleTransmittance(atmosphereTransmittanceLut, viewHeight, dot(toSun, up))` at the camera.
  Evaluated once per pixel at the camera rather than along the ray; the fog layer is thin.
- `HenyeyGreenstein(g, cos) = (1 - g^2) / (4 pi (1 + g^2 - 2 g cos)^1.5)`. This is the sun-facing
  glow, Unreal's directional inscattering, but physically normalised.
- `albedo`: an rgb tint the artist sets, default white.

No shadowing of the sun inside the fog. That would be Unreal's Volumetric Fog, which this design
leaves out.

## Where it is applied

1. **Surfaces**: inside `ApplyAerialPerspective(color, worldPosition)` in
   `atmosphere_sampling.glsl`, after the aerial perspective lookup:
   `ApplyHeightFog(atmosphereResult, worldPosition)`. The fog sits on top of the atmosphere, which
   is also Unreal's order. Every caller then gets it with no changes:
   - `deferred_lighting.frag:136` (opaque, deferred)
   - `triangle.frag:327` (forward and transparent)
   - `gi_composite.frag:79`, which derives the transmittance as `Apply(1) - Apply(0)`. This stays
     right because the fog is also of the form `color * T + L`.
2. **Sky**: `sky.frag`, only in `ENVIRONMENT_ATMOSPHERE`, after `SampleSky(direction)`:
   `ApplyHeightFogToSky(luminance, direction)`, which is the infinite-distance form above. Keep it
   out of `SampleSky` itself. `SampleSky` also feeds the environment probe and the sky SH
   (`atmosphere_irradiance.comp`), and fogging those would feed the fog colour back into itself.
3. Keep the rename-free, one-function structure: `ApplyHeightFog`, `ApplyHeightFogToSky` and
   `HeightFogInscatter` live in a new `shaders/vulkan/height_fog.glsl`, included by
   `atmosphere_sampling.glsl`.

Pre-exposure is unchanged: the callers multiply by `ubo.exposure.x` after these functions, as now.

## Data

### Scene settings

New struct in `engine/scene/scene_environment.h`, a member of `SceneEnvironment`, not of
`AtmosphereSettings`, so it can later serve other modes:

```cpp
struct HeightFogSettings
{
    bool enabled = false;               // off for scenes saved before the fog existed
    float density = 0.002f;             // extinction per metre at fogHeight
    float heightFalloff = 0.02f;        // per metre (a 50 m scale height)
    float fogHeight = 0.0f;             // world y, metres
    float startDistance = 0.0f;         // metres from the camera
    float maxOpacity = 1.0f;            // [0, 1]
    glm::vec3 albedo{1.0f, 1.0f, 1.0f}; // tint of the in-scattered light
    float anisotropy = 0.6f;            // Henyey-Greenstein g, [0, 0.95]
    bool operator==(const HeightFogSettings&) const = default;
};
```

- `EditorScene::AddDefaultSunAndSky` (`engine/logic/editor_scene.cpp:503`) sets
  `m_environment.heightFog.enabled = true`, so new scenes have fog and old scene files do not
  change.
- The defaults are a starting point. Tune them against the Spa grid view (below) until the
  horizon band is gone and the track 300 m out is still clearly readable. Then record the chosen
  values here.

### YAML

`environment.height_fog` next to `environment.atmosphere` in `editor_scene.cpp`, read around
line 224 and written around line 254. Keys: `enabled`, `density`, `height_falloff`, `fog_height`,
`start_distance`, `max_opacity`, `albedo`, `anisotropy`. A missing node keeps the struct defaults.

### Uniforms

`EnvironmentUniformData` (`engine/renderer/atmosphere.h`) grows from 18 to 21 vec4s. Update the
`static_assert` and the matching block at the end of `CameraBuffer` in
`shaders/vulkan/scene_common.glsl`. Check the `CameraUniformData` offset asserts in
`engine/renderer/vulkan/uniform_buffer.h`.

```
vec4 heightFogDensity;   // x density per m (0 when off), y falloff per m, z fogHeight, w startDistance
vec4 heightFogColor;     // rgb albedo, w maxOpacity
vec4 heightFogParams;    // x anisotropy, yzw unused
```

`BuildEnvironmentUniformData` fills them, clamping as `BuildAtmosphereParameters` does:
- density [0, 1]
- falloff [1e-5, 1]
- maxOpacity [0, 1]
- anisotropy [0, 0.95]
- albedo [0, 1]
- startDistance >= 0

Density is written as 0 unless the fog is enabled and the mode is Atmosphere, and the shaders
return early on density 0.

`VulkanEnvironmentProbe` recaptures whenever `CaptureKey(environment)` changes. That is a
`memcmp` of the whole struct (`environment_probe.cpp:48` and `:157`). The fog does not affect the
probe, so `CaptureKey` must zero the three fog vec4s, as it already zeroes the camera's horizontal
position. Otherwise every fog edit would recapture the probe.

### Editor UI

`DrawEnvironmentEditor` (`engine/editor/ui/editor_scene_panel.cpp:180`), inside the Atmosphere
branch, a "Height fog" collapsing section:
- Enabled checkbox
- Density (logarithmic drag, 0 to 0.1 per m)
- Height falloff (logarithmic, 1e-4 to 1)
- Fog height (m)
- Start distance (m)
- Max opacity
- Albedo (ColorEdit3)
- Anisotropy

## C++ mirror and tests

`engine/renderer/height_fog.h/.cpp` holds the same math on the CPU, for the tests:
`HeightFogOpticalDepth(settings, cameraPosition, direction, distance)`,
`HeightFogSkyOpticalDepth(...)` and `HenyeyGreenstein(g, cos)`. The GLSL mirrors it line for
line, as `white_balance` and `atmosphere` already do.

`tests/height_fog_tests.cpp` (register in `tests/CMakeLists.txt` like
`miniengine_white_balance_tests`):
- The closed form matches a 10 000-step numerical integral, for rays up, down and horizontal.
- Near-horizontal rays are continuous across the `|k L| < 1e-4` switch.
- The sky depth diverges as `d.y` goes to 0+, and is infinite for `d.y <= 0`.
- `startDistance` beyond the surface gives 0.
- A camera 10 km below `fogHeight` stays finite (the exponent clamp).
- HG integrates to 1 over the sphere, and `g = 0` is 1 / (4 pi).
- `density = 0` gives 0.

Extend `tests/scene_environment_tests.cpp` with a YAML round trip of `HeightFogSettings`, and
check that a scene without `height_fog` reads as disabled. Extend `tests/atmosphere_tests.cpp`:
the fog vec4s are packed, the density is 0 when disabled or in HDRI mode, and the values are
clamped.

## Verification

Headless, from `captures/`, which already has a replay of the Spa grid view:

```
miniengine_app --state viewport_20260928_142443.state.yaml --frames 600 --capture <out>.png
```

The state's scene file has no `height_fog` node, so copy the scene YAML to a temporary file with
the fog enabled and point a copied state at it. The previous colour-tone check did this with
`tmp_new.scene.yaml`, deleted afterwards.

Accept when:
- there is no visible edge at the horizon, and the band below it matches the sky just above;
- the grandstand and pit building (~100 to 300 m) keep their contrast, and only the far edge
  softens;
- turning toward the sun shows a warm glow in the haze;
- the GPU time added to deferred lighting and the sky is under 0.05 ms at 1080p (it is a few ALU
  ops per pixel);
- all ctest targets pass.

## Not in scope

- Volumetric fog (froxels, sun shadowing in the fog, local lights scattering). This is Unreal's
  separate Volumetric Fog feature.
- A second fog layer (Unreal's Second Fog Data).
- Fog in HDRI or None mode.
- Fog in SSR, DDGI rays and the environment probe. Reflections show the unfogged sky.
- Fog in the white balance references. The frame colour measurement sees the fogged image
  anyway, and the light references stay the physical lights.

## Amendments (implementation, 2026-09-28)

- **Chosen defaults**: density 0.001 per m, falloff 0.02 per m, fog height 0, start 0, max opacity
  1, albedo white, anisotropy 0.3. Tuned against the Spa grid view: at density 0.002 the pit
  building already greyed; at g 0.6 the haze toward the sun (HG peak ~2 / sr against the sky's
  ~1 / 4 pi average) washed out the sky 20 degrees above the horizon.
- **Optical depth factored at the denser end**: tau = density * exp(max(a0, a1)) * L * f(|k L|),
  with a0, a1 the height exponents at the segment's ends and f(x) = (1 - exp(-x)) / x. The form
  above, factored at the start, underflows for a camera kilometres above the fog looking down (the
  start density is exp(-100) and the ground fog vanishes) and overflows exp(-k L) for long falling
  rays. The exponent clamp of 80 now applies to max(a0, a1).
- **Series switch at |k L| < 1e-2** with 1 - x / 2 + x^2 / 6, instead of 1e-4 with 1 - x / 2: the
  quotient loses ~1e-3 of its float precision at 1e-4, and the series' error at 1e-2 is 4e-8.
- **Sun at the camera on the CPU**: `heightFogParams.yzw` carries albedo * sun illuminance *
  `ComputeTransmittanceToSpace` at the camera (zero once the sun is below the ground sphere),
  built in `BuildEnvironmentUniformData`. Per pixel, the ray-sphere test and the transmittance LUT
  lookup cost the lighting pass +0.23 ms at 2913 x 1091 (eight alternating runs, medians 2.66 vs
  2.43 ms). With the term on the CPU the medians are 2.414 ms (on) vs 2.433 ms (off), inside
  the run-to-run noise of about 0.3 ms. The sky average still comes from the SH buffer on the GPU.
- Looking toward the sun, auto exposure rises about 0.8 EV with the fog on: the haze in front of
  the backlit buildings is brighter than the planet ground it replaces. That is the camera
  responding, not a fog error.
