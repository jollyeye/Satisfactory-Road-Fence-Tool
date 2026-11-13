# Adding to the game's own UI: what worked (Satisfactory 1.x, SML 3.12)

Everything Road Maker does to put its own entries into the game's menus (customizer, build menu,
item lists) and its own feedback into the game view (outlines, hints, hologram overlays, the key hint bar, the map), with
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

6. **Stale icons after loading a save a second time** (same game session; the first load was always
   fine). Our option-name → icon table kept `TWeakObjectPtr<UTexture2D>`s taken from the
   descriptors once. A descriptor's `mIcon` is a **soft** pointer (`TSoftObjectPtr`,
   `UFGFactoryCustomizationDescriptor::GetIcon()` loads it); loading another save frees the textures
   and loads new ones, the kept pointers went stale, the tiles were skipped and showed the previous
   tab's pictures. Fix: keep the descriptor **class** (`TWeakObjectPtr<UClass>`) and call `GetIcon()`
   every time (`IconFor` in `RMRoadLinePaint.cpp`); rebuild the table if a class went away
   (*tested 2026-11-12*). Rule: **never cache a texture from a soft pointer across loads**.

### The info box beside the menu
The panel on the right of the customizer (title, picture, "Info", description). Logged
(*tested 2026-11-12*):

| Thing | Value |
|---|---|
| Widget | `Widget_BuildMenu_InfoBox_C` (inside `BPW_BuildMenu_Browser_C` inside `BPW_Customiser_C`; there is more than one instance, only one visible) |
| Option name | `UTextBlock` named `mItemName` (the title, e.g. "4 lanes, continuous") |
| Plain picture | `UImage` named `mPreviewIcon`, brush resource = a plain texture (`T_Icon_RoadMat_Asphalt`, `IconDesc_FicsitWall8x4_512`...); used for Material tab options |
| Live preview | another `UImage` (generated name `Image_<number>`) whose brush is a MID of `MM_UI_CustomiserPreview` (parameters `ID`, `Type`, `StencilMaskMin/Max`): a foundation rendered with the option |
| Our Road Lines options | the game shows the **live foundation preview** (`mPreviewIcon` hidden, keeps the last picture) |

Fix (`FixInfoBoxPicture` in `RMRoadLinePaint.cpp`, in the same 0.1 s tick as the tiles): for each
visible `*BuildMenu_InfoBox*`, if `mItemName` is one of our option names, collapse the image(s)
whose brush resource name contains `CustomiserPreview` and show `mPreviewIcon` (`HitTestInvisible`)
with our icon, keeping the brush's image size. Remember each image's visibility before our change and
what we set; when another option is shown, put the old visibility back **only if the game has not
changed it since** (it sets its own for the option it now shows).

How it was found, so nobody repeats it:
1. Assumed `mPreviewIcon` showed the stale picture and set our icon in it → no change: it was
   hidden; the visible picture was the live foundation preview.
2. Looked for the option name in texts **around** the info box → wrong place; the name is
   `mItemName` inside it.
3. Loggers: first by owner widget name containing "Custom" (missed it: the info box's own class is
   `Widget_BuildMenu_InfoBox_C`), then "big pictures anywhere" (too much HUD noise), then "any
   picture whose parent chain contains `Customi`" → found `mPreviewIcon` and the preview MID; a last
   logger of the info box texts found `mItemName`. Rule: walk the **whole parent chain** when
   filtering widgets, and log per widget instance (several info boxes exist; one shared "last text"
   alternated between them and used up the log budget).

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

## 6. Our key in the build gun's key hint bar (bottom of the screen)

Goal: the road limits key ("Toggle Limits [Left Alt]") in the bar at the bottom while a road is
held, next to Build / Rotate Hologram / Snap Mode, and rebindable in Options > Controls.
*Tested 2026-11-13.* Code: `Private/RMRoadLimits.cpp`, `Private/Holograms/RMRoadHologram.cpp`.

