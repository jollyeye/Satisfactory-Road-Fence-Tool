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

- **Road side slopes (embankments)** (built 2026-11-26, **complete and tested 2026-11-26**): a bank from a raised road's edge down to the
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
  - Look (asked 2026-11-26): road, walking path, intersection and gap fill sides, underside, caps and banks in the
    cement blocks (`M_RoadJersey`: the cement barrier's concrete and joints) instead of plain concrete: 3 m blocks
    fitted to whole blocks per piece (as the grid fence's posts; round the outline on intersections and fills),
    no joints on caps, road mouths and undersides. The cyberpunk look's sides and banks in its circuit material
    (was plain). Layout in code: `RMGeometry::FRMSideUV` (`MakeSideUV`, `AddSideUV`, `WrapSideUV`; the circuit
    layout when the shown look is cyberpunk). Materials: `ROAD_SLAB_LOOKS`. Needs the materials step.
  - Tested 2026-11-26: where the ground was more than 8 m below, the bank just stopped (half banks, odd caps), and the
    road's side showed as well as the bank. Now: no ground within 8 m anywhere = the road gets no banks at all (samples
    marked `SlopeTooDeepMark`; the tool refuses with red lines there, `URMCD_SlopeTooDeep`, `SlopeTooDeepHint`; a
    reshaped or copied road with banks loses them). The road's side face is left out wherever a bank covers it all
    along (`SlopeCovered`, `BuildRoad(..., CoveredLeft, CoveredRight)`): the bank is the side there.
  - Tested and working 2026-11-26.
  - **Intersections** (asked 2026-11-26, built, to test): the same tool switches an intersection's banks
    (`ARMBuildableIntersection::SetSideSlopes`, `mSideSlopes`, `mSlopeSamples`, `mSlopeKey`, mesh section 3). Samples
    about every 2 m along each curb run between two road mouths (`SlopePoints`: outline from `BuildIntersection`,
    outward from its winding, mitred at curb corners; distance round the outline as the slab's side blocks), measured
    with the roads' `MeasureBankAt` (shared: the "above" and "down the bank" traces); near a mouth's corners the traces
    pass through the roads joined there. Curb edges covered all along leave out the slab's side (`CoveredEdges`). Each
    run's ends close with a triangle in the mouth's plane down below the slab. Roads now pass through an intersection
    joined to their end too, so their banks run on to the mouth. Too deep anywhere: no banks (as roads).
  - Tested 2026-11-26: gaps / overlaps where a road's bank met the intersection's, and the run ends were triangles.
    Now a run's end point goes out along the mouth's line (the joined road's sideways direction at its end), so the two
    banks meet flush; and each mouth next to a bank gets a trapezoid cap like a road's (slab end face widened to the
    banks' feet; the slab's own mouth face left out there).
  - Tested 2026-11-26: flush now (the road's feet went along a sloped road; now straight down everywhere; `SlopeVersion`
    in the keys re-measures saved banks). Next: at inside corners (curb fillets) the feet crossed into a bow tie; now
    feet that would cross are pulled onto the crossing (`RMGeometry::UntangleFeet`, also on roads' tight inner curves):
    the banks meet along a valley line.
  - Tested 2026-11-26: a road's banks stopped short beside another road's banks (the trace hit the other's bank and
    took it for a road beside it). Now a hit on another piece's bank (its mesh section, from the collision face index)
    counts as ground: banks run into each other. Feet that fold back against their edge (not crossing) are held too
    (`UntangleFeet` with the tops). `SlopeVersion` 3: saved banks measured again.
  - Tested and working 2026-11-26. Next: banks stopped on a Beryl Nut bush and an ore deposit. Now the ground is only
    the landscape and the map's static scenery (cliffs, rock formations); passed through: the game's own objects (nearest
    native class in FactoryGame: resource nodes, wells, geysers, pickups, creatures, buildings) and foliage (plants,
    trees). Trees and pickups standing where a bank goes poke through it. `SlopeVersion` 4.
  - Tested: a big rock mesh the road sat on was passed through (its nearest native class is in FactoryGame). Now a
    list of what is not ground instead (by class name and parents: FGResourceNodeBase / FGResourceNode /
    FGResourceDeposit, FGItemPickup, FGBuildable, Pawn, InstancedFoliageActor, FoliageInstancedStaticMeshComponent);
    everything else is ground. Pass-throughs logged (Verbose). `SlopeVersion` 5.
  - Rocks are `FGCliffActor` (a FactoryGame class, why the first rule failed). Resource nodes' rocks are
    `NodeMeshActor_C` with a `BoxComponent` hit box bigger than the rock: hit boxes (`ShapeComponent`) are passed
    through now, the rock's own mesh behind still counts. What the banks stand on is logged (Verbose, once each).
    `SlopeVersion` 6.
  - Tested and working 2026-11-26. Decided 2026-11-26: a road whose bank line runs past a rock's edge into more than
    8 m of drop (an upside-down-pyramid rock) stays refused (no half banks); the 16 m wall would not help there.
  - Neighbours (asked 2026-11-26): a road or intersection built, reshaped or copied next to a banked piece makes it
    measure again half a second later (`NudgeSlopeNeighbours`, from `RebuildMesh` when the piece's shape key differs
    from the saved `mSlopeNeighbourKey`: not on loading): its bank on that side goes. Dismantling the new piece leaves
    the side as it is (decided).
  - Asked 2026-11-26: only a road side by side stops a bank (about parallel, within 20 degrees, its slab within 1.5 m
    of the edge: `IsParallelNeighbour`; tightened to 5 degrees, 60 cm and level within 50 cm, and only by the check straight down beside the edge, the trace
    down the bank passes every road); intersections and roads at an angle are ignored (the bank goes on through
    them to the ground; asked 2026-11-26 after trying them as ground). Other pieces' banks are still ground.
  - **Sides chosen by the player** (asked 2026-11-26, replaces the neighbour rules above): the detection of roads beside
    the edge never worked well (roads at a shallow angle, curving or lower counted as neighbours). Now the tool
    switches the bank on the aimed side of a road (`SetSlopeSide`, `mSlopeSides` bit 0 left / 1 right, saved and
    replicated, copied to cut pieces; old banks without sides = both), and every other road and intersection is ignored
    by the traces (other pieces' banks still count as ground). Too deep is checked per side. Intersections still
    switch all their curbs at once. Hints `SlopeAddSideHint`, `SlopeRemoveSideHint`; description updated.
  - Asked 2026-11-26: snap key held = both sides of a road / all curbs of an intersection at once; intersections
    curb by curb otherwise (`mSlopeRuns`, a bit per run between two mouths, `RunAt`, `SetSlopeRuns`; old banks = all).
    Too deep now drops only the side / curb concerned (`DropTooDeepSides`, `DropTooDeepRuns`), also when re-measured.
    Hints `SlopeAddCurbHint`, `SlopeRemoveCurbHint`; the "all" case uses `SlopeAddHint` / `SlopeRemoveHint`.
  - Floors (asked 2026-11-26: the hollow under the embankment could be seen and walked into): roads carry the cap's lower
    edge along (foot to foot, or the slab's bottom corner on a side without a bank) as a floor facing down; intersections
    get one polygon through every foot (slab bottom under curbs without a bank), triangulated in plan.
  - Red vehicle paths after switching a curb's bank (RM.PathCheck: stored "NoFloor", checked now "Valid"): rebuilding the
    mesh takes the collision away until it is cooked again, and paths checked in between stayed red. Now 1.5 s after a
    slope change the server asks the game to check the paths over the piece again
    (`AFGVehicleSubsystem::InvalidateVehiclePathsInVolume`, `RevalidateVehiclePathsSoon`).
  - The tool's cage shows the banks only (no caps, no floor: `bBanksOnly`).
  - Later (maybe): past 8 m a straight-down wall at the foot, to 16 m in all.
  - Reported 2026-11-28: after cutting a banked road, the bank misses its last triangle at the cut (merged again it is
    fine). Not found by reading the code (the joined piece is passed through near the end); each measure now logs what
    the end samples stopped on (Log: "side slope at the ... end") to find it.
    Log 2026-11-28: the joined piece at an end was not always found (the same joint read "1 joined" and later "0"), and
    the gap moved to a road's start. Then the neighbour's bank, which starts right at this edge in the same plane, is
    hit: just below the edge by the trace beside it (= ground above the edge: no bank there) or at once down the bank.
    Now another piece's bank is ignored there (below edge + 5 cm for the trace beside it, the first 50 cm down the
    bank). `SlopeVersion` 13: saved banks measured again.

- **Issue #10: rubber banding walking on long roads in multiplayer** (fixed 2026-11-26, to test with two players):
  the reporter answered: only roads over the length limit (limit toggle off), the whole road, only joining players
  (not the host), driving is fine. Cause: a road past the limit gets extra mesh components (chunks) at run time with
  made-up names (`NewObject` with `NAME_None`), so a walking client's movement base was a component the server could
  not find by name; the server kept correcting the player (vehicles do not send a movement base this way). Fix in
  `ARMBuildableProcedural::PrepareChunks`: chunks named `RMChunk_<n>` (same on every machine) and `SetNetAddressable`;
  a dropped chunk is renamed out of the way first so the name can be used again.

- **Performance check** (asked 2026-11-26: frames lower lately; a 0.5 s dip opening the dismantle tool on a busy
  intersection): the Side Slopes tool rebuilt the intersection's whole slab several times a frame (outline lookups) and
  its cage every frame; now the outline is cached per shape and the cage rebuilt only when something changes. A slow-step
  log was added (`RMPerf::FScope`, `RM.PerfLogMs`, default 8 ms): mesh rebuilds, side slope measuring, the dismantle
  tool's rider search (tracks, paths, markings on a piece), the Side Slopes tool. Log 2026-11-26: the dismantle dip
  did not come back (no slow step); switching a curb's bank: the intersection rebuild took 18-28 ms (13 ms without
  banks), then the game re-checks the paths over it (the same hitch as placing many paths). Floor polygon now thinned
  in plan (15 cm, one pass) and the slopes part timed on its own. Frame drop looking at a busy intersection: the game
  drawing its many vehicle paths, not the mod.

- **Boom barrier** (asked 2026-11-27, redesigned 2026-11-28, to test): `ARMBuildableBarrier` + `ARMBarrierHologram`,
  `Build_BoomBarrier` (build menu next to the Sign Gantry, menu order 11; cost 2 steel pipes, 2 copper sheets; icon
  `render_icons.py barrier`).
  - Placement like the gantry's first two steps: click the side for the post (fence line, white arrow), aim at the other
    side straight across (same road's other edge or a road beside it): the arm reaches it (2 to 20 m); click builds.
  - Foundations (asked 2026-11-28): the post snaps to a foundation's 0.5 m grid on its top (from its centre, like the
    fences' grid), the arm along the foundation axis nearest the view; the scroll wheel turns it by quarter turns.
    Second click: a foundation grid point (or a road edge) measured along the arm from the post (2 to 20 m, within 3 m
    up or down). A road post can also end on a foundation (straight across the road). Hint `BarrierFoundationPost`.
  - Look (design `Resources/Qwen_Upscale_2.1_00003.png`): 64 x 64 cm steel base plate with four bolts, a 44 x 44 cm
    painted body (rounded edges) up to 1.02 m, a light band all around (6 cm), a steel cap to 1.16 m, a steel hinge
    block on the road side (arm centre at 0.88 m) and a vent grille low on both faces along the road (2026-11-28: in the
    body material's normal map, a recessed panel with five dark slots, instead of steel slats; those faces have u + 2). Mesh sections:
    0 steel (`MI_RM_GuardRailPost`, the grid fence's posts), 1 body (`M_RM_BarrierBody`: the cyberpunk road's plastic
    without the circuit, primary colour x 0.8; paintable, the FICSIT wall's swatches, FICSIT by default; no secondary
    colour for now), 2 light (`M_RM_BarrierLight`: glows in custom primitive data 32, set per mode: green open, orange
    automatic, red closed; the red pure and dimmer (0.45, 0, 0) since bright red read as orange, 2026-11-28).
  - Arm: a hologram (`M_RM_BarrierHolo`: unlit, translucent, two-sided; red and white stripes every 0.5 m with soft
    edges, brighter and more solid at the long edges, a slow scan line), 12 x 6 cm, from the hinge block's face to the
    other side. No collision at all (players, creatures, vehicles, the build gun) and no shadow. It draws back into the
    hinge block (squeezed along its length, eased, about 1 s) instead of turning.
  - Modes (saved, replicated), set in the window: Automatic (arm draws back while a wheeled vehicle is within 12 m along
    the road and the arm's reach across), Always open, Always closed.
  - Vehicles: no more path block (the invisible box and the path re-check are gone). Server, 4 times a second, from the
    game's vehicle list (`AFGVehicleSubsystem::GetAllVehicles`, real place via `GetVehicleLocationAndRotation`, so far
    proxies count, 10 times a second): always closed, a vehicle with autopilot on, no driver, within the arm's reach
    across (plus 3 m) and heading towards the arm (backwards when reversing) gets `SetAutopilotEnabled(false)` once its
    front (half its local bounds' length ahead of its centre) is within 1.5 m of the arm's line plus what it covers
    until the next check (tested 2026-11-28: at 25 m they stopped far too early: they stop at once); nothing is remembered
    (2026-11-28: no list kept, asked: a vehicle can be dismantled meanwhile). Leaving always closed (or dismantled):
    autopilot back on for every vehicle in the zone (arm's reach across plus 3 m, up to 30 m either side of the arm)
    with it off, no driver and `CanEnableAutopilot`. Player-driven vehicles are never touched.
  - Cost of many barriers (asked 2026-11-28, a thousand): each checks the game's vehicle list on the server, first by
    distance to the post (arm + 35 m), 10 times a second while a vehicle is that close, twice a second otherwise; the
    arm's timer runs only while it moves; post and arm not drawn past 250 m. The cap's underside is closed.
  - Window: E opens `URMBarrierWindow` (C++ `UFGInteractWidget`, set as the building's interact widget through
    reflection). The game's frame (`Widget_Window_DarkMode`, title tab "Boom Barrier", its X, no inventory) around our
    page: three mode buttons (the chosen one orange), arm and vehicle state, an "Automation" button when Automated
    Machines is installed (its `OpenAutomationWindow` called by name, MODDING.md). Clients set the mode through
    `ServerSetBarrierMode` (RCO).
  - Automated Machines (name convention, no dependency): `AM_Condition_Vehicle_Waiting`, `AM_Condition_Open`, one
    action `AM_Action_Mode(ERMBarrierMode)` (a dropdown Automatic / Open / Closed from the entry names; saved by value:
    new entries only at the end), `AM_Info_*`.
  - History: the first version (2026-11-27) had a solid arm turning up 85 degrees and, always closed, an invisible 4 m box
    that blocked the vehicle paths (vehicles had climbed over the thin arm).

## Waiting
- Issue #10: reply to the reporter once tested (with the user's go-ahead).
