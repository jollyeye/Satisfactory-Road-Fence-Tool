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
# Roads (RW > 0.5, the road width in UV2) map along the curve instead: RUV = UV1 = (metres across,
# metres along the centre line), so the texture follows the road's bends.
ATLAS_UV_HLSL = r"""
float3 a = abs(N);
float2 p = WP.xy;
if (Tri > 0.5 && a.z < 0.7) { p = a.x > a.y ? WP.yz : WP.xz; }
if (RW > 0.5) { p = float2(RUV.y, -RUV.x) * 100.0; }   // turned 90 degrees: u along, v across
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
if (RW > 0.5)
{
    // Along the road, turned 90 degrees: U = mesh tangent (forward), V = across (to the left),
    // W = surface normal.
    p = float2(RUV.y, -RUV.x) * 100.0;
    U = normalize(Tg);
    V = -normalize(cross(N, U));
    W = normalize(N);
}
float2 h = frac(p / (2.0 * Period));
float2 s = float2(h.x < 0.5 ? 1.0 : -1.0, h.y < 0.5 ? 1.0 : -1.0);
// Signs checked in game (the first guess lit bumps from the wrong side).
float3 n = normalize(-U * (T.x * s.x * Str) + V * (T.y * s.y * Str) + W * max(T.z, 0.2));
return normalize(lerp(n, normalize(N), Flat));
"""
NORMAL_STRENGTH = 2.5


def road_inputs(mat, node_, road, x, y):
    """RUV / RW inputs of the atlas nodes: the road's UV1 / UV2 nodes, or zeros (world projection)."""
    if road:
        link(road[0], "", node_, "RUV")
        rw = node(mat, unreal.MaterialExpressionComponentMask, x, y, r=True, g=False, b=False, a=False)
        link(road[1], "", rw, "")
        link(rw, "", node_, "RW")
    else:
        link(node(mat, unreal.MaterialExpressionConstant2Vector, x, y), "", node_, "RUV")
        link(const(mat, 0.0, x, y + 60), "", node_, "RW")


def world_normal(mat, tex_n, rect, triplanar, flat, x, y, road=None):
    """Connects the atlas normal map (world space) to the material's Normal."""
    t = atlas_sample(mat, tex_n, rect, triplanar, x - 500, y, road)
    mat.set_editor_property("tangent_space_normal", False)
    wp = node(mat, unreal.MaterialExpressionWorldPosition, x - 300, y + 250)
    vn = node(mat, unreal.MaterialExpressionVertexNormalWS, x - 300, y + 330)
    wn = custom(mat, WORLD_NORMAL_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                ["T", "WP", "N", "Tri", "Period", "Flat", "Str", "RUV", "RW", "Tg"], x, y, "WorldNormal")
    road_inputs(mat, wn, road, x - 300, y + 700)
    link(node(mat, unreal.MaterialExpressionVertexTangentWS, x - 300, y + 820), "", wn, "Tg")
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


def atlas_sample(mat, tex, rect, triplanar, x, y, road=None):
    """Texture sample of one atlas square, mirrored every 8 m in world space (or along the road)."""
    wp = node(mat, unreal.MaterialExpressionWorldPosition, x - 600, y)
    vn = node(mat, unreal.MaterialExpressionVertexNormalWS, x - 600, y + 100)
    uv = custom(mat, ATLAS_UV_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT2, ["WP", "N", "Rect", "Tri", "Period", "RUV", "RW"], x - 350, y, "AtlasUV")
    road_inputs(mat, uv, road, x - 600, y + 450)
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
uv1 = node(asphalt, unreal.MaterialExpressionTextureCoordinate, -1100, 600, coordinate_index=1)
uv2 = node(asphalt, unreal.MaterialExpressionTextureCoordinate, -1100, 700, coordinate_index=2)
# Roads: the asphalt follows the curve (UV1 metres across / along); intersections (no UV2 width)
# keep the world projection.
road_uv = (uv1, uv2)
if tex_bc:
    base = atlas_sample(asphalt, tex_bc, RECT_TOP_LEFT, False, -700, -400, road_uv)
else:
    base = noise_blend(asphalt, (0.025, 0.025, 0.027), (0.06, 0.06, 0.062), 0.08, 0)

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
    world_normal(asphalt, tex_n, RECT_TOP_LEFT, False, any_line, -250, 1300, road_uv)
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

