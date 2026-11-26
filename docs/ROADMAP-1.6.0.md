# Road Maker 1.6.0 roadmap

Started 2026-11-26, after 1.5.1 (released 2026-11-24).

## Workflow (decided 2026-11-26)
Lay the road out as straight pieces whose ends only touch (corners), move the corners with **Adjust Joint**, then
round each corner with **Smooth Joint**. Two roads count as joined when an end of one is within 30 cm of an end of
the other (`RMRoadUtils::JoinTolerance`), whatever their directions; this is checked from the end positions each
time, nothing is stored.

## Planned
- **Straight build mode really straight** (asked 2026-11-26, built, tested 2026-11-26): it was locked to the direction of the
  road it started from, and an end snapped to another road end made a flush curve. Now a straight line from the start
  to the aim in any direction, ending exactly on a snapped road end's point at an angle (corners meet; Smooth Joint
  rounds them). Roads, walking paths and fences (they share `ARMSplineHologram::UpdateCurve`).
- **Adjust Curvature becomes Adjust Joint** (decided 2026-11-26, built, tested 2026-11-26): English name changed only; its string
  table key stays (translations are marked out of date, not lost).
  - Straight roads stay straight: a road whose whole course lies on a line (within a few cm) swings around its far
    end, a straight line from the far end to the moved joint. Its far end turns with it (a joint there gets a small
    corner, as when moving a polygon corner).
  - Two straight roads: the corner moves, both stay straight, the angle between them changes. The scroll wheel does
    nothing there.
  - Straight + curved: the straight one swings and stays straight; the curved one keeps its far end (place and
    direction) and meets the straight one flush, in its direction (no kink).
  - Two curved roads: as today (both curves meet in the joint direction; the scroll wheel turns it).
  - Built: `IsStraightRoad` (course within 5 cm of the line between its ends, seen from above) in
    `ARMRoadAdjustHologram::ComputeCurves`; a straight road becomes a straight 3D line from its far end to the joint
    (constant slope). Description updated in the string table (key kept) and the PO msgid (translation out of date).
  - Height (asked 2026-11-26, built, to test): the joint no longer drops onto the ground under the aim. It moves
    level at its own height (where the aim crosses that height); where the ground or anything else is in the way
    first, it stops at its last place (tested 2026-11-26: climbing onto the ground was not wanted); H + nudge up / down
    changes the height it then keeps (`mJointZ`). Snapping to a road end / foundation
    unchanged.
  - Unchanged: three or more ends at one point, or an end joined to an intersection, cannot be grabbed.
- **Smooth Joint** (new tool, Road Tools next to Adjust Joint; asked 2026-11-26, built 2026-11-26, to test): rounds the corner where
  two roads meet. Click the joint (circles on joints of exactly two road ends), drag away from it: both roads are cut
  back by the same distance along their course and a new road joins the two cut points, meeting each flush (a
  Bezier curve with the old corner as its control point: symmetric, close to a circular arc; on slopes the height
  eases between the two). The distance is shown on screen (1 m steps), at most each road's length minus a little.
  Second click: both roads shortened, the new curve built with their width, lines, material and paint; markings and
  vehicle paths on the cut-away parts handled like the Cut tool. Decided 2026-11-26: different materials: one of the
  two roads' (the clicked first); joints already flush: allowed (widens a bend); intersections: left out.
  - Built: `ARMRoadSmoothHologram` (on Adjust Joint: its circles, hidden roads, previews), `Holo_RoadSmooth`,
    `Build_RoadSmooth` (menu order 6), icon `T_Icon_RoadSmooth` (`render_icons.py smoothglyph`). Full hologram preview
    (both roads cut back and the curve, as road holograms; asked 2026-11-26 instead of a wireframe). Step 0: circles
    on joints of two road ends only; step 1: setback = the aim's distance from the joint (seen from above), 1 m steps,
    first 8 m, at most each road's length minus its minimum; on-screen text. The curve: `MakeQuadratic` from road A's cut
    towards the joint to road B's cut away from it, each road's slope at its end; checked like a road (length, turn
    radius, grade; red when not). Build: `CutAt` on each road (its tracks and paths split there like the Cut tool),
    the piece at the joint dismantled with its markings / tracks (paths go after it), the curve spawned like road A
    (`SpawnLike`), cost by length like road A. PO entries added.
  - Tested 2026-11-26: works; the preview was hard to read (the light outline of the whole old roads stayed, the
    curve's ends not visible, hologram colour only). Now: no outline of the old roads in step 1 (the previews show what
    is left); an orange bar across each road at the cut with posts at its edges (where the curve starts / ends); the
    three previews in the roads' real surface (look material, paint) with the valid / invalid state as their outline.

- **Dashed lane lines meet evenly** (asked 2026-11-26, built, to test): the dashes (3 m dash, 6 m gap) ran from each
  road's start, so where a road ended mid-cycle the next one's dashes did not follow on. Now, like the grid fence's
  panels, each road fits a whole number of 9 m cycles, stretched to its length (`ARMBuildableRoad::DashScaleFor`:
  cycles = round(length / 9 m), at least 1), and starts and ends half way through a gap: two roads meet with a 6 m gap
  (stretched) whichever way each runs. The scale goes to the materials as custom primitive data 32 (`DashScale`, 1 when
  not pushed); the road hologram, Adjust Joint and Smooth Joint previews set their own. Existing roads change their
  dash positions once. Needs the materials step (`make_materials.py`).

- **Road side slopes (embankments)** (defined 2026-11-26, not built): a bank from a raised road's edge down to the
  terrain, so roads on fill look built into the landscape.
  - Per road, off by default (people may want to go under a road): a new tool in Road Tools turns it on / off for the
    aimed road (click). Roads and walking paths.
  - Part of the road piece (its own mesh section), not a separate building: built, dismantled and paid with it; made
    again when the road is reshaped (Adjust Joint, Smooth Joint, Cut / Split, Road Mode) or switched on.
  - One slope for now: 1:1.5 (about 34 degrees; can change later). One material: concrete (the road body's).
    The terrain's own look would be ideal, but the landscape material cannot be borrowed for our meshes (to check).
  - Height: banks down to 8 m below the road edge for now. Later: past 8 m the bank's foot goes straight down as a wall,
    to at most 16 m in all (deep dips, the desert); deeper than that: no bank (a bridge, pillars).
  - Fill only: where the road edge is at or below the ground, no bank (the game cannot dig).
  - Optimised (it has collision, players and vehicles walk / drive on it): measured once when built and saved with the
    road (no traces when loading). Every 2 m along each edge a trace down the slope finds where it meets the ground;
    the bank stops there (about 30 cm into it, so bumpy ground shows no gap); stretches with no bank leave nothing;
    straight runs over even ground merged into long quads (as roads / tunnels); one surface, no thickness; the ground
    is only the terrain (landscape, rocks), not foundations or buildings. Check with `RM.Wireframe [metres]`.
  - Neighbours: a road, path or intersection beside the edge (touching, or parallel close by): no bank there, or the
    bank stops half way to it, so banks of side-by-side roads do not cross (to settle when building).

## Waiting
- Issue #10 (multiplayer rubberbanding walking on roads): waiting for the reporter's answers. Suspect: road sections
  on long roads have run-time component names a client cannot name to the server.
