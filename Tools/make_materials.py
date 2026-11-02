# Road Maker: creates simple tiling materials and assigns them to the buildables.
# The game's foundation materials sample texture atlases laid out for its own meshes, so on a
# procedural road they show the whole atlas (FICSIT tiles). These materials need no special UVs.
# Run inside the editor: UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file>
import unreal

LOG_PATH = r"D:\SFMods\materials-log.txt"
_log = open(LOG_PATH, "w", encoding="utf-8")


def log(msg):
    _log.write(msg + "\n")
    _log.flush()
    unreal.log("[RoadMaker] " + msg)


ROOT = "/RoadMaker/Materials"
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary
MP = unreal.MaterialProperty


def new_material(name):
    path = ROOT + "/" + name
    if eal.does_asset_exist(path):
        mat = eal.load_asset(path)
        MEL.delete_all_material_expressions(mat)
        log("Rebuilding " + path)
    else:
        mat = asset_tools.create_asset(name, ROOT, unreal.Material, unreal.MaterialFactoryNew())
        log("Created " + path)
    return mat


def node(mat, cls, x, y, **props):
    n = MEL.create_material_expression(mat, cls, x, y)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def const(mat, v, x, y):
    return node(mat, unreal.MaterialExpressionConstant, x, y, r=v)


def color(mat, r, g, b, x, y):
    return node(mat, unreal.MaterialExpressionConstant3Vector, x, y, constant=unreal.LinearColor(r, g, b, 1.0))


def link(a, out, b, inp):
    MEL.connect_material_expressions(a, out, b, inp)


def noise_blend(mat, dark, light, scale, y):
    """Lerp between two colours with world-space noise: breaks up flat surfaces."""
    n = node(mat, unreal.MaterialExpressionNoise, -700, y, scale=scale, quality=1, levels=3,
             output_min=0.0, output_max=1.0, turbulence=True)
    a = color(mat, *dark, x=-700, y=y + 200)
    b = color(mat, *light, x=-700, y=y + 300)
    lerp = node(mat, unreal.MaterialExpressionLinearInterpolate, -400, y + 150)
    link(a, "", lerp, "A")
    link(b, "", lerp, "B")
    link(n, "", lerp, "Alpha")
    return lerp


def finish(mat):
    MEL.recompile_material(mat)
    eal.save_loaded_asset(mat, only_if_is_dirty=False)


# --- Asphalt: dark, rough, fine grain ------------------------------------------------------
asphalt = new_material("M_RoadAsphalt")
base = noise_blend(asphalt, (0.025, 0.025, 0.027), (0.06, 0.06, 0.062), 0.08, 0)
MEL.connect_material_property(base, "", MP.MP_BASE_COLOR)
MEL.connect_material_property(const(asphalt, 0.85, -300, 400), "", MP.MP_ROUGHNESS)
finish(asphalt)

# --- Concrete: light grey, slightly mottled --------------------------------------------------
concrete = new_material("M_RoadConcrete")
base = noise_blend(concrete, (0.32, 0.31, 0.29), (0.45, 0.44, 0.42), 0.02, 0)
MEL.connect_material_property(base, "", MP.MP_BASE_COLOR)
MEL.connect_material_property(const(concrete, 0.9, -300, 400), "", MP.MP_ROUGHNESS)
finish(concrete)

# --- Galvanised steel ------------------------------------------------------------------------
steel = new_material("M_RoadSteel")
base = noise_blend(steel, (0.45, 0.46, 0.47), (0.6, 0.61, 0.62), 0.05, 0)
MEL.connect_material_property(base, "", MP.MP_BASE_COLOR)
MEL.connect_material_property(const(steel, 1.0, -300, 400), "", MP.MP_METALLIC)
MEL.connect_material_property(const(steel, 0.45, -300, 500), "", MP.MP_ROUGHNESS)
finish(steel)

# --- Wire grid: masked lines from UVs (panel UVs are 1 per 2 m, so 20 cells = 10 cm) ---------
grid = new_material("M_RoadGridFence")
grid.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
grid.set_editor_property("two_sided", True)
grid.set_editor_property("opacity_mask_clip_value", 0.42)
uv = node(grid, unreal.MaterialExpressionTextureCoordinate, -1100, 0, u_tiling=20.0, v_tiling=20.0)
frac = node(grid, unreal.MaterialExpressionFrac, -900, 0)
link(uv, "", frac, "")
sub = node(grid, unreal.MaterialExpressionSubtract, -750, 0)
link(frac, "", sub, "A")
sub.set_editor_property("const_b", 0.5)
ab = node(grid, unreal.MaterialExpressionAbs, -600, 0)
link(sub, "", ab, "")
mr = node(grid, unreal.MaterialExpressionComponentMask, -450, -60, r=True, g=False, b=False, a=False)
mg = node(grid, unreal.MaterialExpressionComponentMask, -450, 60, r=False, g=True, b=False, a=False)
link(ab, "", mr, "")
link(ab, "", mg, "")
mx = node(grid, unreal.MaterialExpressionMax, -300, 0)
link(mr, "", mx, "A")
link(mg, "", mx, "B")
MEL.connect_material_property(mx, "", MP.MP_OPACITY_MASK)
MEL.connect_material_property(color(grid, 0.5, 0.51, 0.52, -300, 200), "", MP.MP_BASE_COLOR)
MEL.connect_material_property(const(grid, 1.0, -300, 300), "", MP.MP_METALLIC)
MEL.connect_material_property(const(grid, 0.5, -300, 400), "", MP.MP_ROUGHNESS)
finish(grid)

# --- Assign ----------------------------------------------------------------------------------
ASSIGN = {
    "Build_Road_1x": {"m_top_material": asphalt, "m_body_material": concrete},
    "Build_Road_2x": {"m_top_material": asphalt, "m_body_material": concrete},
    "Build_Road_3x": {"m_top_material": asphalt, "m_body_material": concrete},
    "Build_RoadIntersection": {"m_top_material": asphalt, "m_body_material": concrete},
    "Build_Fence_GuardRail": {"m_main_material": steel},
    "Build_Fence_Jersey": {"m_main_material": concrete},
    "Build_Fence_Grid": {"m_main_material": steel, "m_panel_material": grid},
}
for name, props in ASSIGN.items():
    path = "/RoadMaker/Buildables/" + name
    if not eal.does_asset_exist(path):
        log("  ! missing " + path)
        continue
    bp = eal.load_asset(path)
    d = unreal.get_default_object(bp.generated_class())
    for k, m in props.items():
        try:
            d.set_editor_property(k, m)
        except Exception as e:
            log("  ! %s.%s: %s" % (name, k, e))
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    eal.save_loaded_asset(bp, only_if_is_dirty=False)
    log("Assigned materials to " + name)

log("DONE")
_log.close()