# --- Concrete fence wall: the game's concrete wall texture, painted with the customizer colour -----
# The wall mesh carries UV1 = (metres along, metres up from its base), so the texture follows the
# curve. Layout of TX_WallSetConcrete_01 (read from the real texture): one HALF of an 8 m x 4 m wall
# panel, with the panel centre at u = 0 and the panel edge at u ~ 0.856; mirrored for the other
# half. About 0.214 texture units per metre; the top of the wall at v ~ 0.021, the bottom at ~0.888.
# Paint: the wall pushes its primary colour as custom primitive data 24. Like the game's concrete
# walls (compared in game with the same swatches), paint only stains the concrete: the texture is
# multiplied by lerp(white, paint, WALL_PAINT_AMOUNT), so blue gives a bluish grey, red a reddish tan.
WALL_TEX = "/Game/FactoryGame/Buildable/Building/Wall/ConcreteWallSet/Textures/"
WALL_BC = WALL_TEX + "TX_WallSetConcrete_01_BC"
WALL_N = WALL_TEX + "TX_WallSetConcrete_01_N"
WALL_PAINT_REF = 0.85     # default paint (no colour pushed yet): light grey
WALL_PAINT_AMOUNT = 0.45
WALL_UV_HLSL = r"""
float p = frac(UV1.x / 8.0) * 8.0 - 4.0;          // metres from the panel centre
float u = abs(p) * 0.214;
float v = 0.021 + (4.0 - clamp(UV1.y, 0.0, 4.0)) * 0.21675;
return float2(u, v);
"""
# World-space normal from the map: the mesh tangent A already runs the way the texture's +u grows
# (away from the panel centre), +v runs down. Signs as for the foundation textures (checked in game
# there): red tilts towards -u, green towards +v.
WALL_NORMAL_HLSL = r"""
float s = 1.0;
float3 Along = normalize(A);
float3 Up = float3(0, 0, 1);
float3 n = N * max(T.z, 0.2) - Along * (s * T.x * Str) - Up * (T.y * Str);
return normalize(n);
"""
fwall = new_material("M_RoadFenceWall")
tex_wbc = load_tex(WALL_BC)
tex_wn = load_tex(WALL_N)
fu1 = node(fwall, unreal.MaterialExpressionTextureCoordinate, -1300, 0, coordinate_index=1)
fuv = custom(fwall, WALL_UV_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT2, ["UV1"], -1050, 0, "WallUV")
link(fu1, "", fuv, "UV1")
if tex_wbc:
    fbase = node(fwall, unreal.MaterialExpressionTextureSample, -800, 0)
    fbase.set_editor_property("texture", tex_wbc)
    fbase.set_editor_property("sampler_type", sampler_for(tex_wbc))
    link(fuv, "", fbase, "UVs")
else:
    fbase = noise_blend(fwall, (0.32, 0.31, 0.29), (0.45, 0.44, 0.42), 0.02, 0)
if tex_wn:
    fwall.set_editor_property("tangent_space_normal", False)
    fn = node(fwall, unreal.MaterialExpressionTextureSample, -800, 500)
    fn.set_editor_property("texture", tex_wn)
    fn.set_editor_property("sampler_type", sampler_for(tex_wn))
    link(fuv, "", fn, "UVs")
    fwn = custom(fwall, WALL_NORMAL_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT3, ["T", "UV1", "N", "A", "Str"], -400, 600, "WallNormal")
    link(fn, "", fwn, "T")
    link(fu1, "", fwn, "UV1")
    link(node(fwall, unreal.MaterialExpressionVertexNormalWS, -700, 800), "", fwn, "N")
    link(node(fwall, unreal.MaterialExpressionVertexTangentWS, -700, 880), "", fwn, "A")
    link(const(fwall, 1.5, -700, 960), "", fwn, "Str")
    MEL.connect_material_property(fwn, "", MP.MP_NORMAL)
fpaint = node(fwall, unreal.MaterialExpressionVectorParameter, -700, 300,
              parameter_name="PaintColor", default_value=unreal.LinearColor(WALL_PAINT_REF, WALL_PAINT_REF, WALL_PAINT_REF, 1.0))
