/*
 * FacetOS 1.0 - a tiny 32-bit desktop operating system that draws every pixel itself.
 * Classic desktop style, original art. Boots via Multiboot2 (Limine / GRUB) on any x86 PC,
 * including 32-bit-only emulators like v86.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t i32; typedef uint64_t u64;

typedef struct { u8 adv, w, h; signed char xo, yo; u32 off; } Glyph;
typedef struct { const Glyph *g; const char *a; int lh; } Font;
typedef struct { int w, h; const u32 *pal; const char *d; } Image;
#include "assets.h"

/* ====================================================================
 *  Boot: Multiboot2 header + entry point
 * ==================================================================== */
#define MB2_MAGIC 0xE85250D6u
__attribute__((section(".multiboot"), aligned(8), used))
static const u32 mb2_header[] = {
    MB2_MAGIC, 0, 48, (u32)(0u - (MB2_MAGIC + 0u + 48u)),
    5 | (0 << 16), 20, 1024, 768, 32, 0,      /* framebuffer tag: 1024x768x32 */
    0, 8,                                      /* end tag */
};
__asm__(
    ".section .bss\n.align 16\nboot_stack: .skip 65536\nboot_stack_top:\n"
    ".section .text\n.global _start\n_start:\n"
    "  cli\n  mov $boot_stack_top, %esp\n  push %ebx\n  push %eax\n  call kmain\n"
    "1: cli\n  hlt\n  jmp 1b\n");

static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static void halt(void) { for (;;) __asm__ volatile("cli; hlt"); }

void *memset(void *d, int c, size_t n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    u8 *a = d; const u8 *b = s;
    if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
int memcmp(const void *x, const void *y, size_t n) {
    const u8 *a = x, *b = y;
    for (; n--; a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iabs(int a) { return a < 0 ? -a : a; }

/* ====================================================================
 *  Timer (PIT polled, 1.193182 MHz) + PC speaker
 * ==================================================================== */
static u32 ms_now; static u16 pit_last; static u32 pit_acc;
static u16 pit_read(void) { outb(0x43, 0x00); u8 lo = inb(0x40); u8 hi = inb(0x40); return (u16)(lo | hi << 8); }
static void pit_init(void) { outb(0x43, 0x34); outb(0x40, 0); outb(0x40, 0); pit_last = pit_read(); }
static void tick(void) {
    u16 c = pit_read();
    pit_acc += (u16)(pit_last - c); pit_last = c;
    while (pit_acc >= 1193) { pit_acc -= 1193; ms_now++; }
}
static void sleep_ms(u32 ms) { u32 end = ms_now + ms; while ((i32)(end - ms_now) > 0) tick(); }
static void tone(u32 hz) {
    if (!hz) { outb(0x61, inb(0x61) & 0xFC); return; }
    u32 div = 1193182 / hz;
    outb(0x43, 0xB6); outb(0x42, (u8)div); outb(0x42, (u8)(div >> 8));
    outb(0x61, inb(0x61) | 3);
}

/* ====================================================================
 *  Graphics core: everything is drawn into a back buffer, then copied
 * ==================================================================== */
#define MAXW 1280
#define MAXH 1024
static u32 bb[MAXW * MAXH];          /* composed scene */
static u32 bgbuf[MAXW * MAXH];       /* desktop wallpaper */
static u32 *fb; static u32 fb_stride;
static int W, H;
static int cx0, cy0, cx1, cy1;       /* clip rectangle */

static void clip_set(int x, int y, int w, int h) {
    cx0 = imax(x, 0); cy0 = imax(y, 0); cx1 = imin(x + w, W); cy1 = imin(y + h, H);
}
static void clip_all(void) { clip_set(0, 0, W, H); }
static inline u32 blend(u32 d, u32 s, u32 a) {      /* s over d with alpha a (0..255) */
    if (a >= 255) return s;
    if (!a) return d;
    u32 ia = 255 - a;
    u32 rb = (s & 0xFF00FF) * a + (d & 0xFF00FF) * ia + 0x800080;   /* red+blue together */
    u32 g = (s & 0x00FF00) * a + (d & 0x00FF00) * ia + 0x008000;
    rb = ((rb + ((rb >> 8) & 0xFF00FF)) >> 8) & 0xFF00FF;           /* fast divide by 255 */
    g = ((g + ((g >> 8) & 0x00FF00)) >> 8) & 0x00FF00;
    return rb | g;
}
static u32 mix(u32 c1, u32 c2, int t, int n) {   /* t/n of the way from c1 to c2 */
    int r = ((c1 >> 16) & 255) + ((((int)((c2 >> 16) & 255)) - (int)((c1 >> 16) & 255)) * t) / n;
    int g = ((c1 >> 8) & 255) + ((((int)((c2 >> 8) & 255)) - (int)((c1 >> 8) & 255)) * t) / n;
    int b = (c1 & 255) + ((((int)(c2 & 255)) - (int)(c1 & 255)) * t) / n;
    return (u32)(r << 16 | g << 8 | b);
}
static inline void pblend(int x, int y, u32 c, u32 a) {
    if (x >= cx0 && x < cx1 && y >= cy0 && y < cy1) { u32 *p = &bb[y * MAXW + x]; *p = blend(*p, c, a); }
}
#define CLIPRECT int x0 = imax(x, cx0), y0 = imax(y, cy0), x1 = imin(x + w, cx1), y1 = imin(y + h, cy1)
static void fill(int x, int y, int w, int h, u32 c) {
    CLIPRECT;
    for (int j = y0; j < y1; j++) { u32 *r = &bb[j * MAXW]; for (int i = x0; i < x1; i++) r[i] = c; }
}
static void fill_a(int x, int y, int w, int h, u32 c, u32 a) {
    CLIPRECT;
    for (int j = y0; j < y1; j++) { u32 *r = &bb[j * MAXW]; for (int i = x0; i < x1; i++) r[i] = blend(r[i], c, a); }
}
static void gradient(int x, int y, int w, int h, u32 c1, u32 c2) {
    CLIPRECT;
    for (int j = y0; j < y1; j++) {
        u32 c = mix(c1, c2, j - y, h > 1 ? h - 1 : 1), *r = &bb[j * MAXW];
        for (int i = x0; i < x1; i++) r[i] = c;
    }
}
static void hline(int x, int y, int w, u32 c) { fill(x, y, w, 1, c); }
static void vline(int x, int y, int h, u32 c) { fill(x, y, 1, h, c); }
static void frame(int x, int y, int w, int h, u32 c) {
    hline(x, y, w, c); hline(x, y + h - 1, w, c); vline(x, y, h, c); vline(x + w - 1, y, h, c);
}
static void bevel(int x, int y, int w, int h, bool pressed) {   /* 3D button look */
    gradient(x, y, w, h, pressed ? 0xB8BCC4 : 0xFDFDFD, pressed ? 0xD8DBE0 : 0xD2D5DB);
    frame(x, y, w, h, 0x5A5E66);
    if (!pressed) { hline(x + 1, y + 1, w - 2, 0xFFFFFF); vline(x + 1, y + 1, h - 2, 0xFFFFFF); }
}
/* anti-aliased rounded rectangle */
static u32 corner_cov(int i, int j, int ccx, int ccy, int r) {
    int n = 0, rr = r * 8 * r * 8;
    for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
        int dx = i * 8 + 1 + sx * 2 - ccx * 8, dy = j * 8 + 1 + sy * 2 - ccy * 8;
        if (dx * dx + dy * dy <= rr) n++;
    }
    return (u32)(n * 255 / 16);
}
static void rrect(int x, int y, int w, int h, int r, u32 c, u32 alpha) {
    CLIPRECT;
    for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) {
        u32 a = 255;
        bool L = i < x + r, R = i >= x + w - r, T = j < y + r, B = j >= y + h - r;
        if (L && T) a = corner_cov(i, j, x + r, y + r, r);
        else if (R && T) a = corner_cov(i, j, x + w - r, y + r, r);
        else if (L && B) a = corner_cov(i, j, x + r, y + h - r, r);
        else if (R && B) a = corner_cov(i, j, x + w - r, y + h - r, r);
        if (a) pblend(i, j, c, a * alpha / 255);
    }
}
static void soft_shadow(int x, int y, int w, int h, int r) {
    for (int k = 4; k >= 1; k--) rrect(x - k + 3, y - k + 4, w + 2 * k - 2, h + 2 * k - 2, r + k, 0, 18);
}

/* ---- images (palette + one char per pixel) ---- */
static int pidx(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c >= 'a' && c <= 'z') return c - 'a' + 36;
    return c == '+' ? 62 : 63;
}
static void image(const Image *im, int x, int y, u32 alpha, bool dark) {
    int w = im->w, h = im->h;
    CLIPRECT;
    for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) {
        u32 p = im->pal[pidx(im->d[(j - y) * im->w + (i - x)])];
        u32 a = (p >> 24) * alpha / 255;
        if (!a) continue;
        u32 c = p & 0xFFFFFF;
        if (dark) c = blend(c, 0x1A1F3A, 110);
        pblend(i, j, c, a);
    }
}

