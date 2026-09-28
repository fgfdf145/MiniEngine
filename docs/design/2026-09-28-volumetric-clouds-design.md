# Volumetric Clouds

## Goal

Match the GT7 reference captures of Spa and Mount Panorama (grid view, the run up to Raidillon,
Conrod Straight): a broken cumulus layer over a blue sky, white where the sun lights the tops, grey
blue under the flat bases, a bright silver lining on the edges between the camera and the sun, and
the sun's disk and glare hidden or shining through gaps. The atmosphere (Hillaire 2020) and the
height fog already give the sky and the haze; the sky itself is empty.

Scope: the Atmosphere environment mode only, on the sky and in the environment probe (so the car's
clear coat reflects the clouds). HDRI and None are left as they are.

## The model

A cumulus layer in a shell around the planet, `baseAltitude` to `baseAltitude + thickness` above
the ground (curved, so the layer drops to the horizon), ray marched through two tiling noise
volumes (Schneider, *The Real-time Volumetric Cloudscapes of Horizon Zero Dawn*, 2015, and *Nubis*,
2017) and lit as Hillaire, *Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite*,
2016.

### Noise

`cloud_noise.comp` builds both volumes once, on the first `VulkanAtmosphere::Record`:

- shape, 128³ RGBA8: r Perlin-Worley (billowy seven-octave Perlin remapped by inverted Worley fBm
  at 4 cells per tile), g b a Worley fBm at 4, 8 and 16 cells;
- detail, 32³ RGBA8: r g b Worley fBm at 2, 4 and 8 cells.

Every noise wraps at the tile edge (hashes of the wrapped cell), so a REPEAT sampler shows no seam.
Each channel is stretched from its 1st to 99th percentile onto [0, 1]
(`NormalizePerlinWorley`, `NormalizeWorley`), measured over the volumes. Without that the raw
Perlin-Worley sits in [0.55, 0.9] and the Nubis base-shape formula is dense nearly everywhere: the
first CPU render of the layer was a uniform overcast at coverage 0.45.

### Density

Per kilometre, at a point p (planet-centred km) with height fraction h in the layer:

```
weather  = stretch(shape(p.xz * f_weather, 0.37).r * 0.65 + shape(p.xz * f_weather * 2.63 + 0.19, 0.71).g * 0.35)
shape    = remap(shape(p * f_shape).r, (1 - fbm(gba)) * 0.6, 1, 0, 1)          (clamped)
field    = (0.55 * weather + 0.45 * shape) * gradient(h)
cloud    = clamp((field - (1 - coverage)) / 0.2, 0, 1)                         (0 at coverage 0)
cloud    = remap(cloud, erosion * detail * mix(fbm_d, 1 - fbm_d, clamp(5 h, 0, 1)), 1, 0, 1)
sigma_t  = cloud * density
```

- The weather map is two slices of the shape volume read over the ground, so no third texture;
  the second tap at 2.63 times the frequency breaks the tile's repetition.
- `gradient(h)` is the cumulus profile: rises over the lowest tenth (flat bases), falls linearly
  from 30 % to the top (rounded towers where the field is highest).
- The coverage ramp is 0.2 wide, so edges are sharp as cumulus's are, and the share of the sky
  covered follows `coverage` roughly linearly (top-down slices at the base: 4 % at 0.2, 33 % at
  0.45, 76 % at 0.7).
- The detail erodes wispy at the base and billowy toward the tops, and fades out by 30 km: beyond
  it a step is longer than the detail's features, which then only alias.
- Where the weather alone cannot reach the threshold the shape taps are skipped.

### Lighting

Per ray, once: the sun's illuminance where the ray enters the layer (top-of-atmosphere
illuminance times the transmittance LUT there; zero if the planet hides the sun), the sky above
as `GroundSkyIrradiance / pi`, and the ground below as ground albedo times the ground's sky plus
sun irradiance over pi.

Per step with sigma_t > 0:

- optical depth toward the sun over six light steps doubling from thickness / 32 (the first two
  with detail);
- sun scattering: Wrenninge's octaves (a = b = 0.8, c = 0.5, 5 octaves) over the dual-lobe
  Henyey-Greenstein `mix(HG(g_f), HG(g_b), w)`, defaults g_f = 0.8 (the silver lining), g_b = -0.3,
  w = 0.3;
- ambient: `mix(groundBelow, skyAbove, h) * ambientScale`;
- Hillaire's energy-conserving step: `L += T * albedo * (sun * phase + ambient) * (1 - exp(-sigma_t dt))`,
  `T *= exp(-sigma_t dt)`; stop below T = 0.003.

### Steps and jitter

`CloudShellInterval` clips the ray to the shell (below it: from leaving the base sphere, unless the
planet is hit first; inside or above: to the base sphere if met) and to 120 km. Steps grow from 32
overhead to 96 at the horizon with the path length over four thicknesses. The first step is
jittered by interleaved gradient noise stepped with the TAA frame index (`cloudParams.w`), so TAA
averages it; without TAA the index stands still and so does the noise. TAA reprojects sky pixels
as infinitely far, which a layer kilometres away is nearly.

### Composite

In `sky.frag`, before the height fog:

```
fade = exp(-distance / hazeDistance)
sky' = sky * T + clouds * fade + skyHaze * (1 - T) * (1 - fade)
```

