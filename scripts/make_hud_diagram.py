"""HUD positioning diagram: panels on a sphere around the player (README "HUD Positioning").
usage: python scripts/make_hud_diagram.py docs/hud-capture.png docs/hud-positioning.png
(the capture is a 640x480 flight HUD image in its displayed colours: the RGB part of a hudraw_NN.bin from a
Ctrl+F9 capture, or a hud_NN.bmp with red and blue swapped)"""
import math, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

HUD = Image.open(sys.argv[1]).convert('RGB')
OUT = sys.argv[2]
SS = 2                                                 # supersampling
W, H = 1800, 1100
img = Image.new('RGBA', (W * SS, H * SS), (14, 18, 26, 255))
d = ImageDraw.Draw(img)

def font(sz, bold=False):
    for n in (['segoeuib.ttf', 'arialbd.ttf'] if bold else ['segoeui.ttf', 'arial.ttf']):
        try: return ImageFont.truetype(n, sz * SS)
        except OSError: pass
    return ImageFont.load_default()
F_T, F_L, F_S, F_XS = font(34, True), font(19, True), font(17), font(15)
LABEL, NOTE, BLUE = (255, 220, 150), (150, 165, 180), (90, 200, 255)

# defaults from src/vr.cpp (PANEL_<NAME> = x0,y0,x1,y1, azimuth, elevation, width), PANEL_RADIUS 0.8 m
R = 0.8
PANELS = [("VELOCITY", (236, 0, 406, 58), 0, 24, 0.36), ("COMMS", (0, 0, 124, 190), -34, 18, 0.24),
          ("TARGET", (2, 342, 120, 478), -44, -22, 0.28), ("TARGET_SHIELDS", (138, 382, 254, 476), -22, -26, 0.24),
          ("RADAR", (254, 338, 388, 478), 0, -28, 0.30), ("SHIELDS", (388, 382, 504, 476), 22, -26, 0.24),
          ("RIGHT", (504, 342, 640, 478), 44, -22, 0.28), ("MISSILES", (516, 0, 640, 124), 34, 22, 0.24),
          ("GUNS", (520, 124, 640, 264), 40, 4, 0.24), ("MINES", (520, 264, 640, 342), 40, -11, 0.24)]
NO_CAPTURE = set()                                     # panels with no capture: outline only
HUD_DIST, TAN_H = 1.0, 320 / 461.0357

# ------------------------------------------------------------------ 3D camera (x right, y up, -z forward; head at 0)
cam = np.array([0.75, 1.75, 1.95]); look = np.array([0.0, -0.30, -0.62])
fwd = look - cam; fwd /= np.linalg.norm(fwd)
right = np.cross(fwd, [0, 1, 0]); right /= np.linalg.norm(right); up = np.cross(right, fwd)
FOC = 1250 * SS; CX, CY = 620 * SS, 560 * SS
def P(p):
    v = np.asarray(p, float) - cam
    z = v @ fwd
    return (CX + FOC * (v @ right) / z, CY - FOC * (v @ up) / z), z

def sph(az, el, r=R):
    a, e = math.radians(az), math.radians(el)
    return np.array([r * math.sin(a) * math.cos(e), r * math.sin(e), -r * math.cos(a) * math.cos(e)])

def line3(a, b, col, w=1, n=24):
    d.line([P(a + (b - a) * t / n)[0] for t in range(n + 1)], fill=col, width=w * SS)

# ------------------------------------------------------------------ sphere grid (front half)
GRID = (70, 110, 140, 110)
for az in range(-90, 91, 15):
    d.line([P(sph(az, el))[0] for el in range(-60, 61, 3)], fill=GRID, width=SS)
for el in range(-60, 61, 15):
    d.line([P(sph(az, el))[0] for az in range(-90, 91, 3)], fill=GRID if el else (110, 170, 210, 170), width=SS * (2 if el == 0 else 1))
(ex, ey), _ = P(sph(-90, 0)); d.text((ex - 8 * SS, ey), "eye level", font=F_XS, fill=(120, 175, 215), anchor='rm')

