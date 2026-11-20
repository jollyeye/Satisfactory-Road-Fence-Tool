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
- **Rebuild on SML's official 1.2.6 headers** when they are published (may come first as 1.4.2): `git checkout master`
  and `git pull` in `D:\SFMods\SML` (now on branch `pr-433`), rebuild, test, release.

- **Road Mode: what tilts with the road** (issue #7, list from the user 2026-11-20; built 2026-11-20, to test): besides our
  road pieces, markings and the game's splines, these now follow the road's slope instead of staying level: everything
  in the Architecture tab (every class under AFGBuildableFactoryBuilding: foundations, ramps, roofs, frames, all walls
  with doors and windows, corner walls, quarter pipes, beams, pillars, barriers, stairs) except catwalks, walkways,
  ladders, modern and industrial railings; pipeline junctions, pipeline supports, pumps (Mk.1 / Mk.2) and valves,
  conveyor throughput monitors, wall flood lights, buffer stops, hypertube entrances, hypertube supports and wall
  supports, jump pads. Machines, storage, poles, lights and everything else stay level (`TiltsByRule` in
  RMRoadTemplate.cpp). The wall flood light also snaps to our fence walls and the gantry like the other wall
  accessories (first build step; its back on the wall's face; the lamp angle step as the game's).

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
- Gap Fill slabs on the map.
- Capturing a built road with everything along it as a template (the part of "road templates" Road Mode does not cover).

## Decided against / parked
- Loading the blueprint once for all Road Mode tiles (2026-11-17: not needed).
- Fence placement preview at the new spacing (2026-11-17: fine as it is).
- Road Mode tile length measured from the content instead of the designer size (issue #8, 2026-11-18).