/* ---- anti-aliased text ---- */
static int hexv(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }
static int text(const Font *f, int x, int y, const char *s, u32 col) {
    for (; *s; s++) {
        int ch = (u8)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const Glyph *g = &f->g[ch - 32];
        for (int j = 0; j < g->h; j++) {
            int py = y + g->yo + j;
            if (py < cy0 || py >= cy1) continue;
            for (int i = 0; i < g->w; i++) {
                int a = hexv(f->a[g->off + j * g->w + i]);
                if (a) pblend(x + g->xo + i, py, col, (u32)a * 17);
            }
        }
        x += g->adv;
    }
    return x;
}
static int text_w(const Font *f, const char *s) {
    int w = 0;
    for (; *s; s++) { int ch = (u8)*s; if (ch < 32 || ch > 126) ch = '?'; w += f->g[ch - 32].adv; }
    return w;
}
static void text_c(const Font *f, int cx, int y, const char *s, u32 col) { text(f, cx - text_w(f, s) / 2, y, s, col); }
static char *utoa(u32 v, char *buf) {     /* returns end of string */
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *buf++ = t[--n];
    *buf = 0; return buf;
}
static char *scat(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }

/* ====================================================================
 *  Overlays drawn straight onto the screen: mouse cursor + drag outline
 * ==================================================================== */
static const char *const CURSOR[19] = {
    "B...........", "BB..........", "BwB.........", "BwwB........", "BwwwB.......", "BwwwwB......",
    "BwwwwwB.....", "BwwwwwwB....", "BwwwwwwwB...", "BwwwwwwwwB..", "BwwwwwwwwwB.", "BwwwwwwBBBBB",
    "BwwwBwwB....", "BwwBBwwB....", "BwB..BwwB...", "BB...BwwB...", "B.....BwwB..", "......BwwB..",
    ".......BB...",
};
static int cur_x, cur_y;
static bool ol_on; static int ol_x, ol_y, ol_w, ol_h;
static void blit(int x, int y, int w, int h) {     /* back buffer -> screen */
    int x0 = imax(x, 0), y0 = imax(y, 0), x1 = imin(x + w, W), y1 = imin(y + h, H);
    for (int j = y0; j < y1; j++) {
        u32 *s = &bb[j * MAXW], *d = &fb[j * fb_stride];
        for (int i = x0; i < x1; i++) d[i] = s[i];
    }
}
static void outline_draw(void) {
    if (!ol_on) return;
    for (int k = 0; k < 2; k++) {
        int x = ol_x + k, y = ol_y + k, w = ol_w - 2 * k, h = ol_h - 2 * k;
        for (int i = 0; i < w; i++) for (int e = 0; e < 2; e++) {
            int px = x + i, py = e ? y + h - 1 : y;
            if (px >= 0 && px < W && py >= 0 && py < H && ((px + py) & 1)) fb[py * fb_stride + px] = bb[py * MAXW + px] ^ 0xFFFFFF;
        }
        for (int j = 0; j < h; j++) for (int e = 0; e < 2; e++) {
            int py = y + j, px = e ? x + w - 1 : x;
            if (px >= 0 && px < W && py >= 0 && py < H && ((px + py) & 1)) fb[py * fb_stride + px] = bb[py * MAXW + px] ^ 0xFFFFFF;
        }
    }
}
static void outline_erase(void) {
    if (!ol_on) return;
    blit(ol_x, ol_y, ol_w, 2); blit(ol_x, ol_y + ol_h - 2, ol_w, 2);
    blit(ol_x, ol_y, 2, ol_h); blit(ol_x + ol_w - 2, ol_y, 2, ol_h);
}
static void cursor_draw(void) {
    for (int j = 0; j < 19; j++) for (int i = 0; i < 12; i++) {
        char c = CURSOR[j][i];
        int x = cur_x + i, y = cur_y + j;
        if (x >= W || y >= H) continue;
        if (c == 'B') fb[y * fb_stride + x] = 0x000000;
        else if (c == 'w') fb[y * fb_stride + x] = 0xFFFFFF;
        else if (x > cur_x && y > cur_y + 1 && CURSOR[j - 2][i - 1] != '.')   /* soft shadow */
            fb[y * fb_stride + x] = blend(bb[y * MAXW + x], 0, 70);
    }
}
static void present(int x, int y, int w, int h) {
    blit(x, y, w, h);
    if (ol_on) outline_draw();
    if (cur_x < x + w && cur_x + 12 > x && cur_y < y + h && cur_y + 19 > y) cursor_draw();
}
static void cursor_move(int nx, int ny) {
    blit(cur_x, cur_y, 12, 19);
    if (ol_on) outline_draw();
    cur_x = nx; cur_y = ny;
    cursor_draw();
}

/* ====================================================================
 *  System info
 * ==================================================================== */
static u32 mem_mb = 0; static char cpu_vendor[13] = "Unknown"; static char loader[48] = "Multiboot2";
extern char _kernel_end[];
static void cpu_info(void) {
    u32 a = 0, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    u32 v[3] = {b, d, c};
    memcpy(cpu_vendor, v, 12); cpu_vendor[12] = 0;
}

/* ====================================================================
 *  Desktop wallpapers (computed pixel by pixel)
 * ==================================================================== */
