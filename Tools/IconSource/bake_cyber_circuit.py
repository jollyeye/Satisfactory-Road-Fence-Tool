# Bakes the cyberpunk road's circuit pattern (CIRCUIT_HLSL in make_materials.py) into a tiling grayscale texture
# for Low quality: Tools/Icons/T_RM_CyberCircuit.png, 48 m along x 16 m across, 4096 x 2048 px (~1.2 x 0.8 cm per px).
# 16 m across so a wide road (up to 32 m) shows each strip at most twice, with different chips in each 8 m band.
# Same shapes as the live pattern; to tile, every cell grid divides the tile: the second layer's sections are 2.4 m
# (2.5 m live) and its density cells 8 m along (7.5 m live), and every hash reads its cell index wrapped to the tile.
# Usage: python3 bake_cyber_circuit.py [out.png]
import sys
import numpy as np
from PIL import Image

TILE = (48.0, 16.0)
SIZE = (4096, 2048)
out = sys.argv[1] if len(sys.argv) > 1 else "T_RM_CyberCircuit.png"

xs = (np.arange(SIZE[0]) + 0.5) * TILE[0] / SIZE[0]
ys = (np.arange(SIZE[1]) + 0.5) * TILE[1] / SIZE[1]
QX, QY = np.meshgrid(xs, ys)  # QX along, QY across; arrays [across, along]

def frac(v):
    return v - np.floor(v)

def h1(v):
    return frac(np.sin(v) * 43758.5453)

