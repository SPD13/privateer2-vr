# Builds the installer artwork (installer/ui/hero.jpg, favicon.png, installer/icon.png and installer/logo.png) from the game's Steam
# library art (Steam\appcache\librarycache\3067130), with a "VR MOD" badge.
#   python scripts/make_installer_art.py [librarycache folder]
import glob, os, sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageEnhance

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = sys.argv[1] if len(sys.argv) > 1 else r'C:\Program Files (x86)\Steam\appcache\librarycache\3067130'
UI = os.path.join(ROOT, 'installer', 'ui')
os.makedirs(UI, exist_ok=True)

def find(name):
    hits = glob.glob(os.path.join(SRC, '**', name), recursive=True)
    if not hits: sys.exit(f'{name} not found in {SRC}')
    return hits[0]

def font(size, weight='Bold'):
    f = ImageFont.truetype(r'C:\Windows\Fonts\bahnschrift.ttf', size)
    try: f.set_variation_by_name(weight)
    except Exception: pass
    return f

def badge(height, text='VR MOD'):
    """Rounded amber badge with white text and a soft glow, transparent background."""
    f = font(int(height * 0.62), 'Bold')
    tw = f.getbbox(text)[2] - f.getbbox(text)[0]
    pad = int(height * 0.42); glow = int(height * 0.25)
    w, h = tw + 2 * pad, height
    im = Image.new('RGBA', (w + 2 * glow, h + 2 * glow), (0, 0, 0, 0))
    # glow
    g = Image.new('RGBA', im.size, (0, 0, 0, 0)); gd = ImageDraw.Draw(g)
    gd.rounded_rectangle([glow, glow, glow + w, glow + h], radius=h // 4, fill=(255, 120, 20, 170))
    im = Image.alpha_composite(im, g.filter(ImageFilter.GaussianBlur(glow * 0.6)))
    # body: vertical gradient orange -> deep red
    body = Image.new('RGBA', (w, h)); bd = ImageDraw.Draw(body)
    for y in range(h):
        t = y / max(1, h - 1)
        bd.line([(0, y), (w, y)], fill=(int(255 - 40 * t), int(150 - 95 * t), int(30 - 10 * t), 255))
    mask = Image.new('L', (w, h), 0); ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, h - 1], radius=h // 4, fill=255)
    im.paste(body, (glow, glow), mask)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([glow, glow, glow + w - 1, glow + h - 1], radius=h // 4, outline=(255, 225, 170, 255), width=max(2, h // 22))
    bb = f.getbbox(text)
    tx = glow + (w - (bb[2] - bb[0])) // 2 - bb[0]; ty = glow + (h - (bb[3] - bb[1])) // 2 - bb[1]
    d.text((tx + max(1, h // 30), ty + max(1, h // 30)), text, font=f, fill=(90, 20, 0, 200))
    d.text((tx, ty), text, font=f, fill=(255, 255, 255, 255))
    return im

hero = Image.open(find('library_hero.jpg')).convert('RGB')          # 1920 x 620
logo = Image.open(find('logo.png')).convert('RGBA')                 # 640 x 360, transparent
capsule = Image.open(find('library_capsule.jpg')).convert('RGB')    # 300 x 450

# logo trimmed to its content
logo = logo.crop(logo.getbbox())

# ---- logo.png: game logo + badge under its right end
lw = 900; logo_s = logo.resize((lw, int(logo.height * lw / logo.width)), Image.LANCZOS)
b = badge(84)
out = Image.new('RGBA', (lw + 20, logo_s.height + b.height - 10), (0, 0, 0, 0))
out.alpha_composite(logo_s, (0, 0))
out.alpha_composite(b, (lw + 20 - b.width, logo_s.height - 26))
out.save(os.path.join(ROOT, 'installer', 'logo.png'))

# ---- hero.jpg: 1520 x 520 banner, darkened on the left behind the logo, logo + badge on it
H_W, H_H = 1520, 520
hs = hero.resize((int(hero.width * H_H / hero.height), H_H), Image.LANCZOS)
hs = hs.crop((hs.width - H_W, 0, hs.width, H_H)).convert('RGBA')
shade = Image.new('RGBA', (H_W, H_H)); sd = ImageDraw.Draw(shade)
for x in range(H_W):
    a = int(200 * max(0.0, 1 - x / (H_W * 0.62)) ** 1.3)
    sd.line([(x, 0), (x, H_H)], fill=(4, 8, 16, a))
for y in range(H_H):   # bottom fade into the page background
    a = int(230 * max(0.0, (y - H_H * 0.7) / (H_H * 0.3)) ** 1.5)
    sd.line([(0, y), (H_W, y)], fill=(11, 16, 24, a))
hs = Image.alpha_composite(hs, shade)
lw2 = 760; l2 = logo.resize((lw2, int(logo.height * lw2 / logo.width)), Image.LANCZOS)
sh = Image.new('RGBA', l2.size, (0, 0, 0, 0)); sh.putalpha(l2.getchannel('A').point(lambda v: v * 0.8))
hs.alpha_composite(sh.filter(ImageFilter.GaussianBlur(8)), (64, 70))
hs.alpha_composite(l2, (56, 60))
b2 = badge(70)
hs.alpha_composite(b2, (56 + lw2 - b2.width + 24, 60 + l2.height - 4))
hs.convert('RGB').save(os.path.join(UI, 'hero.jpg'), quality=88)

# ---- icon: the capsule's ships on the planet, square, with a VR badge
sq = capsule.crop((0, 150, 300, 450)).resize((512, 512), Image.LANCZOS)
sq = ImageEnhance.Contrast(sq).enhance(1.1).convert('RGBA')
mask = Image.new('L', (512, 512), 0); ImageDraw.Draw(mask).rounded_rectangle([0, 0, 511, 511], radius=96, fill=255)
icon = Image.new('RGBA', (512, 512), (0, 0, 0, 0)); icon.paste(sq, (0, 0), mask)
vb = badge(150, 'VR')
icon.alpha_composite(vb, (512 - vb.width + 20, 512 - vb.height + 20))
icon.save(os.path.join(ROOT, 'installer', 'icon.png'))
icon.resize((64, 64), Image.LANCZOS).save(os.path.join(UI, 'favicon.png'))
print('artwork written to', UI)