struct Theme { const char *name; u32 c1, c2; bool classic; };
static const struct Theme THEMES[5] = {
    {"Ocean", 0x2A8FB0, 0x0D2547, false},
    {"Sunset", 0xF2A365, 0x5A2A6E, false},
    {"Graphite", 0x8B929C, 0x2E3238, false},
    {"Meadow", 0x9CCB72, 0x1F5C4A, false},
    {"Classic", 0x9AA3B8, 0x8C95AA, true},
};
static int theme = 0;
static const u8 BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
static void make_wallpaper(void) {
    const struct Theme *t = &THEMES[theme];
    for (int y = 0; y < H; y++) {
        u32 base = mix(t->c1, t->c2, y, H);
        for (int x = 0; x < W; x++) {
            u32 c;
            if (t->classic) c = ((x ^ y) & 1) ? t->c1 : t->c2;
            else {
                int u = (x * 2 + y + 4000) / 300, v = (x * 2 - y + 4000) / 300, k = (y + 50) / 260;
                int f = ((u * 7 + v * 13 + k * 5) % 5) - 2;          /* facet shade */
                int d = (BAYER[(x & 3) + (y & 3) * 4] - 8) / 4;       /* dither */
                int r = (int)((base >> 16) & 255) + f * 5 + d;
                int g = (int)((base >> 8) & 255) + f * 5 + d;
                int b = (int)(base & 255) + f * 5 + d;
                r = imax(0, imin(255, r)); g = imax(0, imin(255, g)); b = imax(0, imin(255, b));
                c = (u32)(r << 16 | g << 8 | b);
            }
            bgbuf[y * MAXW + x] = c;
        }
    }
}

/* ====================================================================
 *  Windows, menus, apps: state
 * ==================================================================== */
#define MB 22               /* menu bar height  */
#define TB 22               /* title bar height */
#define ACCENT 0x4A5BD6
enum { W_ABOUT, W_DISK, W_NOTES, W_PAINT, W_PUZZLE, W_README, W_TRASH, NWIN };
struct Win { const char *title; int x, y, w, h; bool open, shaded; };
static struct Win win[NWIN];
static int zord[NWIN], nz;

struct DIcon { const char *label; const Image *img; int win; int x, y; };
static struct DIcon dicons[] = {
    {"Facet HD", &IC_DISK, W_DISK, 0, 0}, {"Notes", &IC_NOTES, W_NOTES, 0, 0},
    {"Paint", &IC_PAINT, W_PAINT, 0, 0}, {"Puzzle", &IC_PUZZLE, W_PUZZLE, 0, 0},
    {"Read Me", &IC_README, W_README, 0, 0}, {"Trash", &IC_TRASH, W_TRASH, 0, 0},
};
#define NDICON 6
static int dsel = -1;
/* items inside the Facet HD window */
static struct DIcon hd_items[] = {
    {"Notes", &IC_NOTES, W_NOTES, 0, 0}, {"Paint", &IC_PAINT, W_PAINT, 0, 0},
    {"Puzzle", &IC_PUZZLE, W_PUZZLE, 0, 0}, {"Read Me", &IC_README, W_README, 0, 0},
    {"System Info", &IC_COMPUTER, W_ABOUT, 0, 0},
};
#define NHD 5
static int hd_sel = -1;

enum { A_SEP, A_ABOUT, A_NOTES, A_PAINT, A_PUZZLE, A_README, A_NEWNOTE, A_DISK, A_CLOSE,
       A_CLRNOTES, A_CLRCANVAS, A_SHUFFLE, A_T0, A_T1, A_T2, A_T3, A_T4, A_TRASH, A_RESTART, A_SHUTDOWN };
struct MItem { const char *label; int act; };
struct Menu { const char *title; int n; struct MItem it[7]; int x, w; };
static struct Menu menus[] = {
    {0, 6, {{"About FacetOS", A_ABOUT}, {"", A_SEP}, {"Notes", A_NOTES}, {"Paint", A_PAINT},
            {"Puzzle", A_PUZZLE}, {"Read Me", A_README}}, 0, 0},
    {"File", 4, {{"New Note", A_NEWNOTE}, {"Open Facet HD", A_DISK}, {"", A_SEP}, {"Close Window", A_CLOSE}}, 0, 0},
    {"Edit", 3, {{"Clear Notes", A_CLRNOTES}, {"Clear Canvas", A_CLRCANVAS}, {"Shuffle Puzzle", A_SHUFFLE}}, 0, 0},
    {"View", 5, {{"Ocean", A_T0}, {"Sunset", A_T1}, {"Graphite", A_T2}, {"Meadow", A_T3}, {"Classic", A_T4}}, 0, 0},
    {"Special", 4, {{"Empty Trash", A_TRASH}, {"", A_SEP}, {"Restart", A_RESTART}, {"Shut Down", A_SHUTDOWN}}, 0, 0},
};
#define NMENU 5
static int menu_open = -1, menu_hover = -1; static bool menu_sticky;

static int mx, my, mbtn;
static int drag_win = -1, drag_dx, drag_dy;
static u32 last_click_ms; static int last_click_x, last_click_y;
static int clock_h = -1, clock_m = -1;
static bool caret_on = true; static u32 caret_ms;

/* Notes */
static char notes[2048]; static int nlen;
/* Paint */
#define CW 418
#define CH 262
static u8 canvas[CW * CH];
static const u32 PCOL[12] = {0xFFFFFF, 0x1E2230, 0xE63946, 0xFF8C3C, 0xFCC419, 0x2EC464,
                             0x28B4C8, 0x3884FF, 0x8B5CF6, 0xF06EA0, 0x8B5A2B, 0x9AA0AA};
static int pcolor = 1, psize = 1; static bool painting; static int plx, ply;
/* Puzzle */
static u8 tiles[16]; static u32 rng = 12345; static int moves;
/* damage tracking */
static bool dmg; static int dx0, dy0, dx1, dy1;
static void damage(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!dmg) { dmg = true; dx0 = x; dy0 = y; dx1 = x + w; dy1 = y + h; return; }
    dx0 = imin(dx0, x); dy0 = imin(dy0, y); dx1 = imax(dx1, x + w); dy1 = imax(dy1, y + h);
}
static int win_h(int id) { return win[id].shaded ? TB : win[id].h; }
static void damage_win(int id) { damage(win[id].x - 2, win[id].y - 2, win[id].w + 10, win_h(id) + 10); }

/* ====================================================================
 *  Window management
 * ==================================================================== */
static void win_front(int id) {
    int k = -1;
    for (int i = 0; i < nz; i++) if (zord[i] == id) k = i;
    if (k < 0 || k == nz - 1) return;
    int old = zord[nz - 1];
    for (int i = k; i < nz - 1; i++) zord[i] = zord[i + 1];
    zord[nz - 1] = id;
    damage_win(id); damage_win(old);
}
static void win_open(int id) {
    if (!win[id].open) {
        win[id].open = true; win[id].shaded = false; zord[nz++] = id;
        if (nz > 1) damage_win(zord[nz - 2]);
        damage_win(id);
    }
    win_front(id);
}
static void win_close(int id) {
    if (!win[id].open) return;
    damage_win(id);
    win[id].open = false;
    int k = 0;
    for (int i = 0; i < nz; i++) if (zord[i] != id) zord[k++] = zord[i];
    nz = k;
    if (nz) damage_win(zord[nz - 1]);
}
static int top_win(void) { return nz ? zord[nz - 1] : -1; }
static bool inside(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }
static int win_at(int x, int y) {
    for (int i = nz - 1; i >= 0; i--) {
        struct Win *w = &win[zord[i]];
        if (inside(x, y, w->x, w->y, w->w, win_h(zord[i]))) return zord[i];
    }
    return -1;
}
static void content_rect(int id, int *x, int *y, int *w, int *h) {
    *x = win[id].x + 1; *y = win[id].y + TB; *w = win[id].w - 2; *h = win[id].h - TB - 1;
}

/* ====================================================================
 *  Drawing: icons, windows, apps
 * ==================================================================== */
