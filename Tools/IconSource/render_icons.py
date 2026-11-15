# Stylised isometric icons for Road Maker buildables: tiny painter's-algorithm renderer.
import math, sys
from PIL import Image, ImageDraw, ImageFilter
import numpy as np

S = 2048          # draw size (supersampled)
OUT = 512
L = np.array([0.35, 0.75, 1.0]); L = L / np.linalg.norm(L)
CAM = np.array([1.0, 1.0, 1.15]); CAM = CAM / np.linalg.norm(CAM)

def proj(p):
    x, y, z = p
    return np.array([(x - y) * 0.866, (x + y) * 0.5 - z * 1.0])

class Scene:
    def __init__(s): s.faces = []; s.bounds = []; s.overlays = []  # bounds: extra points that only count for the framing
    # overlays: (origin, u edge, v edge, RGBA image) pasted flat on a face after the polygons (texture u right, v down)
    def face(s, pts, col, layer=0, two=False, shade=True):
        s.faces.append((np.array(pts, float), np.array(col, float), layer, two, shade))
    def quad(s, a, b, c, d, col, **k): s.face([a, b, c, d], col, **k)

def shade_col(n, col):
    k = 0.42 + 0.62 * max(0.0, float(np.dot(n, L)))
    return np.clip(col * k, 0, 255)

def render(scene, path, shadow=True):
    faces = []
    for pts, col, layer, two, sh in scene.faces:
        n = np.cross(pts[1] - pts[0], pts[2] - pts[0])
        if np.linalg.norm(n) < 1e-9:
            continue
        n = n / np.linalg.norm(n)
        if np.dot(n, CAM) < 0:
            if not two:
                continue
            n = -n
        depth = float(np.mean(pts @ CAM))
        faces.append((layer, depth, pts, shade_col(n, col) if sh else col))
    allp = np.array([proj(p) for f in faces for p in f[2]] + [proj(p) for p in scene.bounds])
    lo, hi = allp.min(0), allp.max(0)
    scale = 0.84 * S / max(hi - lo)
    off = np.array([S, S]) / 2 - (lo + hi) / 2 * scale
    def P(p): q = proj(p) * scale + off; return (float(q[0]), float(q[1]))
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    if shadow:
        sh = Image.new("L", (S, S), 0)
        d = ImageDraw.Draw(sh)
        zmin = min(p[2] for f in faces for p in f[2])
        for f in faces:
            q = [P((p[0] + (p[2] - zmin) * 0.35, p[1] + (p[2] - zmin) * 0.35, zmin)) for p in f[2]]
            d.polygon(q, fill=110)
        sh = sh.filter(ImageFilter.GaussianBlur(S / 90))
        black = Image.new("RGBA", (S, S), (10, 12, 16, 255))
        img = Image.composite(black, img, sh)
    d = ImageDraw.Draw(img)
    for layer, depth, pts, col in sorted(faces, key=lambda f: (f[1] + f[0] * 1000.0)):
        c = tuple(int(v) for v in col) + (255,)
        d.polygon([P(p) for p in pts], fill=c, outline=c)
    for o, eu, ev, tex in scene.overlays:
        # The projection is affine: output pixel = P(o) + a * Pu + b * Pv, a, b in texture pixels.
        o2 = np.array(P(o)); pu = (np.array(P(np.add(o, eu))) - o2) / tex.width; pv = (np.array(P(np.add(o, ev))) - o2) / tex.height
        M = np.linalg.inv(np.array([[pu[0], pv[0]], [pu[1], pv[1]]]))
        t = -M @ o2
        layer = tex.transform((S, S), Image.AFFINE, (M[0, 0], M[0, 1], t[0], M[1, 0], M[1, 1], t[1]), Image.BICUBIC)
        img.alpha_composite(layer)
    img = img.resize((OUT, OUT), Image.LANCZOS)
    img.save(path)

def badge_walker(path):
    """Walking man badge in the bottom-left corner of a rendered icon (white on a blue sign)."""
    im = Image.open(path).convert("RGBA")
    k = 4; W = im.width * k
    big = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    box = (int(W * 0.03), int(W * 0.60), int(W * 0.40), int(W * 0.97))
    d.rounded_rectangle(box, radius=int(W * 0.045), fill=(36, 98, 186, 255), outline=(255, 255, 255, 255), width=int(W * 0.012))
    cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
    size = (box[3] - box[1]) * 0.78
    for poly in walker_polys():
        d.polygon([(cx + x * size, cy + size * 0.5 - y * size) for (x, y) in poly], fill=(255, 255, 255, 255))
    big = big.resize(im.size, Image.LANCZOS)
    im.alpha_composite(big)
    im.save(path)

# ---- geometry helpers -----------------------------------------------------------------------
def frames_arc(length, bend, n=24):
    """Path along +x bending towards +y; returns list of (pos, fwd, right(level))."""
    out = []
    if abs(bend) < 1e-6:
        for i in range(n + 1):
            t = length * i / n
            out.append((np.array([t, 0, 0.0]), np.array([1, 0, 0.0])))
    else:
        r = length / bend
        for i in range(n + 1):
            a = bend * i / n
            out.append((np.array([r * math.sin(a), r * (1 - math.cos(a)), 0.0]), np.array([math.cos(a), math.sin(a), 0.0])))
    res = []
    for p, f in out:
        right = np.array([f[1], -f[0], 0.0])   # level right
        res.append((p, f, right))
    return res

def at(fr, lat, up):
    p, f, r = fr
    return p + r * lat + np.array([0, 0, up])

def extrude(sc, frs, profile, col, closed=True, layer=0, two=False):
    m = len(profile)
    edges = m if closed else m - 1
    for e in range(edges):
        a, b = profile[e], profile[(e + 1) % m]
        for i in range(len(frs) - 1):
            sc.quad(at(frs[i], *a), at(frs[i], *b), at(frs[i + 1], *b), at(frs[i + 1], *a), col, layer=layer, two=two)

