# Road Maker 1.2.0 roadmap

Agreed list (2026-11-11). Notes are first thoughts on how each could be done; nothing started.
Tick items off and add findings as we go. Changes for the release go in the changelog in
`Tools/Release/ficsit-page.md` as usual.

| # | Item | Size | Notes |
|---|---|---|---|
| 1 | Better road curves | M | **Done (to test):** quadratic Bezier ported from the developer's 2D test (`CurveRoads`, `solveControlPoint`) into `RMGeometry::MakeQuadratic`: one quadratic for C-curves (control point = where the direction lines meet), two joined quadratics for S-curves, wide turns, U-turns. Free end heads along the chord, turned by the wheel. Old saves unaffected (stored as cubic points). |
| 2 | Tool: road / fence slicer | M | Cut a built road or fence at the aimed point into two pieces. Spline split is easy (De Casteljau / Hermite split at t); keep paint, lane lines, wall look; refund nothing. Needs a tool entry (build gun mode or a build-menu "tool" building like the game's). |
| 3 | Tool: road splitter | M | Clarify vs 2: likely "drop an intersection into an existing road" (cut it and connect both halves to a new intersection). Builds on 2. |
| 4 | Tool: move end points | M-L | Grab a road / fence end and drag it; connected intersections and neighbours follow or detach. Rebuild the spline from the new end, keep the other end's direction. |
| 5 | 4-lane road | S-M | New width (e.g. 24 m) with lane-line options for 4 lanes (Road Lines tab gets 4 dashed / 4 continuous). Intersections must accept it. |
| 6 | New road materials: concrete and cyberpunk | L | A **Roads** section in the game's Material tab (Asphalt / Concrete / Cyberpunk), switched in place like the fence walls. Cyberpunk = the matte plastic with circuit grooves and glowing lane lines from the concepts (`docs/concepts/`): surface takes the primary colour, lines the secondary. Concrete: foundation-style concrete atlas. Intersections too. |
| 7 | Road hologram snaps beside a built road | S-M | Aiming at a road's side puts the new road parallel next to it (offset by both half widths), same course: like fence walls snapping to the edge. |
| 8 | Road preview on the map | M-L | Show the hologram's course on the map / compass while placing. Needs the game's map representation system (`AFGActorRepresentationManager`) or drawing on the map widget; research first (logger). |
| 9 | Highway sign gantry | M | A frame over the road (posts each side, beam across, sized to the road width) holding the game's signs, or our own sign panels. Snap to a road at the aimed point. |
| 10 | Road markings: arrows, crosswalk, numbers | M | Decals on roads and intersections (placed like signs, snapped to the lane). Could be customizer options or small buildables; paint colour from the swatch. |
| 11 | Traffic lights (decorative) | S-M | Pole + light heads, lights cycle on a timer (emissive material parameter), no traffic logic. Snap to intersection corners. |

## Suggested order
Small and self-contained first, then the tools, then the big visual work:
5 (4-lane) → 7 (side snap) → 2 → 3 (slicer / splitter, shared code) → 1 (curves) → 4 (move ends)
→ 11 → 9 → 10 → 6 (materials) → 8 (map preview, research-heavy, can slip to 1.3.0).