static void draw_icon(const struct DIcon *ic, int x, int y, bool sel, bool on_desktop) {
    image(ic->img, x + 24, y, 255, sel);
    int tw = text_w(&F_REG, ic->label), lx = x + 40 - tw / 2;
    if (sel) { rrect(lx - 4, y + 35, tw + 8, 17, 4, ACCENT, 255); text(&F_REG, lx, y + 37, ic->label, 0xFFFFFF); }
    else if (on_desktop) { text(&F_REG, lx + 1, y + 38, ic->label, 0x000000); text(&F_REG, lx, y + 37, ic->label, 0xFFFFFF); }
    else text(&F_REG, lx, y + 37, ic->label, 0x1E2230);
}

static const char *const README[] = {
    "Welcome to FacetOS 1.0!",
    "",
    "FacetOS is a tiny operating system that draws every",
    "single pixel on this screen by itself - no Linux,",
    "no libraries. Just one C file and some pixel art.",
    "",
    "Tips:",
    "  - Double-click icons to open them.",
    "  - Drag windows by their striped title bar.",
    "  - Double-click a title bar to roll the window up.",
    "  - Try View for new desktop pictures.",
    "  - Paint lets you draw with the mouse.",
    0,
};

static void draw_puzzle(int x, int y) {
    for (int i = 0; i < 16; i++) {
        int tx = x + 10 + (i % 4) * 44, ty = y + 10 + (i / 4) * 44;
        if (!tiles[i]) { rrect(tx, ty, 40, 40, 6, 0xC9CDD6, 255); continue; }
        rrect(tx, ty + 2, 40, 40, 6, 0x101830, 60);
        rrect(tx, ty, 40, 40, 6, 0x2E3A8C, 255);
        rrect(tx + 1, ty + 1, 38, 38, 5, ACCENT, 255);
        rrect(tx + 3, ty + 2, 34, 14, 4, 0xFFFFFF, 40);
        char b[4]; utoa(tiles[i], b);
        text_c(&F_BIG, tx + 20, ty + 7, b, 0xFFFFFF);
    }
    bool solved = true;
    for (int i = 0; i < 15; i++) if (tiles[i] != i + 1) solved = false;
    char s[40]; char *p = scat(s, solved ? "Solved! Moves: " : "Moves: "); utoa((u32)moves, p);
    text_c(&F_BOLD, x + 98, y + 190, s, solved ? 0x1E8C4A : 0x3A3F4A);
}

static void draw_content(int id, int x, int y, int w, int h) {
    switch (id) {
    case W_ABOUT: {
        gradient(x, y, w, h, 0xFFFFFF, 0xECEEF2);
        image(&GEM_BIG, x + 18, y + 14, 255, false);
        image(&WORDMARK, x + 110, y + 22, 255, false);
        text(&F_REG, x + 112, y + 70, "Version 1.0  \"Pixel\"", 0x5A6070);
        char s[80], *p;
        p = scat(s, "Built-in memory:  "); p = utoa(mem_mb, p); scat(p, " MB");
        text(&F_REG, x + 20, y + 104, s, 0x1E2230);
        p = scat(s, "Processor:  "); p = scat(p, cpu_vendor); scat(p, " (32-bit)");
        text(&F_REG, x + 20, y + 122, s, 0x1E2230);
        p = scat(s, "Display:  "); p = utoa((u32)W, p); p = scat(p, " x "); p = utoa((u32)H, p); scat(p, ", millions of colors");
        text(&F_REG, x + 20, y + 140, s, 0x1E2230);
        p = scat(s, "Started by:  "); scat(p, loader);
        text(&F_REG, x + 20, y + 158, s, 0x1E2230);
        u32 used = ((u32)_kernel_end - 0x200000u) / (1024 * 1024) + 1;
        text(&F_BOLD, x + 20, y + 184, "FacetOS", 0x1E2230);
        p = utoa(used, s); scat(p, " MB used");
        text(&F_REG, x + w - 20 - text_w(&F_REG, s), y + 184, s, 0x5A6070);
        int bw = w - 40, fw = mem_mb ? (int)(bw * used / mem_mb) : 10;
        fw = imax(fw, 6);
        rrect(x + 20, y + 204, bw, 12, 4, 0xC9CDD6, 255);
        rrect(x + 20, y + 204, fw, 12, 4, ACCENT, 255);
        break; }
    case W_DISK: {
        gradient(x, y, w, 20, 0xF4F5F7, 0xDCDFE4);
        hline(x, y + 20, w, 0x9AA0AA);
        char s[60], *p = scat(s, "5 items, "); p = utoa(mem_mb > 4 ? mem_mb - 4 : 0, p); scat(p, " MB available");
        text_c(&F_REG, x + w / 2, y + 3, s, 0x3A3F4A);
        fill(x, y + 21, w, h - 21, 0xFFFFFF);
        for (int i = 0; i < NHD; i++) {
            hd_items[i].x = x + 10 + (i % 4) * 84; hd_items[i].y = y + 32 + (i / 4) * 68;
            draw_icon(&hd_items[i], hd_items[i].x, hd_items[i].y, hd_sel == i, false);
        }
        break; }
    case W_NOTES: {
        fill(x, y, w, h, 0xFFF6B8);
        for (int ly = y + 22; ly < y + h; ly += 18) hline(x + 6, ly, w - 12, 0xF0E08A);
        int px = x + 8, py = y + 6, right = x + w - 8;
        for (int i = 0; i <= nlen; i++) {
            if (i == nlen) { if (caret_on && top_win() == W_NOTES) fill(px, py + 1, 2, 15, 0x1E2230); break; }
            char ch = notes[i];
            if (ch == '\n') { px = x + 8; py += 18; continue; }
            char str[2] = {ch, 0};
            int cw = text_w(&F_REG, str);
            if (px + cw > right) { px = x + 8; py += 18; }
            text(&F_REG, px, py, str, 0x2A2A20);
            px += cw;
        }
        break; }
    case W_PAINT: {
        gradient(x, y, w, 30, 0xF4F5F7, 0xD8DBE0);
        hline(x, y + 30, w, 0x8A909A);
        for (int i = 0; i < 12; i++) {
            int sx = x + 8 + i * 20;
            frame(sx - 1, y + 6, 18, 18, i == pcolor ? 0x101010 : 0x8A909A);
            if (i == pcolor) frame(sx - 2, y + 5, 20, 20, 0x101010);
            fill(sx, y + 7, 16, 16, PCOL[i]);
        }
        for (int i = 0; i < 3; i++) {
            int bx = x + 258 + i * 26, ps = 3 + i * 4;
            bevel(bx, y + 5, 22, 20, psize == i * 3 + 1);
            rrect(bx + 11 - ps / 2, y + 15 - ps / 2, ps, ps, ps / 2, 0x1E2230, 255);
        }
        bevel(x + w - 62, y + 5, 54, 20, false);
        text_c(&F_REG, x + w - 35, y + 7, "Clear", 0x1E2230);
        /* canvas: only loop over the clipped part */
        int ox = x, oy = y + 31;
        int i0 = imax(cx0 - ox, 0), i1 = imin(cx1 - ox, CW), j0 = imax(cy0 - oy, 0), j1 = imin(cy1 - oy, CH);
        for (int j = j0; j < j1; j++) {
            u32 *row = &bb[(oy + j) * MAXW + ox];
            const u8 *src = &canvas[j * CW];
            for (int i = i0; i < i1; i++) row[i] = PCOL[src[i]];
        }
        break; }
    case W_PUZZLE:
        gradient(x, y, w, h, 0xE9EBEF, 0xD3D6DC);
        draw_puzzle(x, y);
        break;
    case W_README:
        fill(x, y, w, h, 0xFFFFFF);
        for (int i = 0; README[i]; i++)
            text(i == 0 ? &F_BOLD : &F_REG, x + 14, y + 10 + i * 18, README[i], 0x1E2230);
        break;
    case W_TRASH:
        fill(x, y, w, h, 0xFFFFFF);
        image(&IC_TRASH, x + 20, y + 24, 255, false);
        text(&F_BOLD, x + 70, y + 26, "The Trash is empty.", 0x1E2230);
        text(&F_REG, x + 70, y + 44, "Nothing to throw away yet.", 0x5A6070);
        break;
    }
}

