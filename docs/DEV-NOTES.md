# Developer notes (Road Maker, and a starting point for new mods)

Notes for the next session working on this mod, or on a new Satisfactory mod with the same setup.
They cover how we work and what we learned building Road Maker 1.0.0 to 1.1.0. They are not a
user manual; the mod's features are described in `Tools/Release/ficsit-page.md`.

## Setup on the developer's PC (Windows)

| What | Where |
|---|---|
| SML starter project | `D:\SFMods\SML\FactoryGame.uproject` |
| This mod (git clone) | `D:\SFMods\SML\Mods\RoadMaker` |
| Unreal Engine (CSS build) | `D:\UE-CSS` (or `D:\UE-CSS\Unreal Engine - CSS`) |
| Game install | `D:\games\Satisfactory` |
| One-click update script | `D:\SFMods\update-and-package.bat` (copy in `Tools/update-and-package.bat`) |
| Script logs | `D:\SFMods\update-log.txt`, `assets-log.txt`, `materials-log.txt`, `*-editor-log.txt` |
| Game log | `C:\Users\magikgpdwin4\AppData\Local\FactoryGame\Saved\Logs\FactoryGame.log` |
| Screenshots | `D:\SFMods\Screenshots` |

GitHub: `jollyeye/Satisfactory-Road-Fence-Tool` (branch `main`). Pushes sometimes fail with a
server error; retrying a few seconds later works. Use the capitalised URL
`https://github.com/jollyeye/Satisfactory-Road-Fence-Tool.git` (the lowercase one redirects).

## How we work

1. Claude edits the code in its own clone, commits and pushes to `main`.
2. The developer closes the game and runs `update-and-package.bat`:
   git pull, build the editor, `create_assets.py`, `make_materials.py`, package into the game.