# forward axis (gun line) and the HUD glass at HUD_DIST
line3(np.array([0, 0, 0.0]), np.array([0, 0, -2.6]), (255, 190, 70, 150), 2)
gw = 2 * HUD_DIST * TAN_H; gh = gw * 0.75
g = [np.array([sx * gw / 2, sy * gh / 2, -HUD_DIST]) for sx, sy in ((-1, 1), (1, 1), (1, -1), (-1, -1))]
glass = Image.new('RGBA', img.size, (0, 0, 0, 0)); gd = ImageDraw.Draw(glass)
gd.polygon([P(p)[0] for p in g], fill=(90, 160, 220, 26), outline=(120, 190, 240, 150))
img.alpha_composite(glass)

# ------------------------------------------------------------------ textured quads
def persp_coeffs(dst, src):
    A = []; B = []
    for (x, y), (u, v) in zip(dst, src):
        A.append([x, y, 1, 0, 0, 0, -u * x, -u * y]); B.append(u)
        A.append([0, 0, 0, x, y, 1, -v * x, -v * y]); B.append(v)
    return np.linalg.solve(np.array(A, float), np.array(B, float))

def keyed(crop):  # black HUD background -> transparent, like the headset keying
    a = np.asarray(crop, float); lum = a.max(axis=2)
    alpha = np.clip((lum - 10) * 6, 0, 255).astype(np.uint8)
    return Image.fromarray(np.dstack([a.astype(np.uint8), alpha]), 'RGBA')

def quad(corners3, tex, fill, outline):
    pts = [P(c)[0] for c in corners3]
    layer = Image.new('RGBA', img.size, (0, 0, 0, 0)); ld = ImageDraw.Draw(layer)
    ld.polygon(pts, fill=fill)
    if tex is not None:
        tw, th = tex.size
        warped = tex.transform(img.size, Image.PERSPECTIVE, tuple(persp_coeffs(pts, [(0, 0), (tw, 0), (tw, th), (0, th)])), Image.BICUBIC)
        mask = Image.new('L', img.size, 0); ImageDraw.Draw(mask).polygon(pts, fill=255)
        layer.alpha_composite(Image.composite(warped, Image.new('RGBA', img.size, (0, 0, 0, 0)), mask))
    if outline: ImageDraw.Draw(layer).line(pts + [pts[0]], fill=outline, width=2 * SS)
    img.alpha_composite(layer)
    return pts

# the crosshair on the glass
cr = HUD.crop((270, 195, 370, 285)); cs = 100 / 640 * gw
quad([np.array([-cs / 2, cs * 0.45, -HUD_DIST]), np.array([cs / 2, cs * 0.45, -HUD_DIST]),
      np.array([cs / 2, -cs * 0.45, -HUD_DIST]), np.array([-cs / 2, -cs * 0.45, -HUD_DIST])],
     keyed(cr.resize((400, 360), Image.LANCZOS)), (0, 0, 0, 0), None)

# panels, far to near
items = []
for name, r, az, el, w in PANELS:
    c = sph(az, el); n_out = c / np.linalg.norm(c)
    rt = np.array([math.cos(math.radians(az)), 0, math.sin(math.radians(az))])
    upv = np.cross(rt, n_out); upv /= np.linalg.norm(upv)
    h = w * (r[3] - r[1]) / (r[2] - r[0])
    corners = [c - rt * w / 2 + upv * h / 2, c + rt * w / 2 + upv * h / 2, c + rt * w / 2 - upv * h / 2, c - rt * w / 2 - upv * h / 2]
    items.append((P(c)[1], name, r, az, el, w, corners))
labels = {}
for z, name, r, az, el, w, corners in sorted(items, key=lambda t: -t[0]):
    tex = None if name in NO_CAPTURE else keyed(HUD.crop(r).resize(((r[2] - r[0]) * 4, (r[3] - r[1]) * 4), Image.LANCZOS))
    pts = quad(corners, tex, (10, 22, 34, 215), BLUE + (255 if tex else 170,))
    if name in NO_CAPTURE:
        cx = sum(p[0] for p in pts) / 4; cy = sum(p[1] for p in pts) / 4
        d.text((cx, cy), name.title(), font=F_XS, fill=(140, 200, 240), anchor='mm')
    labels[name] = (pts, az, el)

