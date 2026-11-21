# Road Maker 1.5.0 roadmap

Started 2026-11-20, after 1.4.1 (rebuilt for Satisfactory 1.2.6 / build 520140 on the community headers of SML PR #433).
Nothing here is decided in detail yet: each item gets its design written down before it is built.

## Planned
- **1 m tall fence walls** (added 2026-11-20): low versions of the fence walls (concrete, steel, steel mesh, FICSIT) for
  kerbs, parking edges and planters, plus a 1 m sign wall with the game's 1 m sign (already noted as "later" when the
  sign wall was built). Today every wall is 4 m tall (`ARMBuildableFence::GetWallHeight()` is a fixed 400 cm): the height
  becomes a property of the wall type. To decide: separate buildings in the menu or a height option on each wall;
  cost per 4 m scaled by height (1/4); how the panel textures and the steel wall's cells fit 1 m; stacking (four 1 m
  walls = one 4 m wall?); wall attachments and the snap rules on a low wall.
- **Tunnel** (added 2026-11-20): a road (and walking path) enclosed in a tunnel shell following the road's curve: side
  walls and a ceiling, lights along it, portals at the ends. To decide first, because it shapes everything else:
  - Terrain: Satisfactory cannot dig the landscape, and its collision stays inside the hill. Options: a tunnel only as
    an above-ground / covered road (cut-and-cover look, gallery, snow shed), or one placed into a hill where the road
    inside rides above the landscape collision (the road surface is ours, so vehicles and players drive on it, but the
    terrain may still poke through the floor or walls on curves and bumps).
  - Shape: one piece that wraps a road of a given width (1 to 4 lanes, with or without walking paths), or wall + roof
    pieces snapping to road edges like the fence walls.
  - Lighting inside (the game's light sources along the ceiling, or emissive strips in the material), the map (show
    it as a covered road), Road Mode (a tunnel in a blueprint stretched along the curve like the fence walls).
- **Boom barrier** (moved here from 1.4.0 on 2026-11-16; concept image `docs/concepts/boom-barrier-concept.png`): the
  striped arm that blocks vehicles at parking / factory entrances. A post beside the lane with a hinged arm across it,
  snapping to a road edge like the street lights; first version for show (arm up / down by interacting), later maybe
  opening for vehicles on a path through it (same detection idea as the traffic lights).
