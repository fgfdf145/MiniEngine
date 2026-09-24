# Bloom

## Goal

Bright light scatters in a real lens and eye: the sun, emissive surfaces and sharp specular
highlights bleed a soft glow into their surroundings. The renderer works in physical units, so a
sunlit highlight is thousands of times brighter than a shadowed wall, but none of that shows past
the tone mapper's clip. Bloom adds the glow, energy-conserving, so it reads as brightness rather
than as a filter.

With bloom off the renderer must produce the image it produces today.

## Decisions

1. **Jimenez 2014 (Call of Duty: Advanced Warfare), as most engines now ship it.** A chain of mips
   from half resolution, each a 13-tap downsample of the one above; then back up, each level a 3x3
   tent upsample of the one below added to its own downsample. The first downsample uses Karis'
   average (each 2x2 group weighted by `1 / (1 + exposed luma)`) so a single very bright pixel, the
   sun disk at 65504, cannot turn into a flickering blob.
2. **No threshold; a physical mix.** `result = mix(scene, bloom / levels, intensity)` with
   intensity 0.04 by default: every pixel gives up 4% of its energy to its surroundings, as a lens
   does, and the image's total energy is kept (short of what the blur pushes off screen).
3. **After TAA, in place, in both orders.** `ScenePassId::Bloom` runs after `Taa` and composites back
   into `SceneTaa`, which it declares as a write (it samples and stores it in `GENERAL`), so the
   exposure histogram and tone mapping read the bloomed image unchanged. It works on the TAA output
   so the glow is stable, and it needs no motion vectors, so the forward-only order has it too.
4. **The mip chain belongs to the pass.** One RGBA16F image with up to six levels, half resolution
   at the top, recreated with the scene targets, in `GENERAL`, rewritten every frame: a barrier at
   the start discards it, and memory barriers order its dispatches.
5. **One compute shader, four modes** (first downsample, downsample, upsample, composite), one set
   layout (a sampled source, a storage destination).
6. **Settings:** `RenderDebugSettings::bloom` (`enabled` true, `intensity` 0.04), "Bloom" and its
   intensity (0-0.2) in Graphics Debug. Off, the pass records nothing.
7. **The chain's sizes are a pure function**, `BuildBloomMipChain(extent)`: half the extent (rounded
   down, at least 1) per level, stopping at six levels or when a side would fall below 2.

## Automated Verification

- `BuildBloomMipChain`: halves from half resolution, never zero, at most six levels, stops before a
  side under 2; a 1x1 or 2x2 viewport gets a usable (possibly one-level) chain.
- Pass order tests: `Bloom` right after `Taa` in both orders.

## Manual Acceptance

1. Off: Sponza (five lights), both orders, within run-to-run noise of the previous build.
2. On: a scene with the sun or an emissive surface in view shows a soft glow around it; a dark area
   away from it does not change.
3. On: the mean brightness of Sponza changes little (the mix keeps energy).

## Out of Scope

- Lens dirt, anamorphic streaks, a brightness threshold.
- Bloom before TAA or at lower cost (quarter-resolution start).