3. If it prints "FAILED: the game still has the OLD version", Claude reads `D:\SFMods\update-log.txt`
   (through the desktop app's file bridge) and fixes the compile error.
4. The developer tests in game and reports, often with screenshots (vanilla building next to ours
   for comparisons).
5. Claude reads `FactoryGame.log` for the mod's own lines (`LogRoadMaker`) when something is unclear.

Claude cannot compile; keep edits careful. Count braces after big edits, and double-check renamed
variables. A missed rename broke the build once.

## Project layout

- `Source/RoadMaker/...` C++ (all logic; Blueprints are only data holders made by the scripts).
- `Tools/create_assets.py`: creates and updates every Blueprint asset (buildables, descriptors,
  recipes, schematic, customizer entries, game world module) from code. Re-run on every build.
- `Tools/make_materials.py`: creates the mod's materials and assigns materials to the Blueprints.
- `Tools/IconSource/render_icons.py`: draws the stylised isometric build-menu icons (Python + PIL),
  output in `Tools/Icons` (imported by `create_assets.py`).
- `Tools/Release/`: ficsit.app page text, release how-to, logo.
- `docs/screenshots/`: images used by the ficsit.app page (linked via raw.githubusercontent.com).

## Things that work well (reuse them)

### Everything procedural, small saves
Buildables are `UProceduralMeshComponent`s rebuilt from a few saved numbers (spline points, widths,
options) on spawn, load and replication. Distance levels: `mMesh` (detailed, collision),
`mFarMesh`, `mFar2Mesh`, switched by a ticker per camera distance (`UpdateLODsForCamera`).

### Using the game's own materials on our meshes
The editor project only has **placeholder** game textures and materials (tiny stubs), so their
layout cannot be seen in the editor. What worked:
1. **Dump the real textures in game:** a temporary function drew game textures into PNGs
   (`UKismetRenderingLibrary::DrawMaterialToRenderTarget` with an unlit material that shows a texture
   parameter, then `ExportRenderTarget`) into `D:\SFMods\TextureDump`. The code was removed before
   release; it is in git history before commit `7322d94` (`URMGameWorldModule::DumpGameTexturesForDevelopment`
   and the `M_RM_TextureDump` materials in `make_materials.py`). The dump is sRGB-encoded (flat normals
   read ~0.73, not 0.5).
2. **Read the layout** from the PNGs (Python/numpy: line and bolt positions) and from in-game
   screenshots of the vanilla building, then give our mesh UV0 in the same layout.
3. **Put the game's material instance on our mesh** (e.g. `MI_WallSetConcrete_8x4`). It then looks
   and paints exactly like the game's building.
4. **Paint:** `FResolvedFactoryCustomizationData(Data, mColorSlot).ApplyToMeshComponent(Mesh)` passes
   colours, finish and pattern to the game's materials the way the game does. Road Maker's own materials
   read colours from custom primitive data 24 (primary) and 28 (secondary).

Layouts found (see `RMGeometry.cpp` / `RMBuildableFence.cpp`):
- Concrete wall `TX_WallSetConcrete_01`: half a panel, centre at u = 0, edge at u ~0.856, mirrored;
  ~0.214 u per metre; v 0.021 (top) to 0.888 (bottom) for 4 m.
- Steel wall `TX_WallSetSteel_01`: ribbed panel u 0..~0.875 (dark frame strip beyond), v 0.2..0.9;
  bolt columns at u 0.19 and 0.65. Seamless repeat: u 0.08..0.54 every 2 m.
- FICSIT 1 m wall `TX_WallSetOrange_8x1`: 4 m x 1 m tile, repeats sideways; grooves at u 0.11 / 0.87;
  top and bottom edges break the grooves, so use the middle band (v 0.3..0.7) for taller walls.
- Foundation concrete and asphalt atlases: one square per 4 m, mirrored; normal map signs: red tilts
  towards -u, green towards +v (checked in game).

### Customizer
- Our own customizer tab (Road Lines) is a category derived from the game's `CC_Material`, with our
  own descriptor classes; firing with one selected is intercepted (hook on
  `AFGBuildGun::OnPrimaryFirePressed`) and applied ourselves, with a local preview while hovering.
- The game's **material swap** replaces the whole building (new class from a recipe), which loses a
  spline's shape. For our walls we intercept the fire and **switch the look in place**
  (`ARMBuildableFence::SetWallLook`), charging the cost difference; preview on hover.
- Game wall materials (`MaterialDesc_Wall_*`) list buildables in `mBuildableMap`; they only apply to
  classes in `mValidBuildables`.

### Unlocks without editing game assets
- Add our recipe to a game schematic's `UFGUnlockRecipe::mRecipes` (reflection) at world init, and if
  the save already bought it, `AFGRecipeManager::AddAvailableRecipes` (see `RMGameWorldModule`).
- "Unlock together with a game recipe": scan all schematics for the game recipe and add ours next to it
  (used for the concrete / steel fence walls and the AWESOME Shop wall materials).
- Find milestones by display name in `create_assets.py` (`Schematic_1-1` = Base Building,
  `Schematic_3-x` containing "steel" = Basic Steel Production).

### Hooks and access to protected game members
- SML `SUBSCRIBE_METHOD`, `SUBSCRIBE_METHOD_VIRTUAL` (needs a CDO), `..._AFTER`, `Scope.Cancel()`,
  `Scope.Override()`.
- Protected members: a struct deriving from the game class with static accessors (e.g. `FRMSnapAccess`
  reads `mSnapToGuideLines`, which is true while the snap key is held).
- Private / UPROPERTY members: `FindFProperty` reflection.
- Multiplayer: client requests go through `URMRemoteCallObject` (a `UFGRemoteCallObject`, registered
  per player controller); server-only changes, replicated properties for the rest.

## Pitfalls we hit

- **Unity builds** merge `.cpp` files: two anonymous-namespace helpers with the same name in different
  files break the build ("already has a body"). Give helpers file-specific names.
- **ficsit.app:** `Version` in the `.uplugin` must equal the SemVer major; engine plugins we depend on
  need `"BasePlugin": true` or users get "mod not found".
- Placeholder game assets: never judge a game texture or material from the editor.
- The game's own build-gun flows (material swap, upgrade) assume normal buildables; spline buildables
  need their shape handed over (`PreUpgrade` / `Upgrade`) or their own in-place handling.
- Procedural meshes need a material on every section, or the build effect crashes.

## Ideas for Road Maker 1.2.0 (agreed in brainstorming, not started)

The full 1.2.0 list is in `docs/ROADMAP-1.2.0.md` (item 6 is the plastic road below).

**New road surface: matte "toy" plastic with circuit-board grooves** (concepts in `docs/concepts/`).
- A **Roads** section in the game's Material tab with two options: Asphalt and Plastic. Switching works
  in place like the fence walls (preview on hover, cost difference). Intersections get it too.
- Colours (option 2): the surface takes the **primary** colour, **all lane lines the secondary** colour.
  Plastic lane lines **glow** (softer in daylight). Decide what happens to existing roads' line colours.
- Grooves fade with distance to avoid shimmer; possibly keep traces clear of the lane lines.
- Build menu: one entry per width that builds asphalt; switch afterwards.
- Still open: does asphalt take a primary-colour tint? Should plastic cost something?
