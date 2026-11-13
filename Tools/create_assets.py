# Road Maker: creates every Blueprint asset the mod needs.
# Run inside the editor: UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file>
# Safe to re-run: existing assets are loaded and updated, not duplicated.
import unreal

LOG_PATH = r"D:\SFMods\assets-log.txt"
_log = open(LOG_PATH, "w", encoding="utf-8")


def log(msg):
    _log.write(msg + "\n")
    _log.flush()
    unreal.log("[RoadMaker] " + msg)


ROOT = "/RoadMaker"
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary
registry = unreal.AssetRegistryHelpers.get_asset_registry()
# Without the editor window the registry may not have scanned the mod folder yet.
registry.scan_paths_synchronous(["/RoadMaker", "/Game/FactoryGame"], True)

# ---------------------------------------------------------------------------------------------
# Finding game assets and classes
# ---------------------------------------------------------------------------------------------
_game_assets = None


def game_assets():
    global _game_assets
    if _game_assets is None:
        f = unreal.ARFilter(package_paths=["/Game/FactoryGame"], recursive_paths=True)
        _game_assets = registry.get_assets(f)
        log("Indexed %d game assets" % len(_game_assets))
    return _game_assets


def find_game_asset(name, contains=False):
    """Find a game asset by exact (case-insensitive) name, or by substring."""
    low = name.lower()
    hits = []
    for a in game_assets():
        n = str(a.asset_name).lower()
        if n == low or (contains and low in n):
            hits.append(a)
    return hits


def load_game_class(name):
    """Blueprint generated class for a game asset name like 'Desc_Cement'."""
    for a in find_game_asset(name):
        obj = a.get_asset()
        if isinstance(obj, unreal.Blueprint):
            return obj.generated_class()
        path = str(a.package_name) + "." + str(a.asset_name) + "_C"
        cls = unreal.load_class(None, path)
        if cls:
            return cls
    log("  ! game class not found: " + name)
    return None


def native_class(*names):
    for n in names:
        cls = getattr(unreal, n, None)
        if cls is not None:
            return cls
    log("  ! native class not found: " + "/".join(names))
    return None


# ---------------------------------------------------------------------------------------------
# Blueprint helpers
# ---------------------------------------------------------------------------------------------
def make_bp(folder, name, parent):
    path = "%s/%s/%s" % (ROOT, folder, name) if folder else "%s/%s" % (ROOT, name)
    try:
        bp = unreal.load_asset("%s.%s" % (path, name))
    except Exception:
        bp = None
    if bp:
        log("Updating " + path)
    else:
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", parent)
        pkg = path.rsplit("/", 1)[0]
        bp = asset_tools.create_asset(name, pkg, None, factory)
        log("Created " + path)
    return bp


def cdo(bp):
    return unreal.get_default_object(bp.generated_class())


def setp(obj, value, *names):
    """Set the first property name that exists. Logs what happened."""
    for n in names:
        try:
            obj.set_editor_property(n, value)
            return True
        except Exception:
            continue
    log("  ! could not set " + "/".join(names))
    return False


def getp(obj, *names):
    for n in names:
        try:
            return obj.get_editor_property(n)
        except Exception:
            continue
    return None


def finish(bp):
    try:
        unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    except Exception as e:
        log("  ! compile failed: %s" % e)
    eal.save_loaded_asset(bp, only_if_is_dirty=False)


def item_amount(cls, amount):
    ia = unreal.ItemAmount()
    setp(ia, cls, "item_class", "ItemClass")
    setp(ia, amount, "amount", "Amount")
    return ia


def text(s):
    return unreal.Text(s) if hasattr(unreal, "Text") else s


# ---------------------------------------------------------------------------------------------
# Look up everything we need from the game
# ---------------------------------------------------------------------------------------------
log("== Looking up game assets ==")
Desc_Cement = load_game_class("Desc_Cement")
Desc_IronPlate = load_game_class("Desc_IronPlate")
Desc_IronRod = load_game_class("Desc_IronRod")
Desc_Wire = load_game_class("Desc_Wire")
BuildGun = load_game_class("BP_BuildGun")
VehiclePathSubCat = load_game_class("SC_VehiclePaths")
VehicleCat = load_game_class("BC_Vehicles")
WallsCat = load_game_class("BC_Walls")
WallsSubCat = load_game_class("SC_Walls")
VanillaConcreteWall = load_game_class("Build_Wall_Concrete_8x4")
VanillaFicsitWall = load_game_class("Build_Wall_8x4_01") or load_game_class("Build_Wall_Orange_8x4_01")
VanillaSteelWall = load_game_class("Build_SteelWall_8x4")
Desc_SteelBeam = load_game_class("Desc_SteelPlate")
Desc_SteelPipe = load_game_class("Desc_SteelPipe") or load_game_class("Desc_Pipe")
if Desc_SteelPipe:
    log("  steel pipe item: " + Desc_SteelPipe.get_name())

UnlockRecipe = load_game_class("BP_UnlockRecipe") or native_class("FGUnlockRecipe")

# Icons: borrow a foundation icon for roads and the intersection, a railing icon for fences.
def icons_from(names):
    for n in names:
        cls = load_game_class(n)
        if cls:
            d = unreal.get_default_object(cls)
            small = getp(d, "m_small_icon", "mSmallIcon")
            big = getp(d, "m_persistent_big_icon", "mPersistentBigIcon")
            if small or big:
                log("  icons from " + n)
                return small, big
    return None, None


road_icons = icons_from(["Desc_Foundation_8x2_01", "Desc_Foundation_8x4_01", "Desc_Foundation_8x1_01"])
fence_icons = icons_from(["Desc_Railing_01", "Desc_ChainLinkFence", "Desc_Barrier_Low_01", "Desc_Foundation_8x1_01"])


def find_material(keywords):
    """Pick a material whose name contains all keywords; log the candidates."""
    hits = []
    for a in game_assets():
        cls = str(a.asset_class_path.asset_name) if hasattr(a, "asset_class_path") else str(a.asset_class)
        if cls not in ("Material", "MaterialInstanceConstant"):
            continue
        n = str(a.asset_name).lower()
        if all(k in n for k in keywords):
            hits.append(a)
    hits.sort(key=lambda a: (0 if str(a.asset_class_path.asset_name) == "MaterialInstanceConstant" else 1, len(str(a.asset_name))))
    log("  materials matching %s: %s" % (keywords, ", ".join(str(h.asset_name) for h in hits[:8]) or "none"))
    return hits[0].get_asset() if hits else None


mat_asphalt = find_material(["asphalt"])
mat_concrete = find_material(["concrete"])
mat_metal = find_material(["metal"]) or find_material(["steel"])
mat_grid = find_material(["fence"]) or find_material(["grate"]) or find_material(["mesh"])

