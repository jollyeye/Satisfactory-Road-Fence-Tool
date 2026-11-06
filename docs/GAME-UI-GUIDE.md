# Adding to the game's own UI: what worked (Satisfactory 1.x, SML 3.12)

Everything Road Maker does to put its own entries into the game's menus (customizer, build menu,
item lists) and its own feedback into the game view (outlines, hints, hologram overlays), with
the problems we hit and how they were fixed. Written so a future developer or AI can reuse it
without repeating the trial and error.

Tested on Satisfactory 1.1/1.2 (Experimental and Early Access builds of 2026), SML 3.12. Notes
marked *tested <date>* were confirmed in game on that date.

Rule of thumb that holds for everything below: **the editor project only has placeholder game
assets** (empty widgets, stub materials), so you cannot learn the game's UI from the editor. Learn it
at run time (section 1), then change it from C++.

---

## 1. Learning the game's UI at run time: the diag logger

The breakthrough for every hard UI bug was a temporary logger that prints only what *changes*.

- File: `Source/RoadMaker/Private/Diag/RMDiag.cpp`, installed from `RoadMakerModule.cpp`
  (`RMDiag::InstallHooks()`). Removed for the 1.1.1 release; get it back from git history (it is
  in commit `1913f2e` and earlier). **Always remove it before a release.**
- An `FTSTicker` runs every 0.1 s, builds one line of text per thing it watches, and logs a
  `[Diag]` line only when that text differs from last time. Logs stay short even at 10 Hz.
- What it watched:
  - every visible customizer tile: label, the image's brush resource name, the MID parameters
    (`Texture`, `IsColor`, colours) and the draw settings;
  - the paint gun: active material descriptor, `IsSwatchHitValid / IsPatternHitValid /
    IsMaterialHitValid`, the aimed actor and each of its components' `bRenderCustomDepth` and
    `CustomDepthStencilValue`;
  - every component in the world with custom depth on ("outlined now"), to see what the game
    itself outlines;
  - the signature of a Blueprint event (`UFunction` parameters and `ParmsSize`), before calling it
    with `ProcessEvent`.
- Workflow: the developer reproduces the bug (often comparing a vanilla building with ours), then
  Claude reads `FactoryGame.log` (`%LOCALAPPDATA%\FactoryGame\Saved\Logs`) through the desktop app
  file bridge and greps `[Diag]`.
- Comparing **a vanilla building and ours** in the same session is what gave the answers (the
  outline stencil values, how the game fills tile icons).

---

## 2. A customizer tab of our own (Road Lines)

### How it is made
| Piece | What | Where |
|---|---|---|
| Tab | Blueprint `CustCat_RoadLines`, parent = the game's `CC_Material` (material-style tiles with icons), own name, icon and priority | `Tools/create_assets.py` ("Road Lines (customizer)") |
| Sub-tabs | `UFGCustomizerSubCategory` Blueprints: Sidelines, Lanes | same |
| Options | Blueprints of `URMRoadLineDescriptor` (a `UFGFactoryCustomizationDescriptor_Material` subclass) with `mCategory`, `mSubCategories`, `mMenuPriority`, `mValidBuildables` = our road / intersection classes | `RMRoadLineDescriptor.h`, `create_assets.py` |
| Recipes | customization recipes so the options show up (free) | `create_assets.py` |

Deriving the tab from a plain `UFGCustomizerCategory` gives colour-swatch tiles; deriving from
`CC_Material` gives tiles that show a picture. Our own options still use our descriptor class.

### Knowing what the player picked
`SUBSCRIBE_METHOD_AFTER(UFGBuildGunStatePaint::SetActiveCustomization, ...)` and
`...SetActiveRecipe` (we hook both; the recipe gives the descriptor via `UFGCustomizationRecipe::GetCustomizationDescriptor`). If the descriptor is ours,
remember it; any other pick clears it.

### Applying it ourselves
The game's paint state knows nothing about our options, so:
- **Fire:** hook `AFGBuildGun::OnPrimaryFirePressed`; when a Road Lines option is active and the
  aimed actor is a road / intersection, apply it and `Scope.Cancel()` the game's handling. Clients
  send it through `URMRemoteCallObject`.
- **"Invalid customization target!"**: our options map to no building, so the game flags every
  target. While one of ours is active, empty the paint state's `mConstructDisqualifiers` after its
  tick (`SUBSCRIBE_METHOD_VIRTUAL_AFTER(UFGBuildGunStatePaint::TickState_Implementation, CDO, ...)`,
  then `FindFProperty<FArrayProperty>` + `FScriptArrayHelper::EmptyValues`).
