# Generates assets.h for FacetOS 1.2 "Prism".
# Fonts: Inter (SIL Open Font License, see fonts/LICENSE.txt), rasterized to 4-bit alpha.
# Icons, logo and wordmark: original FacetOS artwork drawn here with simple shapes.
import math, os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
FD = os.path.join(HERE, 'fonts') + '/'
ALPH = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz+/'
OUT = []

# ---------------------------------------------------------------- fonts
def font(name, path, size):
    f = ImageFont.truetype(FD + path, size)
    asc, desc = f.getmetrics()
    glyphs, data, off = [], [], 0
    for c in range(32, 127):
        ch = chr(c)
        adv = round(f.getlength(ch))
        l, t, r, b = f.getbbox(ch)
        w, h = max(r - l, 0), max(b - t, 0)
        if w and h:
            im = Image.new('L', (w, h), 0)
            ImageDraw.Draw(im).text((-l, -t), ch, font=f, fill=255)
            data.append(''.join('0123456789abcdef'[(p * 15 + 127) // 255] for p in im.get_flattened_data()))
        glyphs.append('{%d,%d,%d,%d,%d,%d}' % (adv, w, h, l, t, off))
        off += w * h
    OUT.append('static const Glyph %s_G[95] = {%s};' % (name, ','.join(glyphs)))
    s = ''.join(data)
    OUT.append('static const char %s_A[] =\n"%s";' % (name, '"\n"'.join(s[i:i + 120] for i in range(0, len(s), 120))))
    OUT.append('static const Font %s = {%s_G, %s_A, %d};' % (name, name, name, asc + desc))

# ---------------------------------------------------------------- images
def emit(name, im):
    q = im.quantize(colors=63, method=Image.Quantize.FASTOCTREE)
    pal = q.getpalette('RGBA')
    cols = ['0x%02x%02x%02x%02x' % (pal[i*4+3], pal[i*4], pal[i*4+1], pal[i*4+2]) for i in range(len(pal) // 4)]
    s = ''.join(ALPH[p] for p in q.get_flattened_data())
    w, h = im.size
    OUT.append('static const u32 %s_P[] = {%s};' % (name, ','.join(cols)))
    OUT.append('static const char %s_D[] =\n"%s";' % (name, '"\n"'.join(s[i:i + w] for i in range(0, len(s), w))))
    OUT.append('static const Image %s = {%d, %d, %s_P, %s_D};' % (name, w, h, name, name))

def hexc(h, a=255):
    h = h.lstrip('#'); return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)

def lerp(c1, c2, t): return tuple(int(c1[i] + (c2[i] - c1[i]) * t) for i in range(4))

N = 256                     # icons are drawn at 256 px, then shrunk
def octagon(n, inset, cut):
    a, b = inset, n - 1 - inset
    return [(a + cut, a), (b - cut, a), (b, a + cut), (b, b - cut), (b - cut, b), (a + cut, b), (a, b - cut), (a, a + cut)]

def tile(c1, c2):
    """faceted octagon tile with a diagonal light/dark split"""
    im = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    grad = Image.new('RGBA', (N, N))
    gd = ImageDraw.Draw(grad)
    for y in range(N):
        gd.line([(0, y), (N, y)], fill=lerp(hexc(c1), hexc(c2), y / N))
    mask = Image.new('L', (N, N), 0)
    ImageDraw.Draw(mask).polygon(octagon(N, 10, 62), fill=255)
    im.paste(grad, (0, 0), mask)
    # facet shading: light upper-left triangle, darker lower-right
    sh = Image.new('RGBA', (N, N), (0, 0, 0, 0)); sd = ImageDraw.Draw(sh)
    sd.polygon([(0, 0), (N, 0), (0, N)], fill=(255, 255, 255, 34))
    sd.polygon([(N, N), (N, N * 0.45), (N * 0.45, N)], fill=(0, 0, 0, 40))
    sh.putalpha(Image.composite(sh.split()[3], Image.new('L', (N, N), 0), mask))
    im = Image.alpha_composite(im, sh)
    d = ImageDraw.Draw(im)
    d.line(octagon(N, 10, 62) + [octagon(N, 10, 62)[0]], fill=(255, 255, 255, 90), width=5)
    d.line(octagon(N, 4, 64) + [octagon(N, 4, 64)[0]], fill=(10, 12, 24, 170), width=6)
    return im

W = (255, 255, 255, 255)
def glyph_layer():
    return Image.new('RGBA', (N, N), (0, 0, 0, 0))

def finish(base, g):
    shadow = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    shadow.putalpha(g.split()[3].point(lambda v: v * 110 // 255))
    shadow = shadow.filter(ImageFilter.GaussianBlur(5))
    s2 = Image.new('RGBA', (N, N), (0, 0, 0, 0)); s2.paste(shadow, (0, 6), shadow)
    return Image.alpha_composite(Image.alpha_composite(base, s2), g)

def g_system():
    g = glyph_layer(); d = ImageDraw.Draw(g)
    d.rounded_rectangle((62, 66, 194, 158), 12, outline=W, width=13)
    d.line([(128, 160), (128, 184)], fill=W, width=13); d.line([(88, 190), (168, 190)], fill=W, width=13)
    d.polygon([(128, 92), (146, 112), (128, 134), (110, 112)], fill=W)
    return g
def g_vault():
    g = glyph_layer(); d = ImageDraw.Draw(g)
    d.rounded_rectangle((60, 62, 196, 194), 16, outline=W, width=13)
    d.ellipse((96, 96, 160, 160), outline=W, width=12)
    for a in range(0, 360, 90):
        x, y = 128 + 44 * math.cos(math.radians(a)), 128 + 44 * math.sin(math.radians(a))
        d.line([(128 + 32 * math.cos(math.radians(a)), 128 + 32 * math.sin(math.radians(a))), (x, y)], fill=W, width=10)
    d.ellipse((118, 118, 138, 138), fill=W)
    return g
def g_notes():
    g = glyph_layer(); d = ImageDraw.Draw(g)
    for i, y in enumerate((84, 116, 148, 180)):
        d.line([(66, y), (190 - (40 if i == 3 else 0), y)], fill=W, width=13)
    return g
def g_paint():
    g = glyph_layer(); d = ImageDraw.Draw(g)
    d.line([(178, 62), (116, 140)], fill=W, width=22)
    d.polygon([(104, 130), (128, 152), (96, 196), (64, 200), (70, 168)], fill=W)
    return g
def g_calc():
    g = glyph_layer(); d = ImageDraw.Draw(g)
    d.line([(70, 92), (114, 92)], fill=W, width=12); d.line([(92, 70), (92, 114)], fill=W, width=12)
    d.line([(142, 92), (186, 92)], fill=W, width=12)
    d.line([(74, 146), (110, 182)], fill=W, width=12); d.line([(110, 146), (74, 182)], fill=W, width=12)
    d.line([(142, 154), (186, 154)], fill=W, width=12); d.line([(142, 176), (186, 176)], fill=W, width=12)
    return g
def g_puzzle():
    g = glyph_layer(); d = ImageDraw.Draw(g)
    for (x, y) in ((66, 66), (134, 66), (66, 134)):
        d.rounded_rectangle((x, y, x + 56, y + 56), 10, fill=W)
    d.rounded_rectangle((134, 134, 190, 190), 10, outline=W, width=8)
    return g
def g_settings():
    g = glyph_layer(); d = ImageDraw.Draw(g)
    for i, (y, k) in enumerate(((84, 150), (128, 96), (172, 132))):
        d.line([(64, y), (192, y)], fill=W, width=10)
        d.ellipse((k - 16, y - 16, k + 16, y + 16), fill=W)
    return g
def g_readme():
    g = glyph_layer(); d = ImageDraw.Draw(g)
    d.ellipse((114, 58, 142, 86), fill=W)
    d.rounded_rectangle((114, 104, 142, 196), 8, fill=W)
    d.rectangle((100, 104, 128, 120), fill=W); d.rectangle((100, 184, 156, 198), fill=W)
    return g
def g_bin():
    g = glyph_layer(); d = ImageDraw.Draw(g)
    d.line([(64, 82), (192, 82)], fill=W, width=12); d.rounded_rectangle((106, 56, 150, 82), 6, outline=W, width=10)
    d.polygon([(80, 96), (176, 96), (166, 198), (90, 198)], outline=W, fill=None)
    d.line([(80, 96), (176, 96), (166, 198), (90, 198), (80, 96)], fill=W, width=12, joint='curve')
    for x in (110, 128, 146): d.line([(x, 118), (x, 178)], fill=W, width=8)
    return g

def app_icon(c1, c2, gfn):
    return finish(tile(c1, c2), gfn())

def doc_icon(kind):
    im = Image.new('RGBA', (N, N), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    body = [(56, 18), (164, 18), (204, 58), (204, 238), (56, 238)]
    sh = Image.new('RGBA', (N, N), (0, 0, 0, 0)); ImageDraw.Draw(sh).polygon([(x, y + 8) for x, y in body], fill=(0, 0, 0, 110))
    im = Image.alpha_composite(im, sh.filter(ImageFilter.GaussianBlur(6))); d = ImageDraw.Draw(im)
    d.polygon(body, fill=(240, 244, 252, 255))
    d.polygon([(164, 18), (164, 58), (204, 58)], fill=(190, 200, 222, 255))
    d.line(body + [body[0]], fill=(30, 34, 52, 255), width=6)
    if kind == 'txt':
        for i, y in enumerate((96, 126, 156, 186)):
            d.line([(84, y), (176 - (40 if i == 3 else 0), y)], fill=(90, 100, 130, 255), width=10)
        d.polygon([(56, 200), (120, 200), (120, 238), (56, 238)], fill=(245, 158, 11, 255))
        d.text((66, 202), 'TXT', fill=(255, 255, 255, 255), font=ImageFont.truetype(FD + 'InterDisplay-Bold.ttf', 30))
    else:
        d.rectangle((80, 84, 180, 180), fill=(56, 189, 248, 255))
        d.polygon([(80, 180), (118, 124), (148, 160), (162, 142), (180, 180)], fill=(20, 120, 90, 255))
        d.ellipse((148, 96, 168, 116), fill=(252, 211, 77, 255))
        d.polygon([(56, 200), (124, 200), (124, 238), (56, 238)], fill=(236, 72, 153, 255))
        d.text((64, 202), 'BMP', fill=(255, 255, 255, 255), font=ImageFont.truetype(FD + 'InterDisplay-Bold.ttf', 30))
    return im

def gem(n=512):
    im = Image.new('RGBA', (n, n), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    k = n / 128
    P = lambda *pts: [(x * k, y * k) for x, y in pts]
    top, mid, bot = 22, 50, 116
    d.polygon(P((30, mid), (44, top), (64, mid)), fill=(94, 234, 212, 255))
    d.polygon(P((44, top), (84, top), (64, mid)), fill=(165, 243, 252, 255))
    d.polygon(P((84, top), (98, mid), (64, mid)), fill=(56, 189, 248, 255))
    d.polygon(P((14, mid), (30, mid), (44, top), (36, top)), fill=(45, 212, 191, 255))
    d.polygon(P((114, mid), (98, mid), (84, top), (92, top)), fill=(59, 130, 246, 255))
    d.polygon(P((14, mid), (30, mid), (64, bot)), fill=(20, 184, 166, 255))
    d.polygon(P((30, mid), (64, mid), (64, bot)), fill=(56, 189, 248, 255))
    d.polygon(P((64, mid), (98, mid), (64, bot)), fill=(99, 102, 241, 255))
    d.polygon(P((98, mid), (114, mid), (64, bot)), fill=(124, 58, 237, 255))
    d.line(P((36, top), (92, top), (114, mid), (64, bot), (14, mid), (36, top)), fill=(230, 250, 255, 255), width=int(3 * k), joint='curve')
    d.line(P((14, mid), (114, mid)), fill=(230, 250, 255, 200), width=int(1.5 * k))
    d.polygon(P((48, 26), (60, 26), (52, 40)), fill=(255, 255, 255, 220))
    return im

def wordmark(width):
    f = ImageFont.truetype(FD + 'InterDisplay-Bold.ttf', 160)
    im = Image.new('RGBA', (1000, 220), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    d.text((0, 0), 'Facet', font=f, fill=(244, 246, 255, 255))
    wf = d.textlength('Facet', font=f)
    # prism-gradient "OS"
    m = Image.new('L', im.size, 0); ImageDraw.Draw(m).text((wf + 4, 0), 'OS', font=f, fill=255)
    g = Image.new('RGBA', im.size)
    gd = ImageDraw.Draw(g)
    x0, x1 = int(wf), int(wf + d.textlength('OS', font=f) + 8)
    for x in range(im.width):
        t = min(1, max(0, (x - x0) / max(1, x1 - x0)))
        c = lerp(hexc('#5EEAD4'), hexc('#8B5CF6'), t) if t < 1 else hexc('#8B5CF6')
        gd.line([(x, 0), (x, im.height)], fill=c)
    im.paste(g, (0, 0), m)
    im = im.crop(im.getbbox())
    return im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)

APPS = [('SYSTEM', '#64748B', '#1E293B', g_system), ('VAULT', '#14B8A6', '#0F5E59', g_vault),
        ('NOTES', '#FBBF24', '#B45309', g_notes), ('PAINT', '#F472B6', '#9D174D', g_paint),
        ('CALC', '#818CF8', '#3730A3', g_calc), ('PUZZLE', '#A78BFA', '#5B21B6', g_puzzle),
        ('SETTINGS', '#38BDF8', '#075985', g_settings), ('README', '#60A5FA', '#1D4ED8', g_readme),
        ('BIN', '#FB7185', '#9F1239', g_bin)]

if __name__ == '__main__':
    OUT.append('/* FacetOS 1.2 assets - generated by gen_assets.py.\n'
               ' * Font: Inter by Rasmus Andersson (SIL Open Font License 1.1, see licenses/).\n'
               ' * Icons, logo and wordmark: original FacetOS artwork. */')
    font('F_REG', 'Inter-Regular.ttf', 13)
    font('F_BOLD', 'Inter-SemiBold.ttf', 13)
    font('F_BIG', 'InterDisplay-Bold.ttf', 22)
    font('F_HUGE', 'InterDisplay-Bold.ttf', 34)
    big = {}
    for name, c1, c2, gfn in APPS:
        im = app_icon(c1, c2, gfn)
        big[name] = im
        emit('IC_' + name, im.resize((48, 48), Image.LANCZOS))
        emit('IS_' + name, im.resize((20, 20), Image.LANCZOS))
    for k in ('txt', 'bmp'):
        im = doc_icon(k)
        emit('IC_DOC_' + k.upper(), im.resize((48, 48), Image.LANCZOS))
    g = gem(1024)
    emit('GEM_BIG', g.resize((96, 96), Image.LANCZOS))
    emit('GEM_MED', g.resize((30, 30), Image.LANCZOS))
    emit('GEM_SMALL', g.resize((18, 18), Image.LANCZOS))
    emit('WORDMARK', wordmark(230))
    open(os.path.join(os.path.dirname(HERE) if os.path.basename(HERE) == 'tools' else HERE, 'assets.h'), 'w').write('\n'.join(OUT) + '\n')
    # preview sheet
    sheet = Image.new('RGBA', (900, 260), (14, 18, 32, 255))
    x = 10
    for name, *_ in APPS:
        sheet.alpha_composite(big[name].resize((64, 64), Image.LANCZOS), (x, 10)); x += 76
    x = 10
    for name, *_ in APPS:
        sheet.alpha_composite(big[name].resize((48, 48), Image.LANCZOS), (x, 90))
        sheet.alpha_composite(big[name].resize((20, 20), Image.LANCZOS), (x + 54, 104)); x += 86
    sheet.alpha_composite(doc_icon('txt').resize((48, 48), Image.LANCZOS), (10, 160))
    sheet.alpha_composite(doc_icon('bmp').resize((48, 48), Image.LANCZOS), (70, 160))
    sheet.alpha_composite(g.resize((96, 96), Image.LANCZOS), (140, 150))
    sheet.alpha_composite(wordmark(230), (250, 180))
    sheet.save(os.path.join(HERE, 'assets_preview.png'))
    print('ok', sum(len(s) for s in OUT))
