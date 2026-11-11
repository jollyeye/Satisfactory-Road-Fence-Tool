# Road Maker: creates every Blueprint asset the mod needs (all 7 tools).
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

holo_road = make_bp("Holograms", "Holo_Road", RMRoadHologram)
setp(cdo(holo_road), bm.generated_class(), "m_build_mode_straight", "mBuildModeStraight")
setp(cdo(holo_road), bm_smooth.generated_class(), "m_build_mode_smooth", "mBuildModeSmooth")
finish(holo_road)

holo_fence = make_bp("Holograms", "Holo_Fence", RMFenceHologram)
setp(cdo(holo_fence), bm.generated_class(), "m_build_mode_straight", "mBuildModeStraight")
finish(holo_fence)

holo_x = make_bp("Holograms", "Holo_Intersection", RMIntersectionHologram)
finish(holo_x)

# Lane Paint moved to the customizer (Road Lines tab): remove its build-menu assets.
for old in ("Unlocks/Recipe_LanePaint", "Buildables/Desc_LanePaint", "Buildables/Build_LanePaint",
            "Holograms/Holo_LanePaint", "Holograms/BM_LaneMenu",
            "Holograms/BM_PickEnds"):
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
    ("Road_1x", RMBuildableRoad, holo_road, "Road (1 wide)", "A road one foundation wide, with asphalt on top and concrete sides. Click the start, click the end: it curves like a vehicle path, snaps to road ends and foundation edges, and the mouse wheel turns it. Lines are set in the customizer (Road Lines). Free to build.",
     {"m_width_in_foundations": 1}, [], road_icons, {"m_top_material": mat_asphalt, "m_body_material": mat_concrete}),
    ("Road_2x", RMBuildableRoad, holo_road, "Road (2 wide)", "A road two foundations wide, with asphalt on top and concrete sides. Click the start, click the end: it curves like a vehicle path, snaps to road ends and foundation edges, and the mouse wheel turns it. Lines are set in the customizer (Road Lines). Free to build.",
     {"m_width_in_foundations": 2}, [], road_icons, {"m_top_material": mat_asphalt, "m_body_material": mat_concrete}),
    ("Road_3x", RMBuildableRoad, holo_road, "Road (3 wide)", "A road three foundations wide, with asphalt on top and concrete sides. Click the start, click the end: it curves like a vehicle path, snaps to road ends and foundation edges, and the mouse wheel turns it. Lines are set in the customizer (Road Lines). Free to build.",
     {"m_width_in_foundations": 3}, [], road_icons, {"m_top_material": mat_asphalt, "m_body_material": mat_concrete}),
    ("Road_4x", RMBuildableRoad, holo_road, "Road (4 wide)", "A road four foundations wide, with asphalt on top and concrete sides. Click the start, click the end: it curves like a vehicle path, snaps to road ends and foundation edges, and the mouse wheel turns it. Lines are set in the customizer (Road Lines). Free to build.",
     {"m_width_in_foundations": 4}, [], road_icons, {"m_top_material": mat_asphalt, "m_body_material": mat_concrete}),
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
    ("RoadIntersection", RMBuildableIntersection, holo_x, "Road Intersection", "Click free road ends (or free intersection ends) to pick them, up to 6, then click away from them to build: they are joined with curved curbs. Click a picked end again to drop it. Side lines are set in the customizer (Road Lines > Sidelines). Free to build.",
     {}, [], road_icons, {"m_top_material": mat_asphalt, "m_body_material": mat_concrete}),
]

FenceEnum = getattr(unreal, "RMFenceType", None)

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



def tool_icon(name):
    try:
        return unreal.load_asset("%s/Icons/T_Icon_%s.T_Icon_%s" % (ROOT, name, name))
    except Exception:
        return None


recipes = []

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
        setp(c, v, k)
    for k, m in mats.items():
        if m:
            setp(c, m, k)
    if parent in (RMBuildableRoad, RMBuildableIntersection):
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
    if name in WALLS_MENU and WallsCat and wall_subcat:
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
    else:
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
for i, (key, display) in enumerate((("Sidelines", "Sidelines"), ("Lanes", "Lanes"))):
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