fpaint.set_editor_property("use_custom_primitive_data", True)
fpaint.set_editor_property("primitive_data_index", 24)
fdetail = node(fwall, unreal.MaterialExpressionLinearInterpolate, -500, 100)
link(const(fwall, 1.0, -700, 150), "", fdetail, "A")
link(fpaint, "", fdetail, "B")
link(const(fwall, WALL_PAINT_AMOUNT, -700, 200), "", fdetail, "Alpha")
fmul = node(fwall, unreal.MaterialExpressionMultiply, -350, 150)
link(fbase, "", fmul, "A")
link(fdetail, "", fmul, "B")
fsat = node(fwall, unreal.MaterialExpressionSaturate, -200, 150)
link(fmul, "", fsat, "")
MEL.connect_material_property(fsat, "", MP.MP_BASE_COLOR)
MEL.connect_material_property(const(fwall, 0.85, -300, 400), "", MP.MP_ROUGHNESS)
finish(fwall)
log("Concrete fence wall (paintable)")

# The temporary texture-dump materials (used during development) are removed from the mod.
for old in ("M_RM_TextureDump", "M_RM_TextureDumpN"):
    if eal.does_asset_exist(ROOT + "/" + old):
        eal.delete_asset(ROOT + "/" + old)
        log("Deleted " + ROOT + "/" + old)

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

# The game's own concrete wall material: the wall mesh carries the same texture layout (UV0) and the
# wall passes the paint to it the way the game does, so it looks and paints like the game's walls.
# Road Maker's M_RoadFenceWall stays as the fallback.
game_wall_mat = None
for path in ("/Game/FactoryGame/Buildable/Building/Wall/ConcreteWallSet/MI_WallSetConcrete_8x4.MI_WallSetConcrete_8x4",
             "/Game/FactoryGame/Buildable/Building/Wall/ConcreteWallSet/MI_WallSetConcrete_8x4"):
    try:
        game_wall_mat = unreal.load_asset(path)
    except Exception:
        game_wall_mat = None
    if game_wall_mat:
        log("Fence wall uses the game's material " + path)
        break
if not game_wall_mat:
    log("  ! game wall material not found, the fence wall uses M_RoadFenceWall")
game_wall_mats = {}
for key, path in (("ficsit", "/Game/FactoryGame/Buildable/Building/Wall/FicsitWallSet/MI_WallSetOrange_8x1.MI_WallSetOrange_8x1"),
                  ("steel", "/Game/FactoryGame/Buildable/Building/Wall/SteelWallSet/MI_SteelWall_8x4.MI_SteelWall_8x4")):
    try:
        m = unreal.load_asset(path)
    except Exception:
        m = None
    log(("Fence wall %s uses " % key) + (path if m else "a fallback (not found: %s)" % path))
    if m:
        game_wall_mats[key] = m

# Fence wall looks for the customizer's Material tab: concrete, FICSIT, steel (switched in place).
wall_looks = [game_wall_mat or fwall, game_wall_mats.get("ficsit") or game_wall_mat or fwall,
              game_wall_mats.get("steel") or game_wall_mat or fwall]

# --- Road looks for the Material tab: concrete blocks (pavers) and cyberpunk -------------------
def cpd_param(mat, name, index, default, x, y):
    p = node(mat, unreal.MaterialExpressionVectorParameter, x, y,
             parameter_name=name, default_value=default)
    p.set_editor_property("use_custom_primitive_data", True)
    p.set_editor_property("primitive_data_index", index)
    return p


def lane_masks(mat, x, y):
    """(mid, side) lane-marking masks from the mesh UVs, same as the asphalt."""
    u1 = node(mat, unreal.MaterialExpressionTextureCoordinate, x, y, coordinate_index=1)
    u2 = node(mat, unreal.MaterialExpressionTextureCoordinate, x, y + 100, coordinate_index=2)
    u3 = node(mat, unreal.MaterialExpressionTextureCoordinate, x, y + 200, coordinate_index=3)
    m = custom(mat, MARKINGS_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT2, ["UV1", "UV2", "UV3"], x + 300, y, "LaneMarkings")
    link(u1, "", m, "UV1")
    link(u2, "", m, "UV2")
    link(u3, "", m, "UV3")
    mid = node(mat, unreal.MaterialExpressionComponentMask, x + 550, y, r=True, g=False, b=False, a=False)
    side = node(mat, unreal.MaterialExpressionComponentMask, x + 550, y + 100, r=False, g=True, b=False, a=False)
    link(m, "", mid, "")
    link(m, "", side, "")
    return mid, side, u1, u2


