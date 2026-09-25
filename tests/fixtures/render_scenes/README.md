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

| Scene | What it shows | Feature |
| --- | --- | --- |
| `sponza_local_shadows.yaml` | Sponza with a point, a spot and an area light casting shadows | Local-light shadows |
| `sponza_spot_shadow.yaml` | A spot light past the front-left column; its shadow on the floor and wall | Local-light shadows |
| `anisotropy_layers.yaml` | Anisotropy 0 to 1, rotation, direction map; striped coat map, checker sheen map, coat plus anisotropy | Anisotropy and layer textures |
| `smooth_spheres_sun.yaml` | Smooth black spheres, chrome and white under the sun: crisp sun highlights | BRDF accuracy (GGX floor fix) |
| `smooth_spheres_lamp_point.yaml` | The same spheres in the dark under a point lamp of radius 0 | BRDF accuracy (source radius) |
| `smooth_spheres_lamp_sized.yaml` | The same lamp with a 0.2 m source radius: highlights become disks | BRDF accuracy (source radius) |
| `area_light_floor.yaml` | A 1.6 x 0.4 m panel over a glossy floor and three spheres | LTC area lights |
| `ior_specular_coat.yaml` | IOR 1.33 / 1.5 / 2.4, specular colour and 0; coat normal map; coat, sheen and anisotropy together | G-buffer layers, IOR, specular |
| `iridescence.yaml` | Thin films of 250, 400, 550 nm, an anodised metal, a control | Forward-shaded materials, iridescence |
| `texture_transforms_unlit.yaml` | A checker scaled, rotated and offset; the second UV set; unlit; a Mask cutout's shadow; a rotated normal map | Texture transforms, second UV set, unlit |

The two Sponza scenes need Khronos' New Sponza (`assets/NewSponza_Main_glTF_003/`), which is not in
the repository either. Sponza is turned 90 degrees and lowered 1.7 m in front of the default camera,
and a dim Ambient light replaces the fallback ambient so the local lights read.

The sphere models are generated glTFs: one UV sphere (radius 0.4 m, with tangents) per material, one
node each, and for `area_light_floor` an 8 x 8 m floor. Each scene file starts with a comment that
says what to look for.
