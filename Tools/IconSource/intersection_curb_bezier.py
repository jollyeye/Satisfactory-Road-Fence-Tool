"""Intersection curbs: current (one quadratic or straight fallback) vs the roads' quadratic algorithm.

Ports of RMGeometry BuildIntersection (outline part) and MakeQuadratic. Draws edge cases side by side:
grey = roads, red dashed = current curb, orange = curb with the roads' algorithm.
Run: python intersection_curb_bezier.py out.png
"""
import math
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon


def cross(a, b):
    return a[0] * b[1] - a[1] * b[0]


def add(a, b):
    return (a[0] + b[0], a[1] + b[1])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1])


def mul(a, s):
    return (a[0] * s, a[1] * s)


def norm(a):
    l = math.hypot(*a)
    return (a[0] / l, a[1] / l) if l > 1e-9 else (0.0, 0.0)


def dist(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def sorted_ends(ends):
    cx = sum(e[0][0] for e in ends) / len(ends)
    cy = sum(e[0][1] for e in ends) / len(ends)
    c = (cx, cy)
    out = []
    for centre, inward, width in ends:
        i = norm(inward)
        right = (-i[1], i[0])  # cross(up, in)
        p1 = sub(centre, mul(right, width / 2))
        p2 = add(centre, mul(right, width / 2))
        first_p1 = cross(sub(p1, c), sub(p2, c)) > 0
        out.append((math.atan2(centre[1] - cy, centre[0] - cx), p1 if first_p1 else p2, p2 if first_p1 else p1, i))
    out.sort(key=lambda e: e[0])
    return out


def curb_current(p, dp, q, dq, n=16):
    denom = cross(dp, dq)
    control = mul(add(p, q), 0.5)
    if abs(denom) > 1e-3:
        pq = sub(q, p)
        t = cross(pq, dq) / denom
        u = cross(pq, dp) / denom
        limit = 4 * dist(p, q) + 100
        if 0 < t < limit and 0 < u < limit:
            control = add(p, mul(dp, t))
    pts = []
    for k in range(1, n):
        t = k / n
        a, b, c = (1 - t) ** 2, 2 * (1 - t) * t, t * t
        pts.append((p[0] * a + control[0] * b + q[0] * c, p[1] * a + control[1] * b + q[1] * c))
    return pts


def make_quadratic(p0, u0, p1, u1):
    """MakeQuadratic: list of quadratic pieces (A, C, B)."""
    d = sub(p1, p0)
    L = math.hypot(*d)
    if L < 1:
        return [(p0, mul(add(p0, p1), 0.5), p1)]
    u0, u1 = norm(u0), norm(u1)

    def da(a, b):
        x = a - b
        while x > math.pi:
            x -= 2 * math.pi
        while x < -math.pi:
            x += 2 * math.pi
        return x
    chord = math.atan2(d[1], d[0])
    a0 = math.atan2(u0[1], u0[0])
    a1 = math.atan2(u1[1], u1[0])
    d0, d1 = da(a0, chord), da(a1, chord)
    inflection = math.sin(d0) * math.sin(d1) > 0.04
    lim = math.radians(68)
    wide = (abs(d0) > lim or abs(d1) > lim or abs(da(a1, a0)) > math.radians(110)
            or math.cos(d0) <= 0.2 or math.cos(d1) <= 0.2)
    det = u0[0] * u1[1] - u0[1] * u1[0]
    if not inflection and not wide and abs(det) > 0.08:
        s = (d[0] * u1[1] - d[1] * u1[0]) / det
        t = (u0[0] * d[1] - u0[1] * d[0]) / det
        if L * 0.04 < s < L * 1.35 and L * 0.04 < t < L * 1.35 and max(s / t, t / s) <= 3:
            return [(p0, add(p0, mul(u0, s)), p1)]

    def arm(diff):
        if inflection:
            return L * 0.38
        return L * (0.35 + 0.18 * min(1, abs(math.sin(diff))))
    c0 = add(p0, mul(u0, arm(d0)))
    c1 = sub(p1, mul(u1, arm(d1)))
    m = mul(add(c0, c1), 0.5)
    return [(p0, c0, m), (m, c1, p1)]


def make_fixed_ends(p0, s, p1, e):
    """MakeFixedEnds: Hermite tangents of a circular arc (what vehicle paths and rails use)."""
    L = max(dist(p0, p1), 1.0)
    s, e = norm(s), norm(e)
    theta = math.acos(max(-1, min(1, s[0] * e[0] + s[1] * e[1])))
    cq = math.cos(min(theta, math.radians(170)) * 0.25)
    m = L / max(cq * cq, 0.25)
    return mul(s, m), mul(e, m)


def hermite(p0, t0, p1, t1, n=24, ends=False):
    pts = []
    for k in range(0 if ends else 1, n + 1 if ends else n):
        t = k / n
        h00, h10 = 2 * t**3 - 3 * t**2 + 1, t**3 - 2 * t**2 + t
        h01, h11 = -2 * t**3 + 3 * t**2, t**3 - t**2
        pts.append((h00 * p0[0] + h10 * t0[0] + h01 * p1[0] + h11 * t1[0],
                    h00 * p0[1] + h10 * t0[1] + h01 * p1[1] + h11 * t1[1]))
    return pts


def curb_paths(p, dp, q, dq, n=24):
    t0, t1 = make_fixed_ends(p, dp, q, (-dq[0], -dq[1]))
    return hermite(p, t0, q, t1, n)


def curb_new(p, dp, q, dq, n=16):
    pieces = make_quadratic(p, dp, q, (-dq[0], -dq[1]))
    pts = []
    for i, (a, c, b) in enumerate(pieces):
        for k in range(1, n + 1):
            t = k / n
            if i == len(pieces) - 1 and k == n:
                break
            w0, w1, w2 = (1 - t) ** 2, 2 * (1 - t) * t, t * t
            pts.append((a[0] * w0 + c[0] * w1 + b[0] * w2, a[1] * w0 + c[1] * w1 + b[1] * w2))
    return pts


def outline(ends, curb):
    s = sorted_ends(ends)
    ring = []
    for i, cur in enumerate(s):
        nxt = s[(i + 1) % len(s)]
        ring += [cur[1], cur[2]]
        ring += curb(cur[2], cur[3], nxt[1], nxt[3])
    return ring


def self_intersects(ring):
    n = len(ring)

    def seg_x(a, b, c, d):
        d1, d2 = cross(sub(b, a), sub(c, a)), cross(sub(b, a), sub(d, a))
        d3, d4 = cross(sub(d, c), sub(a, c)), cross(sub(d, c), sub(b, c))
        return d1 * d2 < 0 and d3 * d4 < 0
    for i in range(n):
        for j in range(i + 2, n):
            if i == 0 and j == n - 1:
                continue
            if seg_x(ring[i], ring[(i + 1) % n], ring[j], ring[(j + 1) % n]):
                return True
    return False


def end(cx, cy, deg, width=1600):
    a = math.radians(deg)
    return ((cx, cy), (math.cos(a), math.sin(a)), width)


CASES = [
    ("Normal 4-way", [end(-1600, 0, 0), end(1600, 0, 180), end(0, -1600, 90), end(0, 1600, 270)]),
    ("T junction", [end(-1600, 0, 0), end(1600, 0, 180), end(0, -1600, 90)]),
    ("3-way, 60 / 150 / 150 degrees", [end(0, -1800, 90), end(-1500, 900, -30), end(1500, 900, 210)]),
    ("Two ends offset, facing each other", [end(-2000, -500, 0), end(2000, 500, 180)]),
    ("L bend", [end(-1600, 0, 0), end(0, -1600, 90)]),
    ("Narrow Y split", [end(0, -2000, 90), end(-1100, 1900, -80), end(1100, 1900, 260)]),
    ("Two roads side by side, same side", [end(-1000, -1600, 90), end(1000, -1600, 90), end(0, 1800, 270, 2400)]),
    ("Irregular 5-way, mixed widths",
     [end(-1900, -200, 5), end(1800, 300, 185, 2400), end(-300, -1900, 85), end(900, 1700, 250, 800), end(-1200, 1500, 300)]),
]


def road_stub(ax, e, length=1400):
    (cx, cy), d, w = e
    d = norm(d)
    r = (-d[1], d[0])
    back = sub((cx, cy), mul(d, length))
    pts = [add((cx, cy), mul(r, w / 2)), sub((cx, cy), mul(r, w / 2)), sub(back, mul(r, w / 2)), add(back, mul(r, w / 2))]
    ax.add_patch(Polygon(pts, closed=True, facecolor="#5a5c62", edgecolor="#3a3c40", lw=0.8))


def inside(pt, ring):
    x, y = pt
    c = False
    for i in range(len(ring)):
        (x1, y1), (x2, y2) = ring[i - 1], ring[i]
        if (y1 > y) != (y2 > y) and x < x1 + (y - y1) / (y2 - y1) * (x2 - x1):
            c = not c
    return c


def lane_paths(ends):
    """Vehicle paths through the junction like the snap-key junction tool: one lane each way per road."""
    out = []
    for a in ends:
        for b in ends:
            if a is b:
                continue
            (ac, ad, aw), (bc, bd, bw) = a, b
            ad, bd = norm(ad), norm(bd)
            ar, br = (-ad[1], ad[0]), (-bd[1], bd[0])
            start = add(ac, mul(ar, aw / 4))            # right of the way in
            stop = sub(bc, mul(br, bw / 4))             # right of the way out
            t0, t1 = make_fixed_ends(start, ad, stop, (-bd[0], -bd[1]))
            out.append(hermite(start, t0, stop, t1, 40, True))
    return out


def outside_count(paths, ring):
    bad = 0
    for path in paths:
        if any(not inside(p, ring) for p in path[2:-2]):
            bad += 1
    return bad


def main(out):
    fig, axes = plt.subplots(2, 4, figsize=(21, 11))
    fig.patch.set_facecolor("#f4f1ea")
    for ax, (title, ends) in zip(axes.flat, CASES):
        for e in ends:
            road_stub(ax, e)
        old = outline(ends, curb_current)
        new = outline(ends, curb_paths)
        paths = lane_paths(ends)
        ax.add_patch(Polygon(new, closed=True, facecolor="#f2c79a", edgecolor="none", alpha=0.7))
        for path in paths:
            xs, ys = zip(*path)
            ax.plot(xs, ys, "-", color="#2e86c1", lw=0.9, alpha=0.8)
        xo, yo = zip(*(old + old[:1]))
        xn, yn = zip(*(new + new[:1]))
        ax.plot(xo, yo, "--", color="#c0392b", lw=1.8, label="current curb")
        ax.plot(xn, yn, "-", color="#e67e22", lw=2.4, label="curb like the paths")
        ax.plot([], [], "-", color="#2e86c1", lw=1, label="vehicle paths")
        ob, nb = outside_count(paths, old), outside_count(paths, new)
        extra = "\nOUTLINE CROSSES ITSELF -> keeps current" if self_intersects(new) else ""
        ax.set_title(f"{title}\npaths leaving the curb: current {ob} / new {nb} of {len(paths)}{extra}", fontsize=11)
        ax.set_aspect("equal")
        ax.axis("off")
        ax.autoscale_view()
        x0, x1 = ax.get_xlim()
        y0, y1 = ax.get_ylim()
        ax.set_xlim(x0 - 300, x1 + 300)
        ax.set_ylim(y0 - 300, y1 + 300)
    axes.flat[0].legend(loc="upper left", fontsize=9)
    fig.suptitle("Intersection curbs: current vs the same curve as vehicle paths and rails (MakeFixedEnds)", fontsize=14)
    fig.tight_layout()
    fig.savefig(out, dpi=110, facecolor=fig.get_facecolor())


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "intersection_curb_bezier.png")