# ------------------------------------------------------------------ the player: seated mannequin
def box(c, sx, sy, sz, col, edge=(30, 34, 40, 255)):
    c = np.asarray(c, float); v = [c + np.array([x * sx / 2, y * sy / 2, z * sz / 2]) for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)]
    faces = [(0, 1, 3, 2), (4, 5, 7, 6), (0, 1, 5, 4), (2, 3, 7, 6), (0, 2, 6, 4), (1, 3, 7, 5)]
    shade = [0.75, 0.75, 0.6, 1.0, 0.7, 0.9]
    fl = []
    for f, s in zip(faces, shade):
        ctr = sum(v[i] for i in f) / 4
        if (cam - ctr) @ (ctr - c) <= 0: continue
        fl.append((P(ctr)[1], [P(v[i])[0] for i in f], tuple(int(k * s) for k in col[:3]) + (255,)))
    for _, pts, cc in sorted(fl, key=lambda t: -t[0]): d.polygon(pts, fill=cc, outline=edge)

def ball(c, r, col):
    (x, y), z = P(c); rr = FOC * r / z
    d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=col, outline=(30, 34, 40, 255), width=SS)

SKIN, SUIT, SEAT, DARK = (196, 160, 130), (70, 96, 128), (52, 56, 64), (40, 44, 52)
parts = [  # (centre, size, colour); drawn far to near from the camera
    ([0, -0.66, 0.20], (0.56, 0.08, 0.55), SEAT),                                  # seat
    ([-0.11, -0.55, -0.02], (0.13, 0.13, 0.46), SUIT), ([0.11, -0.55, -0.02], (0.13, 0.13, 0.46), SUIT),   # thighs
    ([-0.11, -0.80, -0.24], (0.11, 0.40, 0.11), SUIT), ([0.11, -0.80, -0.24], (0.11, 0.40, 0.11), SUIT),   # shins
    ([0, -0.32, 0.20], (0.38, 0.50, 0.22), SUIT),                                  # torso
    ([-0.24, -0.26, 0.20], (0.10, 0.30, 0.10), SUIT), ([0.24, -0.26, 0.20], (0.10, 0.30, 0.10), SUIT),     # upper arms
    ([-0.24, -0.41, 0.00], (0.09, 0.09, 0.40), SUIT), ([0.24, -0.41, 0.00], (0.09, 0.09, 0.40), SUIT),     # forearms
    ([-0.24, -0.38, -0.24], (0.07, 0.13, 0.07), (34, 34, 38)), ([0.24, -0.38, -0.24], (0.07, 0.13, 0.07), (34, 34, 38)),  # controllers
    ([0, -0.10, 0.10], (0.10, 0.08, 0.10), SKIN)]                                   # neck
for c, s, col in sorted(parts, key=lambda t: -P(t[0])[1]): box(c, *s, col)
ball([0, 0.02, 0.08], 0.105, SKIN + (255,))                                         # head
(hx, hy), _ = P([-0.08, 0.06, 0.10]); d.text((hx - 150 * SS, hy + 70 * SS), "you:\nthe sphere's centre", font=F_S, fill=(235, 240, 245), anchor='rm', align='right', spacing=2 * SS)
d.line([(hx - 146 * SS, hy + 70 * SS), (hx, hy)], fill=(235, 240, 245, 180), width=SS)

# leader labels: (anchor corner of the panel, label offset in px)
LBL = {"VELOCITY": ("top", (0, -50)), "COMMS": ("top", (-40, -45)), "MISSILES": ("top", (30, -45)),
       "GUNS": ("right", (70, 0)), "MINES": ("right", (70, 10)), "RIGHT": ("right", (60, 40)),
       "TARGET": ("left", (-70, -10)), "TARGET_SHIELDS": ("left", (-110, 45)),
       "RADAR": ("bottom", (-10, 75)), "SHIELDS": ("bottom", (70, 70))}
