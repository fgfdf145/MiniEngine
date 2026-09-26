# Hemisphere Lights, One-Bounce Indirect Diffuse, Sharper IBL

Asked for together on 2026-09-27 ("半球光和一次反射gi还有ibl"), after the Khronos comparison was
extended to the glTF core-spec models (whose findings are in `4a1e8cd`).

## Hemisphere Light

A light type with a sky colour above an up axis and a ground colour below, in cd/m^2 like Ambient.

- **Up axis**: the light transform's +Y, the axis a directional light shines down along, so rotating
  the entity tilts the horizon. No position.
- **Exact folding**: the irradiance of sky S above u and ground G below on a surface facing n is
  pi (S (1 + n.u) / 2 + G (1 - n.u) / 2), linear in n. Any number of hemisphere lights and Ambient
  lights therefore sum on the CPU into one constant (the existing `ambientLuminance`) and one gradient
  vector per colour channel (`ambientGradient[3]`, appended to the camera block). No light slot, no
  per-light loop, and the fallback ambient turns off as it does for an Ambient light.
- **Shading**: `SceneAmbientAlong(d)` everywhere the uniform ambient luminance was read: the diffuse
  lobe at N (exact), diffuse transmission at -N, the specular, coat and sheen lobes along their
  reflection vectors. For a lobe that is the cosine-blurred radiance rather than the hard horizon:
  exact for rough lobes, softer than reality for a mirror. Accepted: the light is a fill light.
- **Serialized** as `light_type: hemisphere` and `ground_color`; absent keys load the default ground.

## One-Bounce Indirect Diffuse

Screen-space, the indirect half of the visibility bitmask paper (Therrien et al. 2023) whose AO half
the engine already runs (`vbao_trace.comp`).

- **Why after lighting**: the bounced light is this frame's lit HDR image, which holds direct light,
  sky and ambient light and emission but not yet any indirect diffuse. Tracing it after the lighting
  pass and adding the result afterwards gives exactly one bounce with no feedback and no frame of
  lag. The alternative, sampling last frame's TAA history before lighting as SSR does, feeds the
  bounce back into itself (infinite bounces, and a frame late).
- **Passes** (deferred order, between Lighting and Scatter): `GiTrace` marches the AO's slices and
  32-sector bitmask; a sample's newly covered sectors receive its radiance (clamped to 64 frame-buffer
  units against fireflies) if it faces the receiver. The sectors are cosine-weighted, so the sum is
  irradiance / pi, the ambient term's unit. `GiResolve` filters it (depth- and normal-aware 5x5, then
  32-frame temporal accumulation with the AO's disocclusion test; history RGBA32F with distance and
  count packed into alpha). `GiComposite` is a full-screen triangle blended ONE + ONE over the HDR
  target: SceneGi times the diffuse albedo the ambient term uses (dielectric share under the
  dielectric's specular albedo), the material occlusion, the sheen and coat layers' transmission and
  the aerial perspective's transmittance.
- **Not lit**: forward-shaded, transmissive, Blend and unlit surfaces; off-screen light; specular.
- **Settings**: Graphics Debug, "Indirect diffuse (one bounce)": radius 3 m, thickness 0.5 m,
  2 slices x 12 steps, strength 1, filters. On by default; off in the Khronos reference view and the
  forward-only order. Debug view 13 shows SceneGi.

## IBL

- **Resolution**: the radiance cube goes from 128^2 to 256^2 per face (9 mips), the Sample Viewer's
  size. The roughness-0 reflections were visibly softer (IORTestGrid 5.8).
- **Caching**: the capture and prefilter ran every frame. They now run only when the environment
  uniform data changes (for the atmosphere, the camera's altitude to the metre instead of its
  position) or a new HDRI image loads (`Invalidate`). The 4x cost of the larger cube is paid only on
  changes; a static sky and camera cost nothing.
