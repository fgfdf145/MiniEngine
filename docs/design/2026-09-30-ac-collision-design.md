# Assetto Corsa Collision

## Goal

Drive on a track the way its author meant. A kn5 track ships its collision apart from its looks: a
`physics.FBX` of meshes the game never draws, each named after the surface it is (`20ASPH-SPA_BLACK_004`,
`01WALL003`), with friction per surface in `data/surfaces.ini`. The import used to drop all of it, and
Play mode collided the car with whatever was drawn: 975,000 grass cards on Spa's shoulder, marshals,
lamp posts, a car that bounced and stopped at the verge.

## What Spa ships

`models.ini` places six kn5. `3.kn5` is the physics: 455 meshes, none rendered, 588,295 triangles, every
name a number then a surface KEY. `1.kn5` also holds hidden meshes (`cameraface*`, TV camera planes),
whose names start with a letter. Surfaces: `ASPH-SPA_BLACK` (0.98, 67% of the triangles), the
red/green/blue/violet run-off asphalts (0.96 to 0.97), `KERB` and `CURB` (0.94), `CONCRETE` (0.85),
`GRILLE`, `OUT`, `PITSPA`, `CARPET` (0.65), and three names no surfaces.ini defines: `GRASS` and `SAND`
(the game's own `system/data/surfaces.ini` does: 0.6 and 0.8) and `WALL` (nobody does).

## Decisions

1. **A physics mesh is a mesh the game never draws whose name starts with a digit.** That is AC's own
   rule for what collides; the `cameraface` planes and the `AC_START_n` cubes do not qualify.
2. **A glTF extension, `MINIENGINE_collision`, on the node**, `{"surface": "GRASS", "friction": 0.6}`;
   the mesh has POSITION and indices only, no material. Other viewers draw it as an untextured skin
   (glTF has nothing that says "never draw", and `KHR_node_visibility` would need a loader that honours
   it everywhere); the importer is for this engine.
3. **Friction from surfaces.ini.** The track's `data/surfaces.ini`, then the game's
   `system/data/surfaces.ini` found above the track folder, then the four keys every install has
   (`ROAD` 1, `GRASS` 0.6, `KERB` 0.92, `SAND` 0.8). A name matches the longest KEY it starts with, after
   its leading digits, ignoring case; the first definition wins. An unknown one is its own name at
   friction 0.8.
4. **The loader collects them apart**: `LoadedModelData::collisionMeshes`, one entry per surface with its
   meshes merged in the model's space. They are not submeshes, not drawn, not in the bounds, counted in
   the model cache's size. The extension is on the loader's implemented list (which also lacked
   `MINIENGINE_materials_detail_layers`, warned about on every multilayer import).
5. **Play mode collides a model through its collision meshes alone** when it has any, each surface as a
   static body at its friction; the drawn meshes of that entity are left out. A model without them
   (Sponza, a hand-made track) collides through its drawn meshes as before, less glass, decals and
   ground cover (`IsGroundCover`, 5aef7f8).
6. **Friction reaches the tyres.** Jolt's wheel grip is `sqrt(tyre * body friction)`; static geometry
   had Jolt's default 0.2, so a car had less than half the grip of a tyre on tarmac (0.49 against 1.08
   at full throttle's peak) and reached 34 km/h in five seconds. `PhysicsWorld::AddStaticMesh` takes the
   friction, defaulting to 1.
7. **Wheels ignore walls.** A static mesh is split by triangle normal into the walkable ones (normal
   within 70 degrees of up) and the steep ones, two bodies, the second flagged in its user data. The
   vehicle's wheel casts filter out flagged bodies; the chassis box still hits both. Jolt's cylinder cast
   has no slope limit (its ray and sphere casts do): a car scraping a wall at 6 degrees, the ground
   running on past the wall's foot, climbed it to 0.5 m and 14 degrees of roll.

## Out of scope

- What the surface does besides grip: `IS_VALID_TRACK` (cutting the track, penalties), `IS_PITLANE`,
  the rumble of `SIN_HEIGHT`/`SIN_LENGTH` on grass and sand, `DAMPING`, dirt on the tyres, kerb
  vibration and sound.
- Dynamic objects (`DYNAMIC_OBJECT_n` in models.ini) and the car's own `collider.ini`.
- Welding the physics meshes' vertices (1.6M vertices become 0.33M): tried, no effect on the ride.

## Automated Verification

- kn5: the rules (`IsPhysicsMeshName`, `ParseSurfaces`, `MatchSurface`: longest key, case, digits, a
  malformed FRICTION, a section that is not a surface); a track import writes its physics mesh as
  collision only, on the surface and friction the track's surfaces.ini gives, in the model's space,
  outside its bounds.
- Physics: friction sets grip (tarmac against ice); a car scraping a wall at 6 degrees stays on the
  ground and the wall stops the chassis (fails, 0.48 m up, with the wall filter off).

## Manual Acceptance (by drive)

Re-import Spa (`assets/spa` still holds the old import, without collision). Play the Porsche from the
grid: it leaves the line at once instead of crawling, crosses the shoulder and the kerbs without
bouncing, scrapes a wall without climbing it, and drives through grass, marshals and pit props.

## Measured on Spa

Import: 455 physics meshes, 588,295 triangles, +21 MB in the buffer; the drawn meshes are unchanged
(1,029 submeshes, 2,354,866 triangles, the render differs from before by noise). Collision: 22 bodies
and 586,000 triangles, against 852 and 1,275,000 through the drawn meshes. Sixteen crossings of either
shoulder of the start straight (four starting offsets, four angles towards the verge), 12 s each at
full throttle: with the
wheels colliding with walls 5 bounced (over 6 degrees of roll or over ten frames with a wheel off the
ground), with the wall filter 1 did (3.5 degrees, twenty frames); through the drawn meshes, 5 with the
filter. Nought to 100 km/h takes 7.5 to 11 s on tarmac on the default tuning (runs that leave it for
the grass or the wall take longer).