- **Translations, starting with Chinese (zh-Hans)** (GitHub issue #9, 2026-11-20): muaxl translated the 42 C++ strings.
  Add `LocalizationTargets` to `RoadMaker.uplugin` and gather / compile text in `update-and-package.bat`; turn the
  three `FText::FromString` texts ("Road Lines: ", "Intersection: ", "Road") into localizable text; import the PO; then
  make the building names and descriptions from `create_assets.py` localizable, gather them and post the new strings
  on the issue for translation.
  - Started 2026-11-20: `LocalizationTargets` in the uplugin, `Config/Localization/RoadMaker.ini` (gather source +
    assets, import the PO files, compile .locres; no PO export, it rewrote the tracked files), a GatherText step in
    `update-and-package.bat`, the three FromString texts now NSLOCTEXT (RoadLinesHud, IntersectionLinesHud,
    RoadModeBlueprintName). PO files live in `Content/Localization/RoadMaker/<culture>/RoadMaker.po` (tracked in git).
    Asset texts (building names / descriptions) are written by create_assets.py as plain strings, which the gather
    skips: they need keyed texts (an editor helper making FText with namespace / key) before they can be translated.
  - Phase 2 (2026-11-21): every asset text in create_assets.py goes into one string table,
    `/RoadMaker/Text/ST_RoadMaker` (namespace `RoadMakerAssets`), under a fixed key such as `Road_1x.Name`, through
    `URMEditorTools::TableText` (editor only). A building, its descriptor and its recipe share one key, so each text
    is translated once. The marking hint (arrow and vehicle names) and "Blueprint Road ({0} wide)" are NSLOCTEXT now.
    The zh-Hans PO has the 121 new entries with empty translations (108 asset texts, 13 code texts) for muaxl.
    The lane paint menu (LanePaint/) still has plain texts, but nothing opens it any more.
    Built and checked 2026-11-21: the gather finds 103 distinct asset texts (namespace RoadMakerAssets) and the code
    texts. From now on every new text is localizable (rules in `CLAUDE.md`). Reply to muaxl on issue #9 at the 1.5.0
    release (decided 2026-11-21), with all untranslated entries including the new 1.5.0 buildings.
- **Rebuild on SML's official 1.2.6 headers** when they are published (may come first as 1.4.2): `git checkout master`
  and `git pull` in `D:\SFMods\SML` (now on branch `pr-433`), rebuild, test, release.
  - 2026-11-21: SML v3.12.1 is on master ("Update headers to CL520140"), but it does not build: `FGPlayerController.h`
    uses `FClientInitialLoadStatusData` (RPC `Client_InitialNetLoadStatusUpdate`) and master deleted
    `FGClientInitialLoadStatusData.h`, which defined it (only a forward declaration is left), so UHT fails before
    our code. Its headers also differ from PR #433 in 32 files, including `FGUseableInterface.h` (the 1.4.0 crash),
    `FGPlayerController.h`, `FGBuildableFactory.h` and `FGBuildablePipelinePump.h`. Staying on `pr-433` until master
    builds. Decided: keep everything for 1.5.0, no 1.4.2 unless there is an emergency.
  - 2026-11-21, later: SML master builds with the new CSS engine **Unreal 5.6.1** (release 5.6.1-css-87, installed in
    `D:\UE-CSS`; the game 1.2.6 / CL520140 runs on 5.6.1). The user switched `D:\SFMods\SML` to master (also needed
    for another mod). Road Maker compiled with no errors on 5.6.1 and the game loads it with a save. Found:
    `M_RoadGridFence` failed to compile for DirectX 11 (SM5; fxc "Negate modifier not allowed", from negated / abs()
    values in `? :` choices): rewritten with lerp, to check in the next build. A harmless Cast<> warning for
    `RM_HOOK_SET_LOCATION(AFGPowerPoleHologram)` (runtime IsA checks). `en-US/RoadMaker.locres` is not found by the
    game (English comes from the source texts, so nothing missing).

- **Road Mode: what tilts with the road** (issue #7, list from the user 2026-11-20; built 2026-11-20, to test): besides our
  road pieces, markings and the game's splines, these now follow the road's slope instead of staying level: everything
  in the Architecture tab (every class under AFGBuildableFactoryBuilding: foundations, ramps, roofs, frames, all walls
  with doors and windows, corner walls, quarter pipes, beams, pillars, barriers, stairs) except catwalks, walkways,
  ladders, modern and industrial railings; pipeline junctions, pipeline supports, pumps (Mk.1 / Mk.2) and valves,
  conveyor throughput monitors, wall flood lights, buffer stops, hypertube entrances, hypertube supports and wall
  supports, jump pads. The Foundations tab stays level (foundations, ramps, frames...: decided 2026-11-20, read from each piece's build menu category, learned from the available recipes). Machines, storage, poles, lights and everything else stay level (`TiltsByRule` in
  RMRoadTemplate.cpp). The wall flood light also snaps to our fence walls and the gantry like the other wall
  accessories (first build step; its back on the wall's face; the lamp angle step as the game's).

- **Road Mode cost and build fixes** (2026-11-20, tested): the game takes the blueprint's own cost once at the click
  by itself (it ignores the hologram's cost multiplier for that, even when nothing gets built): measured from the
  inventory and given back before Road Mode charges what was built. Live cost while drawing: the plan's cost written
  into the blueprint descriptor (restored on the click / leaving Road Mode). Preview and affordability by whole
  tiles. The game's pieces play their own build effect at once (they waited ~5 s for a blueprint group effect).
  The mesh queue no longer takes whole frames. "Road Mode: paid" and the inventory check lines are in the normal log.

- **Road Mode: a tile is as long as its content** (issue #8; asked again by the user 2026-11-21, built, tested 2026-11-21): one
  tile runs from the first thing in the blueprint to the last along the arrow, all content, shorter or longer than the
  designer's box (it was the box, so a 6 m road in an 8 m blueprint left 2 m gaps). Our roads, paths and fences count
  by their course (pillars or curbs past an end would leave gaps), the game's belts / pipes / rails / hypertubes by their
  spline points (their mesh bounds made a 36 m blueprint a 40.2 m tile, a 4 m gap), the rest by its meshes (not the
  hologram's own box outline and arrow); ends rounded to 10 cm; the log names what sets each end;
  nothing measurable: the box as before. Measured once per hologram (`MeasureContent`, cached). The game still centres
  the hologram on the box, so the aim-point shift uses the box (`BoxX0` / `BoxL`). Trade-off accepted: empty space
  left on purpose (a lamp in the middle of a 16 m blueprint) is no longer kept; a key to switch back to the designer
  size can come later if asked. Reply on issue #8 at the 1.5.0 release (with the user's go-ahead).

## Waiting on something
- **Traffic lights** (on hold since 2026-11-15): pole + light heads at intersection corners following the
  intersection's state; waits for the Automated Machines mod so the lights can use its alarms / actions.
- **Integration with Automated Machines** (postponed 2026-11-12): name-based contract through reflection (`AM_GetAlarms`,
  `AM_GetActions`, `AM_DoAction`, `AM_OnAlarm`); intersection "vehicle enters / busy / clear", road "vehicle on road",
  boom barrier and traffic light actions.
- **Road text marking** (on hold since 2026-11-13): text painted on a lane in any language, drawn with the game's font
  into a texture at run time.

## Loose ends from 1.4.0 (optional)
- Road Mode: round off splines on very tight inside curves; multiplayer with a client still untested; issue #7: let
  structural pieces (pillars, beams, foundations, walls) tilt with the slope while machines, poles and lights stay level.
- Gap Fill slabs on the map: decided against (2026-11-20).
- Capturing a built road with everything along it as a template (the part of "road templates" Road Mode does not cover).

## Decided against / parked
- Loading the blueprint once for all Road Mode tiles (2026-11-17: not needed).
- Fence placement preview at the new spacing (2026-11-17: fine as it is).
