"""Steam Frame controllers with the mod's default flight bindings (README "VR controllers").
usage: python scripts/make_controller_diagram.py <steam-frame-controllers image> docs/steam-frame-controls.png
The bindings are the Steam Frame defaults of src/commands.h (framePath) and src/vr.cpp (sticks, recenter, menus)."""
import sys
from PIL import Image, ImageDraw, ImageFont

SRC, OUT = sys.argv[1], sys.argv[2]
SS = 2                                      # supersampling
W, H = 1700, 1000
K = 2.0                                     # photo scale
OX, OY = 350, 70                            # photo position
img = Image.new('RGB', (W * SS, H * SS), (255, 255, 255))
photo = Image.open(SRC).convert('RGB')
photo = photo.resize((int(photo.width * K * SS), int(photo.height * K * SS)), Image.LANCZOS)
img.paste(photo, (OX * SS, OY * SS))
d = ImageDraw.Draw(img)

def font(sz, bold=False):
    for n in (['segoeuib.ttf', 'arialbd.ttf'] if bold else ['segoeui.ttf', 'arial.ttf']):
        try: return ImageFont.truetype(n, sz * SS)
        except OSError: pass
    return ImageFont.load_default()
F_T, F_H, F_L, F_S = font(30, True), font(21, True), font(20), font(17)
INK, SUB, ARROW = (25, 30, 38), (95, 105, 118), (226, 104, 20)

def at(x, y):                               # photo pixel -> canvas (in supersampled pixels)
    return ((OX + x * K) * SS, (OY + y * K) * SS)

def arrow(p0, p1):                          # from the label (p0) to the button (p1), arrowhead and dot on the button
    import math
    a = math.atan2(p1[1] - p0[1], p1[0] - p0[0]); back = 11 * SS
    tip = (p1[0] - math.cos(a) * back, p1[1] - math.sin(a) * back)
    for col, w in (((255, 255, 255), 7), (ARROW, 3)):
        d.line([p0, tip], fill=col, width=w * SS)
    hl, hw = 16 * SS, 8 * SS
    base = (tip[0] - math.cos(a) * hl, tip[1] - math.sin(a) * hl)
    head = [tip, (base[0] + math.sin(a) * hw, base[1] - math.cos(a) * hw), (base[0] - math.sin(a) * hw, base[1] + math.cos(a) * hw)]
    d.polygon(head, fill=ARROW, outline=(255, 255, 255))
    r = 5 * SS; d.ellipse([p1[0] - r, p1[1] - r, p1[0] + r, p1[1] + r], fill=ARROW, outline=(255, 255, 255), width=2 * SS)

def label(x, y, title, sub, side):          # side 'r': text right-aligned ending at x; 'l': starting at x
    anchor = 'rm' if side == 'r' else 'lm'
    d.text((x * SS, (y - 11) * SS), title, font=F_L, fill=INK, anchor=anchor)
    d.text((x * SS, (y + 13) * SS), sub, font=F_S, fill=SUB, anchor=anchor)

# left controller: (button x, y on the photo), label y, action, button name
LEFT = [((103, 68), 130, "Nav map", "view button"),
        ((55, 82), 215, "Speed to maximum", "d-pad up"),
        ((32, 105), 300, "Damage display", "d-pad left"),
        ((79, 105), 385, "Cycle weapons", "d-pad right"),
        ((55, 128), 470, "Select / deselect weapon", "d-pad down")]
RIGHT = [((395, 68), 130, "Jump", "menu button"),
         ((438, 78), 215, "Toggle missile display", "Y"),
         ((464, 102), 300, "Cycle targets backward", "B"),
         ((414, 102), 385, "Comms", "X"),
         ((438, 128), 470, "Cycle targets", "A")]
for (bx, by), ly, t, s in LEFT:
    label(320, ly, t, s, 'r'); arrow(((330) * SS, ly * SS), at(bx, by))
for (bx, by), ly, t, s in RIGHT:
    label(1380, ly, t, s, 'l'); arrow(((1370) * SS, ly * SS), at(bx, by))

# sticks and the buttons on the back: one box under each controller
def box(x0, y0, x1, y1, title, rows):
    d.rounded_rectangle([x0 * SS, y0 * SS, x1 * SS, y1 * SS], radius=12 * SS, fill=(244, 246, 249), outline=(205, 212, 222), width=2 * SS)
    d.text(((x0 + 20) * SS, (y0 + 16) * SS), title, font=F_H, fill=INK)
    y = y0 + 56
    for k, v in rows:
        d.text(((x0 + 20) * SS, y * SS), k, font=F_S, fill=SUB)
        d.text(((x0 + 170) * SS, y * SS), v, font=F_L, fill=INK)
        y += 31

box(60, 800, 830, 950, "Left controller, on the back", [("Trigger", "fire missile"), ("Grip", "match target speed"), ("Bumper", "drop mines / decoys")])
box(870, 800, 1640, 950, "Right controller, on the back", [("Trigger", "fire guns"), ("Grip", "afterburner"), ("Bumper", "target nearest hostile")])

# the sticks: labels in the gap between the controllers
def mid(x, y, title, lines, stick, side):   # arrow from the title's left (side -1) or right (side +1) end
    d.text((x * SS, y * SS), title, font=F_H, fill=INK, anchor='ma')
    for i, t in enumerate(lines): d.text((x * SS, (y + 30 + i * 24) * SS), t, font=F_S, fill=SUB, anchor='ma')
    tw = d.textlength(title, font=F_H) / SS
    arrow(((x + side * (tw / 2 + 8)) * SS, (y + 13) * SS), at(*stick))
mid(835, 450, "Left stick", ["roll (left / right)", "throttle (up / down)", "click: speed to zero"], (138, 124), -1)
mid(865, 610, "Right stick", ["pitch / yaw", "click: recenter view"], (352, 117), 1)

d.text((40 * SS, 24 * SS), "Steam Frame controllers: default flight controls", font=F_T, fill=INK)
d.text((W / 2 * SS, 978 * SS), "On menus, bases and films: "
       "right controller = pointer, right trigger = click, right grip = right click, left trigger = Escape.",
       font=F_S, fill=SUB, anchor='mm')

img.resize((W, H), Image.LANCZOS).save(OUT, optimize=True)
print('saved', OUT)
