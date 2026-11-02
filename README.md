# Road Maker — Satisfactory mod (C++ source)

Adds to **Build menu → Transportation**:

| Tool | C++ class | Blueprint child to make |
|---|---|---|
| Road 1 wide | `ARMBuildableRoad` | `Build_Road_1x` (`mWidthInFoundations = 1`) |
| Road 2 wide | `ARMBuildableRoad` | `Build_Road_2x` (`mWidthInFoundations = 2`) |
| Road 3 wide | `ARMBuildableRoad` | `Build_Road_3x` (`mWidthInFoundations = 3`) |
| Guard rail | `ARMBuildableFence` | `Build_Fence_GuardRail` (`mFenceType = Metal Guard Rail`) |
| Cement blocks | `ARMBuildableFence` | `Build_Fence_Jersey` (`mFenceType = Cement Blocks`) |
| Grid fence | `ARMBuildableFence` | `Build_Fence_Grid` (`mFenceType = Metal Grid Fence`) |
| Intersection | `ARMBuildableIntersection` | `Build_RoadIntersection` |

Targets Satisfactory 1.2 (UE 5.6) with SML 3.12. Written without access to the game headers, so expect
a few compile fixes on the first build (see "First build" below).

---

## How it works

**Roads** are two-click spline pieces, placed like the 1.2 Vehicle Path tool:
- Click 1 sets the start, click 2 sets the end. Without snapping, the curve is one circular arc that
  keeps the start direction (same idea as railways). The **Straight** build mode forces a straight line.
- Aim near a free road end to snap: the new road continues tangentially from it, or arrives into it.
- Rules (per Blueprint, defaults): min turn radius 12 m (+4 m per extra foundation of width),
  max grade 25°, length 2–100 m. Breaking a rule shows a red reason on the hologram.
- Cross-section: `width × mRoadHeight` (default 200 cm, half of a 4 m foundation). The spline runs
  along the top centre; the bottom rests on what you aim at.
- Section 0 (top) uses `mTopMaterial` (asphalt), section 1 (sides, bottom, ends) uses `mBodyMaterial` (concrete).
- Anchors `StartAnchor` / `EndAnchor` (tag `RMRoadAnchor`, X axis pointing outward) sit on each end.
- Cost and refund scale with size: recipe = cost of one 8 m × 1-foundation tile.

**Fences**
- Aim at a road → the fence follows the edge on the side you look at, along that whole road
  section, one click. The edge is the centre line shifted sideways: `Q(s) = P(s) + d·N(s)`, where
  `N` is the level normal of the centre spline and `d = ±(width/2 − inset)`. The exact offset of a cubic
  is not a cubic, so it is resampled every 4 m and refitted (Catmull-Rom).
- Aim anywhere else → free two-click spline, no rules.

**Intersection**
- Aim between road ends. The nearest free ends (max 6) within 25 m are joined. One click.
- Ends are sorted by angle around their centroid. Neighbouring roads are joined by a curb fillet: a
  quadratic Bézier whose control point is where the two road edges meet when extended. The outline
  is fan-triangulated from the centre and extruded down to the road thickness.
- **Saving:** only the road ends are saved (`FRMRoadEnd`: centre, direction, width — about 28 bytes
  each) plus the thickness. The mesh is rebuilt on load. Same for roads and fences: only spline points
  are saved, so save files stay small and the variable geometry is never serialized.
- If the ends are too irregular for a clean slab (outline not star-shaped around its centre) the
  hologram says so instead of building something broken.

All geometry is generated with `UProceduralMeshComponent`, with triangle collision so vehicles and
pioneers can drive and walk on it.

---

## Install the source

1. Set up the modding environment per docs.ficsit.app (custom UE, Visual Studio, Starter Project).
2. Copy this `RoadMaker` folder into `<StarterProject>/Mods/RoadMaker`.
3. Right-click `FactoryGame.uproject` → *Generate Visual Studio project files*.
4. Build **Development Editor / Win64**, then open the editor.

