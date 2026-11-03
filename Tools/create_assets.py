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

holo_road = make_bp("Holograms", "Holo_Road", RMRoadHologram)
setp(cdo(holo_road), bm.generated_class(), "m_build_mode_straight", "mBuildModeStraight")
finish(holo_road)

holo_fence = make_bp("Holograms", "Holo_Fence", RMFenceHologram)
setp(cdo(holo_fence), bm.generated_class(), "m_build_mode_straight", "mBuildModeStraight")
finish(holo_fence)

holo_x = make_bp("Holograms", "Holo_Intersection", RMIntersectionHologram)
finish(holo_x)

holo_paint = make_bp("Holograms", "Holo_LanePaint", native_class("RMLanePaintHologram"))
finish(holo_paint)

log("== Sub-category ==")
subcat = None
if VehiclePathSubCat:
    subcat_bp = make_bp("Buildables", "SC_Roads", VehiclePathSubCat)
    setp(cdo(subcat_bp), text("Roads"), "m_display_name", "mDisplayName")
    setp(cdo(subcat_bp), 50.0, "m_menu_priority", "mMenuPriority")
    finish(subcat_bp)
    subcat = subcat_bp.generated_class()

# name, parent, hologram, display, description, props, recipe ingredients, icons, materials
TOOLS = [
    ("Road_1x", RMBuildableRoad, holo_road, "Road (1 wide)", "A road one foundation wide. Cost is per 8 m.",
     {"m_width_in_foundations": 1}, [(Desc_Cement, 4)], road_icons, {"m_top_material": mat_asphalt, "m_body_material": mat_concrete}),
    ("Road_2x", RMBuildableRoad, holo_road, "Road (2 wide)", "A road two foundations wide. Cost is per 8 m per foundation of width.",
     {"m_width_in_foundations": 2}, [(Desc_Cement, 4)], road_icons, {"m_top_material": mat_asphalt, "m_body_material": mat_concrete}),
    ("Road_3x", RMBuildableRoad, holo_road, "Road (3 wide)", "A road three foundations wide. Cost is per 8 m per foundation of width.",
     {"m_width_in_foundations": 3}, [(Desc_Cement, 4)], road_icons, {"m_top_material": mat_asphalt, "m_body_material": mat_concrete}),
    ("Fence_GuardRail", RMBuildableFence, holo_fence, "Guard Rail", "Metal guard rail. Aim at a road to follow its edge. Cost is per 4 m.",
     {"m_fence_type": "GUARD_RAIL"}, [(Desc_IronPlate, 1), (Desc_IronRod, 1)], fence_icons, {"m_main_material": mat_metal}),
    ("Fence_Jersey", RMBuildableFence, holo_fence, "Cement Blocks", "Concrete road barrier. Aim at a road to follow its edge. Cost is per 4 m.",
     {"m_fence_type": "JERSEY_BARRIER"}, [(Desc_Cement, 3)], fence_icons, {"m_main_material": mat_concrete}),
    ("Fence_Grid", RMBuildableFence, holo_fence, "Metal Grid Fence", "Metal grid fence. Aim at a road to follow its edge. Cost is per 4 m.",
     {"m_fence_type": "GRID_FENCE"}, [(Desc_IronRod, 1), (Desc_Wire, 2)], fence_icons, {"m_main_material": mat_metal, "m_panel_material": mat_grid}),
    ("RoadIntersection", RMBuildableIntersection, holo_x, "Road Intersection", "Aim between road ends: joins up to 6 nearby free ends. Cost is per connected road.",
     {}, [(Desc_Cement, 6)], road_icons, {"m_top_material": mat_asphalt, "m_body_material": mat_concrete}),
    ("LanePaint", native_class("RMBuildableLanePaint"), holo_paint, "Lane Paint",
     "Left-click a road to paint its lane markings. Right-click to choose 1, 2 or 3 lanes and side lines.",
     {}, [(Desc_IronPlate, 1)], road_icons, {}),
]

FenceEnum = getattr(unreal, "RMFenceType", None)

# Light default swatch for roads (lines would otherwise start FICSIT orange / grey).
light_swatch = None
for n in ("SwatchDesc_Concrete", "SwatchDesc_White", "SwatchDesc_Unpainted"):
    light_swatch = load_game_class(n)
    if light_swatch:
        log("  default road swatch: " + n)
        break
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
    if parent == RMBuildableRoad:
        setp(c, True, "allow_coloring", "m_allow_coloring", "mAllowColoring")
        setp(c, None, "m_default_swatch_customization_override", "mDefaultSwatchCustomizationOverride")
    finish(b)

    d = make_bp("Buildables", "Desc_" + name, BuildingDescriptor)
    dc = cdo(d)
    setp(dc, b.generated_class(), "m_buildable_class", "mBuildableClass")
    setp(dc, text(display), "m_display_name", "mDisplayName")
    setp(dc, text(desc), "m_description", "mDescription")
    if VehicleCat:
        setp(dc, VehicleCat, "m_category", "mCategory", "m_build_menu_category")
    if subcat or VehiclePathSubCat:
        setp(dc, [subcat or VehiclePathSubCat], "m_sub_categories", "mSubCategories")
    if icons[0]:
        setp(dc, icons[0], "m_small_icon", "mSmallIcon")
    if icons[1]:
        setp(dc, icons[1], "m_persistent_big_icon", "mPersistentBigIcon", "m_big_icon")
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
    log("  parent class: " + RMWorldModule.get_name())
except Exception as e:
    log("  ! reparent failed: %s" % e)
g = cdo(gwm)
setp(g, True, "root_module", "b_root_module", "bRootModule")
setp(g, [sch.generated_class()], "m_schematics", "mSchematics")
if light_swatch:
    setp(g, light_swatch, "m_default_road_swatch", "mDefaultRoadSwatch")
finish(gwm)

log("DONE")
_log.close()