### A real, rebindable key binding
| Piece | What | Where |
|---|---|---|
| Input action | `IA_RM_ToggleLimits` (`UInputAction`, bool, not consuming input), Player Mappable Key Settings: Name `RoadMaker_ToggleLimits`, Display Name "Toggle Road Limits" | `create_assets.py` ("Input"): `DataAssetFactory` with `data_asset_class` = the class, `unreal.new_object(unreal.PlayerMappableKeySettings, outer=action)` |
| Mapping context | `MC_RM_RoadTools` (`UFGInputMappingContext`, `mDisplayName` "Road Maker"), `map_key(action, LeftAlt)` | same |
| Bound while a road is held | `AFGPlayerController::SetMappingContextBoundWithHandle(Handle, Context, bBind, 10)`, counted (switching road widths makes the new hologram before the old one goes) | `RMRoadLimits::BindRoadInput` |
| Reading the key | the key the action is mapped to right now: `UEnhancedInputLocalPlayerSubsystem` > `GetPlayerInput()` > `GetEnhancedActionMappings()`, entries whose `Action` is ours; polled with `IsInputKeyDown` (lets us ignore Alt+Tab / Alt+Enter) | `LimitsKeys` |

Result: the "Road Maker" section shows up in Options > Controls by itself and rebinding works.
**But the hint bar did not show the key** with this alone.

### The hint bar is filled by the build mode widget, not by the bindings
Tries that did **not** put our key in the bar:
1. The mapping context bound (above).
2. The road hologram listening to the action on its own input component (`EnableInput` +
   `UEnhancedInputComponent::BindAction`, empty handler). `UFGButtonHintBar` in its "Enhanced Input"
   mode is not what the build gun uses.

What InfiniteNudge does (github.com/mklierman/SatisfactoryMods, `InfiniteNudge`): a Blueprint hook on
`Widget_BuildMode` > `SetKeybindingHints` that edits an existing hint's text. We cannot make hook
assets from our script, so we do the same from C++ every frame. Logged layout (*tested 2026-11-13*):

