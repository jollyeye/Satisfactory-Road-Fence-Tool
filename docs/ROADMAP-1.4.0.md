# Road Maker 1.4.0 roadmap

1.3.0 released 2026-11-14; 1.3.1 (issue #3 fixes) being finished in [ROADMAP-1.3.0.md](ROADMAP-1.3.0.md). Items moved here from its "Later" section on 2026-11-15.
Status words: **DONE** (tested in game), **built, awaiting test**, otherwise to do.

## Done

## In progress

## To do
- **Sidewalks** (planned for 1.4.0, decided 2026-11-14; answer to GitHub issue #1, a 1.5-foundation road for trucks): no 1.5-wide road. Instead a sidewalk piece that snaps along a road's side like a road beside a road (whole length, edge to edge, same curve and slope), widening it: e.g. 2 m on each side of a 1-wide road = 12 m, 1.5 foundations. Not a road: no lanes, lane lines, markings or path / track snapping on it; the road keeps all of that. Fences and fence walls snap to the sidewalk's outer edge like to a road edge (follow it, face the road). To decide: widths (2 m / 4 m?), a curb step or flush, material (concrete like the road top, paintable?), what happens on intersections (follow the curbs?), Cut / Join / Adjust Curvature carrying sidewalks along.
- **Sign wall** (planned for 1.4.0, decided 2026-11-13): a fence that shows a sign design repeated along its whole length, following its curve. Its own fence object, one side only (the road side), no material tab and no swatch / colour options: the design is its only look. First version 4 m tall, using the 4 m sign's size and layouts (read from the game's sign type at run time, so the proportions and the repeat length match); a 1 m tall wall with the 1 m sign later.
  How: the game's widget signs draw their design (FPrefabSignData) into a render target through AFGSignSubsystem and show it on a plane with a material instance (`AFGBuildableWidgetSign::SetupMaterialInstanceForProxyPlane` is virtual). The wall owns a hidden companion sign (subclass of AFGBuildableWidgetSign: no mesh, no collision, not in the build menu) standing inside the wall at the middle of its length, its significance range raised to half the wall's length plus the normal sign range so the design shows anywhere along it. The wall puts the companion's render target on its own mesh with UVs repeating every (height x the sign's aspect). Saved reference wall -> companion. E on the wall opens the game's sign editor for the companion; copy / paste settings and middle click copy the design; multiplayer and saving come from the game's sign. Dismantle takes the companion along (free, nothing to refund); Cut / Join give each new piece its own companion with a copy of the design. Check blueprints carry both.
  Limits: fixed sign sizes (the game's editor has no free proportions); the design is as sharp as the game's sign texture (soft right next to a tall wall); far away, like signs, no design (plain colour).
- **Traffic lights** (item 11; moved out of 1.3.0 on 2026-11-13): pole + light heads at intersection corners, lights following the intersection's state (free path detection), emissive material parameter for the lamps.
- **Boom barrier** (added 2026-11-12; postponed 2026-11-13, too many iterations for this release, concept image `docs/concepts/boom-barrier-concept.png`): the striped arm that blocks vehicles at parking / factory entrances. A post beside the lane with a hinged arm across it, snapping to a road edge like the street lights; first version for show (arm up / down by interacting), later maybe opening for vehicles on a path through it (same detection idea as the traffic lights).
- **Cyberpunk road on Low quality: circuit grooves back** (decision for 1.4.0, raised 2026-11-15). Today the Low branch of M_RoadCyber drops the procedural circuit (CIRCUIT_HLSL + bump_normal) and shows plain plastic with the glowing lines. Options (High is untouched by all of them, the QualitySwitch keeps the procedural version): (1) bake the circuit height into a repeating texture in make_materials.py (draw a bake material to a render target, save it as a texture; cell indices wrapped so it tiles every 48 x 8 m) and read it on Low for normal and groove colour; tracks are ~2 cm wide so it needs ~1 cm/px, ~4096 x 1024, ~4-5 MB, a cut track every 48 m; (2) a cheaper procedural Low: one layer of tracks, no rings or chips, about half the cost, no texture; (3) a small hand-made 512 px groove texture: very cheap but a different design. Suggested: (1).

## On hold
- **Road text marking** (discussed 2026-11-13, on hold): text painted on a lane in any language. Preferred: the text drawn with the game's font (engine fallback fonts for other scripts) into a texture at run time, cutting a quad on the road like the vehicle icons (drapes, line colour, glow, paint gun); one texture per distinct text; a rebindable key opens a text box, E edits a placed text. Alternative looked at: Unreal's Text 3D (may not ship with the game; flat meshes would not follow curves).

## Later (1.5.0 or after)
- **Integration with other mods / Automated Machines** (discussed 2026-11-12, postponed): no hard link between mods; a
  name-based contract through Unreal reflection (works in the shipped game, where metadata is stripped). An actor is
  "automatable" when it has `AM_GetAlarms()`, `AM_GetActions()`, `AM_DoAction(Action, On)` and an `AM_OnAlarm(Alarm)`
  event; Automated Machines finds them by name and lists them like a machine's alarms / actions. Road Maker candidates:
  intersection "Vehicle enters / leaves", "Busy / Clear" (the detection traffic lights need), road "Vehicle on road /
  Road clear", boom barrier actions "Open / Closed" + alarms, traffic light "Turns green / red" + "Hold red". Optional
  extra: a Road Maker subsystem publishing world events (road built / dismantled / cut / split / merged / joined,
  intersection built) for other mods. Open questions: `AM_` naming as the shared standard; which vehicles count.
