# Generates assets.h for FacetOS: anti-aliased fonts + original icons.
# Icons are drawn here with simple shapes at 4x size, then downscaled.
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import math

OUT = []
FD = '/usr/share/fonts/truetype/dejavu/'
ALPH = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz+/'

# ---------------------------------------------------------------- fonts
def font(name, path, size, chars=None):
    f = ImageFont.truetype(FD + path, size)
    asc, desc = f.getmetrics()
    chars = chars or [chr(c) for c in range(32, 127)]
    glyphs, data = [], []
    off = 0
    for c in range(32, 127):
        ch = chr(c)
        if ch not in chars:
            glyphs.append('{0,0,0,0,0,0}'); continue
        adv = round(f.getlength(ch))
        l, t, r, b = f.getbbox(ch)
        w, h = max(r - l, 0), max(b - t, 0)
        if w and h:
            im = Image.new('L', (w, h), 0)
            ImageDraw.Draw(im).text((-l, -t), ch, font=f, fill=255)
            px = list(im.getdata())
            data.append(''.join('0123456789abcdef'[(p * 15 + 127) // 255] for p in px))
        glyphs.append('{%d,%d,%d,%d,%d,%d}' % (adv, w, h, l, t, off))
        off += w * h
    OUT.append('static const Glyph %s_G[95] = {%s};' % (name, ','.join(glyphs)))
    s = ''.join(data)
    lines = [s[i:i + 120] for i in range(0, len(s), 120)]
    OUT.append('static const char %s_A[] =\n"%s";' % (name, '"\n"'.join(lines)))
    OUT.append('static const Font %s = {%s_G, %s_A, %d};' % (name, name, name, asc + desc))

# ---------------------------------------------------------------- icons
def emit(name, im, size):
    im = im.resize((size, size), Image.LANCZOS)
    q = im.quantize(colors=63, method=Image.Quantize.FASTOCTREE)
    pal = q.getpalette('RGBA')
    n = len(pal) // 4
    cols = []
    for i in range(n):
        r, g, b, a = pal[i * 4:i * 4 + 4]
        cols.append('0x%02x%02x%02x%02x' % (a, r, g, b))
    px = list(q.getdata())
    s = ''.join(ALPH[p] for p in px)
    lines = [s[i:i + size] for i in range(0, len(s), size)]
    OUT.append('static const u32 %s_P[] = {%s};' % (name, ','.join(cols)))
    OUT.append('static const char %s_D[] =\n"%s";' % (name, '"\n"'.join(lines)))
    OUT.append('static const Image %s = {%d, %d, %s_P, %s_D};' % (name, size, size, name, name))

def canvas(n=128):
    im = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)

def vgrad(size, box, c1, c2, radius=0):
    """rounded rect filled with vertical gradient"""
    x0, y0, x1, y1 = box
    g = Image.new('RGBA', size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(g)
    for y in range(y0, y1 + 1):
        t = (y - y0) / max(1, y1 - y0)
        c = tuple(int(c1[i] + (c2[i] - c1[i]) * t) for i in range(3)) + (255,)
        gd.line([(x0, y), (x1, y)], fill=c)
    m = Image.new('L', size, 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius=radius, fill=255)
    out = Image.new('RGBA', size, (0, 0, 0, 0))
    out.paste(g, (0, 0), m)
    return out

def shadow(im, off=4, blur=4, alpha=90):
    a = im.split()[3]
    sh = Image.new('RGBA', im.size, (0, 0, 0, 0))
    sh.putalpha(a.point(lambda v: v * alpha // 255))
    sh = sh.filter(ImageFilter.GaussianBlur(blur))
    base = Image.new('RGBA', im.size, (0, 0, 0, 0))
    base.paste(sh, (off // 2, off), sh)
    return Image.alpha_composite(base, im)

INK = (40, 44, 52, 255)

def icon_disk():
    im = vgrad((128, 128), (10, 38, 118, 94), (240, 242, 246), (150, 158, 170), 12)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((10, 38, 118, 94), 12, outline=INK, width=4)
    d.line([(14, 76), (114, 76)], fill=(110, 118, 130, 255), width=3)
    d.ellipse((92, 81, 104, 91), fill=(52, 211, 153, 255), outline=(20, 90, 60, 255), width=2)
    for i in range(5):
        d.line([(22 + i * 8, 82), (22 + i * 8, 90)], fill=(90, 98, 110, 255), width=3)
    d.polygon([(64, 46), (74, 56), (64, 68), (54, 56)], fill=(99, 102, 241, 255))
    d.polygon([(64, 46), (74, 56), (64, 56)], fill=(56, 189, 248, 255))
    return shadow(im)

def icon_doc(lines_col=(120, 160, 230, 255), margin=True):
    im, d = canvas()
    d.polygon([(26, 8), (84, 8), (104, 28), (104, 120), (26, 120)], fill=(255, 255, 255, 255), outline=INK)
    d.line([(26, 8), (84, 8), (104, 28), (104, 120), (26, 120), (26, 8)], fill=INK, width=4, joint='curve')
    d.polygon([(84, 8), (84, 28), (104, 28)], fill=(215, 220, 228, 255))
    d.line([(84, 8), (84, 28), (104, 28)], fill=INK, width=4)
    for y in range(42, 112, 12):
        d.line([(34, y), (96, y)], fill=lines_col, width=3)
    if margin:
        d.line([(42, 30), (42, 116)], fill=(235, 90, 90, 255), width=3)
    return shadow(im)

def icon_readme():
    im, d = canvas()
    d.rectangle((26, 8, 104, 120), fill=(255, 255, 255, 255), outline=INK, width=4)
    d.ellipse((50, 22, 80, 52), fill=(74, 91, 214, 255))
    d.text((60, 25), 'i', fill=(255, 255, 255, 255), font=ImageFont.truetype(FD + 'DejaVuSans-Bold.ttf', 26))
    for i, y in enumerate(range(64, 112, 11)):
        d.line([(36, y), (94 - (i % 2) * 18, y)], fill=(150, 156, 168, 255), width=4)
    return shadow(im)

def icon_paint():
    im, d = canvas()
    d.ellipse((8, 24, 112, 110), fill=(240, 214, 160, 255), outline=INK, width=4)
    d.ellipse((66, 76, 88, 96), fill=(0, 0, 0, 0), outline=INK, width=4)
    for (x, y, c) in [(30, 46, (230, 57, 70)), (54, 36, (252, 196, 25)), (80, 42, (56, 132, 255)),
                      (28, 72, (46, 196, 100)), (46, 90, (160, 90, 220))]:
        d.ellipse((x - 9, y - 9, x + 9, y + 9), fill=c + (255,), outline=(60, 50, 40, 255), width=2)
    d.line([(122, 6), (78, 66)], fill=(150, 90, 40, 255), width=12)
    d.line([(122, 6), (78, 66)], fill=INK, width=2)
    d.polygon([(76, 60), (86, 68), (70, 86), (64, 80)], fill=(190, 195, 205, 255), outline=INK)
    d.polygon([(64, 80), (70, 86), (56, 96)], fill=(230, 57, 70, 255), outline=INK)
    return shadow(im)

def icon_trash():
    im = vgrad((128, 128), (28, 34, 100, 120), (220, 224, 230), (140, 148, 160), 6)
    d = ImageDraw.Draw(im)
    d.polygon([(26, 34), (102, 34), (94, 120), (34, 120)], outline=INK)
    d.line([(26, 34), (102, 34), (94, 120), (34, 120), (26, 34)], fill=INK, width=4)
    for x in (46, 64, 82):
        d.line([(x, 46), (x - (x - 64) // 8, 110)], fill=(100, 108, 120, 255), width=5)
    d.rounded_rectangle((18, 22, 110, 34), 4, fill=(200, 205, 212, 255), outline=INK, width=4)
    d.rounded_rectangle((50, 12, 78, 24), 4, outline=INK, width=4)
    return shadow(im)

def icon_puzzle():
    im, d = canvas()
    cols = [(230, 57, 70), (252, 196, 25), (56, 132, 255), (46, 196, 100),
            (160, 90, 220), (255, 140, 60), (40, 180, 200), (240, 100, 160), None]
    d.rounded_rectangle((8, 8, 120, 120), 10, fill=(70, 76, 90, 255), outline=INK, width=4)
    for i, c in enumerate(cols):
        if c is None: continue
        x, y = 16 + (i % 3) * 34, 16 + (i // 3) * 34
        d.rounded_rectangle((x, y, x + 30, y + 30), 6, fill=c + (255,), outline=(30, 30, 40, 255), width=2)
        d.line([(x + 5, y + 5), (x + 24, y + 5)], fill=(255, 255, 255, 120), width=3)
    return shadow(im)

def icon_computer():
    im = vgrad((128, 128), (14, 10, 114, 92), (236, 232, 222), (196, 190, 176), 10)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((14, 10, 114, 92), 10, outline=INK, width=4)
    scr = vgrad((128, 128), (26, 20, 102, 76), (40, 120, 170), (20, 40, 90), 4)
    im.alpha_composite(scr)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((26, 20, 102, 76), 4, outline=INK, width=3)
    d.polygon([(64, 32), (76, 46), (64, 64), (52, 46)], fill=(167, 139, 250, 255))
    d.polygon([(64, 32), (76, 46), (64, 46)], fill=(94, 234, 212, 255))
    d.rectangle((44, 92, 84, 104), fill=(190, 184, 170, 255), outline=INK, width=4)
    d.rounded_rectangle((24, 102, 104, 118), 5, fill=(214, 208, 194, 255), outline=INK, width=4)
    return shadow(im)

def icon_mouse():
    im = vgrad((128, 128), (36, 30, 92, 118), (250, 250, 252), (180, 186, 196), 26)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((36, 30, 92, 118), 26, outline=INK, width=4)
    d.line([(64, 32), (64, 64)], fill=INK, width=3)
    d.line([(38, 64), (90, 64)], fill=INK, width=3)
    d.line([(64, 30), (64, 6)], fill=INK, width=4)
    return shadow(im)

def icon_keyboard():
    im = vgrad((128, 128), (4, 36, 124, 98), (236, 238, 242), (170, 176, 186), 10)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((4, 36, 124, 98), 10, outline=INK, width=4)
    for r in range(3):
        for c in range(8):
            x, y = 14 + c * 13 + (r % 2) * 5, 46 + r * 14
            d.rounded_rectangle((x, y, x + 9, y + 9), 2, fill=(255, 255, 255, 255), outline=(90, 96, 108, 255))
    d.rounded_rectangle((34, 86, 94, 93), 2, fill=(255, 255, 255, 255), outline=(90, 96, 108, 255))
    return shadow(im)

def icon_clock():
    im, d = canvas()
    d.ellipse((8, 8, 120, 120), fill=(74, 91, 214, 255), outline=INK, width=4)
    d.ellipse((18, 18, 110, 110), fill=(255, 255, 255, 255), outline=INK, width=3)
    for i in range(12):
        a = i * math.pi / 6
        r1, r2 = (34, 44) if i % 3 else (28, 44)
        d.line([(64 + r1 * math.sin(a), 64 - r1 * math.cos(a)), (64 + r2 * math.sin(a), 64 - r2 * math.cos(a))],
               fill=INK, width=3 if i % 3 else 5)
    d.line([(64, 64), (64, 32)], fill=INK, width=6)
    d.line([(64, 64), (88, 76)], fill=INK, width=5)
    d.ellipse((59, 59, 69, 69), fill=(230, 57, 70, 255))
    return shadow(im)

def icon_sound():
    im, d = canvas()
    d.rectangle((14, 46, 40, 82), fill=(200, 205, 214, 255), outline=INK, width=4)
    d.polygon([(40, 46), (72, 18), (72, 110), (40, 82)], fill=(160, 168, 180, 255), outline=INK)
    d.line([(40, 46), (72, 18), (72, 110), (40, 82)], fill=INK, width=4)
    for r in (16, 30, 44):
        d.arc((72 - r + 10, 64 - r, 72 + r + 10, 64 + r), -50, 50, fill=(74, 91, 214, 255), width=6)
    return shadow(im)

def gem(n=512, shadowed=False):
    im, d = canvas(n)
    k = n / 128
    P = lambda *pts: [(x * k, y * k) for x, y in pts]
    top, mid, bot = 22, 50, 116
    # crown facets
    d.polygon(P((30, mid), (44, top), (64, mid)), fill=(94, 234, 212, 255))
    d.polygon(P((44, top), (84, top), (64, mid)), fill=(165, 243, 252, 255))
    d.polygon(P((84, top), (98, mid), (64, mid)), fill=(56, 189, 248, 255))
    d.polygon(P((14, mid), (30, mid), (44, top), (36, top)), fill=(45, 212, 191, 255))
    d.polygon(P((114, mid), (98, mid), (84, top), (92, top)), fill=(59, 130, 246, 255))
    # pavilion facets
    d.polygon(P((14, mid), (30, mid), (64, bot)), fill=(20, 184, 166, 255))
    d.polygon(P((30, mid), (64, mid), (64, bot)), fill=(56, 189, 248, 255))
    d.polygon(P((64, mid), (98, mid), (64, bot)), fill=(99, 102, 241, 255))
    d.polygon(P((98, mid), (114, mid), (64, bot)), fill=(124, 58, 237, 255))
    outline = P((36, top), (92, top), (114, mid), (64, bot), (14, mid), (36, top))
    d.line(outline, fill=(30, 27, 75, 255), width=int(4 * k), joint='curve')
    d.line(P((14, mid), (114, mid)), fill=(30, 27, 75, 200), width=int(2 * k))
    d.polygon(P((48, 26), (60, 26), (52, 40)), fill=(255, 255, 255, 220))
    return shadow(im, off=int(4 * k), blur=int(4 * k)) if shadowed else im

def wordmark():
    f = ImageFont.truetype(FD + 'DejaVuSans-Bold.ttf', 132)
    im = Image.new('RGBA', (900, 180), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.text((0, 0), 'Facet', font=f, fill=(30, 34, 48, 255))
    w = d.textlength('Facet', font=f)
    d.text((w + 6, 0), 'OS', font=f, fill=(74, 91, 214, 255))
    im = im.crop(im.getbbox())
    return im

def emit_rect(name, im, w):
    h = round(im.height * w / im.width)
    im = im.resize((w, h), Image.LANCZOS)
    q = im.quantize(colors=63, method=Image.Quantize.FASTOCTREE)
    pal = q.getpalette('RGBA')
    cols = ['0x%02x%02x%02x%02x' % (pal[i*4+3], pal[i*4], pal[i*4+1], pal[i*4+2]) for i in range(len(pal)//4)]
    s = ''.join(ALPH[p] for p in q.getdata())
    lines = [s[i:i + w] for i in range(0, len(s), w)]
    OUT.append('static const u32 %s_P[] = {%s};' % (name, ','.join(cols)))
    OUT.append('static const char %s_D[] =\n"%s";' % (name, '"\n"'.join(lines)))
    OUT.append('static const Image %s = {%d, %d, %s_P, %s_D};' % (name, w, h, name, name))

OUT.append('/* FacetOS assets - generated by gen_assets.py.\n'
           ' * Fonts rasterized from DejaVu Sans (Bitstream Vera / DejaVu license).\n'
           ' * Icons and logo are original FacetOS artwork. */')
font('F_REG', 'DejaVuSans.ttf', 13)
font('F_BOLD', 'DejaVuSans-Bold.ttf', 13)
font('F_BIG', 'DejaVuSans-Bold.ttf', 22)
for nm, fn in [('IC_DISK', icon_disk), ('IC_NOTES', icon_doc), ('IC_README', icon_readme),
               ('IC_PAINT', icon_paint), ('IC_TRASH', icon_trash), ('IC_PUZZLE', icon_puzzle),
               ('IC_COMPUTER', icon_computer), ('IC_MOUSE', icon_mouse), ('IC_KEYBOARD', icon_keyboard),
               ('IC_CLOCK', icon_clock), ('IC_SOUND', icon_sound)]:
    emit(nm, fn(), 32)
emit('GEM_BIG', gem(512, True), 72)
emit('GEM_SMALL', gem(512), 18)
emit_rect('WORDMARK', wordmark(), 190)
# preview sheet for checking
sheet = Image.new('RGBA', (520, 140), (200, 205, 215, 255))
x = 4
for fn in [icon_disk, icon_doc, icon_readme, icon_paint, icon_trash, icon_puzzle, icon_computer,
           icon_mouse, icon_keyboard, icon_clock, icon_sound]:
    sheet.alpha_composite(fn().resize((32, 32), Image.LANCZOS), (x, 4)); x += 40
sheet.alpha_composite(gem(512, True).resize((72, 72), Image.LANCZOS), (4, 44))
wm = wordmark(); wm = wm.resize((190, round(wm.height * 190 / wm.width)), Image.LANCZOS)
sheet.alpha_composite(wm, (90, 70))
sheet.save('assets_preview.png')
open('assets.h', 'w').write('\n'.join(OUT) + '\n')
print('ok', sum(len(s) for s in OUT))