static void draw_window(int id, bool active) {
    struct Win *wd = &win[id];
    int x = wd->x, y = wd->y, w = wd->w, h = win_h(id);
    /* shadow */
    fill_a(x + w, y + 4, 4, h, 0, 40); fill_a(x + 4, y + h, w - 4, 4, 0, 40);
    fill_a(x + w, y + 6, 2, h - 2, 0, 30); fill_a(x + 6, y + h, w - 6, 2, 0, 30);
    /* title bar */
    if (active) {
        gradient(x, y, w, TB, 0xF7F7F9, 0xD5D8DE);
        for (int k = 0; k < 6; k++) {
            hline(x + 6, y + 5 + k * 2, w - 12, 0xFFFFFF);
            hline(x + 6, y + 6 + k * 2, w - 12, 0xA9AEB8);
        }
        bevel(x + 8, y + 5, 13, 13, false);                       /* close box */
        bevel(x + w - 21, y + 5, 13, 13, false);                  /* roll-up box */
        hline(x + w - 18, y + 10, 7, 0x5A5E66); hline(x + w - 18, y + 13, 7, 0x5A5E66);
    } else gradient(x, y, w, TB, 0xEEEFF2, 0xE2E4E8);
    int tw = text_w(&F_BOLD, wd->title);
    if (active) fill(x + w / 2 - tw / 2 - 8, y + 3, tw + 16, 16, 0xE6E8EC);
    text(&F_BOLD, x + w / 2 - tw / 2, y + 4, wd->title, active ? 0x1E2230 : 0x9AA0AA);
    frame(x, y, w, h, active ? 0x3A3E46 : 0x8A909A);
    if (wd->shaded) return;
    hline(x + 1, y + TB - 1, w - 2, 0x8A909A);
    int ccx, ccy, ccw, cch; content_rect(id, &ccx, &ccy, &ccw, &cch);
    int sx0 = cx0, sy0 = cy0, sx1 = cx1, sy1 = cy1;
    cx0 = imax(cx0, ccx); cy0 = imax(cy0, ccy); cx1 = imin(cx1, ccx + ccw); cy1 = imin(cy1, ccy + cch);
    if (cx0 < cx1 && cy0 < cy1) draw_content(id, ccx, ccy, ccw, cch);
    cx0 = sx0; cy0 = sy0; cx1 = sx1; cy1 = sy1;
}

/* ---- menu bar ---- */
static void menu_rect(int m, int *x, int *y, int *w, int *h) {
    int mw = 0, mh = 4;
    for (int i = 0; i < menus[m].n; i++) {
        int t = text_w(&F_REG, menus[m].it[i].label); if (t > mw) mw = t;
        mh += menus[m].it[i].act == A_SEP ? 9 : 20;
    }
    *x = menus[m].x - 10; *y = MB - 1; *w = mw + 44; *h = mh;
}
static int menu_item_at(int m, int px, int py) {
    int x, y, w, h; menu_rect(m, &x, &y, &w, &h);
    if (!inside(px, py, x, y, w, h)) return -1;
    int yy = y + 2;
    for (int i = 0; i < menus[m].n; i++) {
        int ih = menus[m].it[i].act == A_SEP ? 9 : 20;
        if (py >= yy && py < yy + ih) return menus[m].it[i].act == A_SEP ? -1 : i;
        yy += ih;
    }
    return -1;
}
static int menu_title_at(int px, int py) {
    if (py >= MB) return -1;
    for (int i = 0; i < NMENU; i++) if (px >= menus[i].x - 10 && px < menus[i].x + menus[i].w + 10) return i;
    return -1;
}
static void layout_menus(void) {
    int x = 16;
    for (int i = 0; i < NMENU; i++) {
        menus[i].x = x;
        menus[i].w = menus[i].title ? text_w(&F_BOLD, menus[i].title) : 18;
        x += menus[i].w + 22;
    }
}
static void draw_menubar(void) {
    gradient(0, 0, W, MB - 1, 0xFBFBFC, 0xDCDFE4);
    hline(0, MB - 1, W, 0x6E747E);
    for (int i = 0; i < NMENU; i++) {
        bool on = (i == menu_open);
        if (on) fill(menus[i].x - 10, 0, menus[i].w + 20, MB - 1, ACCENT);
        if (!menus[i].title) image(&GEM_SMALL, menus[i].x, 2, 255, false);
        else text(&F_BOLD, menus[i].x, 3, menus[i].title, on ? 0xFFFFFF : 0x1E2230);
    }
    if (clock_h >= 0) {
        char t[6] = {(char)('0' + clock_h / 10), (char)('0' + clock_h % 10), ':',
                     (char)('0' + clock_m / 10), (char)('0' + clock_m % 10), 0};
        text(&F_BOLD, W - 16 - text_w(&F_BOLD, t), 3, t, 0x1E2230);
    }
    /* rounded screen corners */
    for (int j = 0; j < 6; j++) for (int i = 0; i < 6; i++) {
        u32 a = 255 - corner_cov(i, j, 6, 6, 6);
        pblend(i, j, 0, a); pblend(W - 1 - i, j, 0, a);
        pblend(i, H - 1 - j, 0, a); pblend(W - 1 - i, H - 1 - j, 0, a);
    }
    if (menu_open < 0) return;
    int x, y, w, h; menu_rect(menu_open, &x, &y, &w, &h);
    fill_a(x + 4, y + h, w, 4, 0, 50); fill_a(x + w, y + 4, 4, h - 4, 0, 50);
    fill(x, y, w, h, 0xF4F5F7); frame(x, y, w, h, 0x5A5E66);
    int yy = y + 2;
    for (int i = 0; i < menus[menu_open].n; i++) {
        struct MItem *it = &menus[menu_open].it[i];
        if (it->act == A_SEP) { hline(x + 1, yy + 4, w - 2, 0xC4C8CF); yy += 9; continue; }
        bool hi = (i == menu_hover);
        if (hi) fill(x + 1, yy, w - 2, 20, ACCENT);
        u32 col = hi ? 0xFFFFFF : 0x1E2230;
        text(&F_REG, x + 24, yy + 3, it->label, col);
        if (it->act >= A_T0 && it->act <= A_T4 && it->act - A_T0 == theme) {   /* check mark */
            for (int k = 0; k < 4; k++) { fill(x + 8 + k, yy + 9 + k, 2, 2, col); }
            for (int k = 0; k < 7; k++) { fill(x + 12 + k, yy + 12 - k, 2, 2, col); }
        }
        yy += 20;
    }
}