# Concrete blocks: running bond, 4 m along x 2 m across, 1.6 cm joints with 4 cm bevels.
# Returns (joint, block hash, height in cm); the height feeds the bump normal.
PAVERS_HLSL = r"""
float2 p = UV2.x > 0.5 ? float2(UV1.y, UV1.x) : WP.xy / 100.0;   // metres along, across
float bl = 4.0; float bw = 2.0;
float row = floor(p.y / bw);
float2 c = float2(p.x + fmod(abs(row), 2.0) * 0.5 * bl, p.y);
float2 cell = float2(c.x / bl, c.y / bw);
float2 f = frac(cell);
float2 d = min(f, 1.0 - f) * float2(bl, bw);
float e = min(d.x, d.y);
float aa = max(max(fwidth(p.x), fwidth(p.y)), 0.002);
float fade = saturate(0.03 / aa);
float joint = (1.0 - smoothstep(0.008, 0.008 + aa, e)) * fade;
float h = frac(sin(dot(floor(cell), float2(12.9898, 78.233))) * 43758.5453);
float height = 1.5 * smoothstep(0.008, 0.05, e) * fade;
return float3(joint, h, height);
"""

# Bump normal from a height field (cm) with screen-space derivatives (world space result).
BUMP_HLSL = r"""
float3 dpx = ddx(WP);
float3 dpy = ddy(WP);
float dbs = ddx(Hgt) * Str;
float dbt = ddy(Hgt) * Str;
float3 R1 = cross(dpy, N);
float3 R2 = cross(N, dpx);
float det = dot(dpx, R1);
float3 grad = sign(det) * (dbs * R1 + dbt * R2);
float3 n = normalize(abs(det) * N - grad);
return normalize(lerp(n, N, saturate(Flat)));
"""

