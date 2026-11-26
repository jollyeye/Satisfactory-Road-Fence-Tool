# Road Maker 1.6.0 roadmap

Started 2026-11-26, after 1.5.1 (released 2026-11-24).

## Workflow (decided 2026-11-26)
Lay the road out as straight pieces whose ends only touch (corners), move the corners with **Adjust Joint**, then
round each corner with **Smooth Joint**. Two roads count as joined when an end of one is within 30 cm of an end of
the other (`RMRoadUtils::JoinTolerance`), whatever their directions; this is checked from the end positions each
time, nothing is stored.

## Planned
- **Adjust Curvature becomes Adjust Joint** (decided 2026-11-26, built 2026-11-26, to test): English name changed only; its string
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
  - Unchanged: three or more ends at one point, or an end joined to an intersection, cannot be grabbed.
- **Smooth Joint** (new tool, Road Tools next to Adjust Joint; asked 2026-11-26, not built): rounds the corner where
  two roads meet. Click the joint (circles on joints of exactly two road ends), drag away from it: both roads are cut
  back by the same distance along their course and a new road joins the two cut points, meeting each flush (a
  Bezier curve with the old corner as its control point: symmetric, close to a circular arc; on slopes the height
  eases between the two). The distance is shown on screen (1 m steps), at most each road's length minus a little.
  Second click: both roads shortened, the new curve built with their width, lines, material and paint; markings and
  vehicle paths on the cut-away parts handled like the Cut tool. Decided 2026-11-26: different materials: one of the
  two roads' (the clicked first); joints already flush: allowed (widens a bend); intersections: left out.

## Waiting
- Issue #10 (multiplayer rubberbanding walking on roads): waiting for the reporter's answers. Suspect: road sections
  on long roads have run-time component names a client cannot name to the server.