- **Our own hint and preview:** `URMRoadLinesHint`, a `UUserWidget` built in C++ (`RebuildWidget`
  with a `WidgetTree`: vertical box, spacer, `UBorder`, `UTextBlock`), `AddToViewport(500)`,
  `HitTestInvisible`. Its `NativeTick` traces the aimed actor, shows the option name on a dark box or
  a red warning ("Aim at a road", "Intersections take Sidelines only"), and puts a **local preview**
  of the markings on the aimed road (`SetPreviewMarkings`), cleared when aiming away.

### Icons in our tiles: the hard part
The game builds tile widgets in Blueprint and **recycles them between tabs**. Facts found with the
logger (*tested 2026-11*):

| Thing | Value |
|---|---|
| Tile widget | class name contains `CustomiserIcon` |
| Picture | `UImage` named `mIcon` inside it |
| Its brush resource | a dynamic material instance, `MID_MM_UI_CustomiserIcon_*` |
| MID parameters | `Texture` (e.g. `IconDesc_*_512`), `IsColor` (1 = colour swatch), `PrimaryColor`, `SecondaryColor` |
| How the game fills a tile | it sets these **MID parameters**; it does not replace the brush |

We find our tiles by their label: every 0.1 s while the build gun is out, iterate visible
`UTextBlock`s (`TObjectIterator`), match the text to an option name, go to the outer `UUserWidget`,
then to its `mIcon`.

**Correct method:** set `Texture` = our icon and `IsColor` = 0 on the tile's own MID. Keep the brush.
Hide the tile's "Edit" text (left over from the colour tab: our options are not editable).

**Restoring a recycled tile** (its label is no longer one of ours): put back the original `Texture`
**only if the MID still has our icon** (the game may already have set its own), restore the hidden
"Edit" texts, and **never restore `IsColor`** (the game sets it per tab).

History, so nobody repeats it:
1. Swapped the brush resource for our texture → other tabs showed stale Road Lines pictures: the
   game kept updating the MID we had taken out of the brush.
2. Restored the brush "only if still ours" → still wrong.
3. Restored the draw settings too → still wrong.
4. Logger showed the MID-parameter mechanism → set the `Texture` parameter instead. Fixed tabs.
5. Swatch tiles then showed pictures: our restore put back `IsColor = 0` on a tile the game had
   just made a colour swatch. Never restore `IsColor` → fixed (*tested 2026-11*).

### Our options on the hotbar
Hovering a tile and pressing a hotbar key binds it (`UFGFactoryCustomizationShortcut`, holding the
customization recipe). Logged (*tested 2026-11-06*):

| Widget | Vanilla material (concrete wall) | Our Road Lines option |
|---|---|---|
| `Widget_HotbarEntry_C` > `mIconBG` | MID `MM_UI_HotbarEntryBackground`, `IsCustomiser = 1` | same |
| `BPW_CustomiserIcon_C` (`mCustomiserIcon`) > `mIcon` | MID of `MM_UI_CustomiserIcon`, `Texture = IconDesc_ConcWall8x4_512`, `IsColor = 0` | **bare `MM_UI_CustomiserIcon` material** (defaults: white / magenta swatch) |
| `GetDisplayImage()` of the shortcut | the icon | our icon (correct) |

So the shortcut is fine, but the entry's Blueprint only fills the picture for the game's own
descriptor types. Fix (`FixRoadLinesHotbarIcons` in `RMRoadLinePaint.cpp`): every 0.2 s, for each
visible `*HotbarEntry*` widget, read its shortcut through the first object property of type
`UFGHotbarShortcut` (reflection), and if it holds a Road Lines recipe, give the `mIcon` image a MID
of the same material (if it has none) with `Texture` = `GetDisplayImage()` and `IsColor` = 0.

### Icons themselves
`Tools/IconSource/render_icons.py` (Python + PIL) draws the pictures into `Tools/Icons`;
`create_assets.py` imports every PNG there as a texture.

---

## 3. Our buildings in the game's Material tab (fence walls)

Goal: pick Concrete / FICSIT / Steel wall material in the game's own Material tab and switch a
fence wall.

### Why not let the game do it
The game's wall materials (`MaterialDesc_Wall_Concrete_C`, `_Orange_C`, `_Steel_C`) only apply to
classes listed in `mValidBuildables`, and swap via `mBuildableMap` (`AddType`). The game's swap
spawns a fresh building from a recipe, which **loses a spline building's shape**. So we register
our classes so the menu accepts them, but intercept the fire.

### Our swap (`ARMBuildableFence::SetWallLook`)
- Hook `AFGBuildGun::OnPrimaryFirePressed`; if a game wall material is active and a fence wall is
  aimed, call `SetWallLook` (client → `URMRemoteCallObject::ServerSetFenceWallLook`) and cancel.
