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

- **Road side slopes (embankments)** (built 2026-11-26, to test): a bank from a raised road's edge down to the
  terrain, so roads on fill look built into the landscape.
  - Tool **Side Slopes** (Road Tools, key `RoadSlopes`, `ARMRoadSlopeHologram`, menu order 7, icon `slopesglyph`):
    aim at a road or walking path; a cyan cage shows the banks it would get (measured on the client while aiming,
    once per road shape), a red cage the banks it would lose; click switches (`SetSideSlopes` on the server). Screen
    hints `SlopeAimHint`, `SlopeAddHint`, `SlopeNoneHint`, `SlopeRemoveHint`. Free.
  - Per road (`mSideSlopes`, saved, replicated), off by default. Part of the road piece: mesh section 2, the body's
    concrete, with collision, near and far versions. Copied by `CopyCutSettingsTo` (cut, split, join, merge, Adjust
    Joint, Smooth Joint, Road Mode pieces).
  - Saved samples per side (`mSlopeLeft` / `mSlopeRight`: distance along, drop) and `mSlopeKey` (hash of the road's
    world shape). When the key no longer matches (a new piece, a reshaped one, a blueprint copy elsewhere) the server
    measures again a tick later (so the pieces it replaces are gone) and rebuilds; loading a save traces nothing.
  - Measuring (`MeasureSlopes`): every 2 m along each edge. Ground at or above the edge (a trace down just outside
    it) or a road / intersection there: no bank. Otherwise a trace down the bank (1:1.5) from the edge: the first road
    or intersection hit stops it (no bank there, banks of side-by-side roads do not cross); other buildings (fences,
    foundations) are passed through; the ground (landscape, rocks) gives the drop, plus 30 cm into it; nothing within
    8 m down: no bank.
  - Mesh (`BuildSlopeMesh`): one strip per side, edge and foot vertices shared; samples kept only where a bank starts
    or ends, where edge or foot bends more than 3 cm, and at least every 20 m; triangles only where there is a bank
    (a wedge back to the edge where it ends). UV0 = metres along / down the bank / 4.
  - Tested 2026-11-26: gaps on curves (the bank had its own 2 m points) and an open end. Now the bank is built on the
    road mesh's own cross-sections (same edge), and at an end with a bank the road's cap widens into a trapezoid down
    to the banks' feet (`SlopeCapAt`, `BuildRoad(..., bStartCap, bEndCap)`).
  - Look (asked 2026-11-26): road and walking path sides, underside, caps and banks in the concrete blocks
    (`M_RoadPavers`, with its grooves and bump) instead of plain concrete: 3 m blocks fitted to whole blocks per road
    (as the grid fence's posts, `RMGeometry::BlockAlongScale`), 2 m rows counted down from the top edge (on to the
    banks), UV1 set in code (`AddBlockUV`, wrapped for long roads). The cyberpunk look's sides and banks in its
    circuit material (was plain). Intersections and gap fills keep the plain concrete (`ROAD_LOOKS`; roads use
    `ROAD_SLAB_LOOKS`). Needs the materials step.
  - Later: past 8 m a straight-down wall at the foot, to 16 m in all.

## Waiting
- Issue #10 (multiplayer rubberbanding walking on roads): waiting for the reporter's answers. Suspect: road sections
  on long roads have run-time component names a client cannot name to the server.
