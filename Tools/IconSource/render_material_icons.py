# Material tab icons for the road looks: flat top-down tiles (asphalt, concrete blocks, cyberpunk).
# Usage: python render_material_icons.py   (writes ../Icons/T_Icon_RoadMat_*.png)
import os, random
from PIL import Image, ImageDraw, ImageFilter

S = 256
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Icons")


def tile(bg, draw_fn, name):
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    base = Image.new("RGBA", (S, S), bg)
    d = ImageDraw.Draw(base)
    draw_fn(base, d)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((10, 10, S - 11, S - 11), 28, fill=255)
    img.paste(base, (0, 0), mask)
    ImageDraw.Draw(img).rounded_rectangle((10, 10, S - 11, S - 11), 28, outline=(0, 0, 0, 200), width=4)
    img.save(os.path.join(OUT, name))


def noise(img, amount):
    rnd = random.Random(7)
    px = img.load()
    for y in range(S):
        for x in range(S):
            r, g, b, a = px[x, y]
            n = rnd.randint(-amount, amount)
            px[x, y] = (max(0, min(255, r + n)), max(0, min(255, g + n)), max(0, min(255, b + n)), a)


def asphalt(img, d):
    noise(img, 9)
    d = ImageDraw.Draw(img)
    for y in range(24, S - 24, 64):
        d.rectangle((S // 2 - 6, y, S // 2 + 6, y + 36), fill=(236, 236, 224, 255))
    d.rectangle((34, 0, 42, S), fill=(236, 236, 224, 255))
    d.rectangle((S - 43, 0, S - 35, S), fill=(236, 236, 224, 255))


def concrete(img, d):
    rnd = random.Random(3)
    bh, bw = 64, 128
    for row in range(0, S // bh + 1):
        off = (row % 2) * bw // 2
        for cx in range(-1, S // bw + 2):
            x0 = cx * bw + off
            g = rnd.randint(150, 190)
            d.rectangle((x0 + 3, row * bh + 3, x0 + bw - 3, row * bh + bh - 3), fill=(g, g - 2, g - 6, 255))
    noise(img, 6)


def cyber(img, d):
    d.rectangle((0, 0, S, S), fill=(66, 8, 86, 255))
    rnd = random.Random(11)
    trace = (30, 0, 44, 255)
    for r in range(0, 8):
        y = 20 + r * 30
        x = 10
        while x < S - 10:
            seg = rnd.choice((60, 90, 120))
            d.line((x, y, min(x + seg, S - 10), y), fill=trace, width=5)
            x += seg
            if rnd.random() < 0.6 and y + 30 < S:
                d.line((x, y, x + 14, y + 30), fill=trace, width=5)
                d.ellipse((x - 7, y - 7, x + 7, y + 7), outline=trace, width=4)
                y += 30
                break
    # glowing lane lines
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    for x in (40, S - 40):
        gd.rectangle((x - 5, 0, x + 5, S), fill=(0, 230, 255, 255))
    for y in range(24, S - 24, 64):
        gd.rectangle((S // 2 - 5, y, S // 2 + 5, y + 36), fill=(0, 230, 255, 255))
    blur = glow.filter(ImageFilter.GaussianBlur(7))
    img.alpha_composite(blur)
    img.alpha_composite(glow)


tile((38, 38, 42, 255), asphalt, "T_Icon_RoadMat_Asphalt.png")
tile((160, 158, 152, 255), concrete, "T_Icon_RoadMat_Concrete.png")
tile((66, 8, 86, 255), cyber, "T_Icon_RoadMat_Cyber.png")