for name, (pts, az, el) in labels.items():
    where, (ox, oy) = LBL[name]
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]; cx = sum(xs) / 4; cy = sum(ys) / 4
    ax, ay = {"top": (cx, min(ys)), "bottom": (cx, max(ys)), "left": (min(xs), cy), "right": (max(xs), cy)}[where]
    tx, ty = ax + ox * SS, ay + oy * SS
    d.line([(ax, ay), (tx, ty)], fill=(255, 210, 120, 200), width=SS)
    h = 'r' if ox < -20 else 'l' if ox > 20 else 'm'
    v = 'd' if oy < -20 else 'a' if oy > 20 else 'm'
    d.text((tx + (-5 if h == 'r' else 5 if h == 'l' else 0) * SS, ty), f"{name}\n{az:+d}°  {el:+d}°", font=F_XS, fill=LABEL,
           anchor=h + v, align={'r': 'right', 'l': 'left', 'm': 'center'}[h], spacing=2 * SS)

# annotations
line3(np.array([0, 0.03, 0.0]), sph(0, 24) * 0.97, (255, 255, 255, 170), 1)
(mx, my), _ = P(sph(0, 24) * 0.55); d.text((mx + 8 * SS, my), "radius 0.8 m\n(PANEL_RADIUS)", font=F_XS, fill=(235, 240, 245), anchor='lm', spacing=2 * SS)
(gx, gy), _ = P(g[1]); d.text((gx, gy - 8 * SS), "HUD glass at 1.0 m (HUD_DIST):\ncrosshair, direction arrows", font=F_XS, fill=(150, 200, 240), anchor='rd', align='right', spacing=2 * SS)
(fx, fy), _ = P([0, 0, -2.6]); d.text((fx, fy - 8 * SS), "forward (gun line)", font=F_XS, fill=(255, 200, 110), anchor='md')

# ------------------------------------------------------------------ insets: top view (azimuth) and side view (elevation)
def inset(x0, y0, w, h, title, sub):
    d.rounded_rectangle([x0 * SS, y0 * SS, (x0 + w) * SS, (y0 + h) * SS], radius=10 * SS, fill=(20, 27, 38, 255), outline=(60, 80, 100, 255), width=SS)
    d.text(((x0 + 16) * SS, (y0 + 12) * SS), title, font=F_L, fill=(230, 235, 240))
    d.text(((x0 + 16) * SS, (y0 + h - 14) * SS), sub, font=F_XS, fill=(170, 185, 200), anchor='ls')

# top view: looking down, forward = up on the page
X0, Y0, IW, IH = 1230, 110, 550, 430
inset(X0, Y0, IW, IH, "Top view: azimuth (5th value)", "negative = left, positive = right")
cx, cy, rr = (X0 + IW / 2) * SS, (Y0 + 345) * SS, 165 * SS
d.arc([cx - rr, cy - rr, cx + rr, cy + rr], 180, 360, fill=(110, 170, 210), width=2 * SS)
for a in range(-90, 91, 15):
    t = math.radians(a); d.line([(cx, cy), (cx + rr * math.sin(t), cy - rr * math.cos(t))], fill=(45, 65, 85), width=SS)
d.line([(cx, cy), (cx, cy - rr - 14 * SS)], fill=(255, 190, 70), width=SS)
for name, r, az, el, w in PANELS:
    t = math.radians(az); hw = w / R * rr / 2
    px, py = cx + rr * math.sin(t), cy - rr * math.cos(t)
    d.line([(px - math.cos(t) * hw, py - math.sin(t) * hw), (px + math.cos(t) * hw, py + math.sin(t) * hw)], fill=BLUE, width=5 * SS)
top = [(-44, "TARGET", 3), (-34, "COMMS", 2), (-22, "TARGET_SHIELDS", 1), (22, "SHIELDS", 1), (34, "MISSILES", 2),
       (40, "GUNS, MINES", 3), (44, "RIGHT", 4)]