/* ---- compose a region of the screen ---- */
static void layout_icons(void) {
    for (int i = 0; i < NDICON; i++) {
        dicons[i].x = W - 96; dicons[i].y = MB + 16 + i * 70;
        if (i == NDICON - 1) dicons[i].y = H - 70;
    }
}
static void compose(int x, int y, int w, int h) {
    clip_set(x, y, w, h);
    if (cx0 >= cx1 || cy0 >= cy1) return;
    for (int j = cy0; j < cy1; j++) {
        u32 *d = &bb[j * MAXW], *s = &bgbuf[j * MAXW];
        for (int i = cx0; i < cx1; i++) d[i] = s[i];
    }
    for (int i = 0; i < NDICON; i++)
        if (inside(dicons[i].x + 40, dicons[i].y + 26, x - 60, y - 40, w + 120, h + 80))
            draw_icon(&dicons[i], dicons[i].x, dicons[i].y, dsel == i, true);
    for (int i = 0; i < nz; i++) {
        struct Win *wd = &win[zord[i]];
        if (wd->x < cx1 && wd->x + wd->w + 6 > cx0 && wd->y < cy1 && wd->y + win_h(zord[i]) + 6 > cy0)
            draw_window(zord[i], i == nz - 1);
    }
    draw_menubar();
    int ox = cx0, oy = cy0, ow = cx1 - cx0, oh = cy1 - cy0;
    clip_all();
    present(ox, oy, ow, oh);
}
static void flush(void) {
    if (!dmg) return;
    dmg = false;
    compose(dx0, dy0, dx1 - dx0, dy1 - dy0);
}
static void damage_menu(void) {
    damage(0, 0, W, MB);
    if (menu_open >= 0) { int x, y, w, h; menu_rect(menu_open, &x, &y, &w, &h); damage(x, y, w + 6, h + 6); }
}

/* ====================================================================
 *  Puzzle logic
 * ==================================================================== */
static u32 rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 16; }
static void puzzle_shuffle(void) {
    for (int i = 0; i < 15; i++) tiles[i] = (u8)(i + 1);
    tiles[15] = 0;
    int e = 15;
    for (int k = 0; k < 300; k++) {
        int r = e / 4, c = e % 4, d = (int)(rnd() % 4), n = -1;
        if (d == 0 && r > 0) n = e - 4;
        if (d == 1 && r < 3) n = e + 4;
        if (d == 2 && c > 0) n = e - 1;
        if (d == 3 && c < 3) n = e + 1;
        if (n >= 0) { tiles[e] = tiles[n]; tiles[n] = 0; e = n; }
    }
    moves = 0;
}

/* ====================================================================
 *  Actions
 * ==================================================================== */
static void flush(void);
static void shutdown(void) {
    flush();
    outw(0x604, 0x2000); outw(0xB004, 0x2000); outw(0x4004, 0x3400);   /* QEMU, Bochs, VirtualBox */
    clip_all();
    fill_a(0, 0, W, H, 0x000000, 150);
    int pw = 420, ph = 120, px = W / 2 - pw / 2, py = H / 2 - ph / 2;
    soft_shadow(px, py, pw, ph, 10);
    rrect(px, py, pw, ph, 10, 0x3A3E46, 255);
    rrect(px + 1, py + 1, pw - 2, ph - 2, 9, 0xF4F5F7, 255);
    image(&GEM_BIG, px + 16, py + 24, 255, false);
    text(&F_BOLD, px + 104, py + 40, "It is now safe to switch off", 0x1E2230);
    text(&F_BOLD, px + 104, py + 58, "your computer.", 0x1E2230);
    blit(0, 0, W, H);
    halt();
}
static void restart(void) {
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) ;
    outb(0x64, 0xFE);
    struct { u16 l; u32 b; } __attribute__((packed)) idt = {0, 0};
    __asm__ volatile("lidt %0; int $3" :: "m"(idt));   /* triple fault = reset */
    halt();
}
static void run_action(int a) {
    switch (a) {
    case A_ABOUT: win_open(W_ABOUT); break;
    case A_NOTES: win_open(W_NOTES); break;
    case A_PAINT: win_open(W_PAINT); break;
    case A_PUZZLE: win_open(W_PUZZLE); break;
    case A_README: win_open(W_README); break;
    case A_NEWNOTE: nlen = 0; win_open(W_NOTES); damage_win(W_NOTES); break;
    case A_DISK: win_open(W_DISK); break;
    case A_CLOSE: if (top_win() >= 0) win_close(top_win()); break;
    case A_CLRNOTES: nlen = 0; damage_win(W_NOTES); break;
    case A_CLRCANVAS: memset(canvas, 0, sizeof canvas); damage_win(W_PAINT); break;
    case A_SHUFFLE: puzzle_shuffle(); win_open(W_PUZZLE); damage_win(W_PUZZLE); break;
    case A_T0: case A_T1: case A_T2: case A_T3: case A_T4:
        theme = a - A_T0; make_wallpaper(); damage(0, 0, W, H); break;
    case A_TRASH: win_open(W_TRASH); tone(880); sleep_ms(60); tone(0); break;
    case A_RESTART: restart(); break;
    case A_SHUTDOWN: shutdown(); break;
    }
}
static void close_menu(void) { damage_menu(); menu_open = -1; menu_hover = -1; }

static void paint_dot(int x, int y) {
    int r = psize;
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) {
        if (i * i + j * j > r * r + r / 2) continue;
        int a = x + i, b = y + j;
        if (a >= 0 && a < CW && b >= 0 && b < CH) canvas[b * CW + a] = (u8)pcolor;
    }
}
static void paint_line(int x0, int y0, int x1, int y1) {
    int ddx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1, ddy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = ddx + ddy;
    for (;;) {
        paint_dot(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= ddy) { err += ddy; x0 += sx; }
        if (e2 <= ddx) { err += ddx; y0 += sy; }
    }
    int cx, cy, cw, ch; content_rect(W_PAINT, &cx, &cy, &cw, &ch);
    damage(cx + imin(x0, plx) - psize - 1, cy + 31 + imin(y0, ply) - psize - 1,
           iabs(x0 - plx) + 2 * psize + 3, iabs(y0 - ply) + 2 * psize + 3);
}

static void content_click(int id, int lx, int ly, bool dbl) {
    int cx, cy, cw, ch; content_rect(id, &cx, &cy, &cw, &ch);
    if (id == W_DISK) {
        for (int i = 0; i < NHD; i++)
            if (inside(mx, my, hd_items[i].x + 16, hd_items[i].y, 48, 54)) {
                if (dbl && hd_sel == i) { hd_sel = -1; damage_win(id); win_open(hd_items[i].win); }
                else { hd_sel = i; damage_win(id); }
                return;
            }
        if (hd_sel >= 0) { hd_sel = -1; damage_win(id); }
    } else if (id == W_PAINT) {
        if (ly < 31) {
            for (int i = 0; i < 12; i++) if (inside(lx, ly, 8 + i * 20, 6, 18, 18)) { pcolor = i; damage_win(id); }
            for (int i = 0; i < 3; i++) if (inside(lx, ly, 258 + i * 26, 5, 22, 20)) { psize = i * 3 + 1; damage_win(id); }
            if (inside(lx, ly, cw - 62, 5, 54, 20)) { memset(canvas, 0, sizeof canvas); damage_win(id); }
        } else {
            painting = true; plx = lx; ply = ly - 31;
            paint_dot(plx, ply); paint_line(plx, ply, plx, ply);
        }
    } else if (id == W_PUZZLE) {
        int c = (lx - 10) / 44, r = (ly - 10) / 44;
        if (lx < 10 || ly < 10 || c > 3 || r > 3) return;
        int t = r * 4 + c;
        int nb[4] = {t - 4, t + 4, (c > 0) ? t - 1 : -1, (c < 3) ? t + 1 : -1};
        for (int k = 0; k < 4; k++)
            if (nb[k] >= 0 && nb[k] < 16 && tiles[nb[k]] == 0) {
                tiles[nb[k]] = tiles[t]; tiles[t] = 0; moves++;
                tone(1200); sleep_ms(8); tone(0);
                damage_win(id); break;
            }
    }
}

