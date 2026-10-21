#!/usr/bin/env python3
"""Draws the menu art (original drawings) into ../romfs/.

Run:  python3 make_art.py
Everything is plain shapes drawn at 4x and scaled down for smooth edges.
"""
import math, os, random
from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "romfs")
os.makedirs(OUT, exist_ok=True)
S = 4  # supersampling


def canvas(w, h):
    im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def save(im, name, w, h):
    im.resize((w, h), Image.LANCZOS).save(os.path.join(OUT, name))


def P(*pts):
    return [(x * S, y * S) for x, y in pts]


def E(d, x0, y0, x1, y1, **kw):
    if "width" in kw: kw["width"] *= S
    d.ellipse((x0 * S, y0 * S, x1 * S, y1 * S), **kw)


def R(d, x0, y0, x1, y1, r=0, **kw):
    if "width" in kw: kw["width"] *= S
    d.rounded_rectangle((x0 * S, y0 * S, x1 * S, y1 * S), radius=r * S, **kw)


# ---------------------------------------------------------------- background
def background():
    W, H = 1280, 720
    im = Image.new("RGB", (W, H))
    px = im.load()
    top, mid, low = (22, 18, 58), (120, 52, 110), (247, 150, 110)
    for y in range(H):
        t = y / H
        if t < 0.55:
            k = t / 0.55; c = tuple(int(top[i] + (mid[i] - top[i]) * k) for i in range(3))
        else:
            k = (t - 0.55) / 0.45; c = tuple(int(mid[i] + (low[i] - mid[i]) * k) for i in range(3))
        for x in range(W):
            px[x, y] = c
    d = ImageDraw.Draw(im, "RGBA")
    rnd = random.Random(7)
    for _ in range(140):  # stars in the upper sky
        x, y = rnd.randrange(W), rnd.randrange(int(H * 0.45))
        a = rnd.randrange(80, 230); r = rnd.choice([1, 1, 1, 2])
        d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 255, 240, a))
    # big pale moon with a soft glow
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0)); g = ImageDraw.Draw(glow)
    g.ellipse((900, 70, 1140, 310), fill=(255, 220, 200, 120))
    glow = glow.filter(ImageFilter.GaussianBlur(40)); im.paste(glow, (0, 0), glow)
    d.ellipse((945, 115, 1095, 265), fill=(255, 244, 228, 255))
    d.ellipse((985, 150, 1010, 170), fill=(240, 225, 210, 255))
    d.ellipse((1030, 190, 1060, 215), fill=(240, 225, 210, 255))
    # mountains (the shrine sits on a mountain)
    def ridge(base, amp, color, seed, step=40):
        r2 = random.Random(seed); pts = [(0, H)]
        for x in range(0, W + step, step):
            y = base - amp * (0.5 + 0.5 * math.sin(x / 210 + seed)) - r2.randrange(0, 30)
            pts.append((x, y))
        pts.append((W, H)); d.polygon(pts, fill=color)
    ridge(470, 160, (70, 40, 95, 255), 1)
    ridge(540, 120, (52, 30, 72, 255), 2)
    # mist band
    mist = Image.new("RGBA", (W, H), (0, 0, 0, 0)); m = ImageDraw.Draw(mist)
    m.rectangle((0, 520, W, 600), fill=(255, 200, 210, 70))
    mist = mist.filter(ImageFilter.GaussianBlur(25)); im.paste(mist, (0, 0), mist)
    ridge(640, 70, (34, 20, 48, 255), 3, 30)
    # stone path / shrine ground on the right
    d.polygon([(760, 720), (1280, 720), (1280, 600), (900, 610)], fill=(28, 16, 38, 255))
    # sakura tree branch top-left... kept off the menu side: top-right corner
    for _ in range(90):
        x = rnd.randrange(1080, 1290); y = rnd.randrange(-20, 90)
        r = rnd.randrange(10, 26)
        d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 183, 206, rnd.randrange(120, 200)))
    d.line([(1290, 20), (1180, 60), (1120, 40)], fill=(60, 30, 40, 255), width=7)
    im.save(os.path.join(OUT, "bg.png"))


