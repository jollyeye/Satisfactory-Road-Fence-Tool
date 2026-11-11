# Road Maker — ficsit.app page

## Name
Road Maker

## Mod reference (must be exactly this)
RoadMaker

## Short description (max 128 characters)
Curved roads with lane lines, intersections, fences and fence walls, plus snapping for paths, rails, poles and foundations.

## Full description (paste as Markdown)

**Road Maker** adds real roads to Satisfactory: curved asphalt roads with concrete sides, lane markings you set in the customizer, intersections that join up to six roads, fences and 4 m fence walls that follow the road edge, and one-click vehicle paths and railway tracks along the lanes.

Roads, intersections and road fences unlock in the **Road Maker** milestone in Tier 1; roads and intersections are **free to build**. The fence walls unlock with the game's own progression (see below).

![A curved road through the jungle with a grid fence, cement blocks and street lights](https://raw.githubusercontent.com/jollyeye/Satisfactory-Road-Fence-Tool/main/docs/screenshots/intro.jpg)

![Road Maker buildables in the build menu](https://raw.githubusercontent.com/jollyeye/Satisfactory-Road-Fence-Tool/main/docs/screenshots/build-menu.png)

### Roads (1, 2 and 3 foundations wide)
- Two clicks: start and end. The road curves like a vehicle path; the mouse wheel turns it.
- Build modes: **Default**, **Straight**, and **Smooth** (height eases in and out to join roads at different levels without kinks, up to 45°).
- Snaps to the ends of other roads and intersections, and to foundation edges (top level with the foundation).
- 1 m thick, asphalt and concrete textures matched to the game's foundations.
- Other buildings can be placed on roads, just like on foundations.

### Lane lines — Customizer › Road Lines
![The Road Lines tab in the customizer](https://raw.githubusercontent.com/jollyeye/Satisfactory-Road-Fence-Tool/main/docs/screenshots/road-lines.png)

- **Sidelines:** on / off.
- **Lanes:** 1 lane, 2 lanes dashed or continuous, 3 lanes dashed or continuous.
- Hover a road to preview, click to apply. Middle lines use the **primary colour**, side lines the **secondary colour**; roads have their own default swatch group ("Roads").

### Road Intersection
![A four-way intersection with side lines and fences on the curbs](https://raw.githubusercontent.com/jollyeye/Satisfactory-Road-Fence-Tool/main/docs/screenshots/intersection.jpg)

![Picking two road ends (outlined, with arrows) and the finished intersection](https://raw.githubusercontent.com/jollyeye/Satisfactory-Road-Fence-Tool/main/docs/screenshots/intersection-picking.jpg)

- Click the free road ends (or free intersection ends) you want to join, up to 6 and up to 200 m apart, then click away from them to build: they are joined with curved curbs. The aimed end is outlined in orange, picked ends in white with an arrow.
- Free ends are capped and can be built on again; intersections can join each other.
- Side lines along the curbs (Road Lines › Sidelines).

### Fences
- **Guard Rail**, **Cement Blocks** (with block joints) and **Metal Grid Fence**.
- Aim at a road: the fence follows the whole edge on the side you look at, facing the road. On an intersection it follows the curb.
- Free placement anywhere, snapping to the foundation grid.

### Fence Walls — Walls › Fence Walls
![Steel, Steel Mesh and FICSIT fence walls along a curved, sloped road](https://raw.githubusercontent.com/jollyeye/Satisfactory-Road-Fence-Tool/main/docs/screenshots/fence-walls.jpg)

4 m tall walls that follow a road edge (centred on it) or an intersection's curb, like the fences.
- **FICSIT**, **Concrete** and **Steel Fence Wall** use the game's own wall materials and paint exactly like the game's walls. The customizer's **Material** tab switches a fence wall between concrete, FICSIT and steel in place, with a preview while aiming; switching costs the difference, like the game's walls.
- **Steel Mesh Fence Wall**: a steel frame with a diamond wire mesh (the former 4 m metal grid fence).
- Aim at the upper or lower half of a fence wall to stack another one above or below it. Hold the snap key (Ctrl) to replace the aimed fence wall with a different one, like the game's walls.
- Unlocks: FICSIT with Tier 1 **Base Building**, Steel Mesh with Tier 3 **Basic Steel Production**, Concrete and Steel together with the game's concrete and steel wall materials (AWESOME Shop).

### Lanes: paths and tracks
![One-click vehicle paths: a lane path, an intersection with an invalid connection (red), and all connections of an intersection](https://raw.githubusercontent.com/jollyeye/Satisfactory-Road-Fence-Tool/main/docs/screenshots/intersection-connections.jpg)

- **One-click vehicle paths and railway tracks:** hold the snap key (Ctrl) and aim at a lane: the path or track runs the whole lane, click to build. Scroll flips the direction.
- **Intersection connections:** hold a vehicle path or track and Ctrl and aim at an intersection: every connection through it is shown at once (paths by direction, tracks lane to lane), one click builds them all.

### Snapping for game buildings
- **Power poles** (all tiers) and **street lights** snap to the road's fence lines while holding the snap key (Ctrl); street lights face the road, scroll flips them.
- **Vehicle paths** and **railway tracks** snap to road and intersection ends — one point per lane (every 2 m on 1-lane roads) — and line up with the road, slope included; scroll flips them 180°.
- **Foundations, frames and ramps** snap to free road ends and intersection ends, top level with the road, in 4 m steps; scroll turns them 90°.
- Dismantling a road or intersection also removes the vehicle paths and tracks on it (refunded).

### Performance
- Far-away roads, intersections and fences switch to simpler meshes; fences and fence walls have two distance levels (simpler, then simplest).
- Saves stay small: only a few numbers per piece are saved, meshes are rebuilt on load.

### Notes
- Multiplayer support is included but has not been tested yet — please report issues.
- Source code: https://github.com/jollyeye/Satisfactory-Road-Fence-Tool

## Changelog for version 1.2.0 (not released yet: add to it as features come in)
- **New curves:** roads and fences are drawn as quadratic Bezier curves. Gentle bends are one curve through the point where the start and end directions meet; S-curves, wide turns and U-turns become two smoothly joined curves. Same two clicks as before: the mouse wheel turns the start direction before the first click and the end direction before the second.
- **New: Road (4 wide)**, 32 m, with its own build menu icon.
- **Road Lines: 4 lanes** (dashed or continuous), for any road width.
- The Smooth build mode is now called "Smooth (quadratic Bezier)".
- **Nudging:** with the hologram locked (H), the arrow keys move the point being placed along its own direction and across it (not the world axes); this now works for the end point too.
- **Asphalt follows the road:** the asphalt texture runs along the road's curve instead of being projected from above.
- **New: roads beside roads.** Hold the snap key and aim at a built road: the new road becomes the parallel of that whole road, on the side you aim at, edge to edge and flush with its top (to cover gaps with a flat surface). One click builds it. No length, turn radius or slope rules; only a bend too tight for the new road's width is refused.
- Roads beside intersections: the same with the snap key on an intersection builds a road along the aimed curb, outside the junction.
- **New build menu category: Road Tools** (Builder tab), with **Cut Road** (section Edit road): aim at a road, an orange outline shows the cut, click to split the road into two that keep its curve, lines and paint. Train tracks and vehicle paths built on the road are cut at the same place (refused, with a message, if one of their pieces would be shorter than the game allows).
- **Cut Road: new build mode "Cut also fences"**: this road's fences are cut at the same place too (road fences standing on it, fence walls on its edges, stacked walls included; white outlines show them). Roads beside it and their fences are left alone.
- **New: Cut Fence** (Road Tools > Edit fence): splits a road fence or fence wall where you aim, keeping its course, side, material and paint; nothing else is cut. Hold the snap key near a road end (e.g. where you cut the road) to snap the cut in line with it.
- **New: Split Road** (Road Tools > Edit road): aim across a road 2 or more wide; an orange line along the road shows where it is split lengthwise, in steps of 8 m (a 2 wide road becomes two 1 wide roads, a 3 wide one 1 + 2 or 2 + 1 depending on where you aim, a 4 wide one 1 + 3, 2 + 2 or 3 + 1). One click replaces it with two roads side by side that keep its curve, lines and paint.
- Build menu order: roads 1, 2, 3, 4 wide, then the intersection and road fences.

## Changelog for version 1.1.1
- Road Lines options are no longer listed as items (e.g. under Parts in the creative mode item list).
- Switching a fence wall's material in the customizer now turns it into the real building of that material (its name, recipe and dismantle refund change), keeping its course and paint. Switching again right away works.
- The material switch pays the cost difference from your inventory and the Dimensional Depot, and is free with No Build Cost. When it cannot be applied, the game's failure sound plays.
- Fence walls get the game's outline while aiming with a material: white when the material can be applied, red when the wall already has that material or you cannot pay for it.
- Customizer icons stay correct after visiting the Road Lines tab (no more wrong pictures or colours in other tabs).
- **Road fences replace each other:** hold the snap key and aim at a guard rail, cement blocks or metal grid fence with another road fence selected: it takes its place on click (the old one is refunded). Fence walls still only replace fence walls.
- Fixed the end caps of the cement blocks (a wedge stuck out past the sloped sides).
- Road Lines options bound to the hotbar show their own picture (they showed a blank white / magenta swatch).

## Changelog for version 1.1.0
- **New: Fence Walls** (Walls › Fence Walls; the 4 m metal grid fence is now the Steel Mesh Fence Wall, unlocked with Basic Steel Production; FICSIT with Tier 1 Base Building, concrete and steel with their wall materials): a 4 m wall along the road edge or an intersection curb, stackable, paintable, in concrete, FICSIT or steel (switchable in the customizer).
- **One-click vehicle paths and railway tracks:** hold the snap key (Ctrl) and aim at a lane of a road: the path or track is laid along the whole lane, end to end — click to build. Scroll flips the direction; next to an existing path it continues that path's direction and joins it, and tracks connect to free track ends.
- **One-click intersection connections:** hold a vehicle path or railway track and the snap key (Ctrl) and aim at an intersection: every arriving path is connected to every leaving path on the other ends (no U-turns), and every track end to the track in the same lane on each other end (left to left, middle to middle, right to right, in the travel direction; switches form on their own), shown at once; one click builds them all. If one connection is invalid (red), improve the intersection until all of them fit.
- **New intersection placement:** click each free road end to pick it (up to 6, up to 200 m apart; click again to drop it), then click away from the ends to build. The aimed end gets an orange outline, picked ends a white outline and an arrow, visible through other buildings.
- **Foundations snap to free road ends and intersection ends:** edge on the end, top level with the road, in 4 m steps across it; scroll turns them 90°. Works for foundations, frames, ramps (high or low side at the road) and inverted ramps.
- Dismantling a road or intersection also dismantles the vehicle paths and railway tracks lying on it (refunded too).
- Intersections of two roads at an angle (e.g. 90°) now build: the curved, ring-shaped piece is filled correctly, and big intersections get rounder curbs.
- Fences and fence walls get a second, simplest distance level (beyond 150 m / 200 m).
- Roads without side lines use the full width for their lanes (lane lines and snap points).
- Power poles and street lights snap exactly on the road edge.
- New **Smooth** build mode for roads: the height eases in and out and matches the slope of the roads it connects, so linking roads at different heights has no kinks when seen from the side. Allows up to 45°.
- Vehicle paths and railway tracks follow the road's slope at road-end snap points (start and end).
- Snap points follow the lanes: 1-lane roads every 2 m, 2- and 3-lane roads one point in the middle of each lane. Intersection ends use the lanes of the road joined there.
- Snapping only uses the road or intersection you aim at, and the game's own snapping to existing vehicle paths and tracks takes priority.
- Fixed "vehicles collide with the terrain" when a vehicle path snaps to the raised end of a road (the point moves onto the road when there is no floor beyond the end).
- Railway tracks no longer float above the road at snap points.

## Changelog for version 1.0.0
First release: roads (1/2/3 wide), Road Lines customizer tab, road intersections with side lines, four fence types, snapping for power poles, street lights, vehicle paths and railway tracks, distance LODs.

## Tags (suggested)
Buildings, Quality of Life, Vehicles, Logistics
