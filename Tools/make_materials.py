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


# --- The game's own foundation textures --------------------------------------------------------
# Atlases laid out for the foundation meshes (seen in the real textures, 1024x1024):
#   asphalt BC/N: top-left square = asphalt top, bottom-left square = concrete side
#   concrete BC/N: bottom-left square = plain concrete (top-left has panel seams)
# Each square covers 4 m on a foundation (checked in game). It does not tile, so it is mirrored every 4 m (no seams),
# in world space, so roads, intersections and neighbouring pieces line up.
FOUNDATION_TEX = "/Game/FactoryGame/Buildable/Building/Foundation/"
ASPHALT_BC = FOUNDATION_TEX + "AsphaltSet/Textures/TX_AsphaltFoundation_BC"
ASPHALT_N = FOUNDATION_TEX + "AsphaltSet/Textures/TX_AsphaltFoundation_N"
CONCRETE_BC = FOUNDATION_TEX + "ConcreteSet/Textures/TX_Concrete_BC"
CONCRETE_N = FOUNDATION_TEX + "ConcreteSet/Textures/TX_Concrete_N"
RECT_TOP_LEFT = (3.0 / 1024, 3.0 / 1024, 494.0 / 1024, 494.0 / 1024)        # min u, min v, size u, size v
RECT_BOTTOM_LEFT = (3.0 / 1024, 530.0 / 1024, 494.0 / 1024, 490.0 / 1024)
PERIOD_CM = 400.0  # one atlas square per 4 m (matched to foundations in game)


def load_tex(path):
    try:
        t = unreal.load_asset(path)
    except Exception as e:
        t = None
        log("  ! cannot load %s: %s" % (path, e))
    if t:
        log("  texture %s compression=%s srgb=%s" % (path, t.get_editor_property("compression_settings"), t.get_editor_property("srgb")))
    return t


def sampler_for(tex):
    cs = tex.get_editor_property("compression_settings")
    if cs == unreal.TextureCompressionSettings.TC_NORMALMAP:
        return unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL
    if not tex.get_editor_property("srgb"):
        return unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR
    return unreal.MaterialSamplerType.SAMPLERTYPE_COLOR


def custom(mat, code, out_type, inputs, x, y, desc):
    c = node(mat, unreal.MaterialExpressionCustom, x, y)
    c.set_editor_property("code", code)
    c.set_editor_property("output_type", out_type)
    c.set_editor_property("description", desc)
    lst = []
    for name in inputs:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", name)
        lst.append(ci)
    c.set_editor_property("inputs", lst)
    return c


# Planar world projection: top faces use XY; with Triplanar the dominant axis of the vertex normal
# picks XY, YZ or XZ (for the sides of the slab and the barriers).
ATLAS_UV_HLSL = r"""
float3 a = abs(N);
float2 p = WP.xy;
if (Tri > 0.5 && a.z < 0.7) { p = a.x > a.y ? WP.yz : WP.xz; }
float2 h = frac(p / (2.0 * Period));
float2 m = 1.0 - abs(h * 2.0 - 1.0);
return Rect.xy + m * Rect.zw;
"""
# Normal map in world space, for the same planar/triplanar projection as AtlasUV: mirrored cells
# flip the map's X/Y, Str boosts the (faint) detail, Flat blends back to the vertex normal (lines).
WORLD_NORMAL_HLSL = r"""
float3 a = abs(N);
float2 p = WP.xy;
float3 U = float3(1, 0, 0);
float3 V = float3(0, 1, 0);
float3 W = float3(0, 0, N.z >= 0 ? 1 : -1);
if (Tri > 0.5 && a.z < 0.7)
{
    if (a.x > a.y) { p = WP.yz; U = float3(0, 1, 0); V = float3(0, 0, 1); W = float3(N.x >= 0 ? 1 : -1, 0, 0); }
    else           { p = WP.xz; U = float3(1, 0, 0); V = float3(0, 0, 1); W = float3(0, N.y >= 0 ? 1 : -1, 0); }
}
float2 h = frac(p / (2.0 * Period));
float2 s = float2(h.x < 0.5 ? 1.0 : -1.0, h.y < 0.5 ? 1.0 : -1.0);
// Signs checked in game (the first guess lit bumps from the wrong side).
float3 n = normalize(-U * (T.x * s.x * Str) + V * (T.y * s.y * Str) + W * max(T.z, 0.2));
return normalize(lerp(n, normalize(N), Flat));
"""
NORMAL_STRENGTH = 2.5