Check that the game still ships ProceduralMeshComponent: in `FactoryGame.log` look for
`Mounting plugin ProceduralMeshComponent`. If it is missing, the mod will not load; tell me and I'll
port the mesh code to runtime `UStaticMesh` building instead.

## First build: likely compile fixes

I could not compile against the 1.2 headers. If something fails, these are the spots to check
against `Source/FactoryGame/Public/...`:

- `AFGHologram::mBuildClass` (used to read the buildable defaults from the hologram)
- `UFGConstructDisqualifier::mDisqfualifyingText` (the game's own misspelling)
- `AFGBuildable::GetDismantleRefund_Implementation(...)` and `PostLoadGame_Implementation(...)` signatures
- `AFGHologram::IsCurrentBuildMode`, `GetSupportedBuildModes_Implementation`, `DoMultiStepPlacement`,
  `TrySnapToActor`, `SetHologramLocationAndRotation`, `CheckValidPlacement`, `GetBaseCostMultiplier`

Paste the errors to me and I'll fix them.

If every road face looks inside-out in game, flip `GRMFlipWinding` at the top of `RMGeometry.cpp`.

## Editor setup (Blueprint assets)

Create these under `Content/RoadMaker/...`:

1. **Build mode** — `BM_Straight`, parent `FGHologramBuildModeDescriptor`, display name "Straight".
2. **Holograms**
   - `Holo_Road` (parent `RMRoadHologram`): set `mBuildModeStraight = BM_Straight`.
   - `Holo_Fence` (parent `RMFenceHologram`): set `mBuildModeStraight = BM_Straight`.
   - `Holo_Intersection` (parent `RMIntersectionHologram`).
   - Leave `mPreviewMaterial` empty first; if the preview has no hologram look, set it to the game's
     hologram material.
3. **Buildables** — the 7 Blueprints from the table at the top. On each:
   - `mHologramClass` = the matching hologram.
   - Display name, description.
   - Materials: on roads and the intersection set `mTopMaterial` to the game's asphalt material and
     `mBodyMaterial` to concrete (search "Asphalt" / "Concrete" in the Content Browser; in the Starter
     Project these are placeholders that resolve to the real game assets at runtime). The game's
     customizer foundation materials may expect data that procedural meshes don't provide; if they
     render oddly, make a simple material using the game's asphalt and concrete textures.
   - Fences: `mMainMaterial` (steel or concrete), grid fence also `mPanelMaterial` (a masked grid/mesh material).
4. **Sub-category** — `SC_Roads` (parent `FGBuildSubCategory`), so the 7 tools sit together.
5. **Descriptors** — one `FGBuildingDescriptor` per buildable: `mBuildableClass`, icons,
   `mBuildMenuCategory` = the game's Transport category, `mSubCategories` = `SC_Roads`.
6. **Recipes** — one `FGRecipe` per descriptor, product = descriptor, produced in = the build gun,
   ingredients = cost of **one unit** (one 8 m × 1-foundation road tile, one 4 m fence piece, one
   intersection arm). The tool multiplies it.
7. **Schematic** — `Schematic_Roads` unlocking the 7 recipes; register it in your
   `RootGameWorld_RoadMaker` (Game World Module) `mSchematics`.
8. Package with **Alpakit** and test.

## Test checklist

- [ ] Road places on terrain and on foundations; red reason when turn too tight / too steep / too long.
- [ ] Straight mode works; snapping to a road end continues tangentially.
- [ ] Fence on a road follows the edge you look at, on curves too.
- [ ] Intersection joins 2–6 nearby ends; ends already joined are ignored.
- [ ] Save, quit, reload: everything rebuilds identically.
- [ ] Dismantle refunds the scaled cost.
- [ ] Multiplayer: client-placed pieces appear correctly on the host and other clients.

## Not done yet

- **Vanilla buildings snapping to the road anchors.** The anchors exist, but vanilla holograms
  (foundations, walls…) only know their own snap rules. This needs SML hooks on the vanilla
  holograms' snapping; worth doing once the base mod runs.
- Fences along intersection curbs.
- Vehicle autopilot on roads (you can still lay a vanilla Vehicle Path on top of a road).