| Thing | Value |
|---|---|
| Hint widget | `Widget_Hint_C`, made **at run time** (not in the build mode widget's designed tree, so `WidgetTree->ForEachWidget` from `Widget_BuildMode_C` does not find it) |
| Its panel | parent named `mHintContainer` |
| Texts in a hint | `mHintDescription` ("Rotate") and `mHintKey` ("R"; for unbound actions `UNKNOWN_KEY(<action>)`) |
| Stale copies | many old `Widget_Hint_C` with no parent / hidden: skip them |

Our hint (`RMRoadLimits::UpdateHintBar`, called every frame from the road hologram's `Tick`):
- find a **visible** `Widget_Hint_C` whose parent is named `mHintContainer` (`TObjectIterator<UUserWidget>`);
  `CreateWidget` of the same class, `AddChild` to that panel (it lands after "Hide HUD");
- set `mHintDescription` = "Toggle Limits", `mHintKey` = `UFGInputLibrary::GetAbbreviatedKeyName(key)`
  every frame; description orange (sRGB 250 149 73) while the key is held, like the game's hints;
- the game clears and refills the panel (mode change...): if our hint is no longer in it, add it again;
- removed in the hologram's `EndPlay`.

### The build mode's name carries the state
"Zoop (Limits: ON)": `mDisplayName` of the build mode descriptor's CDO changed by reflection
(`FindFProperty<FTextProperty>(UFGBuildGunModeDescriptor::StaticClass(), "mDisplayName")`), the original
name kept per class. Only our own descriptors (path under `/RoadMaker/`): the game's
`BuildMode_Default` is shared by every building, so roads got their own Default / Straight descriptors
(`mDefaultBuildMode` set to ours before `Super::BeginPlay`). The label refreshes with
`AFGBuildGun::mOnBuildGunModeChanged.Broadcast(GetCurrentBuildGunMode())`.

Dropped on the way: an on-screen hint box of our own (bottom centre) covered the game's HUD.

---

## 7. The map: road layer and a row in the side menu

Code: `Private/Map/RMMapLayer.cpp` (layer), `Private/Map/RMMapRoadsRow.cpp` (row).
*Tested 2026-11-12 / 13.*

### The road layer
- The map widget nests the picture in `Overlay_0` (1024 x 1024 "map units") inside
  `mMapScrollContainer` (render translation = pan) inside `mMapZoomContainer` (render scale = zoom).
  `Overlay_0` holds `mMap`, `BPW_MapAreaPainter` (fog / areas) and `mMapObjectPanel` (icons).
- We find the area painter (class name contains `MapAreaPainter`), take its parent's **live slate**
  `SOverlay` (`GetCachedWidget()`) and add our own `SCanvas` in a slot with the painter's ZOrder, so the
  game pans and zooms it and the icons stay on top. World to map: `UFGMapFunctionLibrary::GetNormalizedPosition(capture actor, location) * 1024`.
- 512 m tiles (512 px, painted at 2x and averaged, with mips), only while the map is open, dirty
  tiles only, time budgets per frame. Details in the code header and `ROADMAP-1.3.0.md`.

### "Show Roads on Map" in the side menu
Logged layout of the game's "Show Stamps on Map" row (*tested 2026-11-13*):

```
BPW_MapMenu_C
  CanvasPanel_0 (6 children)
    Widget_Scrollbox_C        <- the list (Truck Stations ... Drones)
    Overlay_2                 <- the stamps row, placed by its canvas slot
      ScaleBox_0
      mShowStampsButton (UButton)
        HorizontalBox_0
          TextBlock ("Show Stamps on Map")
          BPW_OptionsCheckbox_C   <- the tick (a Blueprint widget, not a UCheckBox)
```

How our row is made (`RMMapRoads::EnsureRow`, from the map layer's tick, at most once a second):
- find the stamps text by `GetText().BuildSourceString()` == "Show Stamps on Map" (the English
  source works in every game language), walk up to the `UButton`, then to the widget whose parent is
  a `UCanvasPanel` (= `Overlay_2`);
- our row (`URMMapRoadsRow`, a C++ `UUserWidget`: overlay > button > horizontal box [text, tick]) is added
  to that canvas with the stamps row's slot layout **one row higher** (height = the row's desired size,
  40);
- whatever in the canvas reaches down to the stamps row (the `Widget_Scrollbox_C` list) is made one
  row shorter (stretched slot: `Offsets.Bottom += height`), so nothing overlaps;
- look: the stamps button's `FButtonStyle`, the text's font (+1 pt), colour and slot padding, a
  2 px grey line on top (sRGB 102 102 103, measured from a screenshot), and a `CreateWidget` copy of
  `BPW_OptionsCheckbox_C` (`HitTestInvisible`, clicks go to our button). Its state is set through its
  Blueprint function **`SetChecked_NoNotify(bool)`** with `ProcessEvent` (found by logging its
  functions; it also has `mIsChecked`);
- state per player, remembered (`GameUserSettings.ini`, `[RoadMaker] bMapShowRoads`, on by default);
  off = our canvas `Collapsed` and nothing painted.

Mistakes made on the way:
1. Required a `UCheckBox` around the text: there is none (the tick is the Blueprint widget), so the
   row was never found.
2. Fallback "nearest vertical box up the chain" climbed out of the menu to the window's
   `mMainContainer`: the whole map got shorter. Walk up only to the first canvas.
3. Placed one row above without shortening the list: the list's bottom entries showed through.
4. A `UImage` with no brush size is drawn 32 px tall (the default): the "2 px line" covered the row.
   Give the brush an explicit `ImageSize`.
5. A clicked `UButton` keeps focus and stays highlighted. `InitIsFocusable` is protected: set the
   `IsFocusable` property by reflection before the button is built.

The game's own map filters (eye buttons) did not work in our tests (1.2, 2026-11); no report found on
questions.satisfactorygame.com for 1.2 (older compass filter reports from 1.0 exist).

---

## 8. Build pitfalls

- **Unity builds** merge `.cpp` files: helpers in anonymous namespaces must have file-specific names
  (`FenceWallTrace`, not `Trace`), or the build fails with "already has a body".
- `SUBSCRIBE_METHOD_VIRTUAL` needs an object (use the CDO, e.g. `GetMutableDefault<...>()`).
- Hooks and UI tickers that touch widgets go inside `#if !WITH_EDITOR`.
- Blueprint events have no C++ symbol: `FindFunction` by name, build a parameter struct matching
  `ParmsSize` (log it first), `ProcessEvent`.
- Protected C++ members: a struct deriving from the game class with static accessors. UPROPERTY
  members: `FindFProperty` reflection.
- `UUserWidget::Invalidate` and `UButton::InitIsFocusable` are protected (compile error C2248): use
  `InvalidateLayoutAndVolatility()` and reflection.
- Logging a widget layout once (names, classes, parents, texts) and reading `FactoryGame.log` beats
  guessing: every UI fix in sections 6 and 7 came from one such log.