# ---------------------------------------------------------------------------------------------
# Native classes from the mod and the game
# ---------------------------------------------------------------------------------------------
RMRoadHologram = native_class("RMRoadHologram")
RMFenceHologram = native_class("RMFenceHologram")
RMIntersectionHologram = native_class("RMIntersectionHologram")
RMBuildableRoad = native_class("RMBuildableRoad")
RMBuildableFence = native_class("RMBuildableFence")
RMBuildableIntersection = native_class("RMBuildableIntersection")
RMRoadCutHologram = native_class("RMRoadCutHologram")
RMRoadAdjustHologram = native_class("RMRoadAdjustHologram")
RMRoadMergeHologram = native_class("RMRoadMergeHologram")
RMRoadJoinHologram = native_class("RMRoadJoinHologram")
RMFenceJoinHologram = native_class("RMFenceJoinHologram")
RMGantryHologram = native_class("RMGantryHologram")
RMBuildableGantry = native_class("RMBuildableGantry")
RMBuildableMarking = native_class("RMBuildableMarking")
RMMarkingHologram = native_class("RMMarkingHologram")
RMBuildableRoadTool = native_class("RMBuildableRoadTool")
BuildModeDescriptor = native_class("FGHologramBuildModeDescriptor")
BuildingDescriptor = native_class("FGBuildingDescriptor")
Recipe = native_class("FGRecipe")
Schematic = native_class("FGSchematic")
GameWorldModule = native_class("GameWorldModule")

# ---------------------------------------------------------------------------------------------
# Assets
# ---------------------------------------------------------------------------------------------
log("== Holograms ==")
bm = make_bp("Holograms", "BM_Straight", BuildModeDescriptor)
setp(cdo(bm), text("Straight"), "m_display_name", "mDisplayName")
finish(bm)

bm_smooth = make_bp("Holograms", "BM_Smooth", BuildModeDescriptor)
setp(cdo(bm_smooth), text("Smooth (quadratic Bezier)"), "m_display_name", "mDisplayName")
setp(cdo(bm_smooth), text("Quadratic Bezier curve (like the default mode), and the height eases in and out, matching the slope of the roads it connects. Up to 45 degrees."), "m_description", "mDescription")
finish(bm_smooth)

bm_long = make_bp("Holograms", "BM_LongCurve", BuildModeDescriptor)
setp(cdo(bm_long), text("Long Curve"), "m_display_name", "mDisplayName")
setp(cdo(bm_long), text("For big sweeping curves: up to 1 km long, the build gun reaches 1 km, and no turn or slope limits, only refused if the road would fold over itself. Snap-key paths and tracks go on it in sections."), "m_description", "mDescription")
finish(bm_long)

holo_road = make_bp("Holograms", "Holo_Road", RMRoadHologram)
setp(cdo(holo_road), bm.generated_class(), "m_build_mode_straight", "mBuildModeStraight")
setp(cdo(holo_road), bm_smooth.generated_class(), "m_build_mode_smooth", "mBuildModeSmooth")
setp(cdo(holo_road), bm_long.generated_class(), "m_build_mode_long", "mBuildModeLong")
finish(holo_road)

holo_fence = make_bp("Holograms", "Holo_Fence", RMFenceHologram)
setp(cdo(holo_fence), bm.generated_class(), "m_build_mode_straight", "mBuildModeStraight")
finish(holo_fence)

holo_x = make_bp("Holograms", "Holo_Intersection", RMIntersectionHologram)
finish(holo_x)

bm_cut_fences = make_bp("Holograms", "BM_CutWithFences", BuildModeDescriptor)
setp(cdo(bm_cut_fences), text("Cut also fences"), "m_display_name", "mDisplayName")
setp(cdo(bm_cut_fences), text("Also cuts this road's fences at the same place: road fences standing on it and fence walls on its edges (stacked walls too). Other roads and their fences are left alone. Train tracks and vehicle paths on the road are always cut."), "m_description", "mDescription")
finish(bm_cut_fences)

holo_cut = make_bp("Holograms", "Holo_RoadCut", RMRoadCutHologram)
setp(cdo(holo_cut), bm_cut_fences.generated_class(), "m_build_mode_with_fences", "mBuildModeWithFences")
finish(holo_cut)

holo_split = make_bp("Holograms", "Holo_RoadSplit", RMRoadCutHologram)
setp(cdo(holo_split), True, "m_split_tool", "mSplitTool")
finish(holo_split)

holo_merge = make_bp("Holograms", "Holo_RoadMerge", RMRoadMergeHologram)
finish(holo_merge)

holo_join = make_bp("Holograms", "Holo_RoadJoin", RMRoadJoinHologram)
finish(holo_join)

holo_adjust = make_bp("Holograms", "Holo_RoadAdjust", RMRoadAdjustHologram)
finish(holo_adjust)

holo_gantry = make_bp("Holograms", "Holo_SignGantry", RMGantryHologram)
finish(holo_gantry)

holo_marking = make_bp("Holograms", "Holo_Marking", RMMarkingHologram)
finish(holo_marking)

holo_fence_cut = make_bp("Holograms", "Holo_FenceCut", RMRoadCutHologram)
setp(cdo(holo_fence_cut), True, "m_fence_tool", "mFenceTool")
finish(holo_fence_cut)

holo_fence_join = make_bp("Holograms", "Holo_FenceJoin", RMFenceJoinHologram)
finish(holo_fence_join)

# Lane Paint moved to the customizer (Road Lines tab): remove its build-menu assets.
for old in ("Unlocks/Recipe_LanePaint", "Buildables/Desc_LanePaint", "Buildables/Build_LanePaint",
            "Holograms/Holo_LanePaint", "Holograms/BM_LaneMenu",
            "Holograms/BM_PickEnds",
            # Medium lane markings dropped (markings scale with the lane), and the build menu section they had.
            "Customization/RoadLines_MarkArrowMedium", "Customization/Recipe_RoadLines_MarkArrowMedium",
            "Customization/RoadLines_MarkNumberMedium", "Customization/Recipe_RoadLines_MarkNumberMedium",
            "Unlocks/Recipe_LaneArrowMedium", "Buildables/Desc_LaneArrowMedium", "Buildables/Build_LaneArrowMedium",
            "Unlocks/Recipe_LaneNumberMedium", "Buildables/Desc_LaneNumberMedium", "Buildables/Build_LaneNumberMedium",
            "Buildables/SC_LaneMarkings"):
    path = ROOT + "/" + old
    try:
        if unreal.load_asset(path + "." + old.split("/")[-1]):
            eal.delete_asset(path)
            log("Deleted " + path)
    except Exception as e:
        log("  ! could not delete %s: %s" % (path, e))


log("== Sub-category ==")
subcat = None
if VehiclePathSubCat:
    subcat_bp = make_bp("Buildables", "SC_Roads", VehiclePathSubCat)
    setp(cdo(subcat_bp), text("Roads"), "m_display_name", "mDisplayName")
    setp(cdo(subcat_bp), 50.0, "m_menu_priority", "mMenuPriority")
    finish(subcat_bp)
    subcat = subcat_bp.generated_class()

# Fence walls: build menu Walls tab, own section "Fence Walls"; unlocked by the game's Tier 1
# Base Building milestone (added at runtime by the world module), not by the Road Maker milestone.
FENCE_WALLS = {"FenceWall_Concrete", "FenceWall_Ficsit", "FenceWall_Steel"}
# Everything in the Walls tab, Fence Walls section (the steel mesh wall too).
WALLS_MENU = FENCE_WALLS | {"Fence_GridTall"}

