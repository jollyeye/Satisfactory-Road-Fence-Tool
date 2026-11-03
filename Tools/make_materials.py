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

# Without the editor window the asset registry may not have scanned the mod's folder yet.
unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous(["/RoadMaker"], True)


def load_bp(name):
    """Load a Blueprint by full object path; does not depend on the registry having indexed it."""
    for path in ("/RoadMaker/Buildables/%s.%s" % (name, name), "/RoadMaker/Buildables/" + name):
        try:
            obj = unreal.load_asset(path)
        except Exception:
            obj = None
        if obj:
            return obj
    return None


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
# Lane markings come from the mesh: UV1 = (metres across, metres along), UV2 = (road width m,
# side line inset m), UV3 = (lanes, side lines 0/1). Lanes 0 (intersections) = no markings.
MARKINGS_HLSL = r"""
float u = UV1.x; float v = UV1.y;
float W = UV2.x; float e = UV2.y;
float N = round(UV3.x);
float flags = round(UV3.y);              // 1 = side lines, 2 = continuous dividers
float sides = fmod(flags, 2.0);
float continuous = flags > 1.5 ? 1.0 : 0.0;
if (N < 0.5 || W < 0.5) return float2(0, 0);
float hw = 0.075;                       // 15 cm wide lines
float aa = max(fwidth(u), 0.002);
float side = 0;
float mid = 0;
if (sides > 0.5)
{
    float d = min(abs(u - e), abs(u - (W - e)));
    side = 1 - smoothstep(hw - aa, hw + aa, d);
}
float L = W - 2 * e;
if (N > 1.5 && L > 0.1)
{
    float t = (u - e) / L * N;
    float k = clamp(round(t), 1, N - 1);
    float d = abs(t - k) * L / N;
    float lineMask = 1 - smoothstep(hw - aa, hw + aa, d);
    float p = frac(v / 9.0) * 9.0;      // 3 m dash, 6 m gap
    float av = max(fwidth(v), 0.002);
    float dash = smoothstep(0, av, p) * (1 - smoothstep(3 - av, 3 + av, p));
    mid = lineMask * max(dash, continuous);
}
return float2(saturate(mid), saturate(side));
"""

asphalt = new_material("M_RoadAsphalt")
base = noise_blend(asphalt, (0.025, 0.025, 0.027), (0.06, 0.06, 0.062), 0.08, 0)

uv1 = node(asphalt, unreal.MaterialExpressionTextureCoordinate, -1100, 600, coordinate_index=1)
uv2 = node(asphalt, unreal.MaterialExpressionTextureCoordinate, -1100, 700, coordinate_index=2)
uv3 = node(asphalt, unreal.MaterialExpressionTextureCoordinate, -1100, 800, coordinate_index=3)
marks = node(asphalt, unreal.MaterialExpressionCustom, -800, 650)
marks.set_editor_property("code", MARKINGS_HLSL)
marks.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT2)
marks.set_editor_property("description", "LaneMarkings")
inputs = []
for name in ("UV1", "UV2", "UV3"):
    ci = unreal.CustomInput()
    ci.set_editor_property("input_name", name)
    inputs.append(ci)
marks.set_editor_property("inputs", inputs)
link(uv1, "", marks, "UV1")
link(uv2, "", marks, "UV2")
link(uv3, "", marks, "UV3")

# Customizer colours, pushed by ARMBuildableRoad as custom primitive data (24 = primary, 28 =
# secondary). The defaults (off-white) show when no colour has been pushed.
def cpd_color(name, index, y):
    p = node(asphalt, unreal.MaterialExpressionVectorParameter, -600, y,
             parameter_name=name, default_value=unreal.LinearColor(0.8, 0.8, 0.76, 1.0))
    p.set_editor_property("use_custom_primitive_data", True)
    p.set_editor_property("primitive_data_index", index)
    return p

primary = cpd_color("MiddleLineColor", 24, 900)
secondary = cpd_color("SideLineColor", 28, 1000)
mid_mask = node(asphalt, unreal.MaterialExpressionComponentMask, -550, 650, r=True, g=False, b=False, a=False)
side_mask = node(asphalt, unreal.MaterialExpressionComponentMask, -550, 750, r=False, g=True, b=False, a=False)
link(marks, "", mid_mask, "")
link(marks, "", side_mask, "")

with_sides = node(asphalt, unreal.MaterialExpressionLinearInterpolate, -350, 150)
link(base, "", with_sides, "A")
link(secondary, "", with_sides, "B")
link(side_mask, "", with_sides, "Alpha")
col = node(asphalt, unreal.MaterialExpressionLinearInterpolate, -200, 150)
link(with_sides, "", col, "A")
link(primary, "", col, "B")
link(mid_mask, "", col, "Alpha")
MEL.connect_material_property(col, "", MP.MP_BASE_COLOR)

any_line = node(asphalt, unreal.MaterialExpressionMax, -350, 450)
link(mid_mask, "", any_line, "A")
link(side_mask, "", any_line, "B")
rough = node(asphalt, unreal.MaterialExpressionLinearInterpolate, -200, 400)
link(const(asphalt, 0.85, -500, 350), "", rough, "A")
link(const(asphalt, 0.6, -500, 420), "", rough, "B")
link(any_line, "", rough, "Alpha")
MEL.connect_material_property(rough, "", MP.MP_ROUGHNESS)
finish(asphalt)
log("Asphalt with lane markings")

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
    bp = load_bp(name)
    if not bp:
        log("  ! missing /RoadMaker/Buildables/" + name)
        continue
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