static void mouse_down(void) {
    bool dbl = (ms_now - last_click_ms < 500) && iabs(mx - last_click_x) < 5 && iabs(my - last_click_y) < 5;
    last_click_ms = ms_now; last_click_x = mx; last_click_y = my;
    if (menu_open >= 0 && menu_sticky) {
        int it = menu_item_at(menu_open, mx, my), m = menu_open;
        close_menu();
        if (it >= 0) { run_action(menus[m].it[it].act); return; }
        int t2 = menu_title_at(mx, my);
        if (t2 < 0 || t2 == m) return;          /* click outside just closes the menu */
    }
    int t = menu_title_at(mx, my);
    if (t >= 0) { menu_open = t; menu_sticky = false; menu_hover = -1; damage_menu(); return; }
    if (my < MB) return;
    int id = win_at(mx, my);
    if (id >= 0) {
        win_front(id);
        struct Win *w = &win[id];
        if (my < w->y + TB) {
            if (inside(mx, my, w->x + 6, w->y + 3, 17, 17)) win_close(id);
            else if (inside(mx, my, w->x + w->w - 23, w->y + 3, 17, 17) || dbl) {
                damage_win(id); w->shaded = !w->shaded; damage_win(id);
                tone(600); sleep_ms(15); tone(0);
            } else {
                drag_win = id; drag_dx = mx - w->x; drag_dy = my - w->y;
                ol_x = w->x; ol_y = w->y; ol_w = w->w; ol_h = win_h(id); ol_on = true;
                flush(); outline_draw();
            }
        } else if (!w->shaded) {
            int cx, cy, cw, ch; content_rect(id, &cx, &cy, &cw, &ch);
            content_click(id, mx - cx, my - cy, dbl);
        }
        return;
    }
    for (int i = 0; i < NDICON; i++)
        if (inside(mx, my, dicons[i].x + 16, dicons[i].y, 48, 54)) {
            if (dbl && dsel == i) { win_open(dicons[i].win); }
            else { if (dsel >= 0) damage(dicons[dsel].x, dicons[dsel].y, 80, 56); dsel = i; damage(dicons[i].x, dicons[i].y, 80, 56); }
            return;
        }
    if (dsel >= 0) { damage(dicons[dsel].x, dicons[dsel].y, 80, 56); dsel = -1; }
}
static void mouse_move(void) {
    if (menu_open >= 0) {
        int h = menu_item_at(menu_open, mx, my);
        if (h != menu_hover) { menu_hover = h; damage_menu(); }
        int t = menu_title_at(mx, my);
        if (t >= 0 && t != menu_open) { close_menu(); menu_open = t; menu_hover = -1; damage_menu(); }
        return;
    }
    if (drag_win >= 0 && mbtn) {
        struct Win *w = &win[drag_win];
        int nx = mx - drag_dx, ny = imax(MB, imin(H - TB, my - drag_dy));
        nx = imax(40 - w->w, imin(W - 40, nx));
        if (nx != ol_x || ny != ol_y) { outline_erase(); ol_x = nx; ol_y = ny; outline_draw(); }
    }
    if (painting && mbtn) {
        int cx, cy, cw, ch; content_rect(W_PAINT, &cx, &cy, &cw, &ch);
        int nx = mx - cx, ny = my - cy - 31;
        paint_line(plx, ply, nx, ny);
        plx = nx; ply = ny;
    }
}
static void mouse_up(void) {
    if (menu_open >= 0) {
        int it = menu_item_at(menu_open, mx, my), m = menu_open;
        if (it >= 0) { close_menu(); run_action(menus[m].it[it].act); }
        else if (menu_title_at(mx, my) == m && !menu_sticky) menu_sticky = true;
        else close_menu();
    }
    if (drag_win >= 0) {
        outline_erase(); ol_on = false;
        struct Win *w = &win[drag_win];
        if (w->x != ol_x || w->y != ol_y) { damage_win(drag_win); w->x = ol_x; w->y = ol_y; damage_win(drag_win); }
        drag_win = -1;
    }
    painting = false;
}

/* ====================================================================
 *  Input devices
 * ==================================================================== */
static void ps2_wait_w(void) { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return; }
static void ps2_wait_r(void) { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return; }
static void mouse_cmd(u8 v) { ps2_wait_w(); outb(0x64, 0xD4); ps2_wait_w(); outb(0x60, v); ps2_wait_r(); (void)inb(0x60); }
static void ps2_flush(void) { for (int i = 0; i < 64 && (inb(0x64) & 1); i++) (void)inb(0x60); }
static void mouse_init(void) {
    ps2_flush();
    ps2_wait_w(); outb(0x64, 0xA8);
    ps2_wait_w(); outb(0x64, 0x20); ps2_wait_r();
    u8 cfg = inb(0x60);
    cfg &= (u8)~0x20;
    ps2_wait_w(); outb(0x64, 0x60); ps2_wait_w(); outb(0x60, cfg);
    mouse_cmd(0xF6); mouse_cmd(0xF4);
}
static const char KMAP[58] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '};
static const char KMAP_S[58] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '};
static bool shift, caps;
static void on_key(u8 sc) {
    if (sc == 0x2A || sc == 0x36) { shift = true; return; }
    if (sc == 0xAA || sc == 0xB6) { shift = false; return; }
    if (sc == 0x3A) { caps = !caps; return; }
    if (sc & 0x80 || sc >= 58) return;
    char c = shift ? KMAP_S[sc] : KMAP[sc];
    if (caps && c >= 'a' && c <= 'z') c -= 32; else if (caps && c >= 'A' && c <= 'Z') c += 32;
    if (!c || c == 27 || c == '\t') return;
    if (top_win() != W_NOTES) return;
    if (c == '\b') { if (nlen) nlen--; }
    else if (nlen < (int)sizeof notes - 1) notes[nlen++] = c;
    caret_on = true; caret_ms = ms_now;
    damage_win(W_NOTES);
}
static u8 cmos(u8 r) { outb(0x70, r); return inb(0x71); }
static void read_clock(void) {
    for (int i = 0; i < 10000 && (cmos(0x0A) & 0x80); i++) ;
    u8 m = cmos(0x02), h = cmos(0x04), b = cmos(0x0B);
    if (!(b & 4)) { m = (u8)((m & 15) + (m >> 4) * 10); h = (u8)(((h & 15) + ((h & 0x70) >> 4) * 10) | (h & 0x80)); }
    if (!(b & 2) && (h & 0x80)) h = (u8)(((h & 0x7F) + 12) % 24);
    clock_h = h % 24; clock_m = m % 60;
    rng ^= (u32)(cmos(0) * 7919 + m * 31 + h);
}

/* ====================================================================
 *  Boot animation
 * ==================================================================== */