`sky` has the sun's disk, which the clouds hide; `skyHaze` is `SampleSkyForLighting`, the air's
own light. `distance` is the march's transmittance-weighted depth. The fade stands for the air
between camera and cloud (the aerial perspective volume stops at 32 km and is screen aligned, so
the probe could not use it).

`environment_capture.comp` applies the same with 12 to 32 steps, one march per texel centre over
the average of the four sky samples, no jitter. `CaptureKey` ignores the frame index, so a still
sky is still captured once.

## Data

- `CloudSettings` in `SceneEnvironment` (`scene_environment.h`), YAML `environment.clouds`
  (enabled, coverage, base_altitude, thickness, density, shape_scale, detail_scale, weather_scale,
  detail_erosion, forward_anisotropy, back_anisotropy, back_weight, albedo, ambient_scale,
  haze_distance). Scenes without the node read as off; `AddDefaultSunAndSky` turns them on.
- `EnvironmentUniformData` grows from 21 to 25 vec4s: `cloudLayer` (base km, thickness km,
  coverage, extinction per km, 0 when off or outside Atmosphere), `cloudScales` (frequencies per km
  and erosion), `cloudPhase` (g_f, g_b, w, albedo), `cloudParams` (ambient scale, haze km, -, frame
  index). Mirrored at the end of the environment block in `scene_common.glsl`.
- Set 0 bindings 24 and 25: the shape and detail volumes, GENERAL, linear REPEAT sampler
  (`VulkanAtmosphere::GetCloudShapeNoiseBinding` / `GetCloudDetailNoiseBinding`); the
  atmosphere's compute set gains storage bindings 7 and 8 for `cloud_noise.comp`.
- Editor: a "Clouds" section under the Atmosphere environment.

## Defaults

Coverage 0.45, base 1500 m, thickness 2500 m, density 0.02 per m, shape tile 7 km, detail tile
900 m, weather tile 40 km, erosion 0.35, g_f 0.8, g_b -0.3, back weight 0.3, albedo 0.98, ambient
1, haze 25 km. Tuned on a CPU port of the noise and the march (numpy, 320 x 180, a stand-in sky and
ACES in place of the atmosphere and the GT7 curve) against the GT7 captures:

- density 0.04 per m left the sun only the outer tens of metres of each cloud; the lit faces read
  grey against the sky. At 0.02 they are white with grey-blue bases.
- a 24 km weather tile repeated visibly across a 40 km top-down slice.
- the 40 km haze left a speckled band of far clouds at the horizon; 25 km lets it melt into the
  haze as in the captures.
- multiple scattering: GT7 measured sunlit cloud tops at 34 000 cd/m² (gt7-rendering-notes 1.1).
  With the sun 25° up behind the camera, the 99th percentile of the opaque clouds' luminance was
  11 800 cd/m² with Hillaire's octaves (a = b = 0.5, 3), 15 900 with 0.7 and 4, and 20 800 with
  0.8 and 5, which also looks closest to the Spa grid capture. The octave sum at zero depth is then
  3.4 times single scattering; thin wisps are too faint for that to show.

## C++ mirror and tests

`engine/renderer/volumetric_clouds.h/.cpp` hold the scalar functions line for line
(`CloudHeightGradient`, `CloudWeather`, `CloudShape`, `CloudField`, `CloudCoverageRamp`,
`CloudPhase`, `CloudSunScattering`, `CloudShellInterval`, `ClampCloudSettings`).
`tests/volumetric_clouds_tests.cpp`: clamping; the profile's zeros and plateau; the coverage ramp
(clear at 0, monotonic in coverage); the field's blend; the dual lobe integrates to 1; the octaves
exceed single scattering and fall with depth; the shell interval from the ground (straight up,
down into the planet, level beyond the horizon, the distance clip), from inside and from above.
`tests/atmosphere_tests.cpp` checks the packing, the off states and the clamps;
`tests/scene_environment_tests.cpp` the YAML round trip, a scene without `clouds` reading as off
and a new scene having them on.

## Verification

Not yet run on a GPU. Headless, as for the height fog, with the Spa grid state and a copy of its
scene with `environment.clouds.enabled: true`:

```
miniengine_app --state viewport_20260928_142443.state.yaml --frames 600 --capture <out>.png
```

Accept when the layer reads like the captures (broken cumulus, white tops, grey-blue bases, silver
edges toward the sun, the sun hidden behind a cloud and its glare shining through gaps), the
clear coat reflects the clouds, and there is no visible tiling. Measure the sky pass at the
viewport's 2913 x 1091: the march is full resolution in every sky pixel.

## Not in scope, next

- **Cost.** A full-resolution march in the sky pass is the simplest correct first step. If it costs
  more than ~1.5 ms, march at quarter resolution in a compute pass with temporal reprojection
  (Schneider's 1 / 16 per frame) and upsample in the sky pass.
- **Cloud shadows.** The sun light and the ground ignore the clouds; a top-down transmittance map
  over the camera would dim both, and the sun behind a cloud would dim the scene.
- **Wind.** The layer is static; an offset per second in the uniforms would move it, at the cost of
  recapturing the probe.
- **Clouds in the sky SH and DDGI.** The ambient light is the clear sky's.
- **Other cloud types.** One cumulus profile; stratus and cirrus would each need a profile and a
  weather channel.