basic_steel = None
for sname in ("Schematic_3-1", "Schematic_3-2", "Schematic_3-3", "Schematic_3-4"):
    scls = load_game_class(sname)
    if not scls:
        continue
    dn = str(getp(unreal.get_default_object(scls), "m_display_name", "mDisplayName") or "")
    log("  %s = '%s'" % (sname, dn))
    if "steel" in dn.lower():
        basic_steel = scls
if not basic_steel:
    log("  ! 'Basic Steel Production' not found by name")
recipe_unlocked_by = {}
# Texture layout of each game wall material on the fence wall (see FRMWallTexLayout):
# (u at panel centre, u per metre, v at the top, v per metre, mirrored half panel)
WALL_LAYOUTS = {
    "FenceWall_Concrete": (0.0, 0.214, 0.021, 0.21675, True),
    "FenceWall_Ficsit": (0.0, 0.214, 0.021, 0.21675, True),
    "FenceWall_Steel": (0.0, 0.214, 0.021, 0.21675, True),
}
fence_wall_classes = {}
wall_subcat = None
if WallsSubCat:
    wsc = make_bp("Buildables", "SC_FenceWalls", WallsSubCat)
    setp(cdo(wsc), text("Fence Walls"), "m_display_name", "mDisplayName")
    setp(cdo(wsc), 90.0, "m_menu_priority", "mMenuPriority")
    finish(wsc)
    wall_subcat = wsc.generated_class()
else:
    log("  ! SC_Walls not found: fence walls go in the Roads section")

# Road Tools: own build menu tab (same look as the game's tabs: a child of the Vehicles category),
# section "Edit road" with Cut Road.
road_tools_cat = None
edit_road_subcat = None
edit_fence_subcat = None
lane_marking_subcat = None
if VehicleCat:
    rtc = make_bp("Buildables", "BC_RoadTools", VehicleCat)
    rc_cdo = cdo(rtc)
    setp(rc_cdo, text("Road Tools"), "m_display_name", "mDisplayName")
    base_prio = getp(unreal.get_default_object(VehicleCat), "m_menu_priority", "mMenuPriority") or 0.0
    setp(rc_cdo, float(base_prio) + 0.5, "m_menu_priority", "mMenuPriority")
    finish(rtc)
    road_tools_cat = rtc.generated_class()
if VehiclePathSubCat:
    erc = make_bp("Buildables", "SC_EditRoad", VehiclePathSubCat)
    setp(cdo(erc), text("Edit road"), "m_display_name", "mDisplayName")
    setp(cdo(erc), 1.0, "m_menu_priority", "mMenuPriority")
    finish(erc)
    edit_road_subcat = erc.generated_class()
    efc = make_bp("Buildables", "SC_EditFence", VehiclePathSubCat)
    setp(cdo(efc), text("Edit fence"), "m_display_name", "mDisplayName")
    setp(cdo(efc), 2.0, "m_menu_priority", "mMenuPriority")
    finish(efc)
    edit_fence_subcat = efc.generated_class()
    lane_marking_subcat = None
# Road Tools entries and their section.
MARKING_TOOLS = ("LaneArrow", "LaneNumber")  # painted from the customizer (Road Lines), not in the build menu
ROAD_TOOLS_MENU = {"RoadCut": "road", "RoadJoin": "road", "RoadSplit": "road", "RoadMerge": "road", "RoadAdjust": "road", "FenceCut": "fence", "FenceJoin": "fence"}

base_building = None
for sname in ("Schematic_1-1", "Schematic_1-2", "Schematic_1-3"):
    scls = load_game_class(sname)
    if not scls:
        continue
    dn = str(getp(unreal.get_default_object(scls), "m_display_name", "mDisplayName") or "")
    log("  %s = '%s'" % (sname, dn))
    if "base building" in dn.lower():
        base_building = scls
if not base_building:
    base_building = load_game_class("Schematic_1-1")
    log("  ! 'Base Building' not found by name, using Schematic_1-1")
wall_recipes = []
fence_wall_recipes = {}