# ---------------------------------------------------------------- torii
def torii():
    """Torii drawn at its final size (it frames the moon in the menu)."""
    K = 1.3                       # design units -> pixels
    w, h = int(380 * K), int(360 * K)
    im, d = canvas(w, h)

    def p(*pts):
        return [(x * K * S, y * K * S) for x, y in pts]

    def curve(x0, x1, y_mid, lift, n=40):
        """points along a beam edge whose ends sweep upward (sori)"""
        pts = []
        for i in range(n + 1):
            t = i / n
            x = x0 + (x1 - x0) * t
            u = abs(t - 0.5) * 2
            pts.append((x, y_mid - lift * u ** 2.4))
        return pts

    red, red_d, red_l = (206, 52, 36, 255), (150, 34, 26, 255), (232, 96, 70, 255)
    black, black_l = (28, 22, 30, 255), (70, 60, 72, 255)

    # pillars, slightly tapered, shaded on the right, moonlit on the left
    for x0, x1 in ((66, 104), (276, 314)):
        d.polygon(p((x0 + 3, 74), (x1 - 3, 74), (x1, 334), (x0, 334)), fill=red)
        d.polygon(p((x1 - 12, 74), (x1 - 3, 74), (x1, 334), (x1 - 10, 334)), fill=red_d)
        d.polygon(p((x0 + 3, 74), (x0 + 7, 74), (x0 + 5, 334), (x0, 334)), fill=red_l)
        # kamebara (black base)
        d.rounded_rectangle((p((x0 - 8, 326))[0][0], p((0, 326))[0][1], p((x1 + 8, 0))[0][0], p((0, 358))[0][1]),
                            radius=int(4 * K * S), fill=black)
    # nuki (lower beam), with ends poking out through the pillars
    d.polygon(p((36, 116), (344, 116), (340, 136), (40, 136)), fill=red)
    d.polygon(p((36, 132), (344, 132), (340, 136), (40, 136)), fill=red_d)
    # gakuzuka + plaque
    d.polygon(p((182, 70), (198, 70), (198, 116), (182, 116)), fill=red)
    d.rounded_rectangle((p((166, 72))[0][0], p((0, 72))[0][1], p((214, 0))[0][0], p((0, 112))[0][1]),
                        radius=int(2 * K * S), fill=black, outline=(214, 170, 80, 255), width=int(2 * K * S))
    # shimaki (red beam under the top)
    top = curve(26, 354, 58, 6)
    bot = curve(30, 350, 74, 6)
    d.polygon(p(*top, *reversed(bot)), fill=red)
    d.polygon(p(*curve(30, 350, 70, 6), *reversed(bot)), fill=red_d)
    # kasagi (black top beam, ends sweep up)
    top = curve(-2, 382, 26, 24)
    bot = curve(8, 372, 52, 16)
    d.polygon(p(*top, *reversed(bot)), fill=black)
    d.line(p(*curve(4, 376, 30, 22)), fill=black_l, width=int(2 * K * S))  # moonlit edge

    # shimenawa: twisted rope with four shide
    rope, rope_d = (224, 196, 132, 255), (176, 146, 90, 255)
    pts = [(78 + i * 4.5, 152 + 20 * math.sin(math.pi * i / 50)) for i in range(51)]
    d.line(p(*pts), fill=rope, width=int(11 * K * S), joint="curve")
    for i in range(0, 50, 3):  # twist marks
        x, y = pts[i]
        d.line(p((x - 3, y - 5), (x + 3, y + 5)), fill=rope_d, width=int(2 * K * S))
    for x in (118, 168, 214, 262):
        y = 152 + 20 * math.sin(math.pi * (x - 78) / 225) + 4
        zig = [(x, y), (x + 9, y), (x + 9, y + 12), (x, y + 12), (x, y + 24), (x + 9, y + 24),
               (x + 9, y + 38), (x + 4, y + 38), (x + 4, y + 28), (x - 5, y + 28), (x - 5, y + 16),
               (x + 4, y + 16), (x + 4, y + 4), (x, y + 4)]
        d.polygon(p(*zig), fill=(246, 242, 232, 255))

    # dusk: darken towards the ground
    shade = Image.new("RGBA", im.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(shade)
    for y in range(im.size[1]):
        a = int(110 * max(0.0, (y / im.size[1] - 0.35) / 0.65))
        sd.line([(0, y), (im.size[0], y)], fill=(20, 10, 30, a))
    alpha = im.getchannel("A")
    im = Image.alpha_composite(im, shade)
    im.putalpha(alpha)
    save(im, "torii.png", w, h)


# ---------------------------------------------------------------- hanko stamps
MINCHO = "/usr/share/fonts/opentype/ipafont-mincho/ipam.ttf"  # apt: fonts-ipafont-mincho


def stamp(name, glyph, color):
    """Seal stamp: square of ink with a frame and one kanji cut out of it."""
    from PIL import ImageFont
    w = h = 84
    im, d = canvas(w, h)
    R(d, 4, 4, 80, 80, 6, fill=color)
    d.rounded_rectangle((10 * S, 10 * S, 74 * S, 74 * S), radius=3 * S, outline=(255, 255, 255, 0), width=3 * S)
    font = ImageFont.truetype(MINCHO, 54 * S)
    d.text((42 * S, 44 * S), glyph, font=font, fill=(255, 255, 255, 0), anchor="mm")
    # cut-outs: make frame + glyph transparent, then rough up the ink
    px = im.load()
    rnd = random.Random(sum(map(ord, name)))
    for y in range(h * S):
        for x in range(w * S):
            r, g, b, a = px[x, y]
            if a and (r, g, b) == (255, 255, 255):
                px[x, y] = (0, 0, 0, 0)
    for _ in range(110):  # ink speckles: tiny missing spots
        x, y = rnd.randrange(w * S), rnd.randrange(h * S)
        rr = rnd.randrange(1, 5)
        d.ellipse((x - rr, y - rr, x + rr, y + rr), fill=(0, 0, 0, 0))
    save(im, name, w, h)


# ---------------------------------------------------------------- frog
def frog():
    """Abstract frog, seen from the front: a few flat shapes, moonlit from above."""
    w, h = 160, 112
    im, d = canvas(w, h)
    body, dark, belly = (60, 128, 98, 255), (34, 80, 66, 255), (110, 164, 124, 255)
    rim, eye = (156, 210, 176, 255), (238, 232, 212, 255)

    # haunches (back legs folded at the sides)
    E(d, 6, 62, 62, 110, fill=dark)
    E(d, 98, 62, 154, 110, fill=dark)
    # body + head in one wide shape
    E(d, 20, 40, 140, 110, fill=body)
    E(d, 32, 30, 128, 86, fill=body)
    # belly
    E(d, 52, 74, 108, 108, fill=belly)
    # eye domes
    for cx in (50, 110):
        E(d, cx - 20, 10, cx + 20, 50, fill=body)
        d.arc(((cx - 20) * S, 10 * S, (cx + 20) * S, 50 * S), 200, 320, fill=rim, width=3 * S)
        E(d, cx - 12, 18, cx + 12, 42, fill=eye)
        R(d, cx - 9, 27, cx + 9, 33, 3, fill=dark)   # calm horizontal pupil
    d.arc((32 * S, 30 * S, 128 * S, 86 * S), 205, 245, fill=rim, width=3 * S)
    d.arc((32 * S, 30 * S, 128 * S, 86 * S), 295, 335, fill=rim, width=3 * S)
    # a wide, quiet smile
    d.arc((44 * S, 36 * S, 116 * S, 70 * S), 25, 155, fill=dark, width=3 * S)
    # front feet
    for cx in (62, 98):
        R(d, cx - 12, 102, cx + 12, 110, 4, fill=dark)
    save(im, "frog.png", w, h)


# ---------------------------------------------------------------- sakura petal
def petal():
    w = h = 24
    im, d = canvas(w, h)
    d.polygon(P((12, 2), (20, 8), (18, 20), (12, 16), (6, 20), (4, 8)), fill=(255, 190, 210, 255))
    d.line(P((12, 6), (12, 14)), fill=(255, 150, 180, 255), width=1 * S)
    save(im, "petal.png", w, h)


if __name__ == "__main__":
    background()
    torii()
    stamp("stamp_on.png", "入", (200, 46, 36, 255))
    stamp("stamp_off.png", "切", (120, 112, 128, 255))
    frog()
    petal()
    print("art written to", os.path.normpath(OUT))