# Cyberpunk circuit board: two layers of 0.5 m / 0.25 m tracks whose sections are staggered per
# track and differ in length (straight, jog up / down, short with a pad, bus, rings), empty patches
# (macro cells), and now and then a chip with legs (16 x 8 m cells). Returns the emboss mask.
CIRCUIT_HLSL = r"""
float2 q = UV2.x > 0.5 ? float2(UV1.y, UV1.x) : WP.xy / 100.0;   // metres along, across
float aa = max(max(fwidth(q.x), fwidth(q.y)), 0.001);
float hw = 0.011;
float soft = 0.004 + aa;
float ln = 0.0;
float rg = 0.0;
for (int layer = 0; layer < 2; layer++)
{
    float S = layer == 0 ? 4.0 : 2.5;
    float R = layer == 0 ? 0.5 : 0.25;
    float2 qq = q + (layer == 0 ? float2(0, 0) : float2(1.3, 0.37));
    float j0 = floor(qq.y / R);
    for (int dj = -1; dj <= 1; dj++)
    {
        float j = j0 + dj;
        float sh = frac(sin(j * 12.9898 + layer * 4.1) * 43758.5453) * S;
        float s = floor((qq.x + sh) / S);
        float x = qq.x + sh - s * S;
        float2 hh = frac(sin(float2(dot(float2(j, s), float2(127.1, 311.7)) + layer * 7.7,
                                    dot(float2(j, s), float2(269.5, 183.3)) + layer * 3.3)) * 43758.5453);
        float hj = hh.x; float h2 = hh.y;
        float dens = frac(sin(dot(floor(float2(qq.x / (S * 3.0), qq.y / (R * 8.0))), float2(41.3, 17.9)) + layer) * 43758.5453);
        if (dens < 0.2) continue;
        float2 P = float2(x, qq.y - (j + 0.5) * R);
        float2 A[3]; float2 B[3];
        for (int k = 0; k < 3; k++) { A[k] = float2(100, 100); B[k] = float2(100, 100); }
        if (hj < 0.3)
        {
            A[0] = float2(0, 0); B[0] = float2(S, 0);
            rg = max(rg, 1.0 - smoothstep(hw, hw + soft, abs(length(P - float2(S * (0.25 + 0.5 * h2), 0)) - 0.05)));
        }
        else if (hj < 0.65)
        {
            float jx = S * (0.3 + 0.4 * h2);
            float up = hj < 0.5 ? R : -R;
            A[0] = float2(0, 0);          B[0] = float2(jx - 0.2, 0);
            A[1] = float2(jx - 0.2, 0);   B[1] = float2(jx + 0.2, up);
            A[2] = float2(jx + 0.2, up);  B[2] = float2(S, up);
        }
        else if (hj < 0.8)
        {
            float len = S * (0.3 + 0.5 * h2);
            A[0] = float2(0, 0); B[0] = float2(len, 0);
            rg = max(rg, 1.0 - smoothstep(hw, hw + soft, abs(length(P - float2(len + 0.06, 0)) - 0.05)));
        }
        else if (hj < 0.92)
        {
            A[0] = float2(0, -0.12); B[0] = float2(S, -0.12);
            A[1] = float2(0, 0);     B[1] = float2(S * (0.6 + 0.4 * h2), 0);
            A[2] = float2(0, 0.12);  B[2] = float2(S, 0.12);
        }
        else
        {
            float lr = length(P - float2(S * 0.5, 0));
            rg = max(rg, 1.0 - smoothstep(hw, hw + soft, abs(lr - 0.07)));
            rg = max(rg, 1.0 - smoothstep(hw, hw + soft, abs(lr - 0.03)));
        }
        for (int m = 0; m < 3; m++)
        {
            float2 pa = P - A[m]; float2 ba = B[m] - A[m];
            float t = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-5));
            ln = max(ln, 1.0 - smoothstep(hw, hw + soft, length(pa - ba * t)));
        }
    }
}
float2 C = float2(16.0, 8.0);
float2 cc = floor(q / C);
float hc = frac(sin(dot(cc, float2(91.7, 33.1))) * 43758.5453);
float hc2 = frac(sin(dot(cc, float2(17.3, 71.9))) * 43758.5453);
float chip = 0.0;
float clear = 1.0;
if (hc < 0.4)
{
    float2 c = (cc + float2(0.25 + 0.5 * hc2, 0.25 + 0.5 * frac(hc * 7.3))) * C;
    float2 hs = float2(0.9 + 0.8 * hc2, 0.6 + 0.3 * hc);
    float2 b = abs(q - c) - hs;
    float sd = length(max(b, 0.0)) + min(max(b.x, b.y), 0.0);
    float outline = 1.0 - smoothstep(hw, hw + soft, abs(sd));
    float lx = abs(frac((q.x - c.x) / 0.3 + 0.5) - 0.5) * 0.3;
    float ly = abs(frac((q.y - c.y) / 0.3 + 0.5) - 0.5) * 0.3;
    float legY = (b.y > 0 && b.y < 0.3 && b.x < -0.1) ? 1.0 - smoothstep(0.02, 0.02 + soft, lx) : 0.0;
    float legX = (b.x > 0 && b.x < 0.3 && b.y < -0.1) ? 1.0 - smoothstep(0.02, 0.02 + soft, ly) : 0.0;
    float dotm = 1.0 - smoothstep(0.04, 0.04 + soft, length(q - (c - hs + 0.18)));
    chip = max(max(outline, dotm), max(legY, legX));
    clear = smoothstep(0.30, 0.45, sd);
}
float m = max(max(ln, rg) * clear, chip);
return saturate(m) * saturate(0.03 / aa);
"""


def lines_on(mat, base, mid, side, primary, secondary, x, y):
    with_sides = node(mat, unreal.MaterialExpressionLinearInterpolate, x, y)
    link(base, "", with_sides, "A")
    link(secondary, "", with_sides, "B")
    link(side, "", with_sides, "Alpha")
    col = node(mat, unreal.MaterialExpressionLinearInterpolate, x + 150, y)
    link(with_sides, "", col, "A")
    link(primary, "", col, "B")
    link(mid, "", col, "Alpha")
    return col