# name, parent, hologram, display, description, props, recipe ingredients, icons, materials
TOOLS = [
    ("Road_1x", RMBuildableRoad, holo_road, "Road (1 wide)", "A road one foundation wide, with asphalt on top and concrete sides. Click the start, click the end: it curves like a vehicle path, snaps to road ends and foundation edges, and the mouse wheel turns it. Hold the snap key and aim at a built road (or an intersection curb) to build a road right beside it, edge to edge and flush, in one click. Lines are set in the customizer (Road Lines). Free to build.",
     {"m_width_in_foundations": 1}, [], road_icons, {"m_top_material": mat_asphalt, "m_body_material": mat_concrete}),
    ("Road_2x", RMBuildableRoad, holo_road, "Road (2 wide)", "A road two foundations wide, with asphalt on top and concrete sides. Click the start, click the end: it curves like a vehicle path, snaps to road ends and foundation edges, and the mouse wheel turns it. Hold the snap key and aim at a built road (or an intersection curb) to build a road right beside it, edge to edge and flush, in one click. Lines are set in the customizer (Road Lines). Free to build.",
     {"m_width_in_foundations": 2}, [], road_icons, {"m_top_material": mat_asphalt, "m_body_material": mat_concrete}),
    ("Road_3x", RMBuildableRoad, holo_road, "Road (3 wide)", "A road three foundations wide, with asphalt on top and concrete sides. Click the start, click the end: it curves like a vehicle path, snaps to road ends and foundation edges, and the mouse wheel turns it. Hold the snap key and aim at a built road (or an intersection curb) to build a road right beside it, edge to edge and flush, in one click. Lines are set in the customizer (Road Lines). Free to build.",
     {"m_width_in_foundations": 3}, [], road_icons, {"m_top_material": mat_asphalt, "m_body_material": mat_concrete}),
    ("Road_4x", RMBuildableRoad, holo_road, "Road (4 wide)", "A road four foundations wide, with asphalt on top and concrete sides. Click the start, click the end: it curves like a vehicle path, snaps to road ends and foundation edges, and the mouse wheel turns it. Hold the snap key and aim at a built road (or an intersection curb) to build a road right beside it, edge to edge and flush, in one click. Lines are set in the customizer (Road Lines). Free to build.",
     {"m_width_in_foundations": 4}, [], road_icons, {"m_top_material": mat_asphalt, "m_body_material": mat_concrete}),
    ("RoadCut", RMBuildableRoadTool, holo_cut, "Cut Road", "Aim at a road: the orange outline shows where it is cut. Click to split it there into two roads that keep its curve, lines and paint; train tracks and vehicle paths on it are cut too. Build mode \"Cut also fences\" also cuts the road's own fences at the same place (white outlines). Free.",
     {}, [], road_icons, {}),
    ("RoadSplit", RMBuildableRoadTool, holo_split, "Split Road", "Aim across a road 2 or more wide: the cyan line shows where it is split lengthwise, in steps of 8 m. Click to replace it with two roads side by side that keep its curve, lines and paint (a 2 wide road becomes two 1 wide roads, a 4 wide one for example 1 + 3). Free.",
     {}, [], road_icons, {}),
    ("RoadJoin", RMBuildableRoadTool, holo_join, "Join Road", "The opposite of Cut Road. Aim near where two roads meet end to end: the green outline shows the joined road; click to replace the two with one road along both, with the aimed road's lanes, material and paint. They must be the same width, continue smoothly into each other and follow the one curve a single road would have (two pieces of a cut road always do; a straight piece and a curved one cannot be joined), and the joined road must not be too long. Markings, paths, tracks and fences stay where they are. Free.",
     {}, [], road_icons, {}),
    ("RoadMerge", RMBuildableRoadTool, holo_merge, "Merge Road", "The opposite of Split Road. Click a road to pick it (white outline), then click the road right beside it: edge to edge, at the same height, starting and ending together. The green outline shows the merged road; click to replace the two with one road of their combined width (at most 4 wide), with the first road's lanes, material and paint (side lines on if either had them). Markings, paths, tracks and fences stay where they are. Click the picked road again to drop it. Free.",
     {}, [], road_icons, {}),
    ("RoadAdjust", RMBuildableRoadTool, holo_adjust, "Adjust Curvature", "Orange circles mark the road ends near you. Click one to grab that end (and the end of the road joined to it, when two roads meet end to end), then move it like a road end: the mouse wheel turns it, the snap key snaps it to a free road end, hold H to nudge it. The roads follow as holograms; click again to replace them with the new curves, keeping their other ends, lines and paint. Ends joined to an intersection stay. Free.",
     {}, [], road_icons, {}),
    ("FenceCut", RMBuildableRoadTool, holo_fence_cut, "Cut Fence", "Aim at a road fence or fence wall: the orange outline shows where it is cut. Click to split it there into two that keep its course, side, material and paint. Hold the snap key near a road end (for example where the road was cut) to snap the cut in line with it. Nothing else is cut. Free.",
     {}, [], fence_icons, {}),
    ("FenceJoin", RMBuildableRoadTool, holo_fence_join, "Join Fence", "The opposite of Cut Fence. Aim near where two road fences or fence walls meet end to end: the green outline shows the joined fence; click to replace the two with one fence along both, with the aimed fence's side, material and paint. They must be the same kind (and look), have the road on the same side, continue smoothly into each other and follow the one curve a single fence would have (two pieces of a cut fence always do; a straight piece and a curved one cannot be joined). Any length up to 1 km. Free.",
     {}, [], fence_icons, {}),
    ("Fence_GuardRail", RMBuildableFence, holo_fence, "Guard Rail", "Metal guard rail. Aim at a road to follow its edge. Hold the snap key and aim at another road fence to replace it. Cost is per 4 m.",
     {"m_fence_type": "GUARD_RAIL"}, [(Desc_IronPlate, 1), (Desc_IronRod, 1)], fence_icons, {"m_main_material": mat_metal}),
    ("Fence_Jersey", RMBuildableFence, holo_fence, "Cement Blocks", "Concrete road barrier. Aim at a road to follow its edge. Hold the snap key and aim at another road fence to replace it. Cost is per 4 m.",
     {"m_fence_type": "JERSEY_BARRIER"}, [(Desc_Cement, 3)], fence_icons, {"m_main_material": mat_concrete}),
    ("Fence_Grid", RMBuildableFence, holo_fence, "Metal Grid Fence", "Metal grid fence. Aim at a road to follow its edge. Hold the snap key and aim at another road fence to replace it. Cost is per 4 m.",
     {"m_fence_type": "GRID_FENCE"}, [(Desc_IronRod, 1), (Desc_Wire, 2)], fence_icons, {"m_main_material": mat_metal, "m_panel_material": mat_grid}),
    ("Fence_GridTall", RMBuildableFence, holo_fence, "Steel Mesh Fence Wall", "4 m steel frame with a diamond wire mesh. Aim at a road to follow its edge (centred on the edge), or at an intersection to follow its curb; aim at the upper or lower half of a fence wall to stack another above or below it, or hold the snap key to replace it. Cost is per 4 m.",
     {"m_fence_type": "GRID_FENCE_TALL", "m_edge_inset": 0.0}, [(Desc_SteelBeam, 1)], fence_icons, {"m_main_material": mat_metal, "m_panel_material": mat_grid}),
    ("FenceWall_Concrete", RMBuildableFence, holo_fence, "Concrete Fence Wall", "4 m concrete wall, 0.5 m thick. Aim at a road to follow its edge (centred on the edge), or at an intersection to follow its curb; aim at the upper or lower half of a fence wall to stack another above or below it. Can be painted. Cost is per 4 m.",
     {"m_fence_type": "CONCRETE_WALL", "m_edge_inset": 0.0}, [(Desc_Cement, 1)], fence_icons, {"m_main_material": mat_concrete}),
    ("FenceWall_Ficsit", RMBuildableFence, holo_fence, "FICSIT Fence Wall", "4 m FICSIT wall, 0.5 m thick. Aim at a road to follow its edge (centred on the edge), or at an intersection to follow its curb; aim at the upper or lower half of a fence wall to stack another above or below it. Can be painted; the customizer's Material tab switches between concrete, FICSIT and steel. Cost is per 4 m.",
     {"m_fence_type": "FICSIT_WALL", "m_edge_inset": 0.0}, [(Desc_IronPlate, 1)], fence_icons, {"m_main_material": mat_concrete}),
    ("FenceWall_Steel", RMBuildableFence, holo_fence, "Steel Fence Wall", "4 m steel wall, 0.5 m thick. Aim at a road to follow its edge (centred on the edge), or at an intersection to follow its curb; aim at the upper or lower half of a fence wall to stack another above or below it. Can be painted; the customizer's Material tab switches between concrete, FICSIT and steel. Cost is per 4 m.",
     {"m_fence_type": "STEEL_WALL", "m_edge_inset": 0.0}, [(Desc_SteelBeam, 1)], fence_icons, {"m_main_material": mat_concrete}),
    ("SignGantry", RMBuildableGantry, holo_gantry, "Sign Gantry", "Two steel columns on the fence lines of a road and a beam across it, at least 8 m above the road. Aim at a road and click to put it across, then the mouse sets the height of the beam (every 2 m) and a second click builds it. Aim a sign at the beam or a column to put it flat on that face, every 2 m (scroll turns it 45 degrees). Cost is per 8 m of steel.",
     {}, [(Desc_SteelBeam, 1), (Desc_SteelPipe, 1)], road_icons, {"m_main_material": mat_metal}),
    ("LaneArrow", RMBuildableMarking, holo_marking, "Lane Arrow", "Paints an arrow on a lane of a road. Aim at a road: it snaps to the middle of the lane you aim at and points the way you are looking along the road. The mouse wheel picks the arrow (straight, left, right, straight or left, straight or right, left or right). It takes the colour of the road's lane lines (the cyberpunk road's glow too); repaint it with the paint gun, or change its look in the customizer's Material tab. Dismantle it on its own or with the road. Free to build.",
     {"m_kind": "ARROW"}, [], road_icons, {}),
    ("LaneNumber", RMBuildableMarking, holo_marking, "Lane Number", "Paints a single digit (0 to 9) on a lane of a road, for speed limits and lane numbers. Aim at a road: it snaps to the middle of the lane you aim at and reads upright for traffic driving the way you are looking along the road. The mouse wheel picks the digit. It takes the colour of the road's lane lines (the cyberpunk road's glow too); repaint it with the paint gun, or change its look in the customizer's Material tab. Dismantle it on its own or with the road. Free to build.",
     {"m_kind": "DIGIT"}, [], road_icons, {}),
    ("RoadIntersection", RMBuildableIntersection, holo_x, "Road Intersection", "Click free road ends (or free intersection ends) to pick them, up to 6, then click away from them to build: they are joined with curved curbs. Click a picked end again to drop it. Side lines are set in the customizer (Road Lines > Sidelines). Free to build.",
     {}, [], road_icons, {"m_top_material": mat_asphalt, "m_body_material": mat_concrete}),
]

