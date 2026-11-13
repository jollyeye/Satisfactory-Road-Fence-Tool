# Sign backgrounds: chevrons pointing left / right (curve warning), white on transparency, filling the whole square.
# Seamless left to right (4 chevrons per 256 px), so the animated icons can scroll them.
# python sign_chevrons.py <out dir>
import sys
from PIL import Image, ImageDraw

N = 256
SS = 4
PERIOD = 64      # one chevron every 64 px
STROKE = 26      # horizontal thickness of a chevron arm
DEPTH = 44       # how far the point sticks out
TOP, BOTTOM = 0, 256    # the whole height: a square background tile


def chevrons(right):
    big = Image.new("RGBA", (N * SS, N * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    mid = (TOP + BOTTOM) / 2
    for k in range(-1, N // PERIOD + 2):     # one extra each side, cut by the edges: seamless
        x0 = k * PERIOD + (PERIOD - STROKE - DEPTH) / 2
        pts = [(x0, TOP), (x0 + STROKE, TOP), (x0 + STROKE + DEPTH, mid), (x0 + STROKE, BOTTOM), (x0, BOTTOM), (x0 + DEPTH, mid)]
        if not right:
            pts = [(N - x, y) for x, y in pts]
        d.polygon([(x * SS, y * SS) for x, y in pts], fill=(255, 255, 255, 255))
    return big.resize((N, N), Image.LANCZOS)


out = sys.argv[1]
chevrons(True).save(out + "/T_RM_Sign_ChevronsRight.png")
chevrons(False).save(out + "/T_RM_Sign_ChevronsLeft.png")