def cap(sc, fr, profile, col):
    sc.face([at(fr, *p) for p in profile], col, two=True)

def box(sc, c, hx, hy, hz, col, fwd=(1, 0, 0), layer=0):
    f = np.array(fwd, float); f /= np.linalg.norm(f)
    r = np.array([f[1], -f[0], 0.0]); u = np.array([0, 0, 1.0]); c = np.array(c, float)
    X, Y, Z = f * hx, r * hy, u * hz
    for s in (1, -1):
        sc.quad(c + s*X - Y - Z, c + s*X + Y - Z, c + s*X + Y + Z, c + s*X - Y + Z, col, layer=layer, two=True)
        sc.quad(c - X + s*Y - Z, c + X + s*Y - Z, c + X + s*Y + Z, c - X + s*Y + Z, col, layer=layer, two=True)
        sc.quad(c - X - Y + s*Z, c + X - Y + s*Z, c + X + Y + s*Z, c - X + Y + s*Z, col, layer=layer, two=True)

ASPHALT = np.array([62, 64, 68.0]); CONCRETE = np.array([178, 174, 166.0]); YELLOW = np.array([250, 190, 40.0])
WHITE = np.array([240, 240, 236.0]); STEEL = np.array([168, 176, 186.0]); DARKSTEEL = np.array([110, 118, 128.0])
ORANGE = np.array([250, 149, 73.0])
PAINT_WHITE = np.array([255, 255, 255.0])
ORANGE_CUT = np.array([255, 110, 20.0])