- Real swap (so name, recipe and refund change): `SpawnActorDeferred` of the variant class
  (`mWallLookClasses`), `SetBuiltWithRecipe`, `SetInwardSign`, `SetCostMultiplier`,
  `SetSplinePoints`, `SetCustomizationData_Native`, `FinishSpawningActor`. The old wall:
  `SetActorEnableCollision(false)`, `SetActorHiddenInGame(true)`, `IFGDismantleInterface::Execute_Dismantle`.
- **Bug: "stops applying for a while":** the old wall, still being dismantled, kept catching the
  traces. Fix: hide it and turn its collision off at once, and skip hidden / dismantled /
  being-destroyed walls in the trace.
- Cost: net difference of the two recipes. Check with inventory + **Dimensional Depot**
  (`AFGCentralStorageSubsystem::GetNumItemsFromCentralStorage`), spend inventory first then the
  depot (`TryRemoveItemsFromCentralStorage`), refund with `AddStack`. Free when
  `AFGGameState::GetCheatNoCost()`.
- Preview: while hovering, the wall shows the selected look (`mPreviewWallLook`).

### Feedback the player expects from the customizer
| Effect | How | Notes |
|---|---|---|
| **White outline** (can apply) | custom depth on the wall's components, **stencil 2** | found by logging a vanilla wall: the game outlines a preview actor with stencil 2 |
| **Red outline** (cannot) | **stencil 7** | used for the same material or not affordable |
| Menu valid / invalid state | `OnHitStatusChanged(TSubclassOf<UFGBuildingDescriptor>, bool validSwatch, bool validPattern, bool validMaterial)` via `ProcessEvent` (ParmsSize 11) | Blueprint event: affects the UI only, **not** the outline |
| Failure sound / effect | Blueprint event `OnApplyCustomizationFailed` via `FindFunction` + `ProcessEvent` | play when the switch is refused |

Mistakes made on the way:
- Called `OnHitStatusChanged` hoping it draws the outline: it does not.
- Used the **hologram** stencils (OK 5, WARNING 3, ERROR 6): the wall looked like a build hologram.
  Those are for holograms only (section 5).

---

## 4. Build menu and item lists

- **Categories:** the build menu uses `mCategory` + `mSubCategories` on the building descriptor.
  Roads use the game's vehicle category with our own sub-category; fence walls go into the game's
  Walls category (`BC_Walls`) with our own sub-category "Fence Walls" (`mMenuPriority` 90). All set
  in `create_assets.py` by loading the game classes by name.
- **Not an item:** descriptors that are not physical (Road Lines options) showed up under *Parts* in
  creative mode's give-item list. Fix: `mForm = EResourceForm::RF_INVALID` (constructor in C++ and
  set by `create_assets.py`).
- **Paint group:** roads have their own swatch group "Roads" (`URMSwatchGroup_Roads`, set as
  `mSwatchGroup` on the buildables). The game only lists groups present in
  `AFGGameState::mSwatchGroupDatum`, so `RMGameWorldModule::EnsureRoadSwatchGroup` adds an entry by
  reflection at world start (with a default swatch).

---

## 5. Feedback in the world while building (holograms)

- **Procedural meshes in holograms:** they are built after the game set the hologram look, so they
  miss it. `RMHologramLook::Apply` puts the game's hologram material on every section and sets
  custom depth with `GetStencilForHologramMaterialState(mPlacementMaterialState)` (valid / warning /
  invalid colours).
- **Intersection pick overlay:** a separate procedural mesh with the unlit, translucent, two-sided
  `M_RM_Overlay` material, **depth test disabled** (visible through buildings), colour from vertex
  colours. It draws ~4 px camera-facing lines around ends (orange = aimed, white = picked) and arrows.
  Custom depth is off on it so the hologram outline does not tint it.
- **Old build-gun menu (before 1.0, no longer in the build menu):** `RMLanePaint` opened its own widget on the build
  gun's mode-select key (`AddToViewport(1000)`) and read the key binding each time to show the
  right letter. Replaced by the customizer tab, which is the game's own place for this.

---

## 6. Build pitfalls

- **Unity builds** merge `.cpp` files: helpers in anonymous namespaces must have file-specific names
  (`FenceWallTrace`, not `Trace`), or the build fails with "already has a body".
- `SUBSCRIBE_METHOD_VIRTUAL` needs an object (use the CDO, e.g. `GetMutableDefault<...>()`).
- Hooks and UI tickers that touch widgets go inside `#if !WITH_EDITOR`.
- Blueprint events have no C++ symbol: `FindFunction` by name, build a parameter struct matching
  `ParmsSize` (log it first), `ProcessEvent`.
- Protected C++ members: a struct deriving from the game class with static accessors. UPROPERTY
  members: `FindFProperty` reflection.