MENU_ORDER = {"RoadCut": 1, "RoadJoin": 2, "RoadSplit": 3, "RoadMerge": 4, "RoadAdjust": 5, "FenceCut": 3, "FenceJoin": 4, "Road_1x": 1, "Road_2x": 2, "Road_3x": 3, "Road_4x": 4, "RoadIntersection": 5,
              "Fence_GuardRail": 6, "Fence_Jersey": 7, "Fence_Grid": 8, "SignGantry": 9,
              "LaneArrow": 1, "LaneNumber": 2, "Fence_GridTall": 1, "FenceWall_Concrete": 2, "FenceWall_Ficsit": 3, "FenceWall_Steel": 4}

FenceEnum = getattr(unreal, "RMFenceType", None)
MarkingKindEnum = getattr(unreal, "RMMarkingKind", None)

# Light default swatch for roads (lines would otherwise start FICSIT orange / grey).
light_swatch = None
for n in ("SwatchDesc_Concrete", "SwatchDesc_White", "SwatchDesc_Unpainted"):
    light_swatch = load_game_class(n)
    if light_swatch:
        log("  default road swatch: " + n)
        break
# Import every PNG in Tools/Icons (build menu icons and Road Lines icons).
import os
ICON_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Icons")
tasks = []
for f in sorted(os.listdir(ICON_DIR)) if os.path.isdir(ICON_DIR) else []:
    if f.lower().endswith(".png"):
        t = unreal.AssetImportTask()
        t.set_editor_property("filename", os.path.join(ICON_DIR, f))
        t.set_editor_property("destination_path", ROOT + "/Icons")
        t.set_editor_property("replace_existing", True)
        t.set_editor_property("automated", True)
        t.set_editor_property("save", True)
        tasks.append(t)
if tasks:
    asset_tools.import_asset_tasks(tasks)
    log("  imported %d icons" % len(tasks))

# Road Tools tab icon: the game's Vehicles tab brush (same size / look) with our glyph.
if road_tools_cat:
    try:
        glyph = unreal.load_asset(ROOT + "/Icons/T_Icon_RoadTools.T_Icon_RoadTools")
        brush = getp(unreal.get_default_object(VehicleCat), "m_category_icon", "mCategoryIcon")
        if glyph and brush is not None:
            brush.set_editor_property("resource_object", glyph)
            rtc_bp = unreal.load_asset(ROOT + "/Buildables/BC_RoadTools.BC_RoadTools")
            setp(cdo(rtc_bp), brush, "m_category_icon", "mCategoryIcon")
            finish(rtc_bp)
            log("  Road Tools tab icon set")
        else:
            log("  ! Road Tools tab icon: glyph or game brush missing")
    except Exception as e:
        log("  ! Road Tools tab icon: %s" % e)



def tool_icon(name):
    name = name.replace("Medium", "")  # medium markings use the small one's icon
    try:
        return unreal.load_asset("%s/Icons/T_Icon_%s.T_Icon_%s" % (ROOT, name, name))
    except Exception:
        return None


recipes = []
tool_recipe_classes = {}
tool_classes = {}

for (name, parent, holo, display, desc, props, cost, icons, mats) in TOOLS:
    log("== " + name + " ==")
    b = make_bp("Buildables", "Build_" + name, parent)
    c = cdo(b)
    setp(c, holo.generated_class(), "m_hologram_class", "mHologramClass")
    setp(c, text(display), "m_display_name", "mDisplayName")
    setp(c, text(desc), "m_description", "mDescription")
    for k, v in props.items():
        if k == "m_fence_type" and FenceEnum is not None:
            v = getattr(FenceEnum, v, v)
        if k == "m_kind" and MarkingKindEnum is not None:
            v = getattr(MarkingKindEnum, v, v)
        setp(c, v, k)
    for k, m in mats.items():
        if m:
            setp(c, m, k)
    if parent in (RMBuildableRoad, RMBuildableIntersection, RMBuildableMarking):
        setp(c, True, "allow_coloring", "m_allow_coloring", "mAllowColoring")
        setp(c, None, "m_default_swatch_customization_override", "mDefaultSwatchCustomizationOverride")
    if name in FENCE_WALLS:
        # Paintable like the game's concrete wall, with the same swatch group and default swatch.
        setp(c, True, "allow_coloring", "m_allow_coloring", "mAllowColoring")
        # Swatch group and default swatch of the matching game wall (FICSIT walls start in the
        # FICSIT factory colours, concrete walls in concrete).
        vanilla = {"FenceWall_Ficsit": VanillaFicsitWall, "FenceWall_Steel": VanillaSteelWall}.get(name) or VanillaConcreteWall
        if vanilla:
            vw = unreal.get_default_object(vanilla)
            for prop in ("m_swatch_group", "m_default_swatch_customization_override"):
                v = getp(vw, prop)
                log("  %s: game wall %s %s = %s" % (name, vanilla.get_name(), prop, v))
                if v or prop == "m_default_swatch_customization_override":
                    setp(c, v, prop)
        elif light_swatch:
            setp(c, light_swatch, "m_default_swatch_customization_override")
        lay = WALL_LAYOUTS.get(name)
        if lay:
            for prop, val in zip(("m_wall_u_centre", "m_wall_u_per_metre", "m_wall_v_top", "m_wall_v_per_metre", "m_wall_mirror"), lay):
                setp(c, val, prop)
    finish(b)
    if name in FENCE_WALLS:
        fence_wall_classes[name] = b.generated_class()

    d = make_bp("Buildables", "Desc_" + name, BuildingDescriptor)
    dc = cdo(d)
    setp(dc, b.generated_class(), "m_buildable_class", "mBuildableClass")
    setp(dc, text(display), "m_display_name", "mDisplayName")
    setp(dc, text(desc), "m_description", "mDescription")
    # Build menu order: the order of TOOLS (roads 1 to 4 wide first).
    setp(dc, float(MENU_ORDER.get(name, 50)), "m_menu_priority", "mMenuPriority")
    tool_subcat = {"road": edit_road_subcat, "fence": edit_fence_subcat}.get(ROAD_TOOLS_MENU.get(name))
    if name in MARKING_TOOLS:
        # No build menu entry: the Road Lines options paint it. Clear what an earlier run set.
        setp(dc, None, "m_category", "mCategory", "m_build_menu_category")
        setp(dc, [], "m_sub_categories", "mSubCategories")
    elif tool_subcat and road_tools_cat:
        setp(dc, road_tools_cat, "m_category", "mCategory", "m_build_menu_category")
        setp(dc, [tool_subcat], "m_sub_categories", "mSubCategories")
    elif name in WALLS_MENU and WallsCat and wall_subcat:
        setp(dc, WallsCat, "m_category", "mCategory", "m_build_menu_category")
        setp(dc, [wall_subcat], "m_sub_categories", "mSubCategories")
    else:
        if VehicleCat:
            setp(dc, VehicleCat, "m_category", "mCategory", "m_build_menu_category")
        if subcat or VehiclePathSubCat:
            setp(dc, [subcat or VehiclePathSubCat], "m_sub_categories", "mSubCategories")
    own = tool_icon(name)  # stylised isometric icon from Tools/Icons, else a borrowed game icon
    small_icon = own or icons[0]
    big_icon = own or icons[1]
    if small_icon:
        setp(dc, small_icon, "m_small_icon", "mSmallIcon")
    if big_icon:
        setp(dc, big_icon, "m_persistent_big_icon", "mPersistentBigIcon", "m_big_icon")
    finish(d)

    r = make_bp("Unlocks", "Recipe_" + name, Recipe)
    rc = cdo(r)
    setp(rc, text(display), "m_display_name", "mDisplayName")
    setp(rc, [item_amount(d.generated_class(), 1)], "m_product", "mProduct")
    setp(rc, [item_amount(cls, amt) for cls, amt in cost if cls], "m_ingredients", "mIngredients")
    if BuildGun:
        ok = setp(rc, [BuildGun], "m_produced_in", "mProducedIn")
        if not ok:
            setp(rc, [unreal.SoftClassPath(BuildGun.get_path_name())], "m_produced_in", "mProducedIn")
    finish(r)
    tool_recipe_classes[name] = r.generated_class()
    tool_classes[name] = b.generated_class()
    if name in FENCE_WALLS:
        fence_wall_recipes[name] = r.generated_class()
    if name in WALLS_MENU:
        # Fence walls are never in the Road Maker milestone: FICSIT unlocks with Base Building,
        # concrete and steel with the game's concrete / steel wall materials (AWESOME Shop), the
        # steel mesh wall with Tier 3 Basic Steel Production.
        if name == "FenceWall_Ficsit" and base_building:
            wall_recipes.append(r.generated_class())
        if name == "Fence_GridTall" and basic_steel:
            recipe_unlocked_by[r.generated_class()] = basic_steel
    elif name not in MARKING_TOOLS:
        recipes.append(r.generated_class())