d.text((cx, cy - rr - 18 * SS), "0°  VELOCITY, RADAR", font=F_XS, fill=LABEL, anchor='md')
for az, nm, k in top:
    t = math.radians(az); ax, ay = cx + (rr + 6 * SS) * math.sin(t), cy - (rr + 6 * SS) * math.cos(t)
    sd = 1 if az > 0 else -1
    lx, ly = cx + sd * 112 * SS, (Y0 + 30 + k * 30) * SS + 30 * SS
    d.line([(ax, ay), (lx, ly)], fill=(255, 210, 120, 140), width=SS)
    d.text((lx + sd * 4 * SS, ly), f"{az:+d}°  {nm}" if sd > 0 else f"{nm}  {az:+d}°", font=F_XS, fill=LABEL, anchor='lm' if sd > 0 else 'rm')
d.ellipse([cx - 20 * SS, cy - 20 * SS, cx + 20 * SS, cy + 20 * SS], fill=SKIN, outline=(30, 34, 40))
d.polygon([(cx - 6 * SS, cy - 18 * SS), (cx + 6 * SS, cy - 18 * SS), (cx, cy - 27 * SS)], fill=SKIN, outline=(30, 34, 40))   # nose

# side view: looking from the right, forward = right on the page
X0, Y0, IW, IH = 1230, 570, 550, 470
inset(X0, Y0, IW, IH, "Side view: elevation (6th value)", "negative = below, positive = above eye level")
cx, cy, rr = (X0 + 80) * SS, (Y0 + 250) * SS, 180 * SS
d.arc([cx - rr, cy - rr, cx + rr, cy + rr], -60, 60, fill=(110, 170, 210), width=2 * SS)
for a in range(-60, 61, 15):
    t = math.radians(a); d.line([(cx, cy), (cx + rr * math.cos(t), cy - rr * math.sin(t))], fill=(45, 65, 85), width=SS)
d.line([(cx, cy), (cx + rr + 10 * SS, cy)], fill=(255, 190, 70), width=SS)
for name, r, az, el, w in PANELS:
    t = math.radians(el); h = w * (r[3] - r[1]) / (r[2] - r[0]); hh = h / R * rr / 2
    px, py = cx + rr * math.cos(t), cy - rr * math.sin(t)
    d.line([(px - math.sin(t) * hh, py - math.cos(t) * hh), (px + math.sin(t) * hh, py + math.cos(t) * hh)], fill=BLUE, width=5 * SS)
side = [(24, "VELOCITY", -150), (22, "MISSILES", -124), (18, "COMMS", -98), (4, "GUNS", -20), (-11, "MINES", 40),
        (-22, "TARGET, RIGHT", 98), (-26, "TARGET_SHIELDS, SHIELDS", 124), (-28, "RADAR", 150)]
for el, nm, yy in side:
    t = math.radians(el); ax, ay = cx + (rr + 6 * SS) * math.cos(t), cy - (rr + 6 * SS) * math.sin(t)
    lx, ly = cx + rr + 50 * SS, cy + yy * SS
    d.line([(ax, ay), (lx - 4 * SS, ly)], fill=(255, 210, 120, 140), width=SS)
    d.text((lx, ly), f"{el:+d}°  {nm}", font=F_XS, fill=LABEL, anchor='lm')
d.ellipse([cx - 20 * SS, cy - 20 * SS, cx + 20 * SS, cy + 20 * SS], fill=SKIN, outline=(30, 34, 40))
d.polygon([(cx + 18 * SS, cy - 6 * SS), (cx + 18 * SS, cy + 4 * SS), (cx + 27 * SS, cy + 2 * SS)], fill=SKIN, outline=(30, 34, 40))   # nose

# title + notes
d.text((40 * SS, 30 * SS), "HUD positioning in VR", font=F_T, fill=(240, 244, 248))
d.text((40 * SS, 80 * SS), "Each HUD panel is a flat plate on a sphere centred on your head, turned to face you.",
       font=F_S, fill=(185, 198, 210))
d.text((40 * SS, (H - 24) * SS), "Panel contents: an in-game HUD capture (black = transparent, as in the headset). "
       "The missiles, guns and mines windows only appear when selected.", font=F_XS, fill=NOTE, anchor='ls')

img.resize((W, H), Image.LANCZOS).convert('RGB').save(OUT, optimize=True)
print('saved', OUT)
