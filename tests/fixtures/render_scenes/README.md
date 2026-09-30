# Render Scenes

The scenes the rendering features were accepted with, by image, on 2026-09-25. They are not unit
tests: open them in the editor, or capture them from the command line, and compare before and after
a change.

`assets/` is not version controlled (see `.gitignore`), so the scenes live here and a script copies
them into the asset workspace, where the editor resolves their models:

```bash
./scripts/install-render-scenes.sh
```

```powershell
.\scripts\install-render-scenes.ps1
```

Models go to `assets/models/<name>/` with their uuid sidecars, scenes to `assets/scenes/test/`.
Existing files are never overwritten. Then, for example:

```bash
out/build/macos-debug/app/miniengine_app --scene assets/scenes/test/iridescence.yaml --frames 1500 --capture iridescence.png
```

A capture can also place the camera (`--camera x,y,z,yaw,pitch`, metres and degrees), move it a fixed
distance every frame (`--camera-velocity x,y,z`), start in a Graphics Debug view (`--debug-view N`,
`GBufferDebugView`'s numbers) or with the DDGI probes off (`--no-ddgi`). For example, inside the track's
tunnel, then the probes' irradiance after driving 200 m at 1 m per frame:

```bash
miniengine_app --scene assets/scenes/test/ddgi_track.yaml --frames 400 --camera 0,0.2,-100,-90,0 --capture tunnel.png
miniengine_app --scene assets/scenes/test/ddgi_track.yaml --frames 200 --camera 0,0.2,150,-90,-5 --camera-velocity 0,0,-1 --debug-view 15 --capture moving.png
```

The editor's Capture viewport writes, beside `captures/viewport_<time>.png`, a snapshot of the scene
(`.scene.yaml`) and the rest of what the frame depends on (`.state.yaml`: camera, viewport size,
every Graphics Debug setting). `--state FILE` replays it: it loads the snapshot, sets the rest, and
counts `--frames` only once the scene, its textures and its ray scene have loaded (`--wait-for-scene`
does that alone). Options after it override it:

```bash
miniengine_app --state captures/viewport_20260927_192026.state.yaml --frames 600 --capture replay.png
```

`--reference PREFIX` compares the DDGI probes with a CPU path tracer over the same ray scene, on every
`--reference-stride`-th pixel with `--reference-samples` paths each, after the last frame, which must
show view 15. It logs the bias and the relative error, writes the two images as PFM and side by side
with their ratio (`PREFIX_compare.png`: red where the probes are brighter), and compares the nearest
probes themselves along six axes (`PREFIX_probes.csv`). `--reference-explain COLUMN,ROW` logs one
point's lookup probe by probe. The path tracer's sky is one radiance, so the comparison scenes light
with the sun alone (`environment: none`, a zero Ambient light):

```bash
miniengine_app --scene assets/scenes/test/cornell_box.yaml --frames 600 --camera 0,0,-4,-90,0 --debug-view 15 --reference cornell --reference-samples 2048
```

| Scene | What it shows | Feature |
| --- | --- | --- |
| `sponza_local_shadows.yaml` | Sponza with a point, a spot and an area light casting shadows | Local-light shadows |
| `sponza_spot_shadow.yaml` | A spot light past the front-left column; its shadow on the floor and wall | Local-light shadows |
| `anisotropy_layers.yaml` | Anisotropy 0 to 1, rotation, direction map; striped coat map, checker sheen map, coat plus anisotropy | Anisotropy and layer textures |
| `smooth_spheres_sun.yaml` | Smooth black spheres, chrome and white under the sun: crisp sun highlights | BRDF accuracy (GGX floor fix) |
| `smooth_spheres_lamp_point.yaml` | The same spheres in the dark under a point lamp of radius 0 | BRDF accuracy (source radius) |
| `smooth_spheres_lamp_sized.yaml` | The same lamp with a 0.2 m source radius: highlights become disks | BRDF accuracy (source radius) |
| `smooth_spheres_hemisphere.yaml` | The same spheres under only a hemisphere light: blue sky above, brown ground below | Hemisphere light |
| `bounce_box.yaml` | A sunlit white floor and a white wall facing away from the sun, no sky or ambient: the wall's foot glows with the floor's bounce | One-bounce indirect diffuse |
| `sponza_sun_bounce.yaml` | Sponza with the sun falling through the open roof onto one side of the floor | One-bounce indirect diffuse |
| `area_light_floor.yaml` | A 1.6 x 0.4 m panel over a glossy floor and three spheres | LTC area lights |
| `ior_specular_coat.yaml` | IOR 1.33 / 1.5 / 2.4, specular colour and 0; coat normal map; coat, sheen and anisotropy together | G-buffer layers, IOR, specular |
| `iridescence.yaml` | Thin films of 250, 400, 550 nm, an anodised metal, a control | Forward-shaded materials, iridescence |
| `texture_transforms_unlit.yaml` | A checker scaled, rotated and offset; the second UV set; unlit; a Mask cutout's shadow; a rotated normal map | Texture transforms, second UV set, unlit |
| `cornell_box.yaml` | A Cornell box (`tools/render_scenes/make_cornell_box.py`) lit only by the sun through a hole in its ceiling: the red and green walls' colour on the white ones | Cascaded DDGI, against the reference path tracer |
| `ddgi_track.yaml` | A generated outdoor track (`tools/render_scenes/make_ddgi_track.py`): road, 60 m tunnel, roofed grandstand, leaf-card trees, a car, afternoon sun | Cascaded DDGI |
| `car_test_track.yaml` | A vehicle test track (`tools/render_scenes/make_car_test_track.py`) and a box car: suspension course (humps, washboard, one-sided bumps, kerb, jump), body attitude course (6/12 degree slopes, waves, a bank swinging ±12 degrees), grip lanes (asphalt to ice, split friction, friction bands). Select the car and press F5 | Vehicle physics |

The two Sponza scenes need Khronos' New Sponza (`assets/NewSponza_Main_glTF_003/`), which is not in
the repository either. Sponza is turned 90 degrees and lowered 1.7 m in front of the default camera,
and a dim Ambient light replaces the fallback ambient so the local lights read.

The sphere models are generated glTFs: one UV sphere (radius 0.4 m, with tangents) per material, one
node each, and for `area_light_floor` an 8 x 8 m floor. Each scene file starts with a comment that
says what to look for.
