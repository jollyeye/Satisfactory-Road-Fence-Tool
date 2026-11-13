# Sign backgrounds: chevrons pointing left / right (curve warning), white on transparency (the sign tints them).
# The sign stretches the picture over its whole face, so the picture has the shape of the 2:1 signs (8 x 4 m):
# 1024 x 512, 4 chevrons across, at the proportions approved on the large sign (2026-11-13).
# python sign_chevrons.py <out dir>
import sys
from PIL import Image, ImageDraw

W, H = 1024, 512
SS = 4
PERIOD = 256     # one chevron every 256 px: 4 across
STROKE = 104     # horizontal thickness of a chevron arm
DEPTH = 176      # how far the point sticks out
TOP, BOTTOM = 0, H


def chevrons(right):
    big = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    mid = (TOP + BOTTOM) / 2
    for k in range(-1, W // PERIOD + 2):     # one extra each side, cut by the edges
        x0 = k * PERIOD + (PERIOD - STROKE - DEPTH) / 2
        pts = [(x0, TOP), (x0 + STROKE, TOP), (x0 + STROKE + DEPTH, mid), (x0 + STROKE, BOTTOM), (x0, BOTTOM), (x0 + DEPTH, mid)]
        if not right:
            pts = [(W - x, y) for x, y in pts]
        d.polygon([(x * SS, y * SS) for x, y in pts], fill=(255, 255, 255, 255))
    return big.resize((W, H), Image.LANCZOS)


out = sys.argv[1]
chevrons(True).save(out + "/T_RM_Sign_ChevronsRight.png")
chevrons(False).save(out + "/T_RM_Sign_ChevronsLeft.png")