static void boot_animation(void) {
    /* 1. dark screen, the gem fades in with a startup chime */
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) bgbuf[y * MAXW + x] = mix(0x2B3550, 0x0B0F1C, y, H);
    clip_all();
    memcpy(bb, bgbuf, sizeof(u32) * MAXW * (u32)H);
    blit(0, 0, W, H);
    int gx = W / 2 - GEM_BIG.w / 2, gy = H / 2 - GEM_BIG.h / 2 - 20;
    static const u32 chime[] = {523, 659, 784, 1047, 0};
    for (int s = 1; s <= 12; s++) {
        for (int j = 0; j < GEM_BIG.h; j++) for (int i = 0; i < GEM_BIG.w; i++)
            bb[(gy + j) * MAXW + gx + i] = bgbuf[(gy + j) * MAXW + gx + i];
        image(&GEM_BIG, gx, gy, (u32)(s * 255 / 12), false);
        blit(gx, gy, GEM_BIG.w, GEM_BIG.h);
        if (s <= 4) tone(chime[s - 1]);
        sleep_ms(s <= 4 ? 140 : 60);
        if (s == 4) { sleep_ms(250); tone(0); }
    }
    tone(0);
    sleep_ms(400);

    /* 2. start-up panel with progress bar, extensions march along the bottom */
    int pw = 440, ph = 230, px = W / 2 - pw / 2, py = H / 2 - ph / 2 - 30;
    memcpy(bb, bgbuf, sizeof(u32) * MAXW * (u32)H);
    soft_shadow(px, py, pw, ph, 14);
    rrect(px, py, pw, ph, 14, 0x1E2230, 255);
    rrect(px + 1, py + 1, pw - 2, ph - 2, 13, 0xF6F7F9, 255);
    rrect(px + 1, py + 1, pw - 2, 120, 13, 0xFFFFFF, 255);
    hline(px + 1, py + 120, pw - 2, 0xDDE0E6);
    image(&GEM_BIG, px + 40, py + 24, 255, false);
    image(&WORDMARK, px + 130, py + 38, 255, false);
    text(&F_REG, px + 132, py + 84, "Version 1.0", 0x6A7080);
    int by = py + 176, bx = px + 50, bw = pw - 100;
    blit(0, 0, W, H);

    const Image *ext[] = {&IC_MOUSE, &IC_KEYBOARD, &IC_COMPUTER, &IC_CLOCK, &IC_SOUND,
                          &IC_DISK, &IC_NOTES, &IC_PAINT, &IC_PUZZLE, &IC_TRASH};
    const char *msg[] = {"Starting mouse...", "Starting keyboard...", "Checking memory...", "Reading the clock...",
                         "Warming up the speaker...", "Mounting Facet HD...", "Loading Notes...",
                         "Loading Paint...", "Loading Puzzle...", "Almost ready..."};
    int n = 10;
    for (int i = 0; i <= n; i++) {
        if (i == 0) mouse_init();
        if (i == 1) ps2_flush();
        if (i == 2) cpu_info();
        if (i == 3) read_clock();
        fill(px + 2, by - 32, pw - 4, 20, 0xF6F7F9);
        text_c(&F_REG, W / 2, by - 30, i < n ? msg[i] : "Welcome to FacetOS", 0x3A3F4A);
        rrect(bx, by, bw, 14, 7, 0x8A909A, 255);
        rrect(bx + 1, by + 1, bw - 2, 12, 6, 0xE3E6EB, 255);
        int fw = (bw - 2) * i / n;
        if (fw > 12) { rrect(bx + 1, by + 1, fw, 12, 6, ACCENT, 255); rrect(bx + 3, by + 2, fw - 4, 4, 2, 0xFFFFFF, 70); }
        if (i < n) image(ext[i], 20 + i * 44, H - 52, 255, false);
        blit(px, py, pw, ph);
        if (i < n) blit(20 + i * 44, H - 52, 32, 32);
        sleep_ms(i < n ? 220 : 900);
    }
}

/* ====================================================================
 *  Main
 * ==================================================================== */
static void setup_windows(void) {
    win[W_ABOUT]  = (struct Win){"About FacetOS", W / 2 - 220, MB + 70, 440, 252, false, false};
    win[W_DISK]   = (struct Win){"Facet HD", 30, MB + 30, 360, 200, false, false};
    win[W_NOTES]  = (struct Win){"Notes", 420, MB + 60, 300, 240, false, false};
    win[W_PAINT]  = (struct Win){"Paint", 120, MB + 120, CW + 2, TB + 31 + CH + 1, false, false};
    win[W_PUZZLE] = (struct Win){"Puzzle", 560, MB + 260, 198, TB + 212, false, false};
    win[W_README] = (struct Win){"Read Me", 200, MB + 90, 420, 272, false, false};
    win[W_TRASH]  = (struct Win){"Trash", W / 2 - 150, H / 2 - 40, 300, 104, false, false};
    const char *hello = "Welcome to FacetOS!\nClick here and start typing...";
    for (nlen = 0; hello[nlen]; nlen++) notes[nlen] = hello[nlen];
    puzzle_shuffle();
}

void kmain(u32 magic, u32 info) {
    if (magic != 0x36d76289) halt();
    u8 *t = (u8 *)info + 8, *end = (u8 *)info + *(u32 *)info;
    while (t < end) {
        u32 type = *(u32 *)t, size = *(u32 *)(t + 4);
        if (type == 0) break;
        if (type == 8) {
            fb = (u32 *)*(u32 *)(t + 8);
            fb_stride = *(u32 *)(t + 16) / 4;
            W = (int)*(u32 *)(t + 20); H = (int)*(u32 *)(t + 24);
            if (t[28] != 32) halt();
        }
        if (type == 4) mem_mb = (*(u32 *)(t + 12) + 1024) / 1024;
        if (type == 2) { int i = 0; const char *s = (const char *)(t + 8); while (s[i] && i < 47) { loader[i] = s[i]; i++; } loader[i] = 0; }
        t += (size + 7) & ~7u;
    }
    if (!fb) halt();
    W = imin(W, MAXW); H = imin(H, MAXH);
    pit_init();
    layout_menus();
    boot_animation();
    rng ^= ms_now * 2654435761u;
    setup_windows();
    layout_icons();
    make_wallpaper();
    win_open(W_DISK);
    cur_x = mx = W / 2; cur_y = my = H / 2;
    dmg = false;
    compose(0, 0, W, H);
    cursor_draw();

    u8 pkt[3]; int pi = 0;
    u32 last_clock = 0;
    for (;;) {
        tick();
        int moved = 0;
        while (inb(0x64) & 1) {
            u8 st = inb(0x64), d = inb(0x60);
            if (st & 0x20) {
                if (pi == 0 && !(d & 0x08)) continue;
                pkt[pi++] = d;
                if (pi < 3) continue;
                pi = 0;
                if (pkt[0] & 0xC0) continue;
                int ddx = pkt[1] - ((pkt[0] << 4) & 0x100), ddy = pkt[2] - ((pkt[0] << 3) & 0x100);
                int nx = imax(0, imin(W - 1, mx + ddx)), ny = imax(0, imin(H - 1, my - ddy));
                int b = pkt[0] & 1;
                if (nx != mx || ny != my) { mx = nx; my = ny; moved = 1; mouse_move(); }
                if (b && !mbtn) { mbtn = 1; flush(); mouse_down(); }
                else if (!b && mbtn) { mbtn = 0; mouse_up(); }
            } else on_key(d);
        }
        if (moved) cursor_move(mx, my);
        if (top_win() == W_NOTES && ms_now - caret_ms > 530) {
            caret_on = !caret_on; caret_ms = ms_now;
            int cx, cy, cw, ch; content_rect(W_NOTES, &cx, &cy, &cw, &ch);
            damage(cx, cy, cw, ch);
        }
        if (ms_now - last_clock > 1000) {
            last_clock = ms_now;
            int oh = clock_h, om = clock_m;
            read_clock();
            if (oh != clock_h || om != clock_m) damage(W - 140, 0, 140, MB);
        }
        if (!(inb(0x64) & 1)) flush();
    }
}