# Concrete blocks.
pavers = new_material("M_RoadPavers")
p_uv1 = node(pavers, unreal.MaterialExpressionTextureCoordinate, -1100, 600, coordinate_index=1)
p_uv2 = node(pavers, unreal.MaterialExpressionTextureCoordinate, -1100, 700, coordinate_index=2)
p_wp = node(pavers, unreal.MaterialExpressionWorldPosition, -1100, 800)
pv = custom(pavers, PAVERS_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT3, ["UV1", "UV2", "WP"], -800, 650, "Pavers")
link(p_uv1, "", pv, "UV1")
link(p_uv2, "", pv, "UV2")
link(p_wp, "", pv, "WP")
joint_m = node(pavers, unreal.MaterialExpressionComponentMask, -550, 650, r=True, g=False, b=False, a=False)
hash_m = node(pavers, unreal.MaterialExpressionComponentMask, -550, 750, r=False, g=True, b=False, a=False)
link(pv, "", joint_m, "")
link(pv, "", hash_m, "")
if tex_c:
    p_base = atlas_sample(pavers, tex_c, RECT_BOTTOM_LEFT, False, -700, -300, (p_uv1, p_uv2))
else:
    p_base = noise_blend(pavers, (0.32, 0.31, 0.29), (0.45, 0.44, 0.42), 0.02, 0)
# Per-block tint 0.8 .. 1.1, joints darker.
tint = node(pavers, unreal.MaterialExpressionLinearInterpolate, -350, 200)
link(const(pavers, 0.8, -550, 150), "", tint, "A")
link(const(pavers, 1.1, -550, 220), "", tint, "B")
link(hash_m, "", tint, "Alpha")
tinted = node(pavers, unreal.MaterialExpressionMultiply, -200, 150)
link(p_base, "", tinted, "A")
link(tint, "", tinted, "B")
jd = node(pavers, unreal.MaterialExpressionLinearInterpolate, -50, 150)
link(tinted, "", jd, "A")
link(color(pavers, 0.03, 0.03, 0.03, -300, 300), "", jd, "B")
link(joint_m, "", jd, "Alpha")
pm, ps, _, _ = lane_masks(pavers, -1100, 1000)
p_primary = cpd_param(pavers, "MiddleLineColor", 24, unreal.LinearColor(0.8, 0.8, 0.76, 1.0), -600, 1300)
p_secondary = cpd_param(pavers, "SideLineColor", 28, unreal.LinearColor(0.8, 0.8, 0.76, 1.0), -600, 1400)
p_col = lines_on(pavers, jd, pm, ps, p_primary, p_secondary, 100, 150)
MEL.connect_material_property(p_col, "", MP.MP_BASE_COLOR)
p_any = node(pavers, unreal.MaterialExpressionMax, -350, 450)
link(pm, "", p_any, "A")
link(ps, "", p_any, "B")
p_rough = node(pavers, unreal.MaterialExpressionLinearInterpolate, 100, 400)
link(const(pavers, 0.9, -200, 350), "", p_rough, "A")
link(const(pavers, 0.6, -200, 420), "", p_rough, "B")
link(p_any, "", p_rough, "Alpha")
MEL.connect_material_property(p_rough, "", MP.MP_ROUGHNESS)


def bump_normal(mat, height, strength, flat, x, y):
    """World-space bump normal from a height field in cm."""
    bn = custom(mat, BUMP_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT3, ["Hgt", "WP", "N", "Str", "Flat"], x, y, "Bump")
    link(height, "", bn, "Hgt")
    link(node(mat, unreal.MaterialExpressionWorldPosition, x - 300, y + 100), "", bn, "WP")
    link(node(mat, unreal.MaterialExpressionVertexNormalWS, x - 300, y + 200), "", bn, "N")
    link(const(mat, strength, x - 300, y + 300), "", bn, "Str")
    if flat is None:
        link(const(mat, 0.0, x - 300, y + 380), "", bn, "Flat")
    else:
        link(flat, "", bn, "Flat")
    mat.set_editor_property("tangent_space_normal", False)
    # Low quality (graphics settings): no bump, the plain vertex normal.
    qs = node(mat, unreal.MaterialExpressionQualitySwitch, x + 300, y)
    link(bn, "", qs, "Default")
    link(node(mat, unreal.MaterialExpressionVertexNormalWS, x, y + 480), "", qs, "Low")
    MEL.connect_material_property(qs, "", MP.MP_NORMAL)
    return bn


p_height = node(pavers, unreal.MaterialExpressionComponentMask, -550, 850, r=False, g=False, b=True, a=False)
link(pv, "", p_height, "")
bump_normal(pavers, p_height, 1.0, p_any, 300, 900)
finish(pavers)
log("Concrete pavers")