def line_strip(sc, frs, lat, w, col, dash=None, z=0.02, shade=True):
    for i in range(len(frs) - 1):
        if dash and (i // dash[0]) % 2 == 1:
            continue
        sc.quad(at(frs[i], lat - w, z), at(frs[i], lat + w, z), at(frs[i + 1], lat + w, z), at(frs[i + 1], lat - w, z), col, layer=1, two=True, shade=shade)

def road(sc, width, lanes, length=18.0, bend=0.55, h=1.0):
    frs = frames_arc(length, bend, 32)
    hw = width / 2
    extrude(sc, frs, [(-hw, 0), (hw, 0)], ASPHALT, closed=False, two=True)
    extrude(sc, frs, [(hw, 0), (hw, -h)], CONCRETE, closed=False, two=True)
    extrude(sc, frs, [(-hw, -h), (-hw, 0)], CONCRETE, closed=False, two=True)
    cap(sc, frs[0], [(-hw, 0), (hw, 0), (hw, -h), (-hw, -h)], CONCRETE)
    cap(sc, frs[-1], [(-hw, 0), (hw, 0), (hw, -h), (-hw, -h)], CONCRETE)
    e = 0.9
    line_strip(sc, frs, -hw + e, 0.16, YELLOW)
    line_strip(sc, frs, hw - e, 0.16, YELLOW)
    inner = width - 2 * e
    for k in range(1, lanes):
        line_strip(sc, frs, -hw + e + inner * k / lanes, 0.13, WHITE, dash=(2, 0))

# Walking man pictogram (2D, height 1, x right, y up): head + thick limbs, as polygons.
WALKER_HEAD = ((0.06, 0.89), 0.095)
WALKER_LIMBS = [  # polylines, drawn thick
    [(0.04, 0.76), (-0.02, 0.43)],                       # torso
    [(0.03, 0.72), (0.15, 0.57), (0.27, 0.62)],          # front arm
    [(0.03, 0.72), (-0.11, 0.58), (-0.17, 0.44)],        # back arm
    [(-0.02, 0.43), (0.11, 0.23), (0.22, 0.02)],         # front leg
    [(-0.02, 0.43), (-0.08, 0.21), (-0.24, 0.06)],       # back leg
]
WALKER_W = 0.085

def walker_polys():
    """The pictogram as a list of 2D polygons (thick segments, round joints, head)."""
    polys = []
    def circle(c, r, n=14):
        return [(c[0] + r * math.cos(2 * math.pi * k / n), c[1] + r * math.sin(2 * math.pi * k / n)) for k in range(n)]
    for line in WALKER_LIMBS:
        for a, b in zip(line, line[1:]):
            d = np.array(b) - np.array(a); d = d / np.linalg.norm(d); nrm = np.array([-d[1], d[0]]) * WALKER_W / 2
            polys.append([tuple(np.array(a) + nrm), tuple(np.array(b) + nrm), tuple(np.array(b) - nrm), tuple(np.array(a) - nrm)])
        for pt in line:
            polys.append(circle(pt, WALKER_W / 2, 10))
    polys.append(circle(*WALKER_HEAD))
    return polys

def path_point(frs, length, along, lat, up=0.0):
    """A point on a path: along (0..length), lat (across, + = right)."""
    t = min(max(along / length, 0.0), 1.0) * (len(frs) - 1)
    i = min(int(t), len(frs) - 2); f = t - i
    p = frs[i][0] * (1 - f) + frs[i + 1][0] * f
    r = frs[i][2] * (1 - f) + frs[i + 1][2] * f
    return p + r * lat + np.array([0, 0, up])

def paint_walker(sc, frs, length, along, lat, size, col, stretch=1.5):
    """The pictogram painted on a path's top, head towards the start (upright seen from the end), drawn
    longer along the path like road paint (stretch)."""
    for poly in walker_polys():
        sc.face([path_point(frs, length, along - (y - 0.5) * size * stretch, lat - x * size, 0.03) for (x, y) in poly], col, layer=2, two=True, shade=False)

def walking_path(sc, width, length=10.0, bend=0.55, h=1.0, walker=None, side_lines=False, middle_line=False, frame_width=None):
    """Walking path: concrete-block top, concrete sides; walker="middle" paints the pictogram on it."""
    frs = frames_arc(length, bend, 32)
    hw = width / 2
    top = np.array([196, 192, 184.0])
    extrude(sc, frs, [(-hw, 0), (hw, 0)], top, closed=False, two=True)
    extrude(sc, frs, [(hw, 0), (hw, -h)], CONCRETE * 0.85, closed=False, two=True)
    extrude(sc, frs, [(-hw, -h), (-hw, 0)], CONCRETE * 0.85, closed=False, two=True)
    cap(sc, frs[0], [(-hw, 0), (hw, 0), (hw, -h), (-hw, -h)], CONCRETE * 0.85)
    cap(sc, frs[-1], [(-hw, 0), (hw, 0), (hw, -h), (-hw, -h)], CONCRETE * 0.85)
    if side_lines:
        line_strip(sc, frs, -hw + 0.3, 0.07, WHITE)
        line_strip(sc, frs, hw - 0.3, 0.07, WHITE)
    if middle_line:
        line_strip(sc, frs, 0.0, 0.06, PAINT_WHITE, shade=False)
    if walker:
        # Same size on every path (1.8 m tall figure); on the 4 m path in the right lane.
        lat = 0.0 if walker == "middle" else width / 4.0
        paint_walker(sc, frs, length, length * 0.55, lat, 1.8, PAINT_WHITE)
    if frame_width:
        # Frame as if the path were frame_width wide, so icons of different widths share one scale.
        fw = frame_width / 2
        for fr in (frs[0], frs[len(frs) // 2], frs[-1]):
            sc.bounds += [at(fr, -fw, 0), at(fr, fw, 0), at(fr, -fw, -h), at(fr, fw, -h)]
    return frs

def guard_rail(sc):
    frs = frames_arc(6.0, 0.35, 24)
    beam = [(-0.08, 0.95), (0.08, 0.95), (0.2, 0.8), (0.08, 0.6), (-0.08, 0.6), (0.0, 0.78)]
    extrude(sc, frs, beam, STEEL)
    cap(sc, frs[0], beam, STEEL); cap(sc, frs[-1], beam, STEEL)
    for i in (0, 8, 16, 24):
        p, f, r = frs[i]
        box(sc, p - r * 0.14 + np.array([0, 0, 0.47]), 0.09, 0.09, 0.47, DARKSTEEL, fwd=f)

def jersey(sc):
    prof = [(-0.3, 0), (0.3, 0), (0.28, 0.08), (0.12, 0.3), (0.075, 0.8), (-0.075, 0.8), (-0.12, 0.3), (-0.28, 0.08)]
    for k in range(3):
        frs = frames_arc(1.9, 0.0, 2)
        frs = [(p + np.array([k * 2.0, 0, 0]), f, r) for p, f, r in frs]
        extrude(sc, frs, prof, CONCRETE)
        cap(sc, frs[0], prof, CONCRETE); cap(sc, frs[-1], prof, CONCRETE)

def grid_fence(sc, scale=1.0):
    length = 6.0
    hgt = 2.0 * scale
    post = 0.05 * (1.6 if scale > 1 else 1.0) * 2
    tube = 0.03 * (1.5 if scale > 1 else 1.0) * 2
    frs = frames_arc(length, 0.0, 2)
    # mesh panel: diamond wires as thin quads in the fence plane (x along, z up)
    cell = 0.32 * (1.3 if scale > 1 else 1.0)
    w = 0.025 * (1.3 if scale > 1 else 1.0)
    z0, z1 = 0.08, hgt
    def seg(a, b):
        (x0, zz0), (x1, zz1) = a, b
        sc.quad((x0 - w, 0, zz0), (x0 + w, 0, zz0), (x1 + w, 0, zz1), (x1 - w, 0, zz1), DARKSTEEL, layer=0, two=True, shade=False)
    n = int((length + hgt) / cell) + 2
    for k in range(-n, n + 1):
        for sgn in (1, -1):
            # line x = c + sgn*z, clipped to the panel rectangle
            c = k * cell
            pts = []
            for z in (z0, z1):
                x = c + sgn * z
                pts.append((x, z))
            # clip in x
            (xa, za), (xb, zb) = pts
            def clipx(xa, za, xb, zb, lo, hi):
                if xa > xb: xa, za, xb, zb = xb, zb, xa, za
                if xb < lo or xa > hi: return None
                if xa < lo: za = za + (zb - za) * (lo - xa) / (xb - xa); xa = lo
                if xb > hi: zb = za + (zb - za) * (hi - xa) / (xb - xa); xb = hi
                return (xa, za), (xb, zb)
            r = clipx(xa, za, xb, zb, 0.0, length)
            if r: seg(*r)
    for z in (z0, z1):
        box(sc, (length / 2, 0, z), length / 2, tube, tube, STEEL)
    for x in np.linspace(0, length, 3):
        box(sc, (x, 0, (hgt + 0.05) / 2), post, post, (hgt + 0.05) / 2, STEEL)

def intersection(sc):
    hw, h, e, stub = 4.0, 1.0, 0.9, 7.0
    R = 3.0
    # outline: plus shape with rounded inner corners (counter clockwise)
    pts = []
    def arc(cx, cy, a0, a1, n=8):
        for i in range(n + 1):
            a = a0 + (a1 - a0) * i / n
            pts.append((cx + R * math.cos(a), cy + R * math.sin(a)))
    pts += [(hw + stub, -hw), (hw + stub, hw)]
    arc(hw + R, hw + R, -math.pi / 2, -math.pi)
    pts += [(hw, hw + stub), (-hw, hw + stub)]
    arc(-hw - R, hw + R, 0, -math.pi / 2)
    pts += [(-hw - stub, hw), (-hw - stub, -hw)]
    arc(-hw - R, -hw - R, math.pi / 2, 0)
    pts += [(-hw, -hw - stub), (hw, -hw - stub)]
    arc(hw + R, -hw - R, math.pi, math.pi / 2)
    P3 = [np.array([x, y, 0.0]) for x, y in pts]
    for i in range(len(P3)):
        sc.face([np.zeros(3), P3[i], P3[(i + 1) % len(P3)]], ASPHALT, two=True)
        a, b = P3[i], P3[(i + 1) % len(P3)]
        sc.quad(a, b, b - [0, 0, h], a - [0, 0, h], CONCRETE, two=True)
    # side lines: road edges along the stubs + inner curb arcs, inset by e
    def lstrip(path):
        for i in range(len(path) - 1):
            a, b = np.array(path[i] + (0.02,)), np.array(path[i + 1] + (0.02,))
            d = b - a; d[2] = 0; d /= np.linalg.norm(d)
            r = np.array([d[1], -d[0], 0]) * 0.16
            sc.quad(a - r, a + r, b + r, b - r, YELLOW, layer=1, two=True)
    for sx, sy, a0 in ((1, 1, -math.pi / 2), (-1, 1, 0), (-1, -1, math.pi / 2), (1, -1, math.pi)):
        cx, cy = sx * (hw + R), sy * (hw + R)
        rr = R + e
        path = []
        # straight bit along x-stub, arc, straight along y-stub
        for i in range(11):
            a = a0 + (-math.pi / 2) * i / 10
            path.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
        start = (sx * (hw + stub), sy * (hw - e))
        end = (sx * (hw - e), sy * (hw + stub))
        lstrip([start] + path + [end] if (sx * sy) > 0 else [end] + path + [start])

def fence_wall(sc, kind="concrete"):
    # One straight 4 m tall fence wall piece, 0.5 m thick, with the look of its material on the face.
    L, H, T = 6.0, 4.0, 0.5
    y = T / 2 + 0.01   # visible face (towards the camera)
    def stripe(x0, x1, z0, z1, col):
        sc.quad((x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1), col, layer=1, two=True, shade=False)
    if kind == "concrete":
        base = CONCRETE * 0.92
        box(sc, (L / 2, 0, H / 2), L / 2, T / 2, H / 2, base)
        seam = CONCRETE * 0.62
        stripe(0.0, L, 0.45, 0.52, seam)          # bottom band
        stripe(0.0, L, 1.96, 2.04, seam)          # middle seam
        stripe(0.0, L, 3.50, 3.57, seam)          # top band
        for x in (0.3, L - 0.3):
            stripe(x - 0.04, x + 0.04, 0.5, 3.55, seam)
        for x in (1.6, 4.4):                      # tie holes
            for z in (1.7, 2.3):
                stripe(x - 0.08, x + 0.08, z - 0.08, z + 0.08, seam)
    elif kind == "steel":
        dark = np.array([58, 60, 64.0])
        panel = np.array([150, 156, 164.0])
        box(sc, (L / 2, 0, 0.2), L / 2, T / 2, 0.2, dark)            # kick rail
        box(sc, (L / 2, 0, H - 0.12), L / 2, T / 2, 0.12, dark)      # top rail
        box(sc, (L / 2, 0, H / 2), L / 2, 0.1, H / 2 - 0.3, panel)   # ribbed sheet
        yy = 0.11
        rib = panel * 0.62
        z = 0.55
        while z < H - 0.3:
            sc.quad((0, yy, z), (L, yy, z), (L, yy, z + 0.06), (0, yy, z + 0.06), rib, layer=1, two=True, shade=False)
            z += 0.24
    elif kind == "sign":
        # The sign wall: metal body (the grid fence's metal), a black design face with orange text on the road side.
        metal = np.array([128, 133, 140.0])
        box(sc, (L / 2, 0, H / 2), L / 2, T / 2, H / 2, metal)
        b = 0.12  # metal rim left round the face
        black = np.array([14, 15, 18.0])
        stripe(b, L - b, b, H - b, black)
        from PIL import ImageFont
        tw, th = 1200, 800
        tex = Image.new("RGBA", (tw, th), (0, 0, 0, 0))
        dt = ImageDraw.Draw(tex)
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf", 330)
        text = "FICSIT"
        box_ = dt.textbbox((0, 0), text, font=font)
        w_, h_ = box_[2] - box_[0], box_[3] - box_[1]
        dt.text(((tw - w_) / 2 - box_[0], (th - h_) / 2 - box_[1]), text, font=font, fill=(250, 149, 73, 255))
        y2 = y + 0.005
        sc.overlays.append(((b, y2, H - b), (L - 2 * b, 0, 0), (0, 0, -(H - 2 * b)), tex))
    else:  # ficsit
        base = ORANGE * 0.95
        box(sc, (L / 2, 0, H / 2), L / 2, T / 2, H / 2, base)
        groove = ORANGE * 0.62
        for x in (0.45, 3.45, 4.45):
            stripe(x - 0.09, x - 0.04, 0.0, H, groove)
            stripe(x + 0.04, x + 0.09, 0.0, H, groove)


def cement_mixer(sc):
    """Gap Fill: a site cement mixer standing on a road tile, in the set's style: few flat faces, an 8-sided orange
    drum tilted up with its mouth open, square legs, 8-sided wheels (like the gantry on its road)."""
    road(sc, 12, 1, length=12.0, bend=0.0)
    DARK = np.array([92, 96, 104.0]); GREY = np.array([150, 154, 162.0]); INSIDE = np.array([34, 30, 28.0])
    def frame_of(d):
        d = np.array(d, float); d /= np.linalg.norm(d)
        a = np.array([0, 0, 1.0]) if abs(d[2]) < 0.9 else np.array([1.0, 0, 0])
        u = np.cross(d, a); u /= np.linalg.norm(u)
        return d, u, np.cross(d, u)
    def ring(c, d, u, v, r, n, rot=0.0):
        return [c + (u * math.cos(2 * math.pi * (i + rot) / n) + v * math.sin(2 * math.pi * (i + rot) / n)) * r for i in range(n)]
    def bar(p0, p1, r, col):  # a square bar between two points
        p0 = np.array(p0, float); p1 = np.array(p1, float)
        d, u, v = frame_of(p1 - p0)
        a, b = ring(p0, d, u, v, r, 4, 0.5), ring(p1, d, u, v, r, 4, 0.5)
        for i in range(4):
            j = (i + 1) % 4
            sc.quad(a[i], a[j], b[j], b[i], col, layer=1)
    def wheel(c, r, w, col):
        d, u, v = frame_of((0, 1, 0))
        c = np.array(c, float)
        a, b = ring(c - d * w, d, u, v, r, 8, 0.5), ring(c + d * w, d, u, v, r, 8, 0.5)
        sc.face(a, col, layer=1, two=True)
        sc.face(b, col, layer=1, two=True)
        for i in range(8):
            j = (i + 1) % 8
            sc.quad(a[i], a[j], b[j], b[i], col, layer=1)
        sc.face(ring(c + d * (w + 0.02), d, u, v, r * 0.4, 8, 0.5), GREY, layer=2, two=True)
    x0 = 6.0
    pivot = np.array([x0, 0.0, 6.0])
    # Stand: two legs from the axle up to the pivot, one at the back; the axle; the wheels; the motor box.
    for y in (-3.2, 3.2):
        bar((x0 + 1.6, y, 1.7), pivot, 0.4, GREY)
        wheel((x0 + 1.6, y * 1.15, 1.7), 1.7, 0.5, DARK)
    bar((x0 - 4.0, 0.0, 0.1), pivot, 0.4, GREY)
    bar((x0 + 1.6, -3.4, 1.7), (x0 + 1.6, 3.4, 1.7), 0.3, GREY)
    box(sc, (x0 - 1.6, 0.0, 4.6), 1.1, 0.9, 0.8, GREY, layer=1)
    # Drum: 8 sides, bottom, belly, mouth; tilted up towards the viewer.
    d, u, v = frame_of((0.55, -0.55, 0.62))
    base = pivot - d * 1.0
    prof = [(0.0, 2.2), (1.2, 3.1), (4.8, 3.1), (6.8, 1.8)]
    rings = [ring(base + d * t, d, u, v, r, 8, 0.5) for t, r in prof]
    for k in range(len(rings) - 1):
        for i in range(8):
            j = (i + 1) % 8
            sc.quad(rings[k][i], rings[k][j], rings[k + 1][j], rings[k + 1][i], ORANGE, layer=1)
    sc.face(rings[0], ORANGE, layer=1, two=True)
    sc.face(ring(base + d * 6.8, d, u, v, 1.5, 8, 0.5), INSIDE, layer=2, two=True, shade=False)

def road_cut(sc):
    """A road cut in two: the pieces pulled apart, an orange outline in the gap where it was cut."""
    frs = frames_arc(20.0, 0.5, 40)
    k = 20
    width, lanes, h, gap = 16, 2, 1.0, 3.6
    hw = width / 2
    first = frs[:k + 1]
    f = frs[k][1]
    second = [(p + f * gap, fw, r) for p, fw, r in frs[k:]]
    for part in (first, second):
        extrude(sc, part, [(-hw, 0), (hw, 0)], ASPHALT, closed=False, two=True)
        extrude(sc, part, [(hw, 0), (hw, -h)], CONCRETE, closed=False, two=True)
        extrude(sc, part, [(-hw, -h), (-hw, 0)], CONCRETE, closed=False, two=True)
        cap(sc, part[0], [(-hw, 0), (hw, 0), (hw, -h), (-hw, -h)], CONCRETE)
        cap(sc, part[-1], [(-hw, 0), (hw, 0), (hw, -h), (-hw, -h)], CONCRETE)
        e = 0.9
        line_strip(sc, part, -hw + e, 0.16, YELLOW)
        line_strip(sc, part, hw - e, 0.16, YELLOW)
        line_strip(sc, part, 0.0, 0.13, WHITE, dash=(2, 0))
    # Cut outline, standing in the gap (bright orange, unshaded, drawn on top).
    p, fw, r = frs[k]
    c = p + fw * (gap / 2)
    t = 0.35
    def bar(a, b):
        a = np.array(a, float); b = np.array(b, float)
        d = (b - a) / np.linalg.norm(b - a)
        n = np.cross(d, fw); n = n / np.linalg.norm(n) * t
        sc.face([a - n, a + n, b + n, b - n], ORANGE_CUT, layer=2, two=True, shade=False)
    top_l = c - r * (hw + 0.3) + np.array([0, 0, 0.3]); top_r = c + r * (hw + 0.3) + np.array([0, 0, 0.3])
    bot_l = top_l - np.array([0, 0, h + 0.6]); bot_r = top_r - np.array([0, 0, h + 0.6])
    bar(top_l, top_r); bar(top_r, bot_r); bar(bot_r, bot_l); bar(bot_l, top_l)


def cut_glyph(path):
    """Cut Road icon: white flat glyph, a road split by a cut line."""
    from PIL import Image as _I, ImageDraw as _D
    n = 1024
    img = _I.new("RGBA", (n, n), (0, 0, 0, 0))
    d = _D.Draw(img)
    W = (255, 255, 255, 255)
    # Two road pieces with a gap, side lines and a dashed middle cut out.
    for y0, y1 in ((120, 440), (584, 904)):
        d.rounded_rectangle([250, y0, 774, y1], radius=40, fill=W)
        d.rectangle([300, y0 + (0 if y0 > 200 else 40), 320, y1 - (40 if y0 > 200 else 0)], fill=(0, 0, 0, 0))
        d.rectangle([704, y0 + (0 if y0 > 200 else 40), 724, y1 - (40 if y0 > 200 else 0)], fill=(0, 0, 0, 0))
        for yy in range(y0 + 50, y1 - 40, 120):
            d.rectangle([497, yy, 527, yy + 60], fill=(0, 0, 0, 0))
    # Cut line across the gap, wider than the road.
    d.rounded_rectangle([140, 492, 884, 532], radius=20, fill=W)
    img = img.resize((512, 512), _I.LANCZOS)
    img.save(path)

def split_glyph(path):
    """Split Road icon: white flat glyph, a road split lengthwise by a vertical line."""
    from PIL import Image as _I, ImageDraw as _D
    n = 1024
    img = _I.new("RGBA", (n, n), (0, 0, 0, 0))
    d = _D.Draw(img)
    W = (255, 255, 255, 255)
    C = (0, 0, 0, 0)
    # Two road halves side by side with a gap; each has an edge line and a dashed line cut out.
    for x0, x1 in ((200, 462), (562, 824)):
        d.rounded_rectangle([x0, 120, x1, 904], radius=40, fill=W)
        outer = x0 + 40 if x0 < 500 else x1 - 60
        d.rectangle([outer, 160, outer + 20, 864], fill=C)
        mid = (x0 + x1) // 2 + (20 if x0 < 500 else -20)
        for yy in range(170, 850, 120):
            d.rectangle([mid - 15, yy, mid + 15, yy + 60], fill=C)
    # Split line along the middle, longer than the road.
    d.rounded_rectangle([492, 50, 532, 974], radius=20, fill=W)
    img = img.resize((512, 512), _I.LANCZOS)
    img.save(path)


def merge_glyph(path):
    """Merge Road icon: white flat glyph, two road halves side by side pushed together by two arrows
    (the opposite of Split Road: no split line, the halves almost touch)."""
    from PIL import Image as _I, ImageDraw as _D
    n = 1024
    img = _I.new("RGBA", (n, n), (0, 0, 0, 0))
    d = _D.Draw(img)
    W = (255, 255, 255, 255)
    C = (0, 0, 0, 0)
    # Two road halves, a thin seam between them; each has an edge line and a dashed line cut out.
    for x0, x1 in ((292, 504), (520, 732)):
        d.rounded_rectangle([x0, 120, x1, 904], radius=36, fill=W)
        outer = x0 + 34 if x0 < 500 else x1 - 52
        d.rectangle([outer, 160, outer + 18, 864], fill=C)
        mid = (x0 + x1) // 2 + (16 if x0 < 500 else -16)
        for yy in range(170, 850, 120):
            d.rectangle([mid - 13, yy, mid + 13, yy + 60], fill=C)
    # Arrows outside the halves, pointing in: pushed together.
    cy = 512
    for side in (-1, 1):
        tip = 512 + side * 245          # just outside the road edge
        back = 512 + side * 470
        d.rounded_rectangle([min(back, tip + side * 70), cy - 26, max(back, tip + side * 70), cy + 26], radius=14, fill=W)
        d.polygon([(tip, cy), (tip + side * 110, cy - 95), (tip + side * 110, cy + 95)], fill=W)
    img = img.resize((512, 512), _I.LANCZOS)
    img.save(path)


def join_glyph(path):
    """Join Road icon: white flat glyph, two road pieces end to end pushed together by two arrows
    along the road (Merge Road's arrows push from the sides; Cut Road has a cut line instead)."""
    from PIL import Image as _I, ImageDraw as _D
    n = 1024
    img = _I.new("RGBA", (n, n), (0, 0, 0, 0))
    d = _D.Draw(img)
    W = (255, 255, 255, 255)
    C = (0, 0, 0, 0)
    # Two road pieces in line with a thin seam; side lines and a dashed middle cut out.
    for y0, y1 in ((262, 504), (520, 762)):
        d.rounded_rectangle([250, y0, 774, y1], radius=36, fill=W)
        top_pad = 36 if y0 < 500 else 0
        bot_pad = 0 if y0 < 500 else 36
        d.rectangle([298, y0 + top_pad, 316, y1 - bot_pad], fill=C)
        d.rectangle([708, y0 + top_pad, 726, y1 - bot_pad], fill=C)
        for yy in range(y0 + 40, y1 - 40, 110):
            d.rectangle([498, yy, 526, yy + 55], fill=C)
    # Arrows beyond the ends, pointing in along the road.
    cx = 512
    for side in (-1, 1):
        tip = 512 + side * 270           # just beyond the road end
        back = 512 + side * 480
        d.rounded_rectangle([cx - 26, min(back, tip + side * 70), cx + 26, max(back, tip + side * 70)], radius=14, fill=W)
        d.polygon([(cx, tip), (cx - 95, tip + side * 110), (cx + 95, tip + side * 110)], fill=W)
    img = img.resize((512, 512), _I.LANCZOS)
    img.save(path)


def adjust_glyph(path):
    """Adjust Curvature icon: white flat glyph, a curved road ending in a grab circle."""
    from PIL import Image as _I, ImageDraw as _D
    import math
    n = 1024
    img = _I.new("RGBA", (n, n), (0, 0, 0, 0))
    d = _D.Draw(img)
    W = (255, 255, 255, 255)
    C = (0, 0, 0, 0)
    # Quadratic curve from bottom-left, bending to the right.
    P0, P1, P2 = (190.0, 880.0), (190.0, 330.0), (680.0, 330.0)
    def pt(t):
        a = (1 - t) ** 2; b = 2 * t * (1 - t); c = t * t
        return (a * P0[0] + b * P1[0] + c * P2[0], a * P0[1] + b * P1[1] + c * P2[1])
    steps = 80
    for i in range(steps + 1):
        x, y = pt(i / steps)
        d.ellipse([x - 78, y - 78, x + 78, y + 78], fill=W)
    # Dashed middle line cut out.
    for i in range(4, steps - 6, 9):
        for k in range(5):
            x, y = pt((i + k) / steps)
            d.ellipse([x - 11, y - 11, x + 11, y + 11], fill=C)
    # Grab circle at the end: ring with a dot.
    cx, cy = 790, 330
    d.ellipse([cx - 120, cy - 120, cx + 120, cy + 120], fill=W)
    d.ellipse([cx - 82, cy - 82, cx + 82, cy + 82], fill=C)
    d.ellipse([cx - 40, cy - 40, cx + 40, cy + 40], fill=W)
    img = img.resize((512, 512), _I.LANCZOS)
    img.save(path)


def fence_cut_glyph(path):
    """Cut Fence icon: white flat glyph, a brick wall split by a vertical cut line."""
    from PIL import Image as _I, ImageDraw as _D
    n = 1024
    img = _I.new("RGBA", (n, n), (0, 0, 0, 0))
    d = _D.Draw(img)
    W = (255, 255, 255, 255)
    gap, mortar = 70, 22
    rows = [(250, 390), (390, 530), (530, 670), (670, 810)]
    for r, (y0, y1) in enumerate(rows):
        # Bricks offset every other row; each half of the wall stops short of the cut.
        off = 0 if r % 2 == 0 else 95
        x = 120 - off
        while x < 904:
            x0, x1 = max(x, 120), min(x + 190, 904)
            for lo, hi in ((120, 512 - gap), (512 + gap, 904)):
                a, b = max(x0, lo), min(x1, hi)
                if b - a > 70:
                    d.rounded_rectangle([a + mortar / 2, y0 + mortar / 2, b - mortar / 2, y1 - mortar / 2], radius=12, fill=W)
            x += 190
    # Vertical cut line, taller than the wall.
    d.rounded_rectangle([492, 150, 532, 910], radius=20, fill=W)
    img = img.resize((512, 512), _I.LANCZOS)
    img.save(path)


def fence_join_glyph(path):
    """Join Fence icon: white flat glyph, two brick wall pieces end to end with a thin straight seam,
    pushed together by two arrows along the wall (Cut Fence has a cut line through a gap instead)."""
    from PIL import Image as _I, ImageDraw as _D
    n = 1024
    img = _I.new("RGBA", (n, n), (0, 0, 0, 0))
    d = _D.Draw(img)
    W = (255, 255, 255, 255)
    seam, mortar, brick = 14, 20, 150
    inner, outer = 512, 314           # each half runs from the seam out to 512 +- 314 (two bricks)
    rows = [(290, 400), (400, 510), (510, 620), (620, 730)]
    for r, (y0, y1) in enumerate(rows):
        for side in (-1, 1):
            # Bricks laid from the seam outwards, every other row starting with a half brick.
            pos = seam
            first = brick // 2 if r % 2 else brick
            length = first
            while pos < outer:
                end = min(pos + length, outer)
                a, b = inner + side * pos, inner + side * end
                lo, hi = min(a, b), max(a, b)
                if hi - lo > 40:
                    d.rounded_rectangle([lo + mortar / 2, y0 + mortar / 2, hi - mortar / 2, y1 - mortar / 2], radius=10, fill=W)
                pos = end
                length = brick
    # Arrows beyond the wall ends, pointing in along the wall.
    cy = 510
    for side in (-1, 1):
        tip = 512 + side * 340           # just beyond the wall end
        back = 512 + side * 505
        d.rounded_rectangle([min(back, tip + side * 70), cy - 24, max(back, tip + side * 70), cy + 24], radius=14, fill=W)
        d.polygon([(tip, cy), (tip + side * 100, cy - 88), (tip + side * 100, cy + 88)], fill=W)
    img = img.resize((512, 512), _I.LANCZOS)
    img.save(path)


def tab_glyph(path):
    """Road Tools tab icon: white glyph of a road in perspective, like /┆\\ ."""
    from PIL import Image as _I, ImageDraw as _D
    n = 1024
    img = _I.new("RGBA", (n, n), (0, 0, 0, 0))
    d = _D.Draw(img)
    W = (255, 255, 255, 255)
    w = 70
    # Edges converging towards the top.
    d.line([(150, 930), (420, 110)], fill=W, width=w)
    d.line([(874, 930), (604, 110)], fill=W, width=w)
    for x, y in ((150, 930), (420, 110), (874, 930), (604, 110)):
        d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=W)
    # Dashed centre line, dashes shorter and thinner towards the top.
    for y0, y1, t in ((800, 930, 64), (560, 680, 52), (360, 450, 42), (200, 260, 34)):
        d.rounded_rectangle([512 - t / 2, y0, 512 + t / 2, y1], radius=t / 2, fill=W)
    img = img.resize((256, 256), _I.LANCZOS)
    img.save(path)


def sign_gantry(sc):
    """A straight road with a gantry across it: two columns on dark feet, a beam, a classic green sign."""
    road(sc, 16, 2, length=18.0, bend=0.0)
    GREEN = np.array([22, 128, 74.0]); DARK = np.array([58, 60, 66.0])
    x = 9.0
    for y in (-7.5, 7.5):
        box(sc, (x, y, 1.05), 0.6, 0.6, 1.05, DARK, layer=1)
        box(sc, (x, y, 5.1), 0.5, 0.42, 3.05, ORANGE, layer=1)
    box(sc, (x, 0, 9.6), 0.42, 8.4, 0.5, ORANGE, layer=1)
    box(sc, (x + 0.52, 0, 7.5), 0.07, 4.4, 1.75, GREEN, layer=1)
    xf = x + 0.6
    W = np.array([240, 240, 236.0])
    def deco(y0, y1, z0, z1):
        sc.quad((xf, y0, z0), (xf, y1, z0), (xf, y1, z1), (xf, y0, z1), W, layer=2, two=True, shade=False)
    deco(-4.1, 4.1, 5.85, 5.95); deco(-4.1, 4.1, 9.05, 9.15)      # border
    deco(-4.1, -4.0, 5.85, 9.15); deco(4.0, 4.1, 5.85, 9.15)
    deco(-3.2, 1.2, 8.1, 8.5); deco(-3.2, -0.6, 7.2, 7.55)         # "text"
    deco(2.0, 3.2, 6.35, 8.5)                                       # arrow shaft
    sc.quad((xf, 1.5, 6.9), (xf, 3.7, 6.9), (xf, 2.6, 6.2), (xf, 2.6, 6.2), W, layer=2, two=True, shade=False)


def marking_icon(sc, kind):
    """A straight road with a big white arrow (or the digit 5) painted on it."""
    road(sc, 8, 1, length=18.0, bend=0.0)
    Z = 0.06; k = 2.9; cx = 9.0
    def rect(x0, x1, y0, y1):
        sc.quad((cx + x0 * k, y0 * k, Z), (cx + x1 * k, y0 * k, Z), (cx + x1 * k, y1 * k, Z), (cx + x0 * k, y1 * k, Z), WHITE, layer=2, two=True, shade=False)
    def tri(a, b, c):
        sc.face([(cx + a[0] * k, a[1] * k, Z), (cx + b[0] * k, b[1] * k, Z), (cx + c[0] * k, c[1] * k, Z)], WHITE, layer=2, two=True, shade=False)
    if kind == "vehicle":
        # A generic utility cart, side view (its top points forward along the lane), not the game's art.
        def g(u0, u1, v0, v1):  # u across the lane (front of the cart at +u), v up the picture = forward
            rect(v0, v1, u0, u1)
        g(-0.6, 0.6, -0.5, -0.3)      # chassis
        g(-0.58, -0.1, -0.3, 0.1)     # cargo box
        g(-0.05, 0.58, -0.3, -0.12)   # seat / front body
        g(0.0, 0.06, -0.12, 0.62)     # rear post
        g(0.5, 0.56, -0.12, 0.62)     # front pillar
        g(-0.05, 0.6, 0.56, 0.68)     # roof
        g(-0.5, -0.22, -0.72, -0.5)   # rear wheel
        g(0.22, 0.5, -0.72, -0.5)     # front wheel
        return
    if kind == "arrow":
        rect(-1.1, 0.55, -0.14, 0.14)
        tri((0.55, -0.5), (1.1, 0.0), (0.55, 0.5))
    else:
        T = 0.24; L, H, Lo, Hi, M = -0.9, 0.9, -0.5, 0.5, 0.0
        rect(H - T, H, Lo, Hi); rect(M - T / 2, M + T / 2, Lo, Hi); rect(L, L + T, Lo, Hi)   # top, middle, bottom
        rect(M + T / 2, H - T, Lo, Lo + T)                                                    # upper left
        rect(L + T, M - T / 2, Hi - T, Hi)                                                    # lower right

what = sys.argv[1]; out = sys.argv[2]
if what == "cutglyph":
    cut_glyph(out); sys.exit(0)
if what == "splitglyph":
    split_glyph(out); sys.exit(0)
if what == "joinglyph":
    join_glyph(out); sys.exit(0)
if what == "mergeglyph":
    merge_glyph(out); sys.exit(0)
if what == "adjustglyph":
    adjust_glyph(out); sys.exit(0)
if what == "fencecutglyph":
    fence_cut_glyph(out); sys.exit(0)
if what == "fencejoinglyph":
    fence_join_glyph(out); sys.exit(0)
if what == "tabglyph":
    tab_glyph(out); sys.exit(0)
sc = Scene()
if what == "road1": road(sc, 8, 1)
elif what == "road2": road(sc, 16, 2)
elif what == "road3": road(sc, 24, 3)
elif what == "road4": road(sc, 32, 4)
elif what == "walk2": walking_path(sc, 2, length=13.0, walker="middle", frame_width=4)
elif what == "walk4": walking_path(sc, 4, length=13.0, walker="right", middle_line=True, frame_width=4)
elif what == "walk2b": walking_path(sc, 2, length=13.0)
elif what == "walk4b": walking_path(sc, 4, length=13.0)
elif what == "roadcut": road_cut(sc)
elif what == "guard": guard_rail(sc)
elif what == "jersey": jersey(sc)
elif what == "grid": grid_fence(sc, 1.0)
elif what == "gridtall": grid_fence(sc, 2.0)
elif what == "cross": intersection(sc)
elif what == "gantry": sign_gantry(sc)
elif what == "lanearrow": marking_icon(sc, "arrow")
elif what == "lanenumber": marking_icon(sc, "digit")
elif what == "lanevehicle": marking_icon(sc, "vehicle")
elif what == "fencewall": fence_wall(sc, "concrete")
elif what == "fencewall_ficsit": fence_wall(sc, "ficsit")
elif what == "fencewall_steel": fence_wall(sc, "steel")
elif what == "fencewall_sign": fence_wall(sc, "sign")
elif what == "gapfill": cement_mixer(sc)
render(sc, out)
if what in ("walk2b", "walk4b"):
    badge_walker(out)