def world_normal(mat, tex_n, rect, triplanar, flat, x, y):
    """Connects the atlas normal map (world space) to the material's Normal."""
    t = atlas_sample(mat, tex_n, rect, triplanar, x - 500, y)
    mat.set_editor_property("tangent_space_normal", False)
    wp = node(mat, unreal.MaterialExpressionWorldPosition, x - 300, y + 250)
    vn = node(mat, unreal.MaterialExpressionVertexNormalWS, x - 300, y + 330)
    wn = custom(mat, WORLD_NORMAL_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                ["T", "WP", "N", "Tri", "Period", "Flat", "Str"], x, y, "WorldNormal")
    link(t, "", wn, "T")
    link(wp, "", wn, "WP")
    link(vn, "", wn, "N")
    link(const(mat, 1.0 if triplanar else 0.0, x - 300, y + 410), "", wn, "Tri")
    link(const(mat, PERIOD_CM, x - 300, y + 480), "", wn, "Period")
    if flat is None:
        link(const(mat, 0.0, x - 300, y + 550), "", wn, "Flat")
    else:
        link(flat, "", wn, "Flat")
    link(const(mat, NORMAL_STRENGTH, x - 300, y + 620), "", wn, "Str")
    MEL.connect_material_property(wn, "", MP.MP_NORMAL)