# Cyberpunk top: matte plastic in the primary colour, circuit grooves, glowing secondary lane lines.
cyber = new_material("M_RoadCyber")
c_uv1 = node(cyber, unreal.MaterialExpressionTextureCoordinate, -1100, 600, coordinate_index=1)
c_uv2 = node(cyber, unreal.MaterialExpressionTextureCoordinate, -1100, 700, coordinate_index=2)
c_wp = node(cyber, unreal.MaterialExpressionWorldPosition, -1100, 800)
cc = custom(cyber, CIRCUIT_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT1, ["UV1", "UV2", "WP"], -800, 650, "Circuit")
link(c_uv1, "", cc, "UV1")
link(c_uv2, "", cc, "UV2")
link(c_wp, "", cc, "WP")
c_primary = cpd_param(cyber, "MiddleLineColor", 24, unreal.LinearColor(0.32, 0.02, 0.40, 1.0), -600, 1300)
c_secondary = cpd_param(cyber, "SideLineColor", 28, unreal.LinearColor(0.0, 0.85, 1.0, 1.0), -600, 1400)
plastic = node(cyber, unreal.MaterialExpressionMultiply, -350, 100)
link(c_primary, "", plastic, "A")
link(const(cyber, 0.8, -500, 150), "", plastic, "B")
groove = node(cyber, unreal.MaterialExpressionMultiply, -350, 250)
link(c_primary, "", groove, "A")
link(const(cyber, 0.7, -500, 300), "", groove, "B")
c_surface = node(cyber, unreal.MaterialExpressionLinearInterpolate, -150, 150)
link(plastic, "", c_surface, "A")
link(groove, "", c_surface, "B")
faint = node(cyber, unreal.MaterialExpressionMultiply, -300, 330)
link(cc, "", faint, "A")
link(const(cyber, 0.5, -450, 380), "", faint, "B")
link(faint, "", c_surface, "Alpha")
cm, cs, _, _ = lane_masks(cyber, -1100, 1000)
c_col = lines_on(cyber, c_surface, cm, cs, c_secondary, c_secondary, 50, 150)
# Low quality: plain plastic, no circuit pattern in the colour either.
c_col_low = lines_on(cyber, plastic, cm, cs, c_secondary, c_secondary, 50, -150)
c_qs = node(cyber, unreal.MaterialExpressionQualitySwitch, 300, 150)
link(c_col, "", c_qs, "Default")
link(c_col_low, "", c_qs, "Low")
MEL.connect_material_property(c_qs, "", MP.MP_BASE_COLOR)
c_any = node(cyber, unreal.MaterialExpressionMax, -350, 450)
link(cm, "", c_any, "A")
link(cs, "", c_any, "B")
glow = node(cyber, unreal.MaterialExpressionMultiply, 100, 450)
glow_col = node(cyber, unreal.MaterialExpressionMultiply, 100, 550)
link(c_secondary, "", glow_col, "A")
link(c_any, "", glow_col, "B")
link(glow_col, "", glow, "A")
link(const(cyber, 4.0, -100, 600), "", glow, "B")
MEL.connect_material_property(glow, "", MP.MP_EMISSIVE_COLOR)
c_rough = node(cyber, unreal.MaterialExpressionLinearInterpolate, 100, 700)
link(const(cyber, 0.14, -200, 650), "", c_rough, "A")
link(const(cyber, 0.1, -200, 720), "", c_rough, "B")
link(c_any, "", c_rough, "Alpha")
MEL.connect_material_property(c_rough, "", MP.MP_ROUGHNESS)
# The circuit is carved into the normal map (0.6 cm); flat under the lane lines.
bump_normal(cyber, cc, -0.6, c_any, 300, 1100)
finish(cyber)
log("Cyberpunk road")

# Cyberpunk slab sides: dark plastic in the primary colour.
cyber_body = new_material("M_RoadCyberBody")
cb_primary = cpd_param(cyber_body, "MiddleLineColor", 24, unreal.LinearColor(0.32, 0.02, 0.40, 1.0), -600, 0)
cb_col = node(cyber_body, unreal.MaterialExpressionMultiply, -300, 0)
link(cb_primary, "", cb_col, "A")
link(const(cyber_body, 0.3, -500, 80), "", cb_col, "B")
MEL.connect_material_property(cb_col, "", MP.MP_BASE_COLOR)
MEL.connect_material_property(const(cyber_body, 0.2, -300, 200), "", MP.MP_ROUGHNESS)
finish(cyber_body)
log("Cyberpunk road body")