# ---------------------------------------------------------------------------------------------
# Customizer: "Road Lines" tab with Sidelines and Lanes
# ---------------------------------------------------------------------------------------------
log("== Road Lines (customizer) ==")
def icon(name):
    try:
        return unreal.load_asset("%s/Icons/T_RoadLines_%s.T_RoadLines_%s" % (ROOT, name, name))
    except Exception:
        return None

# Flat brush icon for the tab: the game's own UI brush / paint icon. Only UI textures count
# (names with Icon / UI / TXUI), never world textures like "TX_Toothbrush".
brush_tex = None
candidates = []
for a in game_assets():
    n = str(a.asset_name)
    low = n.lower()
    if str(a.asset_class_path.asset_name) != "Texture2D":
        continue
    if ("brush" in low or "paint" in low or "customiz" in low) and "tooth" not in low and (
            "icon" in low or "ui" in low.split("_") or low.startswith("txui") or "/interface/" in str(a.package_name).lower()):
        candidates.append(a)
candidates.sort(key=lambda a: (0 if "brush" in str(a.asset_name).lower() else 1, len(str(a.asset_name))))
log("  tab icon candidates: " + ", ".join(str(c.asset_name) for c in candidates[:15]))
if candidates:
    brush_tex = candidates[0].get_asset()
    log("  tab icon: " + str(candidates[0].asset_name))
else:
    brush_tex = icon("Lanes2Dashed")
    log("  tab icon: no brush texture found, using a road icon")

CustCategory = native_class("FGCustomizerCategory")
CustSubCategory = native_class("FGCustomizerSubCategory")
# The customizer menu updates its tiles per tab type; a tab derived from the game's Materials
# category (CC_Material) gets material-style tiles (with icons) while keeping its own name and icon.
MaterialTab = load_game_class("CC_Material")
log("  tab parent: " + ("CC_Material" if MaterialTab else "FGCustomizerCategory (CC_Material not found)"))
cat = make_bp("Customization", "CustCat_RoadLines", MaterialTab or CustCategory)
if MaterialTab:
    try:
        unreal.BlueprintEditorLibrary.reparent_blueprint(cat, MaterialTab)
    except Exception as e:
        log("  ! tab reparent failed: %s" % e)
setp(cdo(cat), text("Road Lines"), "m_display_name", "mDisplayName")
setp(cdo(cat), 50.0, "m_menu_priority", "mMenuPriority")
if brush_tex:
    brush = unreal.SlateBrush()
    brush.set_editor_property("resource_object", brush_tex)
    setp(cdo(cat), brush, "m_category_icon", "mCategoryIcon")
finish(cat)

subcats = {}
for i, (key, display) in enumerate((("Sidelines", "Sidelines"), ("Lanes", "Lanes"), ("Markings", "Markings"))):
    sc = make_bp("Customization", "CustSub_RoadLines_" + key, CustSubCategory)
    setp(cdo(sc), text(display), "m_display_name", "mDisplayName")
    setp(cdo(sc), float(i), "m_menu_priority", "mMenuPriority")
    finish(sc)
    subcats[key] = sc.generated_class()

# Produced-in for customization recipes: copy it from one of the game's own.
cust_produced_in = None
for a in game_assets():
    n = str(a.asset_name)
    if n.startswith("Recipe_Swatch") or n.startswith("Recipe_Pattern"):
        try:
            d = unreal.get_default_object(a.get_asset().generated_class())
            cust_produced_in = getp(d, "m_produced_in", "mProducedIn")
            log("  produced-in copied from " + n)
            break
        except Exception:
            continue