def atlas_sample(mat, tex, rect, triplanar, x, y):
    """Texture sample of one atlas square, mirrored every 8 m in world space."""
    wp = node(mat, unreal.MaterialExpressionWorldPosition, x - 600, y)
    vn = node(mat, unreal.MaterialExpressionVertexNormalWS, x - 600, y + 100)
    uv = custom(mat, ATLAS_UV_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT2, ["WP", "N", "Rect", "Tri", "Period"], x - 350, y, "AtlasUV")
    link(wp, "", uv, "WP")
    link(vn, "", uv, "N")
    rc = node(mat, unreal.MaterialExpressionConstant4Vector, x - 600, y + 200, constant=unreal.LinearColor(*rect))
    link(rc, "", uv, "Rect")
    link(const(mat, 1.0 if triplanar else 0.0, x - 600, y + 300), "", uv, "Tri")
    link(const(mat, PERIOD_CM, x - 600, y + 380), "", uv, "Period")
    ts = node(mat, unreal.MaterialExpressionTextureSample, x - 120, y)
    ts.set_editor_property("texture", tex)
    ts.set_editor_property("sampler_type", sampler_for(tex))
    link(uv, "", ts, "UVs")
    return ts


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
float le = sides > 0.5 ? e : 0.0;      // no side lines: lanes use the whole width
float L = W - 2 * le;
if (N > 1.5 && L > 0.1)
{
    float t = (u - le) / L * N;
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
tex_bc = load_tex(ASPHALT_BC)
tex_n = load_tex(ASPHALT_N)
if tex_bc:
    base = atlas_sample(asphalt, tex_bc, RECT_TOP_LEFT, False, -700, -400)
else:
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

if tex_bc and tex_n:
    # Flat where lines are painted.
    world_normal(asphalt, tex_n, RECT_TOP_LEFT, False, any_line, -250, 1300)
finish(asphalt)
log("Asphalt with lane markings")

# --- Concrete: light grey, slightly mottled --------------------------------------------------
concrete = new_material("M_RoadConcrete")
tex_c = load_tex(CONCRETE_BC)
tex_cn = load_tex(CONCRETE_N)
if tex_c:
    base = atlas_sample(concrete, tex_c, RECT_BOTTOM_LEFT, True, -700, 0)
    if tex_cn:
        world_normal(concrete, tex_cn, RECT_BOTTOM_LEFT, True, None, -250, 900)
else:
    base = noise_blend(concrete, (0.32, 0.31, 0.29), (0.45, 0.44, 0.42), 0.02, 0)
MEL.connect_material_property(base, "", MP.MP_BASE_COLOR)
MEL.connect_material_property(const(concrete, 0.9, -300, 400), "", MP.MP_ROUGHNESS)
finish(concrete)

# --- Cement blocks: the earlier procedural concrete, finer, with block joints ----------------------
# The fence mesh carries UV1 = (metres along, 1 on end caps) and UV2.x = block length (m). Each joint
# is a thin dark groove with a small bevel on both blocks (tangent X runs along the fence).
JOINT_HLSL = r"""
float L = max(UV2.x, 0.1);
float a = frac(UV1.x / L);
float d = min(a, 1.0 - a) * L;          // metres to the nearest joint
float side = a < 0.5 ? -1.0 : 1.0;      // just after a joint the face tilts back towards it
float open = 1.0 - saturate(UV1.y);     // no joints on the end caps
float gap = 0.006;                      // half width of the groove
float bevel = 0.025;
float aa = max(fwidth(UV1.x), 0.0005);
float groove = open * (1.0 - smoothstep(gap - aa, gap + aa, d));
float slope = open * (1.0 - smoothstep(gap, gap + bevel, d)) * side;
return float2(groove, slope);
"""
jersey = new_material("M_RoadJersey")
jbase = noise_blend(jersey, (0.34, 0.33, 0.31), (0.47, 0.46, 0.44), 0.12, 0)
ju1 = node(jersey, unreal.MaterialExpressionTextureCoordinate, -1100, 600, coordinate_index=1)
ju2 = node(jersey, unreal.MaterialExpressionTextureCoordinate, -1100, 700, coordinate_index=2)
joint = custom(jersey, JOINT_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT2, ["UV1", "UV2"], -800, 650, "BlockJoints")
link(ju1, "", joint, "UV1")
link(ju2, "", joint, "UV2")
jgroove = node(jersey, unreal.MaterialExpressionComponentMask, -550, 600, r=True, g=False, b=False, a=False)
jslope = node(jersey, unreal.MaterialExpressionComponentMask, -550, 750, r=False, g=True, b=False, a=False)
link(joint, "", jgroove, "")
link(joint, "", jslope, "")
# Groove: the concrete a bit darker (shadowed), not black.
jshade = node(jersey, unreal.MaterialExpressionMultiply, -450, 250)
link(jbase, "", jshade, "A")
link(const(jersey, 0.6, -600, 300), "", jshade, "B")
jdark = node(jersey, unreal.MaterialExpressionLinearInterpolate, -300, 100)
link(jbase, "", jdark, "A")
link(jshade, "", jdark, "B")
link(jgroove, "", jdark, "Alpha")
MEL.connect_material_property(jdark, "", MP.MP_BASE_COLOR)
MEL.connect_material_property(const(jersey, 0.92, -300, 400), "", MP.MP_ROUGHNESS)
# Tangent-space normal: tilt along the fence on the bevels.
JN_HLSL = r"""return normalize(float3(S * 0.8, 0, 1));"""
jn = custom(jersey, JN_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT3, ["S"], -300, 750, "BevelNormal")
link(jslope, "", jn, "S")
MEL.connect_material_property(jn, "", MP.MP_NORMAL)
finish(jersey)
log("Cement blocks with joints")

# --- Galvanised steel ------------------------------------------------------------------------
steel = new_material("M_RoadSteel")
base = noise_blend(steel, (0.45, 0.46, 0.47), (0.6, 0.61, 0.62), 0.05, 0)
MEL.connect_material_property(base, "", MP.MP_BASE_COLOR)
MEL.connect_material_property(const(steel, 1.0, -300, 400), "", MP.MP_METALLIC)
MEL.connect_material_property(const(steel, 0.45, -300, 500), "", MP.MP_ROUGHNESS)
finish(steel)

# --- Wire grid: diamond mesh on a single two-sided sheet --------------------------------------
# u = metres along the fence (UV1.x), v = world height in metres, so the diamonds stay upright on
# slopes. Opacity mask: wires 1, holes 0.
DIAMOND_HLSL = r"""
float u = UV1.x;
float v = WP.z / 100.0;
float cell = 0.09;                               // diamond size (m, along each diagonal axis)
float wire = 0.006;                              // half wire thickness (m)
float2 p = float2(u + v, u - v) * 0.70710678 / cell;
float2 f = abs(frac(p) - 0.5) * cell;            // distance to the cell centre lines, in m
float d = cell * 0.5 - max(f.x, f.y);            // distance to the nearest wire
float aa = max(fwidth(p.x) * cell, 0.0005);
return 1.0 - smoothstep(wire - aa, wire + aa, d);
"""
grid = new_material("M_RoadGridFence")
grid.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
grid.set_editor_property("two_sided", True)
grid.set_editor_property("opacity_mask_clip_value", 0.4)
gu1 = node(grid, unreal.MaterialExpressionTextureCoordinate, -1100, 0, coordinate_index=1)
gwp = node(grid, unreal.MaterialExpressionWorldPosition, -1100, 120)
dia = custom(grid, DIAMOND_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT1, ["UV1", "WP"], -750, 0, "DiamondGrid")
link(gu1, "", dia, "UV1")
link(gwp, "", dia, "WP")
MEL.connect_material_property(dia, "", MP.MP_OPACITY_MASK)
MEL.connect_material_property(color(grid, 0.5, 0.51, 0.52, -300, 200), "", MP.MP_BASE_COLOR)
MEL.connect_material_property(const(grid, 1.0, -300, 300), "", MP.MP_METALLIC)
MEL.connect_material_property(const(grid, 0.5, -300, 400), "", MP.MP_ROUGHNESS)
finish(grid)

# --- Overlay: picked / aimed road ends of the intersection tool ---------------------------------
# Unlit, drawn over everything (no depth test), colour from the mesh's vertex colours.
overlay = new_material("M_RM_Overlay")
overlay.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
overlay.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
overlay.set_editor_property("two_sided", True)
for prop in ("disable_depth_test", "allow_negative_emissive_color"):
    try:
        overlay.set_editor_property(prop, prop == "disable_depth_test")
    except Exception as e:
        log("  ! overlay %s: %s" % (prop, e))
ovc = node(overlay, unreal.MaterialExpressionVertexColor, -500, 0)
MEL.connect_material_property(ovc, "", MP.MP_EMISSIVE_COLOR)
MEL.connect_material_property(const(overlay, 1.0, -500, 200), "", MP.MP_OPACITY)
finish(overlay)

# --- Assign ----------------------------------------------------------------------------------
ASSIGN = {
    "Build_Road_1x": {"m_top_material": asphalt, "m_body_material": concrete},
    "Build_Road_2x": {"m_top_material": asphalt, "m_body_material": concrete},
    "Build_Road_3x": {"m_top_material": asphalt, "m_body_material": concrete},
    "Build_RoadIntersection": {"m_top_material": asphalt, "m_body_material": concrete},
    "Build_Fence_GuardRail": {"m_main_material": steel},
    "Build_Fence_Jersey": {"m_main_material": jersey},
    "Build_Fence_Grid": {"m_main_material": steel, "m_panel_material": grid},
    "Build_Fence_GridTall": {"m_main_material": steel, "m_panel_material": grid},
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

for path in ("/RoadMaker/Holograms/Holo_Intersection.Holo_Intersection", "/RoadMaker/Holograms/Holo_Intersection"):
    try:
        holo = unreal.load_asset(path)
    except Exception:
        holo = None
    if holo:
        try:
            unreal.get_default_object(holo.generated_class()).set_editor_property("m_overlay_material", overlay)
            unreal.BlueprintEditorLibrary.compile_blueprint(holo)
            eal.save_loaded_asset(holo, only_if_is_dirty=False)
            log("Assigned overlay material to Holo_Intersection")
        except Exception as e:
            log("  ! Holo_Intersection overlay: %s" % e)
        break
else:
    log("  ! missing Holo_Intersection")

log("DONE")
_log.close()