# --- Lane markings (arrows, numbers): flat paint in the road's line colour -----------------------
# Plain: matte paint in the primary colour (the dashed middle lines' colour), like the asphalt's lines.
marking = new_material("M_RoadMarking")
mk_primary = cpd_param(marking, "MiddleLineColor", 24, unreal.LinearColor(0.8, 0.8, 0.76, 1.0), -600, 0)
MEL.connect_material_property(mk_primary, "", MP.MP_BASE_COLOR)
MEL.connect_material_property(const(marking, 0.75, -300, 120), "", MP.MP_ROUGHNESS)
finish(marking)
log("Lane marking paint")

# Cyberpunk: glows like the cyberpunk road's lines (emissive x4), in the hue of its colour brought to full
# brightness, so it glows whatever swatch it is painted with (the game's swatches have dark second colours).
marking_cyber = new_material("M_RoadMarkingCyber")
mc_secondary = cpd_param(marking_cyber, "SideLineColor", 28, unreal.LinearColor(0.0, 0.85, 1.0, 1.0), -900, 0)
mc_bright = custom(marking_cyber, "float m = max(max(C.r, C.g), C.b);\nreturn m < 0.02 ? float3(1, 1, 1) : C.rgb / m;",
                   unreal.CustomMaterialOutputType.CMOT_FLOAT3, ["C"], -600, 0, "Full brightness")
link(mc_secondary, "", mc_bright, "C")
MEL.connect_material_property(mc_bright, "", MP.MP_BASE_COLOR)
mc_glow = node(marking_cyber, unreal.MaterialExpressionMultiply, -300, 100)
link(mc_bright, "", mc_glow, "A")
link(const(marking_cyber, 4.0, -450, 160), "", mc_glow, "B")
MEL.connect_material_property(mc_glow, "", MP.MP_EMISSIVE_COLOR)
MEL.connect_material_property(const(marking_cyber, 0.1, -300, 260), "", MP.MP_ROUGHNESS)
finish(marking_cyber)
log("Cyberpunk lane marking")

# --- Assign ----------------------------------------------------------------------------------
# Asphalt is the default look; the Material tab looks (concrete, cyberpunk) are index look - 1.
ROAD_LOOKS = {"m_top_material": asphalt, "m_body_material": concrete,
              "m_look_top_materials": [pavers, cyber],
              "m_look_body_materials": [concrete, cyber_body]}
MARKING_LOOKS = {"m_marking_material": marking, "m_look_top_materials": [marking, marking_cyber]}
ASSIGN = {
    "Build_Road_1x": ROAD_LOOKS,
    "Build_Road_2x": ROAD_LOOKS,
    "Build_Road_3x": ROAD_LOOKS,
    "Build_Road_4x": ROAD_LOOKS,
    "Build_RoadIntersection": ROAD_LOOKS,
    "Build_SignGantry": {"m_main_material": steel},
    "Build_LaneArrow": MARKING_LOOKS,
    "Build_LaneNumber": MARKING_LOOKS,
    "Build_LaneArrowMedium": MARKING_LOOKS,
    "Build_LaneNumberMedium": MARKING_LOOKS,
    "Build_Fence_GuardRail": {"m_main_material": steel},
    "Build_Fence_Jersey": {"m_main_material": jersey},
    "Build_Fence_Grid": {"m_main_material": steel, "m_panel_material": grid},
    "Build_Fence_GridTall": {"m_main_material": steel, "m_panel_material": grid},
    "Build_FenceWall_Concrete": {"m_main_material": game_wall_mat or fwall, "m_wall_look_materials": wall_looks},
    "Build_FenceWall_Ficsit": {"m_main_material": game_wall_mats.get("ficsit") or game_wall_mat or fwall, "m_wall_look_materials": wall_looks},
    "Build_FenceWall_Steel": {"m_main_material": game_wall_mats.get("steel") or game_wall_mat or fwall, "m_wall_look_materials": wall_looks},
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