LineDesc = native_class("RMRoadLineDescriptor")
CustRecipe = native_class("FGCustomizationRecipe")
OPTIONS = [
    # key, subcategory, display, side-line option?, side lines, lanes, continuous
    ("SidesOff", "Sidelines", "Sidelines off", True, False, 0, False),
    ("SidesOn", "Sidelines", "Sidelines on", True, True, 0, False),
    ("Lanes1", "Lanes", "1 lane (no lines)", False, True, 1, False),
    ("Lanes2Dashed", "Lanes", "2 lanes, dashed", False, True, 2, False),
    ("Lanes2Solid", "Lanes", "2 lanes, continuous", False, True, 2, True),
    ("Lanes3Dashed", "Lanes", "3 lanes, dashed", False, True, 3, False),
    ("Lanes3Solid", "Lanes", "3 lanes, continuous", False, True, 3, True),
    ("Lanes4Dashed", "Lanes", "4 lanes, dashed", False, True, 4, False),
    ("Lanes4Solid", "Lanes", "4 lanes, continuous", False, True, 4, True),
]
for i, (key, sub, display, is_side, sides, lanes, continuous) in enumerate(OPTIONS):
    d = make_bp("Customization", "RoadLines_" + key, LineDesc)
    c = cdo(d)
    setp(c, text(display), "m_display_name", "mDisplayName")
    setp(c, text("Road Lines: " + display + (". Aim at a road or intersection and apply." if is_side else ". Aim at a road and apply.")), "m_description", "mDescription")
    setp(c, cat.generated_class(), "m_category", "mCategory")
    setp(c, [subcats[sub]], "m_sub_categories", "mSubCategories")
    setp(c, float(i), "m_menu_priority", "mMenuPriority")
    setp(c, [RMBuildableRoad, RMBuildableIntersection] if is_side else [RMBuildableRoad], "m_valid_buildables", "mValidBuildables")
    setp(c, 9100 + i, "id", "ID")
    tex = icon(key)
    if tex:
        setp(c, tex, "m_icon", "mIcon")
        setp(c, tex, "m_small_icon", "mSmallIcon")
        setp(c, tex, "m_persistent_big_icon", "mPersistentBigIcon")
    setp(c, is_side, "m_is_side_line_option", "mIsSideLineOption")
    # Not a physical item (keeps it out of item lists such as creative mode's "give items").
    rf = getattr(unreal, "ResourceForm", None)
    if rf is not None and hasattr(rf, "RF_INVALID"):
        setp(c, rf.RF_INVALID, "m_form", "mForm")
    setp(c, sides, "m_side_lines", "mSideLines")
    setp(c, max(lanes, 1), "m_lanes", "mLanes")
    setp(c, continuous, "m_continuous", "mContinuous")
    finish(d)

    r = make_bp("Customization", "Recipe_RoadLines_" + key, CustRecipe)
    rc = cdo(r)
    setp(rc, text(display), "m_display_name", "mDisplayName")
    setp(rc, d.generated_class(), "m_customization_desc", "mCustomizationDesc")
    setp(rc, [item_amount(d.generated_class(), 1)], "m_product", "mProduct")
    if cust_produced_in:
        setp(rc, cust_produced_in, "m_produced_in", "mProducedIn")
    finish(r)
    recipes.append(r.generated_class())


# Markings (arrows, digits): picked here, painted on the aimed lane with the click, wheel = variant.
MARKING_OPTIONS = [
    # key, icon, display, tool entry (the buildable), kind, size
    ("MarkArrow", "MarkArrow", "Lane Arrow", "LaneArrow", "ARROW", 1.0, 9130),
    ("MarkNumber", "MarkNumber", "Lane Number", "LaneNumber", "DIGIT", 1.0, 9131),
]
for i, (key, icon_name, display, tool, kind, size, ident) in enumerate(MARKING_OPTIONS):
    d = make_bp("Customization", "RoadLines_" + key, LineDesc)
    c = cdo(d)
    setp(c, text(display), "m_display_name", "mDisplayName")
    what = "arrow" if kind == "ARROW" else "single digit (0 to 9)"
    big = ""
    setp(c, text("Paints a " + what + " on a lane of a road. Aim at a road: the ghost shows it in the middle of the lane you aim at, pointing the way you look along the road. The mouse wheel picks the " + ("arrow" if kind == "ARROW" else "digit") + ", the click paints it." + big + " It takes the colour of the road's lane lines (the cyberpunk road's glow too); repaint it with a colour, or change its look in the Material tab. Dismantle it like any building."), "m_description", "mDescription")
    setp(c, cat.generated_class(), "m_category", "mCategory")
    setp(c, [subcats["Markings"]], "m_sub_categories", "mSubCategories")
    setp(c, float(i), "m_menu_priority", "mMenuPriority")
    setp(c, [RMBuildableRoad], "m_valid_buildables", "mValidBuildables")
    setp(c, ident, "id", "ID")
    tex = icon(icon_name)
    if tex:
        setp(c, tex, "m_icon", "mIcon")
        setp(c, tex, "m_small_icon", "mSmallIcon")
        setp(c, tex, "m_persistent_big_icon", "mPersistentBigIcon")
    rf = getattr(unreal, "ResourceForm", None)
    if rf is not None and hasattr(rf, "RF_INVALID"):
        setp(c, rf.RF_INVALID, "m_form", "mForm")
    setp(c, True, "m_is_marking", "mIsMarking")
    setp(c, getattr(MarkingKindEnum, kind, kind) if MarkingKindEnum is not None else kind, "m_marking_kind", "mMarkingKind")
    setp(c, size, "m_marking_size", "mMarkingSize")
    setp(c, tool_classes.get(tool), "m_marking_class", "mMarkingClass")
    setp(c, tool_recipe_classes.get(tool), "m_marking_recipe", "mMarkingRecipe")
    finish(d)

    r = make_bp("Customization", "Recipe_RoadLines_" + key, CustRecipe)
    rc = cdo(r)
    setp(rc, text(display), "m_display_name", "mDisplayName")
    setp(rc, d.generated_class(), "m_customization_desc", "mCustomizationDesc")
    setp(rc, [item_amount(d.generated_class(), 1)], "m_product", "mProduct")
    if cust_produced_in:
        setp(rc, cust_produced_in, "m_produced_in", "mProducedIn")
    finish(r)
    recipes.append(r.generated_class())


# ---------------------------------------------------------------------------------------------
# Customizer Material tab: "Roads" section with Asphalt / Concrete / Cyberpunk
# ---------------------------------------------------------------------------------------------
log("== Road materials (customizer) ==")
MaterialDescBase = native_class("FGFactoryCustomizationDescriptor_Material", "FGFactoryCustomizationDescriptorMaterial")
roads_sub = make_bp("Customization", "CustSub_Material_Roads", CustSubCategory)
setp(cdo(roads_sub), text("Roads"), "m_display_name", "mDisplayName")
setp(cdo(roads_sub), 50.0, "m_menu_priority", "mMenuPriority")
finish(roads_sub)
road_material_descs = []
ROAD_MATERIALS = [
    ("Asphalt", "Asphalt", "Dark asphalt with lane markings. The default look of roads and intersections.", 9200),
    ("Concrete", "Concrete Blocks", "Light concrete blocks. Lane markings stay in the road's line colours.", 9201),
    ("Cyber", "Cyberpunk", "Matte plastic with circuit-board grooves in the primary colour. The lane lines glow in the secondary colour.", 9202),
]
if MaterialDescBase and MaterialTab:
    for i, (key, display, desc, ident) in enumerate(ROAD_MATERIALS):
        md = make_bp("Customization", "MaterialDesc_Road_" + key, MaterialDescBase)
        mc = cdo(md)
        setp(mc, text(display), "m_display_name", "mDisplayName")
        setp(mc, text(desc), "m_description", "mDescription")
        setp(mc, MaterialTab, "m_category", "mCategory")
        setp(mc, [roads_sub.generated_class()], "m_sub_categories", "mSubCategories")
        setp(mc, float(i), "m_menu_priority", "mMenuPriority")
        setp(mc, [RMBuildableRoad, RMBuildableIntersection, RMBuildableMarking], "m_valid_buildables", "mValidBuildables")
        setp(mc, ident, "id", "ID")
        try:
            tex = unreal.load_asset("%s/Icons/T_Icon_RoadMat_%s.T_Icon_RoadMat_%s" % (ROOT, key, key))
        except Exception:
            tex = None
        if tex:
            for prop in (("m_icon", "mIcon"), ("m_small_icon", "mSmallIcon"), ("m_persistent_big_icon", "mPersistentBigIcon")):
                setp(mc, tex, *prop)
        rf = getattr(unreal, "ResourceForm", None)
        if rf is not None and hasattr(rf, "RF_INVALID"):
            setp(mc, rf.RF_INVALID, "m_form", "mForm")
        finish(md)

        r = make_bp("Customization", "Recipe_MaterialDesc_Road_" + key, CustRecipe)
        rc = cdo(r)
        setp(rc, text(display), "m_display_name", "mDisplayName")
        setp(rc, md.generated_class(), "m_customization_desc", "mCustomizationDesc")
        setp(rc, [item_amount(md.generated_class(), 1)], "m_product", "mProduct")
        if cust_produced_in:
            setp(rc, cust_produced_in, "m_produced_in", "mProducedIn")
        finish(r)
        recipes.append(r.generated_class())
        road_material_descs.append(md.generated_class())