def smoothstep(a, b, v):
    t = np.clip((v - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)

aa = max(TILE[0] / SIZE[0], TILE[1] / SIZE[1])
hw = 0.011
soft = 0.004 + aa
ln = np.zeros_like(QX)
rg = np.zeros_like(QX)
empty0 = np.zeros(QX.shape, bool)  # where the first layer leaves an empty patch

def seg(P, A, B):
    pax, pay = P[0] - A[0], P[1] - A[1]
    bax, bay = B[0] - A[0], B[1] - A[1]
    t = np.clip((pax * bax + pay * bay) / np.maximum(bax * bax + bay * bay, 1e-5), 0, 1)
    d = np.hypot(pax - bax * t, pay - bay * t)
    return 1.0 - smoothstep(hw, hw + soft, d)

def ring(P, cx, r):
    return 1.0 - smoothstep(hw, hw + soft, np.abs(np.hypot(P[0] - cx, P[1]) - r))

for layer in range(2):
    S = 4.0 if layer == 0 else 2.4
    R = 0.5 if layer == 0 else 0.25
    DENS_X = S * 3.0 if layer == 0 else 8.0
    off = (0.0, 0.0) if layer == 0 else (1.3, 0.37)
    NS = int(round(TILE[0] / S)); NJ = int(round(TILE[1] / R))
    ND = (int(round(TILE[0] / DENS_X)), int(round(TILE[1] / (R * 8.0))))
    qx = QX + off[0]; qy = QY + off[1]
    j0 = np.floor(qy / R)
    for dj in (-1, 0, 1):
        j = j0 + dj
        jw = np.mod(j, NJ)
        sh = h1(jw * 12.9898 + layer * 4.1) * S
        s = np.floor((qx + sh) / S)
        sw = np.mod(s, NS)
        x = qx + sh - s * S
        hj = h1(jw * 127.1 + sw * 311.7 + layer * 7.7)
        h2 = h1(jw * 269.5 + sw * 183.3 + layer * 3.3)
        dx = np.mod(np.floor(qx / DENS_X), ND[0]); dy = np.mod(np.floor(qy / (R * 8.0)), ND[1])
        dens = h1(dx * 41.3 + dy * 17.9 + layer)
        live = dens >= 0.2
        # Empty patches give the board some rest, but where both layers leave one, a bare square shows, the same
        # every 48 m in the tile: the second layer always fills the first one's patches.
        if layer == 0 and dj == 0:
            empty0 = ~live
        if layer == 1:
            live = live | empty0
        P = (x, qy - (j + 0.5) * R)
        Z = np.zeros_like(x)
        m = np.zeros_like(x)
        # straight with a ring
        k = live & (hj < 0.3)
        m = np.where(k, np.maximum(seg(P, (Z, Z), (Z + S, Z)), 0), m)
        rg = np.where(k, np.maximum(rg, ring(P, S * (0.25 + 0.5 * h2), 0.05)), rg)
        # jog up / down
        k = live & (hj >= 0.3) & (hj < 0.65)
        jx = S * (0.3 + 0.4 * h2)
        up = np.where(hj < 0.5, R, -R)
        jog = np.maximum(np.maximum(seg(P, (Z, Z), (jx - 0.2, Z)), seg(P, (jx - 0.2, Z), (jx + 0.2, up))), seg(P, (jx + 0.2, up), (Z + S, up)))
        m = np.where(k, jog, m)
        # short with a pad
        k = live & (hj >= 0.65) & (hj < 0.8)
        L = S * (0.3 + 0.5 * h2)
        m = np.where(k, seg(P, (Z, Z), (L, Z)), m)
        rg = np.where(k, np.maximum(rg, ring(P, L + 0.06, 0.05)), rg)
        # bus
        k = live & (hj >= 0.8) & (hj < 0.92)
        bus = np.maximum(np.maximum(seg(P, (Z, Z - 0.12), (Z + S, Z - 0.12)), seg(P, (Z, Z), (S * (0.6 + 0.4 * h2), Z))), seg(P, (Z, Z + 0.12), (Z + S, Z + 0.12)))
        m = np.where(k, bus, m)
        # double ring
        k = live & (hj >= 0.92)
        rg = np.where(k, np.maximum(rg, np.maximum(ring(P, S * 0.5, 0.07), ring(P, S * 0.5, 0.03))), rg)
        ln = np.maximum(ln, m)

# chips (16 x 8 m cells)
C = (16.0, 8.0)
ccx = np.floor(QX / C[0]); ccy = np.floor(QY / C[1])
cwx = np.mod(ccx, round(TILE[0] / C[0])); cwy = np.mod(ccy, round(TILE[1] / C[1]))
hc = h1(cwx * 91.7 + cwy * 33.1)
hc2 = h1(cwx * 17.3 + cwy * 71.9)
cx = (ccx + 0.25 + 0.5 * hc2) * C[0]
cy = (ccy + 0.25 + 0.5 * frac(hc * 7.3)) * C[1]
hsx = 0.9 + 0.8 * hc2; hsy = 0.6 + 0.3 * hc
bx = np.abs(QX - cx) - hsx; by = np.abs(QY - cy) - hsy
sd = np.hypot(np.maximum(bx, 0), np.maximum(by, 0)) + np.minimum(np.maximum(bx, by), 0)
outline = 1.0 - smoothstep(hw, hw + soft, np.abs(sd))
lx = np.abs(frac((QX - cx) / 0.3 + 0.5) - 0.5) * 0.3
ly = np.abs(frac((QY - cy) / 0.3 + 0.5) - 0.5) * 0.3
legY = np.where((by > 0) & (by < 0.3) & (bx < -0.1), 1.0 - smoothstep(0.02, 0.02 + soft, lx), 0.0)
legX = np.where((bx > 0) & (bx < 0.3) & (by < -0.1), 1.0 - smoothstep(0.02, 0.02 + soft, ly), 0.0)
dotm = 1.0 - smoothstep(0.04, 0.04 + soft, np.hypot(QX - (cx - hsx + 0.18), QY - (cy - hsy + 0.18)))
has = hc < 0.4
chip = np.where(has, np.maximum(np.maximum(outline, dotm), np.maximum(legY, legX)), 0.0)
# Tracks stop crisply around a chip (the live pattern fades them over 15 cm, which baked reads as a smudge).
clear = np.where(has, smoothstep(0.375 - aa * 0.5, 0.375 + aa * 0.5, sd), 1.0)
mask = np.clip(np.maximum(np.maximum(ln, rg) * clear, chip), 0, 1)
Image.fromarray((mask * 255 + 0.5).astype(np.uint8), "L").save(out)
print("wrote", out, mask.shape, "coverage %.3f" % mask.mean())