else:
    log("  ! material descriptor class or CC_Material not found: road looks not in the Material tab")


def make_schematic(name, display, recipe_list):
    log("== " + name + " ==")
    bp = make_bp("Unlocks", name, Schematic)
    c = cdo(bp)
    setp(c, text(display), "m_display_name", "mDisplayName")
    setp(c, 1, "m_tech_tier", "mTechTier")
    st = getattr(unreal, "SchematicType", None)
    if st is not None:
        for n in ("EST_MILESTONE", "MILESTONE"):
            if hasattr(st, n):
                setp(c, getattr(st, n), "m_type", "mType")
                break
    if Desc_IronPlate:
        setp(c, [item_amount(Desc_IronPlate, 1)], "m_cost", "mCost")
    if UnlockRecipe:
        try:
            unlock = unreal.new_object(UnlockRecipe, outer=c)
            setp(unlock, recipe_list, "m_recipes", "mRecipes")
            setp(c, [unlock], "m_unlocks", "mUnlocks")
        except Exception as e:
            log("  ! could not create recipe unlock: %s" % e)
    finish(bp)
    return bp


sch = make_schematic("Schematic_RoadMaker", "Road Maker", recipes)

log("== Game world module ==")
RMWorldModule = native_class("RMGameWorldModule") or GameWorldModule
gwm = make_bp("", "RootGameWorld_RoadMaker", RMWorldModule)
# Existing asset made from the plain GameWorldModule: move it onto the Road Maker module class.
try:
    unreal.BlueprintEditorLibrary.reparent_blueprint(gwm, RMWorldModule)
    log("  parent class set")
except Exception as e:
    log("  ! reparent failed: %s" % e)
g = cdo(gwm)
setp(g, True, "root_module", "b_root_module", "bRootModule")
setp(g, [sch.generated_class()], "m_schematics", "mSchematics")
if light_swatch:
    setp(g, light_swatch, "m_default_road_swatch", "mDefaultRoadSwatch")
# Each fence wall knows the recipe of every look (cost of switching in the customizer).
look_recipes = [fence_wall_recipes.get(n) for n in ("FenceWall_Concrete", "FenceWall_Ficsit", "FenceWall_Steel")]
for wname, wcls in fence_wall_classes.items():
    wbp = unreal.load_asset("%s/Buildables/Build_%s.Build_%s" % (ROOT, wname, wname))
    look_classes = [fence_wall_classes.get(n) for n in ("FenceWall_Concrete", "FenceWall_Ficsit", "FenceWall_Steel")]
    if wbp and all(look_recipes):
        setp(cdo(wbp), look_recipes, "m_wall_look_recipes", "mWallLookRecipes")
        if all(look_classes):
            setp(cdo(wbp), look_classes, "m_wall_look_classes", "mWallLookClasses")
        finish(wbp)

# Road looks of the Material tab: descriptors, and the recipe of each road piece (cost of switching).
if road_material_descs:
    setp(g, road_material_descs, "m_road_material_descs", "mRoadMaterialDescs")
    rmap = {}
    for n in ("Road_1x", "Road_2x", "Road_3x", "Road_4x", "RoadIntersection"):
        if n in tool_classes and n in tool_recipe_classes:
            rmap[tool_classes[n]] = tool_recipe_classes[n]
    try:
        g.set_editor_property("m_road_buildable_recipes", rmap)
        log("  road materials: %d descriptors, %d road pieces" % (len(road_material_descs), len(rmap)))
    except Exception as e:
        log("  ! road buildable recipes: %s" % e)

# Concrete and steel fence walls unlock with the game's concrete / steel wall materials.
alongside = {}
for game_recipe, variant in (("Recipe_Material_Wall_Concrete", "FenceWall_Concrete"), ("Recipe_Material_Wall_Steel", "FenceWall_Steel")):
    gr = load_game_class(game_recipe)
    if gr and variant in fence_wall_recipes:
        alongside[gr] = fence_wall_recipes[variant]
        log("  %s unlocks with %s" % (variant, game_recipe))
try:
    g.set_editor_property("m_unlock_alongside", alongside)
except Exception as e:
    log("  ! unlock alongside: %s" % e)

# Customizer Material tab: each game wall material swaps any fence wall to the variant in it.
setp(g, list(fence_wall_classes.values()), "m_fence_wall_variants", "mFenceWallVariants")
mat_map = {}
for desc_name, variant in (("MaterialDesc_Wall_Concrete", "FenceWall_Concrete"), ("MaterialDesc_Wall_Orange", "FenceWall_Ficsit"),
                           ("MaterialDesc_Wall_Steel", "FenceWall_Steel")):
    dcls = load_game_class(desc_name)
    if dcls and variant in fence_wall_recipes:
        mat_map[dcls] = fence_wall_recipes[variant]
        log("  material %s -> Recipe_%s" % (desc_name, variant))
try:
    g.set_editor_property("m_fence_wall_material_recipes", mat_map)
except Exception as e:
    log("  ! fence wall material map: %s" % e)
try:
    g.set_editor_property("m_recipe_unlocked_by", recipe_unlocked_by)
    log("  steel mesh fence wall unlocks with %s" % (basic_steel.get_name() if basic_steel else "nothing"))
except Exception as e:
    log("  ! recipe unlocked by: %s" % e)
if base_building:
    setp(g, base_building, "m_extra_recipes_schematic", "mExtraRecipesSchematic")
    setp(g, wall_recipes, "m_extra_recipes", "mExtraRecipes")
    log("  fence wall recipes unlock with " + str(base_building.get_name()))
finish(gwm)

log("DONE")
_log.close()
