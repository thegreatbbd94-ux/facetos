/*
 * FacetOS 1.2 "Prism" - a tiny 32-bit desktop operating system that draws every pixel itself.
 * Classic desktop style, original art. Boots via Multiboot2 (Limine / GRUB) on any x86 PC,
 * including 32-bit-only emulators like v86.
 *
 * 1.2: a new look of its own (dark glass, cut corners, Prism Bar, prism accent), window
 * animations, a live "Vault" that keeps files in memory, and read-only hardware detection.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t i32;
typedef uint64_t u64; typedef int64_t i64;

typedef struct { u8 adv, w, h; signed char xo, yo; u32 off; } Glyph;
typedef struct { const Glyph *g; const char *a; int lh; } Font;
typedef struct { int w, h; const u32 *pal; const char *d; } Image;
#include "assets.h"

#define VERSION "1.2"

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
static inline void outl_(u16 p, u32 v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline u32 inl_(u16 p) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }
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
/* 64-bit division helpers (a 32-bit CPU can't divide 64-bit numbers in one instruction) */
static u64 udivmod64(u64 n, u64 d, u64 *rem) {
    u64 q = 0, r = 0;
    if (!d) { if (rem) *rem = 0; return 0; }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (u64)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}
__attribute__((used)) u64 __udivdi3(u64 a, u64 b) { return udivmod64(a, b, 0); }
__attribute__((used)) u64 __umoddi3(u64 a, u64 b) { u64 r; udivmod64(a, b, &r); return r; }
__attribute__((used)) i64 __divdi3(i64 a, i64 b) {
    u64 q = udivmod64(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, 0);
    return ((a < 0) != (b < 0)) ? -(i64)q : (i64)q;
}
__attribute__((used)) i64 __moddi3(i64 a, i64 b) {
    u64 r; udivmod64(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, &r);
    return a < 0 ? -(i64)r : (i64)r;
}
static inline void copy32(u32 *d, const u32 *s, u32 n) {      /* fast copy of n pixels */
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
}
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iabs(int a) { return a < 0 ? -a : a; }
static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static void wr16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

/* ====================================================================
 *  Interrupts: GDT, IDT, PIC, timer. Devices now tell us when something
 *  happens instead of us asking all the time.
 * ==================================================================== */
struct Frame { u32 es, ds, edi, esi, ebp, esp0, ebx, edx, ecx, eax, num, err, eip, cs, eflags; };
__asm__(
    ".section .text\n"
    ".irp n, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31,"
    "32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47\n"
    "isr\\n: push $0\n push $\\n\n jmp isr_common\n.endr\n"
    ".irp n, 8,10,11,12,13,14,17,21,29,30\n"
    "isr\\n: push $\\n\n jmp isr_common\n.endr\n"
    "isr_common:\n"
    "  pusha\n  push %ds\n  push %es\n"
    "  mov $0x10, %ax\n  mov %ax, %ds\n  mov %ax, %es\n"
    "  push %esp\n  call isr_handler\n  add $4, %esp\n"
    "  pop %es\n  pop %ds\n  popa\n  add $8, %esp\n  iret\n"
    ".section .rodata\n.align 4\n.global isr_table\nisr_table:\n"
    ".irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,"
    "32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47\n .long isr\\n\n.endr\n"
    ".section .text\n");
extern const u32 isr_table[48];

static const u64 gdt[3] = {0, 0x00CF9A000000FFFFull, 0x00CF92000000FFFFull};
struct __attribute__((packed)) IdtEntry { u16 lo, sel; u8 zero, flags; u16 hi; };
static struct IdtEntry idt[256];
struct __attribute__((packed)) DescPtr { u16 limit; u32 base; };

static volatile u32 ms_now;                 /* milliseconds since boot (IRQ 0) */
static volatile u16 evq[1024];              /* keyboard + mouse bytes (IRQ 1 / IRQ 12) */
static volatile u32 ev_head, ev_tail;
static void ev_push(u16 v) { u32 n = (ev_head + 1) & 1023; if (n != ev_tail) { evq[ev_head] = v; ev_head = n; } }

static void crash(struct Frame *f);
__attribute__((used)) void isr_handler(struct Frame *f) {
    if (f->num < 32) crash(f);
    int irq = (int)f->num - 32;
    if (irq == 0) ms_now++;
    else if (irq == 1 || irq == 12) {
        for (int i = 0; i < 16; i++) {
            u8 st = inb(0x64);
            if (!(st & 1)) break;
            u8 d = inb(0x60);
            ev_push((u16)((st & 0x20) ? 0x100 | d : d));    /* 0x100 = came from the mouse */
        }
    }
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

static void interrupts_init(void) {
    struct DescPtr g = {sizeof gdt - 1, (u32)gdt};
    __asm__ volatile("lgdt %0\n ljmp $0x08, $1f\n1:\n mov $0x10, %%ax\n mov %%ax, %%ds\n mov %%ax, %%es\n"
                     " mov %%ax, %%fs\n mov %%ax, %%gs\n mov %%ax, %%ss\n" :: "m"(g) : "eax", "memory");
    for (int i = 0; i < 48; i++) {
        idt[i].lo = (u16)isr_table[i]; idt[i].hi = (u16)(isr_table[i] >> 16);
        idt[i].sel = 0x08; idt[i].zero = 0; idt[i].flags = 0x8E;
    }
    struct DescPtr ip = {sizeof idt - 1, (u32)idt};
    __asm__ volatile("lidt %0" :: "m"(ip));
    /* PIC: move IRQs 0-15 to vectors 32-47 */
    outb(0x20, 0x11); outb(0xA0, 0x11); outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02); outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xF8);          /* allow IRQ 0 (timer), 1 (keyboard), 2 (cascade) */
    outb(0xA1, 0xEF);          /* allow IRQ 12 (mouse) */
    /* PIT channel 0: 1000 interrupts per second */
    outb(0x43, 0x34); outb(0x40, (u8)(1193 & 255)); outb(0x40, (u8)(1193 >> 8));
}
static void sleep_ms(u32 ms) { u32 end = ms_now + ms; while ((i32)(end - ms_now) > 0) __asm__ volatile("sti; hlt"); }
static void tone(u32 hz) {
    if (!hz) { outb(0x61, inb(0x61) & 0xFC); return; }
    u32 div = 1193182 / hz;
    outb(0x43, 0xB6); outb(0x42, (u8)div); outb(0x42, (u8)(div >> 8));
    outb(0x61, inb(0x61) | 3);
}
static void beep(void) { tone(440); sleep_ms(70); tone(0); }


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
/* FacetOS cursor: white arrow, dark outline, prism edge (P) */
#define CUR_W 13
#define CUR_H 21
static const char *const CURSOR[CUR_H] = {
    "D............", "DD...........", "DPD..........", "DWPD.........", "DWWPD........", "DWWWPD.......",
    "DWWWWPD......", "DWWWWWPD.....", "DWWWWWWPD....", "DWWWWWWWPD...", "DWWWWWWWWPD..", "DWWWWWWWWWPD.",
    "DWWWWWWDDDDDD", "DWWWDWWD.....", "DWWDDWWPD....", "DWD..DWWD....", "DD...DWWPD...", "D.....DWWD...",
    "......DWWPD..", ".......DWWD..", "........DD...",
};
static int cur_x, cur_y;
static bool ol_on; static int ol_x, ol_y, ol_w, ol_h;
static void blit(int x, int y, int w, int h) {     /* back buffer -> screen */
    int x0 = imax(x, 0), y0 = imax(y, 0), x1 = imin(x + w, W), y1 = imin(y + h, H);
    if (x1 <= x0) return;
    for (int j = y0; j < y1; j++) copy32(&fb[j * fb_stride + x0], &bb[j * MAXW + x0], (u32)(x1 - x0));
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
    for (int j = 0; j < CUR_H; j++) for (int i = 0; i < CUR_W; i++) {
        char c = CURSOR[j][i];
        int x = cur_x + i, y = cur_y + j;
        if (x >= W || y >= H) continue;
        if (c == 'D') fb[y * fb_stride + x] = 0x0B0E18;
        else if (c == 'W') fb[y * fb_stride + x] = 0xFFFFFF;
        else if (c == 'P') fb[y * fb_stride + x] = mix(0x2DD4BF, 0x8B5CF6, j, CUR_H);
        else if (x > cur_x + 1 && y > cur_y + 2 && CURSOR[j - 2][i - 2] != '.')   /* soft shadow */
            fb[y * fb_stride + x] = blend(bb[y * MAXW + x], 0, 80);
    }
}
static void present(int x, int y, int w, int h) {
    blit(x, y, w, h);
    if (ol_on) outline_draw();
    if (cur_x < x + w && cur_x + CUR_W > x && cur_y < y + h && cur_y + CUR_H > y) cursor_draw();
}
static void cursor_move(int nx, int ny) {
    blit(cur_x, cur_y, CUR_W, CUR_H);
    if (ol_on) outline_draw();
    cur_x = nx; cur_y = ny;
    cursor_draw();
}


/* ====================================================================
 *  System info + clock
 * ==================================================================== */
static u32 mem_mb = 0; static char cpu_vendor[13] = "Unknown"; static char loader[48] = "Multiboot2";
extern char _kernel_end[];
static void cpu_info(void) {
    u32 a = 0, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    u32 v[3] = {b, d, c};
    memcpy(cpu_vendor, v, 12); cpu_vendor[12] = 0;
}
static int clock_h = -1, clock_m = -1, clock_s, date_d = 1, date_mo = 1, date_y = 2026;
static u32 rng = 12345;
static u8 cmos(u8 r) { outb(0x70, r); return inb(0x71); }
static int bcd(u8 v, u8 b) { return (b & 4) ? v : (v & 15) + (v >> 4) * 10; }
static void read_clock(void) {
    for (int i = 0; i < 10000 && (cmos(0x0A) & 0x80); i++) ;
    u8 b = cmos(0x0B), h = cmos(0x04);
    clock_s = bcd(cmos(0x00), b); clock_m = bcd(cmos(0x02), b) % 60;
    int hh = bcd(h & 0x7F, b);
    if (!(b & 2) && (h & 0x80)) hh = (hh + 12) % 24;
    clock_h = hh % 24;
    date_d = bcd(cmos(0x07), b); date_mo = bcd(cmos(0x08), b); date_y = 2000 + bcd(cmos(0x09), b);
    rng ^= (u32)(clock_s * 7919 + clock_m * 31 + clock_h);
}

/* ====================================================================
 *  ATA hard disk driver (PIO mode, 28-bit LBA)
 * ==================================================================== */
static u16 ata_io, ata_ctl; static int ata_slave; static u32 ata_sectors; static bool ata_ok;
static void ata_delay(void) { for (int i = 0; i < 4; i++) (void)inb(ata_ctl); }
static bool ata_wait(bool want_drq) {
    for (u32 i = 0; i < 3000000; i++) {
        u8 s = inb(ata_io + 7);
        if (s & 0x80) continue;                 /* busy */
        if (s & 0x21) return false;             /* error / drive fault */
        if (!want_drq || (s & 0x08)) return true;
    }
    return false;
}
struct Drive { u16 io, ctl; int slave; bool atapi; u32 sectors; char model[41]; };
static struct Drive drives[4]; static int ndrives;
static void id_string(const u16 *id, int w0, int nw, char *out) {     /* ATA strings are byte-swapped */
    int k = 0;
    for (int i = 0; i < nw; i++) { out[k++] = (char)(id[w0 + i] >> 8); out[k++] = (char)(id[w0 + i] & 255); }
    out[k] = 0;
    while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == 0)) out[--k] = 0;
    int st = 0; while (out[st] == ' ') st++;
    if (st) { int j = 0; while (out[st + j]) { out[j] = out[st + j]; j++; } out[j] = 0; }
    for (int i = 0; out[i]; i++) if (out[i] < 32 || out[i] > 126) out[i] = '?';
}
static bool ata_probe(u16 io, u16 ctl, int slave, struct Drive *d) {
    if (inb(io + 7) == 0xFF) return false;      /* nothing on this bus */
    ata_io = io; ata_ctl = ctl;
    outb(ctl, 0x02);                            /* no disk interrupts, we poll */
    outb(io + 6, (u8)(0xA0 | slave << 4)); ata_delay();
    outb(io + 2, 0); outb(io + 3, 0); outb(io + 4, 0); outb(io + 5, 0);
    outb(io + 7, 0xEC);                         /* IDENTIFY */
    if (inb(io + 7) == 0) return false;
    for (u32 i = 0; i < 1000000 && (inb(io + 7) & 0x80); i++) ;
    u8 m1 = inb(io + 4), m2 = inb(io + 5);
    bool atapi = (m1 == 0x14 && m2 == 0xEB) || (m1 == 0x69 && m2 == 0x96);
    if (atapi) { outb(io + 7, 0xA1); for (u32 i = 0; i < 1000000 && (inb(io + 7) & 0x80); i++) ; }   /* IDENTIFY PACKET */
    else if (m1 || m2) return false;
    if (!ata_wait(true)) return false;
    u16 id[256]; void *p = id; u32 n = 256;
    __asm__ volatile("rep insw" : "+D"(p), "+c"(n) : "d"(io) : "memory");
    d->io = io; d->ctl = ctl; d->slave = slave; d->atapi = atapi;
    d->sectors = atapi ? 0 : (id[60] | (u32)id[61] << 16);
    id_string(id, 27, 20, d->model);
    if (!d->model[0]) scat(d->model, atapi ? "CD/DVD drive" : "Hard disk");
    return atapi || d->sectors > 0;
}
static void ata_use(int i) {
    ata_io = drives[i].io; ata_ctl = drives[i].ctl; ata_slave = drives[i].slave;
    ata_sectors = drives[i].sectors; ata_ok = !drives[i].atapi;
}
static void ata_select(u32 lba, u8 cmd) {
    outb(ata_io + 6, (u8)(0xE0 | ata_slave << 4 | ((lba >> 24) & 0x0F)));
    outb(ata_io + 2, 1);
    outb(ata_io + 3, (u8)lba); outb(ata_io + 4, (u8)(lba >> 8)); outb(ata_io + 5, (u8)(lba >> 16));
    outb(ata_io + 7, cmd);
    ata_delay();
}
static bool ata_read(u32 lba, void *buf) {
    if (!ata_ok || !ata_wait(false)) return false;
    ata_select(lba, 0x20);
    if (!ata_wait(true)) return false;
    u32 n = 256; void *p = buf;
    __asm__ volatile("rep insw" : "+D"(p), "+c"(n) : "d"(ata_io) : "memory");
    return true;
}
static bool ata_write(u32 lba, const void *buf) {
    if (!ata_ok || !ata_wait(false)) return false;
    ata_select(lba, 0x30);
    if (!ata_wait(true)) return false;
    u32 n = 256; const void *p = buf;
    __asm__ volatile("rep outsw" : "+S"(p), "+c"(n) : "d"(ata_io) : "memory");
    return ata_wait(false);
}
static void ata_flush(void) { if (ata_ok && ata_wait(false)) { outb(ata_io + 7, 0xE7); ata_wait(false); } }
static void ata_init(void) {
    static const u16 ios[2] = {0x1F0, 0x170}, ctls[2] = {0x3F6, 0x376};
    ndrives = 0; ata_ok = false;
    for (int b = 0; b < 2; b++)
        for (int s2 = 0; s2 < 2; s2++)
            if (ata_probe(ios[b], ctls[b], s2, &drives[ndrives])) ndrives++;
}

/* ====================================================================
 *  PCI scan: list storage and USB controllers so the System app can show
 *  what hardware is there (read-only, nothing is changed).
 * ==================================================================== */
struct PciDev { u16 vendor, device; u8 cls, sub, prog; };
static struct PciDev pci_store[12]; static int npci;
static u32 pci_read(u32 bus, u32 dev, u32 fn, u32 reg) {
    outl_(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fn << 8 | (reg & 0xFC));
    return inl_(0xCFC);
}
static void pci_scan(void) {
    npci = 0;
    for (u32 bus = 0; bus < 256; bus++)
        for (u32 dev = 0; dev < 32; dev++) {
            u32 v = pci_read(bus, dev, 0, 0);
            if ((v & 0xFFFF) == 0xFFFF) continue;
            u32 hdr = (pci_read(bus, dev, 0, 0x0C) >> 16) & 0xFF;
            for (u32 fn = 0; fn < ((hdr & 0x80) ? 8u : 1u); fn++) {
                u32 id = pci_read(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                u32 cl = pci_read(bus, dev, fn, 0x08);
                u8 cls = (u8)(cl >> 24), sub = (u8)(cl >> 16), prog = (u8)(cl >> 8);
                if ((cls == 0x01 || (cls == 0x0C && sub == 0x03)) && npci < 12)
                    pci_store[npci++] = (struct PciDev){(u16)id, (u16)(id >> 16), cls, sub, prog};
            }
        }
}
static const char *pci_vendor(u16 v) {
    switch (v) {
    case 0x8086: return "Intel"; case 0x1022: return "AMD"; case 0x10DE: return "NVIDIA"; case 0x144D: return "Samsung";
    case 0x15B7: return "Western Digital"; case 0x1987: return "Phison"; case 0x1E0F: return "KIOXIA";
    case 0x126F: return "Silicon Motion"; case 0x1C5C: return "SK hynix"; case 0x1B4B: return "Marvell";
    case 0x1106: return "VIA"; case 0x1B21: return "ASMedia"; case 0x1AF4: return "VirtIO"; case 0x80EE: return "VirtualBox";
    case 0x1234: return "QEMU"; case 0x1D97: return "Shenzhen Longsys"; case 0x2646: return "Kingston"; case 0xC0A9: return "Micron/Crucial";
    }
    return "Unknown maker";
}
static const char *pci_kind(const struct PciDev *d) {
    if (d->cls == 0x0C) return d->prog == 0x30 ? "USB 3 controller" : d->prog == 0x20 ? "USB 2 controller" : "USB controller";
    switch (d->sub) {
    case 0x01: return "IDE disk controller"; case 0x05: return "ATA disk controller";
    case 0x06: return d->prog == 0x01 ? "SATA controller (AHCI)" : "SATA controller";
    case 0x08: return "NVMe SSD"; case 0x04: return "RAID controller"; case 0x07: return "SAS controller";
    case 0x00: return "SCSI controller";
    }
    return "Storage controller";
}

/* ====================================================================
 *  FAT16 file system ("Facet Disk")
 *  Only disks that are completely blank or labelled FACETDISK are used,
 *  so FacetOS never touches anyone's real hard drive.
 * ==================================================================== */
#define ROOT_ENTS 512
static struct { bool ok; u32 total, fat_lba, root_lba, data_lba, nclust, fatsz; u8 spc; } fs;
static u16 fatc[65536];                    /* the whole FAT, cached */
static u8 rootc[ROOT_ENTS * 32];           /* the root directory, cached */
static u8 fat_dirty[256], root_dirty[32];
static u8 sec[512];
struct FInfo { char name[13]; u32 size; int ent; };
static struct FInfo flist[64]; static int nfiles; static u32 free_kb;

static void set_fat(u32 c, u16 v) { fatc[c] = v; fat_dirty[c / 256] = 1; }
static void fs_flush(void) {
    for (u32 s = 0; s < fs.fatsz; s++) if (fat_dirty[s]) {
        ata_write(fs.fat_lba + s, (u8 *)fatc + s * 512);
        ata_write(fs.fat_lba + fs.fatsz + s, (u8 *)fatc + s * 512);   /* second copy */
        fat_dirty[s] = 0;
    }
    for (u32 s = 0; s < 32; s++) if (root_dirty[s]) { ata_write(fs.root_lba + s, rootc + s * 512); root_dirty[s] = 0; }
    ata_flush();
}
static void fs_list(void) {
    nfiles = 0;
    if (!fs.ok) return;
    for (int i = 0; i < ROOT_ENTS && nfiles < 64; i++) {
        u8 *e = rootc + i * 32;
        if (e[0] == 0) break;
        if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x18)) continue;
        struct FInfo *f = &flist[nfiles++];
        int k = 0;
        for (int j = 0; j < 8 && e[j] != ' '; j++) f->name[k++] = (char)e[j];
        if (e[8] != ' ') { f->name[k++] = '.'; for (int j = 8; j < 11 && e[j] != ' '; j++) f->name[k++] = (char)e[j]; }
        f->name[k] = 0; f->size = rd32(e + 28); f->ent = i;
    }
    u32 fr = 0;
    for (u32 c = 2; c < fs.nclust + 2; c++) if (!fatc[c]) fr++;
    free_kb = fr * fs.spc / 2;
}
static bool fs_mount(void) {
    fs.ok = false;
    if (!ata_read(0, sec)) return false;
    u32 tot = rd16(sec + 19) ? rd16(sec + 19) : rd32(sec + 32);
    u32 fsz = rd16(sec + 22), rsvd = rd16(sec + 14);
    if (rd16(sec + 11) != 512 || sec[16] != 2 || !sec[13] || rd16(sec + 17) != ROOT_ENTS || !fsz || fsz > 256 ||
        memcmp(sec + 43, "FACETDISK  ", 11) || memcmp(sec + 54, "FAT16   ", 8) || tot > ata_sectors) return false;
    fs.spc = sec[13]; fs.total = tot; fs.fatsz = fsz; fs.fat_lba = rsvd;
    fs.root_lba = rsvd + 2 * fsz; fs.data_lba = fs.root_lba + 32;
    fs.nclust = (tot - fs.data_lba) / fs.spc;
    if (fs.nclust + 2 > fsz * 256) return false;
    for (u32 s = 0; s < fsz; s++) if (!ata_read(fs.fat_lba + s, (u8 *)fatc + s * 512)) return false;
    for (u32 s = 0; s < 32; s++) if (!ata_read(fs.root_lba + s, rootc + s * 512)) return false;
    fs.ok = true;
    fs_list();
    return true;
}
static void make83(const char *name, const char *ext, u8 *out) {
    memset(out, ' ', 11);
    for (int i = 0; i < 8 && name[i]; i++) out[i] = (u8)name[i];
    for (int i = 0; i < 3 && ext[i]; i++) out[8 + i] = (u8)ext[i];
}
static int fs_find(const u8 *n83) {
    for (int i = 0; i < ROOT_ENTS; i++) {
        u8 *e = rootc + i * 32;
        if (e[0] == 0) break;
        if (e[0] != 0xE5 && !(e[11] & 0x18) && e[11] != 0x0F && !memcmp(e, n83, 11)) return i;
    }
    return -1;
}
static void free_chain(u32 c) {
    for (u32 n = 0; c >= 2 && c < fs.nclust + 2 && n < fs.nclust; n++) { u32 nx = fatc[c]; set_fat(c, 0); c = nx; }
}
static u32 fs_read(int ent, u8 *buf, u32 max) {
    u8 *e = rootc + ent * 32;
    u32 size = rd32(e + 28), got = 0, c = rd16(e + 26);
    if (size > max) size = max;
    for (u32 n = 0; c >= 2 && c < fs.nclust + 2 && got < size && n < fs.nclust; n++) {
        for (u32 s = 0; s < fs.spc && got < size; s++) {
            if (!ata_read(fs.data_lba + (c - 2) * fs.spc + s, sec)) return got;
            u32 k = size - got < 512 ? size - got : 512;
            memcpy(buf + got, sec, k); got += k;
        }
        c = fatc[c];
    }
    return got;
}
/* returns 0 = ok, 1 = no disk, 2 = disk full */
static int fs_write(const u8 *n83, const u8 *data, u32 size) {
    if (!fs.ok) return 1;
    int ent = fs_find(n83);
    if (ent >= 0) { free_chain(rd16(rootc + ent * 32 + 26)); }
    else {
        for (int i = 0; i < ROOT_ENTS && ent < 0; i++) if (rootc[i * 32] == 0 || rootc[i * 32] == 0xE5) ent = i;
        if (ent < 0) return 2;
    }
    u32 csize = fs.spc * 512u, need = (size + csize - 1) / csize, first = 0, prev = 0, c = 2;
    for (u32 k = 0; k < need; k++) {
        while (c < fs.nclust + 2 && fatc[c]) c++;
        if (c >= fs.nclust + 2) { free_chain(first); fs_flush(); fs_list(); return 2; }
        if (prev) set_fat(prev, (u16)c); else first = c;
        set_fat(c, 0xFFFF); prev = c; c++;
    }
    u32 off = 0;
    for (u32 cl = first, n = 0; cl >= 2 && cl < 0xFFF8 && n < need; n++) {
        for (u32 s = 0; s < fs.spc; s++) {
            memset(sec, 0, 512);
            if (off < size) memcpy(sec, data + off, size - off < 512 ? size - off : 512);
            ata_write(fs.data_lba + (cl - 2) * fs.spc + s, sec);
            off += 512;
        }
        cl = fatc[cl];
    }
    u8 *e = rootc + ent * 32;
    memset(e, 0, 32); memcpy(e, n83, 11); e[11] = 0x20;
    read_clock();
    u32 t = (u32)(clock_h << 11 | clock_m << 5 | clock_s / 2);
    u32 d = (u32)((date_y - 1980) << 9 | date_mo << 5 | date_d);
    wr16(e + 14, t); wr16(e + 16, d); wr16(e + 18, d); wr16(e + 22, t); wr16(e + 24, d);
    wr16(e + 26, first); wr32(e + 28, size);
    root_dirty[ent * 32 / 512] = 1;
    fs_flush();
    fs_list();
    return 0;
}
static void fs_delete(int ent) {
    u8 *e = rootc + ent * 32;
    free_chain(rd16(e + 26));
    e[0] = 0xE5;
    root_dirty[ent * 32 / 512] = 1;
    fs_flush();
    fs_list();
}


/* ====================================================================
 *  The Vault: where files are saved.
 *   - DISK mode: a disk that is already a FacetOS disk (label FACETDISK).
 *   - LIVE mode: no FacetOS disk found, so files live in memory until
 *     shutdown, like a live USB. FacetOS never formats or overwrites a disk.
 * ==================================================================== */
enum { VAULT_LIVE, VAULT_DISK };
static int vault_mode = VAULT_LIVE, vault_drive = -1;
#define RSLOTS 24
#define RSLOT_SIZE (160 * 1024)
static u8 ram_pool[RSLOTS][RSLOT_SIZE];
static struct { bool used; u8 n83[11]; u32 size; } rfiles[RSLOTS];
static void vault_mount(void) {
    vault_mode = VAULT_LIVE; vault_drive = -1; fs.ok = false;
    for (int i = 0; i < ndrives; i++) {
        if (drives[i].atapi) continue;
        ata_use(i);
        if (fs_mount()) { vault_mode = VAULT_DISK; vault_drive = i; return; }
    }
    ata_ok = false;                    /* not a FacetOS disk: never touch it */
}
static void vault_list(void) {
    if (vault_mode == VAULT_DISK) { fs_list(); return; }
    nfiles = 0; u32 used = 0;
    for (int i = 0; i < RSLOTS; i++) if (rfiles[i].used) {
        struct FInfo *f = &flist[nfiles++]; int k = 0; const u8 *e = rfiles[i].n83;
        for (int j = 0; j < 8 && e[j] != ' '; j++) f->name[k++] = (char)e[j];
        if (e[8] != ' ') { f->name[k++] = '.'; for (int j = 8; j < 11 && e[j] != ' '; j++) f->name[k++] = (char)e[j]; }
        f->name[k] = 0; f->size = rfiles[i].size; f->ent = i; used++;
    }
    free_kb = (RSLOTS - used) * (RSLOT_SIZE / 1024);
}
static int vault_find(const u8 *n83) {
    if (vault_mode == VAULT_DISK) return fs_find(n83);
    for (int i = 0; i < RSLOTS; i++) if (rfiles[i].used && !memcmp(rfiles[i].n83, n83, 11)) return i;
    return -1;
}
static int vault_write(const u8 *n83, const u8 *data, u32 size) {     /* 0 ok, 2 full */
    if (vault_mode == VAULT_DISK) return fs_write(n83, data, size);
    if (size > RSLOT_SIZE) return 2;
    int s2 = vault_find(n83);
    for (int i = 0; i < RSLOTS && s2 < 0; i++) if (!rfiles[i].used) s2 = i;
    if (s2 < 0) return 2;
    memcpy(ram_pool[s2], data, size); memcpy(rfiles[s2].n83, n83, 11);
    rfiles[s2].size = size; rfiles[s2].used = true;
    vault_list();
    return 0;
}
static u32 vault_read(int ent, u8 *buf, u32 max) {
    if (vault_mode == VAULT_DISK) return fs_read(ent, buf, max);
    u32 n = rfiles[ent].size < max ? rfiles[ent].size : max;
    memcpy(buf, ram_pool[ent], n);
    return n;
}
static void vault_delete(int ent) {
    if (vault_mode == VAULT_DISK) { fs_delete(ent); return; }
    rfiles[ent].used = false; vault_list();
}

/* ====================================================================
 *  Wallpapers (computed pixel by pixel): low-poly facets + soft light beams
 * ==================================================================== */
struct Theme { const char *name; u32 top, bottom, beam1, beam2; int facet, beam_strength; };
static const struct Theme THEMES[4] = {
    {"Obsidian", 0x161C33, 0x06080F, 0x2DD4BF, 0x8B5CF6, 5, 70},
    {"Aurora",   0x0E3446, 0x170C30, 0x34D399, 0xC084FC, 6, 95},
    {"Prism",    0x2A2F4F, 0x0C0E1C, 0x38BDF8, 0xF472B6, 6, 110},
    {"Daylight", 0x8FB8E8, 0x3D4E86, 0xFFFFFF, 0xC4B5FD, 5, 80},
};
static int theme = 0;
static const u8 BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
/* brightness of a soft beam: distance from the line y = y0 + slope*x (slope in 1/256) */
static int beam(int x, int y, int y0, int slope, int width) {
    int d = y - y0 - (x * slope >> 8);
    if (d < 0) d = -d;
    d = d * 220 >> 8;                               /* rough cos() of the beam angle */
    if (d >= width) return 0;
    int t = 256 - d * 256 / width;
    return t * t >> 8;                              /* 0..256, soft falloff */
}
static void make_wallpaper(void) {
    const struct Theme *t = &THEMES[theme];
    for (int y = 0; y < H; y++) {
        u32 base = mix(t->top, t->bottom, y, H);
        for (int x = 0; x < W; x++) {
            int u = (x * 2 + y + 6000) / 340, v = (x * 2 - y + 6000) / 340, k = (y + 70) / 300;
            int f = ((u * 7 + v * 13 + k * 5) % 5) - 2;
            int d = (BAYER[(x & 3) + (y & 3) * 4] - 8) / 4;
            int r = (int)((base >> 16) & 255) + f * t->facet + d;
            int g = (int)((base >> 8) & 255) + f * t->facet + d;
            int b = (int)(base & 255) + f * t->facet + d;
            int b1 = beam(x, y, -H / 3, 150, W / 5) * t->beam_strength >> 8;
            int b2 = beam(x, y, -H / 6, 130, W / 9) * t->beam_strength >> 8;
            r += (int)(((t->beam1 >> 16) & 255) * b1 + ((t->beam2 >> 16) & 255) * b2) >> 8;
            g += (int)(((t->beam1 >> 8) & 255) * b1 + ((t->beam2 >> 8) & 255) * b2) >> 8;
            b += (int)((t->beam1 & 255) * b1 + (t->beam2 & 255) * b2) >> 8;
            r = imax(0, imin(255, r)); g = imax(0, imin(255, g)); b = imax(0, imin(255, b));
            bgbuf[y * MAXW + x] = (u32)(r << 16 | g << 8 | b);
        }
    }
}

/* ====================================================================
 *  FacetOS "Prism" look: colours, cut-corner shapes, prism gradient
 * ==================================================================== */
#define TB 34                 /* window title bar height */
#define CHAMF 10              /* cut-corner size */
#define DOCK_H 64
#define TXT 0xE8ECF8
#define TXT2 0x9AA3BD
#define PANEL 0x151A29
#define PANEL2 0x1D2436
#define LINE 0x2C3550
#define PRISM 0xFF000000u     /* special colour: use the prism gradient */
static inline void pset_(int x, int y, u32 c) { if (x >= cx0 && x < cx1 && y >= cy0 && y < cy1) bb[y * MAXW + x] = c; }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static bool streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static u32 prism(int t, int n) {                       /* teal -> sky -> indigo -> violet */
    static const u32 st[4] = {0x2DD4BF, 0x38BDF8, 0x6366F1, 0x8B5CF6};
    if (n <= 1) return st[0];
    int p = t * 3 * 256 / (n - 1), k = p >> 8;
    if (k >= 3) return st[3];
    return mix(st[k], st[k + 1], p & 255, 256);
}
/* coverage (0..255) of a pixel inside a rectangle with cut corners */
static u32 cham_cov(int i, int j, int x, int y, int w, int h, int c) {
    int a = i - x, b = j - y, a2 = x + w - 1 - i, b2 = y + h - 1 - j;
    if ((a >= c && a2 >= c) || (b >= c && b2 >= c)) return 255;
    int m = a + b; if (a2 + b < m) m = a2 + b; if (a + b2 < m) m = a + b2; if (a2 + b2 < m) m = a2 + b2;
    m -= c;
    return m > 0 ? 255 : m == 0 ? 140 : m == -1 ? 30 : 0;
}
static void chamfer(int x, int y, int w, int h, int c, u32 col, u32 alpha) {
    CLIPRECT;
    for (int j = y0; j < y1; j++) {
        u32 *row = &bb[j * MAXW];
        for (int i = x0; i < x1; i++) {
            u32 a = cham_cov(i, j, x, y, w, h, c) * alpha / 255;
            if (!a) continue;
            u32 cc = col == PRISM ? prism(i - x, w) : col;
            row[i] = a >= 255 ? cc : blend(row[i], cc, a);
        }
    }
}
static void diamond(int cx, int cy, int r, u32 col, u32 alpha) {
    for (int j = cy - r - 1; j <= cy + r + 1; j++) for (int i = cx - r - 1; i <= cx + r + 1; i++) {
        int d = iabs(i - cx) + iabs(j - cy);
        u32 a = d < r ? 255 : d == r ? 140 : 0;
        if (a) pblend(i, j, col == PRISM ? prism(i - cx + r, 2 * r + 1) : col, a * alpha / 255);
    }
}
static void button(int x, int y, int w, int h, const char *label, bool primary, bool down) {
    if (primary) {
        chamfer(x, y, w, h, 5, PRISM, 255);
        if (down) chamfer(x, y, w, h, 5, 0x000000, 60);
        text_c(&F_BOLD, x + w / 2, y + h / 2 - 8, label, 0x0B0E18);
    } else {
        chamfer(x, y, w, h, 5, 0x3A4566, 255);
        chamfer(x + 1, y + 1, w - 2, h - 2, 4, down ? 0x1A2033 : 0x262E45, 255);
        text_c(&F_BOLD, x + w / 2, y + h / 2 - 8, label, TXT);
    }
}

/* ====================================================================
 *  Apps, windows, state
 * ==================================================================== */
enum { W_SYSTEM, W_VAULT, W_NOTES, W_PAINT, W_CALC, W_PUZZLE, W_SETTINGS, W_README, W_BIN, NWIN };
struct App { const char *name; const Image *ic, *is; };
static const struct App APPS[NWIN] = {
    {"System", &IC_SYSTEM, &IS_SYSTEM}, {"Vault", &IC_VAULT, &IS_VAULT}, {"Notes", &IC_NOTES, &IS_NOTES},
    {"Paint", &IC_PAINT, &IS_PAINT}, {"Calculator", &IC_CALC, &IS_CALC}, {"Puzzle", &IC_PUZZLE, &IS_PUZZLE},
    {"Settings", &IC_SETTINGS, &IS_SETTINGS}, {"Read Me", &IC_README, &IS_README}, {"Bin", &IC_BIN, &IS_BIN},
};
static const int DOCK_APPS[8] = {W_SYSTEM, W_VAULT, W_NOTES, W_PAINT, W_CALC, W_PUZZLE, W_SETTINGS, W_README};
static const int DESK_ICONS[3] = {W_SYSTEM, W_VAULT, W_BIN};
struct Win { const char *title; int x, y, w, h; bool open, minimized; };
static struct Win win[NWIN];
static int zord[NWIN], nz;
static int dsel = -1, fsel = -1;
static bool launcher_open, anims_on = true;
static int dock_x, dock_y, dock_w;

static int mx, my, mbtn;
static int drag_win = -1, drag_dx, drag_dy;
static u32 last_click_ms; static int last_click_x, last_click_y;
static bool caret_on = true; static u32 caret_ms;
static u8 filebuf[512 * 1024];
static int pressed_btn = -1;                  /* which toolbar button is held down (for feedback) */

/* Notes */
static char notes[4096]; static int nlen;
static char notes_file[13], notes_title[40] = "Notes";
/* Paint */
#define CW 480
#define CH 262
static u8 canvas[CW * CH];
static const u32 PCOL[12] = {0xFFFFFF, 0x1E2230, 0xE63946, 0xFF8C3C, 0xFCC419, 0x2EC464,
                             0x28B4C8, 0x3884FF, 0x8B5CF6, 0xF06EA0, 0x8B5A2B, 0x9AA0AA};
static int pcolor = 1, psize = 1; static bool painting; static int plx, ply;
static char paint_file[13], paint_title[40] = "Paint";
/* Puzzle */
static u8 tiles[16]; static int moves;
/* Calculator: fixed point with 4 decimals */
#define CSCALE 10000
#define CMAX ((i64)999999999999 * CSCALE)
static i64 c_acc, c_cur; static int c_op; static char c_ent[16]; static int c_elen;
static bool c_entering, c_err; static int c_pressed = -1;
static const char *CKEYS[19] = {"C", "+/-", "%", "/", "7", "8", "9", "*", "4", "5", "6", "-",
                                "1", "2", "3", "+", "0", ".", "="};
/* Dialog */
enum { D_NONE, D_SAVE_NOTE, D_SAVE_PAINT, D_DELETE, D_INFO };
static struct { int kind; char l1[64], l2[72]; char f[9]; int flen; const char *ok, *cancel; int x, y, w, h; } dlg;

/* damage tracking */
static bool dmg; static int dx0, dy0, dx1, dy1;
static void damage(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!dmg) { dmg = true; dx0 = x; dy0 = y; dx1 = x + w; dy1 = y + h; return; }
    dx0 = imin(dx0, x); dy0 = imin(dy0, y); dx1 = imax(dx1, x + w); dy1 = imax(dy1, y + h);
}
static void damage_win(int id) { damage(win[id].x - 8, win[id].y - 8, win[id].w + 16, win[id].h + 20); }
static void damage_dock(void) { damage(dock_x - 6, dock_y - 6, dock_w + 12, DOCK_H + 12); }

/* ====================================================================
 *  Window management
 * ==================================================================== */
static int top_win(void) {
    for (int i = nz - 1; i >= 0; i--) if (!win[zord[i]].minimized) return zord[i];
    return -1;
}
static void win_front(int id) {
    int k = -1;
    for (int i = 0; i < nz; i++) if (zord[i] == id) k = i;
    if (k < 0) return;
    int old = top_win();
    for (int i = k; i < nz - 1; i++) zord[i] = zord[i + 1];
    zord[nz - 1] = id;
    if (old != id) { damage_win(id); if (old >= 0) damage_win(old); damage_dock(); }
}
static void anim_open(int id);
static void anim_close(int id);
static void anim_minimize(int id, bool restore);
static void flush(void);
static void win_open(int id) {
    if (!win[id].open) {
        int old = top_win();
        win[id].open = true; win[id].minimized = false; zord[nz++] = id;
        if (old >= 0) damage_win(old);
        damage_win(id); damage_dock();
        anim_open(id);
        return;
    }
    if (win[id].minimized) { win[id].minimized = false; anim_minimize(id, true); damage_win(id); damage_dock(); }
    win_front(id);
}
static void win_remove(int id) {
    win[id].open = false;
    int k = 0;
    for (int i = 0; i < nz; i++) if (zord[i] != id) zord[k++] = zord[i];
    nz = k;
    if (top_win() >= 0) damage_win(top_win());
    damage_dock();
}
static void win_close(int id) {
    if (!win[id].open) return;
    if (anims_on && !win[id].minimized) anim_close(id);
    else damage_win(id);
    win_remove(id);
}
static void win_minimize(int id) {
    if (!win[id].open || win[id].minimized) return;
    damage_win(id);
    win[id].minimized = true;
    if (top_win() >= 0) damage_win(top_win());
    damage_dock();
    anim_minimize(id, false);
}
static bool inside(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }
static int win_at(int x, int y) {
    for (int i = nz - 1; i >= 0; i--) {
        struct Win *w = &win[zord[i]];
        if (!w->minimized && inside(x, y, w->x, w->y, w->w, w->h)) return zord[i];
    }
    return -1;
}
static void content_rect(int id, int *x, int *y, int *w, int *h) {
    *x = win[id].x + 1; *y = win[id].y + TB + 1; *w = win[id].w - 2; *h = win[id].h - TB - 1 - CHAMF;
}

/* ====================================================================
 *  Calculator logic
 * ==================================================================== */
static i64 iabs64(i64 v) { return v < 0 ? -v : v; }
static i64 calc_parse(void) {
    i64 ip = 0, fp = 0; int fd = 0; bool dot = false;
    for (int i = 0; i < c_elen; i++) {
        char ch = c_ent[i];
        if (ch == '.') { dot = true; continue; }
        if (!dot) ip = ip * 10 + (ch - '0');
        else if (fd < 4) { fp = fp * 10 + (ch - '0'); fd++; }
    }
    while (fd < 4) { fp *= 10; fd++; }
    return ip * CSCALE + fp;
}
static void fmt_fixed(i64 v, char *out) {
    if (v < 0) { *out++ = '-'; v = -v; }
    u64 ip = (u64)v / CSCALE, fp = (u64)v % CSCALE;
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
    while (n) *out++ = t[--n];
    if (fp) {
        *out++ = '.';
        char f[4]; for (int i = 3; i >= 0; i--) { f[i] = (char)('0' + fp % 10); fp /= 10; }
        int last = 3; while (f[last] == '0') last--;
        for (int i = 0; i <= last; i++) *out++ = f[i];
    }
    *out = 0;
}
static bool calc_apply(i64 a, i64 b, int op, i64 *res) {
    i64 r = 0;
    if (op == '+') r = a + b;
    else if (op == '-') r = a - b;
    else if (op == '*') {
        i64 ai = a / CSCALE, af = a % CSCALE;
        if (ai && iabs64(b) > (i64)9000000000000000000 / iabs64(ai)) return false;
        r = ai * b + af * (b / CSCALE) + (af * (b % CSCALE)) / CSCALE;
    } else if (op == '/') {
        if (!b) return false;
        bool neg = (a < 0) != (b < 0);
        u64 ua = (u64)iabs64(a), ub = (u64)iabs64(b), q = ua / ub, rm = ua % ub;
        if (q > (u64)CMAX / CSCALE) return false;
        for (int i = 0; i < 4; i++) { rm *= 10; q = q * 10 + rm / ub; rm %= ub; }
        r = neg ? -(i64)q : (i64)q;
    }
    if (iabs64(r) > CMAX) return false;
    *res = r; return true;
}
static void calc_key(char k) {
    if (k == 'C') { c_acc = c_cur = 0; c_op = 0; c_elen = 0; c_entering = false; c_err = false; return; }
    if (c_err) return;
    if ((k >= '0' && k <= '9') || k == '.') {
        if (!c_entering) { c_elen = 0; c_entering = true; }
        if (k == '.') { for (int i = 0; i < c_elen; i++) if (c_ent[i] == '.') return; if (!c_elen) c_ent[c_elen++] = '0'; }
        int digits = 0; bool dot = false;
        for (int i = 0; i < c_elen; i++) { if (c_ent[i] == '.') dot = true; else if (!dot) digits++; }
        if (k != '.' && ((!dot && digits >= 12) || c_elen >= 15)) return;
        if (k == '0' && c_elen == 1 && c_ent[0] == '0') return;
        if (k != '.' && c_elen == 1 && c_ent[0] == '0') c_elen = 0;
        c_ent[c_elen++] = k; c_ent[c_elen] = 0;
        c_cur = calc_parse();
        return;
    }
    if (k == 'B') { if (c_entering && c_elen) { c_elen--; c_ent[c_elen] = 0; c_cur = calc_parse(); } return; }
    if (k == 'N') { c_cur = -c_cur; c_entering = false; return; }
    if (k == '%') { c_cur /= 100; c_entering = false; return; }
    if (k == '+' || k == '-' || k == '*' || k == '/' || k == '=') {
        if (c_op && (c_entering || k == '=')) {
            if (!calc_apply(c_acc, c_cur, c_op, &c_acc)) { c_err = true; return; }
            c_cur = c_acc;
        } else c_acc = c_cur;
        c_op = (k == '=') ? 0 : k;
        c_entering = false;
    }
}
static void calc_key_rect(int i, int *x, int *y, int *w, int *h) {
    int col = i % 4, row = i / 4;
    *x = 12 + col * 52; *y = 74 + row * 44; *w = 46; *h = 38;
    if (i == 16) *w = 98;
    if (i == 17) *x = 116;
    if (i == 18) *x = 168;
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
 *  Toolbar buttons inside windows
 * ==================================================================== */
struct TBtn { int win; const char *label; int x, y, w; bool primary; int act; };
enum { B_NEW, B_OPEN, B_SAVE, B_SAVEAS, B_CLEAR, B_PSAVE, B_POPEN, B_VOPEN, B_VDELETE, B_SHUFFLE };
static const struct TBtn TBTNS[] = {
    {W_NOTES, "New", 8, 6, 52, false, B_NEW}, {W_NOTES, "Open", 64, 6, 56, false, B_OPEN},
    {W_NOTES, "Save", 124, 6, 56, true, B_SAVE}, {W_NOTES, "Save As", 184, 6, 72, false, B_SAVEAS},
    {W_PAINT, "Clear", 326, 8, 48, false, B_CLEAR}, {W_PAINT, "Open", 378, 8, 46, false, B_POPEN},
    {W_PAINT, "Save", 428, 8, 46, true, B_PSAVE},
    {W_VAULT, "Open", -150, 6, 68, true, B_VOPEN}, {W_VAULT, "Delete", -76, 6, 68, false, B_VDELETE},
    {W_PUZZLE, "Shuffle", 62, 192, 76, false, B_SHUFFLE},
};
#define NTBTN (int)(sizeof TBTNS / sizeof TBTNS[0])
static void tbtn_rect(int i, int *x, int *y, int *w, int *h) {
    int cx, cy, cw, ch; content_rect(TBTNS[i].win, &cx, &cy, &cw, &ch);
    *x = cx + (TBTNS[i].x < 0 ? cw + TBTNS[i].x : TBTNS[i].x); *y = cy + TBTNS[i].y; *w = TBTNS[i].w; *h = 24;
}
static void draw_tbtns(int id) {
    for (int i = 0; i < NTBTN; i++) if (TBTNS[i].win == id) {
        int x, y, w, h; tbtn_rect(i, &x, &y, &w, &h);
        bool en = true;
        if (TBTNS[i].act == B_VDELETE || TBTNS[i].act == B_VOPEN) en = fsel >= 0;
        if (!en) { chamfer(x, y, w, h, 5, 0x262E45, 255); text_c(&F_BOLD, x + w / 2, y + 4, TBTNS[i].label, 0x5A6380); continue; }
        button(x, y, w, h, TBTNS[i].label, TBTNS[i].primary, pressed_btn == i);
    }
}

/* ====================================================================
 *  Drawing the apps
 * ==================================================================== */
static const char *const README[] = {
    "Welcome to FacetOS " VERSION " \"Prism\"!",
    "",
    "FacetOS is a tiny operating system that draws every single",
    "pixel on this screen by itself - no Linux, no libraries.",
    "",
    "New in 1.2: a look of its own. Dark glass windows with cut",
    "corners, the Prism Bar at the bottom, window animations,",
    "and a Vault that works like a live USB.",
    "",
    "  - Click the gem in the Prism Bar to see all apps.",
    "  - Click an app in the Prism Bar to open or hide it.",
    "  - The Vault keeps your files until you shut down.",
    "  - FacetOS never formats or overwrites your disks.",
    "",
    "Made with Claude. Free and open source.",
    0,
};
static void line_kv(int x, int y, const char *k, const char *v) {
    text(&F_REG, x, y, k, TXT2); text(&F_REG, x + 120, y, v, TXT);
}
static const Image *file_icon(const char *name) {
    int n = slen(name);
    return (n > 4 && streq(name + n - 4, ".BMP")) ? &IC_DOC_BMP : &IC_DOC_TXT;
}
static void draw_content(int id, int x, int y, int w, int h) {
    char s[96], *p;
    switch (id) {
    case W_SYSTEM: {
        fill(x, y, w, h, PANEL);
        image(&GEM_BIG, x + 18, y + 10, 255, false);
        image(&WORDMARK, x + 128, y + 26, 255, false);
        text(&F_REG, x + 130, y + 80, "Version " VERSION "  \"Prism\"", TXT2);
        int yy = y + 116;
        p = utoa(mem_mb, s); scat(p, " MB"); line_kv(x + 20, yy, "Memory", s); yy += 19;
        p = scat(s, cpu_vendor); scat(p, " (32-bit)"); line_kv(x + 20, yy, "Processor", s); yy += 19;
        p = utoa((u32)W, s); p = scat(p, " x "); p = utoa((u32)H, p); scat(p, ", 32-bit colour"); line_kv(x + 20, yy, "Display", s); yy += 19;
        line_kv(x + 20, yy, "Started by", loader); yy += 26;
        text(&F_BOLD, x + 20, yy, "Storage found", TXT); yy += 20;
        int shown = 0;
        for (int i = 0; i < ndrives && shown < 5; i++, shown++) {
            p = scat(s, drives[i].model);
            if (!drives[i].atapi) { p = scat(p, "  -  "); p = utoa(drives[i].sectors / 2048, p); scat(p, " MB"); }
            diamond(x + 26, yy + 8, 4, i == vault_drive ? PRISM : 0x5A6380, 255);
            text(&F_REG, x + 38, yy, s, TXT);
            text(&F_REG, x + w - 20 - text_w(&F_REG, drives[i].atapi ? "CD/DVD" : i == vault_drive ? "FacetOS disk" : "read-only"), yy,
                 drives[i].atapi ? "CD/DVD" : i == vault_drive ? "FacetOS disk" : "read-only", TXT2);
            yy += 18;
        }
        for (int i = 0; i < npci && shown < 7; i++, shown++) {
            p = scat(s, pci_vendor(pci_store[i].vendor)); p = scat(p, " "); scat(p, pci_kind(&pci_store[i]));
            diamond(x + 26, yy + 8, 4, 0x5A6380, 255);
            text(&F_REG, x + 38, yy, s, TXT);
            text(&F_REG, x + w - 20 - text_w(&F_REG, "detected"), yy, "detected", TXT2);
            yy += 18;
        }
        if (!shown) { text(&F_REG, x + 38, yy, "No disks found - running like a live USB.", TXT2); yy += 18; }
        yy = y + h - 30;
        chamfer(x + 14, yy - 4, w - 28, 26, 6, 0x0F2A2E, 255);
        text(&F_REG, x + 26, yy + 1, vault_mode == VAULT_DISK ? "Vault: saving to the FacetOS disk." :
                                     "Vault: live mode - files stay until you shut down.", 0x7EF0DC);
        break; }
    case W_VAULT: {
        fill(x, y, w, h, PANEL);
        fill(x, y, w, 36, PANEL2); hline(x, y + 36, w, LINE);
        p = utoa((u32)nfiles, s); p = scat(p, nfiles == 1 ? " file  -  " : " files  -  "); p = utoa(free_kb, p);
        scat(p, vault_mode == VAULT_DISK ? " KB free on disk" : " KB free (live)");
        text(&F_REG, x + 12, y + 11, s, TXT2);
        draw_tbtns(id);
        if (!nfiles) {
            image(&IC_VAULT, x + w / 2 - 24, y + 80, 160, false);
            text_c(&F_REG, x + w / 2, y + 140, "Nothing here yet. Save something in Notes or Paint.", TXT2);
            if (vault_mode == VAULT_LIVE) text_c(&F_REG, x + w / 2, y + 160, "Live mode: files are kept in memory until you shut down.", 0x5A6380);
            break;
        }
        for (int i = 0; i < imin(nfiles, 12); i++) {
            int ix = x + 18 + (i % 4) * 112, iy = y + 50 + (i / 4) * 84;
            if (fsel == i) chamfer(ix - 6, iy - 4, 100, 78, 8, 0x6366F1, 90);
            image(file_icon(flist[i].name), ix + 20, iy, 255, false);
            text_c(&F_REG, ix + 44, iy + 54, flist[i].name, TXT);
        }
        break; }
    case W_NOTES: {
        fill(x, y, w, 36, PANEL2); hline(x, y + 36, w, LINE);
        draw_tbtns(id);
        fill(x, y + 37, w, h - 37, 0x111522);
        for (int ly = y + 62; ly < y + h; ly += 20) hline(x + 12, ly, w - 24, 0x1C2236);
        int px = x + 14, py = y + 46, right = x + w - 12;
        for (int i = 0; i <= nlen; i++) {
            if (i == nlen) { if (caret_on && top_win() == W_NOTES && !dlg.kind) fill(px, py + 1, 2, 15, 0x5EEAD4); break; }
            char ch = notes[i];
            if (ch == '\n') { px = x + 14; py += 20; continue; }
            if (py > y + h) break;
            char str[2] = {ch, 0};
            int cw = text_w(&F_REG, str);
            if (ch != ' ' && (i == 0 || notes[i - 1] == ' ' || notes[i - 1] == '\n')) {
                int ww = 0;
                for (int j = i; j < nlen && notes[j] != ' ' && notes[j] != '\n'; j++) { char t2[2] = {notes[j], 0}; ww += text_w(&F_REG, t2); }
                if (px + ww > right && px > x + 14 && ww <= right - x - 14) { px = x + 14; py += 20; }
            }
            if (px + cw > right) { px = x + 14; py += 20; }
            text(&F_REG, px, py, str, TXT);
            px += cw;
        }
        break; }
    case W_PAINT: {
        fill(x, y, w, 40, PANEL2); hline(x, y + 40, w, LINE);
        for (int i = 0; i < 12; i++) {
            int sx = x + 10 + i * 19;
            if (i == pcolor) chamfer(sx - 2, y + 10, 20, 20, 4, PRISM, 255);
            chamfer(sx, y + 12, 16, 16, 3, PCOL[i], 255);
        }
        for (int i = 0; i < 3; i++) {
            int bx = x + 246 + i * 25, ps = 3 + i * 4;
            chamfer(bx, y + 8, 22, 24, 5, psize == i * 3 + 1 ? 0x6366F1 : 0x262E45, 255);
            rrect(bx + 11 - ps / 2, y + 20 - ps / 2, ps, ps, ps / 2, TXT, 255);
        }
        draw_tbtns(id);
        int ox = x, oy = y + 41;
        int i0 = imax(cx0 - ox, 0), i1 = imin(cx1 - ox, CW), j0 = imax(cy0 - oy, 0), j1 = imin(cy1 - oy, CH);
        for (int j = j0; j < j1; j++) {
            u32 *row = &bb[(oy + j) * MAXW + ox];
            const u8 *src = &canvas[j * CW];
            for (int i = i0; i < i1; i++) row[i] = PCOL[src[i]];
        }
        break; }
    case W_CALC: {
        fill(x, y, w, h, 0x10141F);
        chamfer(x + 12, y + 12, w - 24, 50, 8, PRISM, 255);
        chamfer(x + 14, y + 14, w - 28, 46, 7, 0x0B0F19, 255);
        if (c_err) scat(s, "Error"); else if (c_entering) scat(s, c_ent); else fmt_fixed(c_cur, s);
        const Font *f = text_w(&F_BIG, s) > w - 50 ? &F_BOLD : &F_BIG;
        text(f, x + w - 26 - text_w(f, s), y + (f == &F_BIG ? 23 : 29), s, TXT);
        if (c_op) { char o[2] = {(char)c_op, 0}; text(&F_BOLD, x + 22, y + 18, o, 0x5EEAD4); }
        for (int i = 0; i < 19; i++) {
            int kx, ky, kw, kh; calc_key_rect(i, &kx, &ky, &kw, &kh);
            kx += x; ky += y;
            bool op = (i % 4 == 3 && i < 16) || i == 18, top = i < 3;
            if (op) { chamfer(kx, ky, kw, kh, 7, PRISM, 255); if (i == c_pressed) chamfer(kx, ky, kw, kh, 7, 0, 70); }
            else {
                chamfer(kx, ky, kw, kh, 7, 0x323B57, 255);
                chamfer(kx + 1, ky + 1, kw - 2, kh - 2, 6, i == c_pressed ? 0x161B2B : top ? 0x262E45 : 0x1F2639, 255);
            }
            text_c(&F_BOLD, kx + kw / 2, ky + 11, CKEYS[i], op ? 0x0B0E18 : TXT);
        }
        break; }
    case W_PUZZLE: {
        fill(x, y, w, h, PANEL);
        for (int i = 0; i < 16; i++) {
            int tx = x + 12 + (i % 4) * 44, ty = y + 10 + (i / 4) * 44;
            if (!tiles[i]) { chamfer(tx, ty, 40, 40, 7, 0x1D2436, 255); continue; }
            u32 c = prism(tiles[i] - 1, 15);
            chamfer(tx, ty, 40, 40, 7, c, 255);
            chamfer(tx + 2, ty + 2, 36, 14, 5, 0xFFFFFF, 40);
            char b[4]; utoa(tiles[i], b);
            text_c(&F_BIG, tx + 20, ty + 8, b, 0x0B0E18);
        }
        bool solved = true;
        for (int i = 0; i < 15; i++) if (tiles[i] != i + 1) solved = false;
        p = scat(s, solved ? "Solved! Moves: " : "Moves: "); utoa((u32)moves, p);
        text(&F_REG, x + 12, y + 183, s, solved ? 0x5EEAD4 : TXT2);
        draw_tbtns(id);
        break; }
    case W_SETTINGS: {
        fill(x, y, w, h, PANEL);
        text(&F_BOLD, x + 20, y + 16, "Wallpaper", TXT);
        for (int i = 0; i < 4; i++) {
            int tx = x + 20 + i * 104, ty = y + 42;
            if (theme == i) chamfer(tx - 3, ty - 3, 98, 70, 10, PRISM, 255);
            chamfer(tx, ty, 92, 64, 8, THEMES[i].bottom, 255);
            int sx0 = cx0, sy0 = cy0, sx1 = cx1, sy1 = cy1;
            cy1 = imin(cy1, ty + 34);
            chamfer(tx, ty, 92, 64, 8, THEMES[i].top, 255);
            cx0 = sx0; cy0 = sy0; cx1 = sx1; cy1 = sy1;
            diamond(tx + 70, ty + 18, 9, THEMES[i].beam1, 150); diamond(tx + 56, ty + 26, 6, THEMES[i].beam2, 150);
            text_c(&F_REG, tx + 46, ty + 72, THEMES[i].name, theme == i ? TXT : TXT2);
        }
        text(&F_BOLD, x + 20, y + 140, "Window animations", TXT);
        text(&F_REG, x + 20, y + 160, "Shimmer when opening, shatter when closing.", TXT2);
        int sx = x + w - 76, sy = y + 144;
        chamfer(sx, sy, 52, 26, 6, anims_on ? PRISM : 0x323B57, 255);
        diamond(anims_on ? sx + 38 : sx + 14, sy + 13, 9, 0xFFFFFF, 255);
        hline(x + 20, y + 196, w - 40, LINE);
        text(&F_REG, x + 20, y + 210, "FacetOS " VERSION " \"Prism\"  -  made with Claude, free and open source.", TXT2);
        break; }
    case W_README:
        fill(x, y, w, h, PANEL);
        for (int i = 0; README[i]; i++)
            text(i == 0 ? &F_BOLD : &F_REG, x + 18, y + 14 + i * 19, README[i], i == 0 ? 0x5EEAD4 : TXT);
        break;
    case W_BIN:
        fill(x, y, w, h, PANEL);
        image(&IC_BIN, x + 24, y + 26, 255, false);
        text(&F_BOLD, x + 90, y + 30, "The Bin is empty.", TXT);
        text(&F_REG, x + 90, y + 50, "Deleted files are gone for good.", TXT2);
        break;
    }
}

static void draw_window(int id, bool active) {
    struct Win *wd = &win[id];
    int x = wd->x, y = wd->y, w = wd->w, h = wd->h;
    chamfer(x - 6, y + 2, w + 12, h + 12, CHAMF + 6, 0, 30);
    chamfer(x - 3, y + 1, w + 6, h + 7, CHAMF + 3, 0, 45);
    if (active) chamfer(x - 2, y - 2, w + 4, h + 4, CHAMF + 2, 0x6366F1, 70);
    chamfer(x, y, w, h, CHAMF, active ? 0x46527A : 0x2A3148, 255);
    chamfer(x + 1, y + 1, w - 2, h - 2, CHAMF - 1, PANEL, 255);
    int sx0 = cx0, sy0 = cy0, sx1 = cx1, sy1 = cy1;
    cy1 = imin(cy1, y + TB);
    chamfer(x + 1, y + 1, w - 2, h - 2, CHAMF - 1, active ? 0x20283E : 0x1A2032, 255);
    if (active) { cy1 = imin(sy1, y + 3); chamfer(x, y, w, h, CHAMF, PRISM, 255); }
    cx0 = sx0; cy0 = sy0; cx1 = sx1; cy1 = sy1;
    hline(x + 1, y + TB, w - 2, LINE);
    image(APPS[id].is, x + 12, y + 7, active ? 255 : 150, false);
    text(&F_BOLD, x + 40, y + 9, wd->title, active ? TXT : TXT2);
    diamond(x + w - 22, y + 17, 7, active ? 0xF43F5E : 0x4A5270, 255);       /* close */
    diamond(x + w - 44, y + 17, 7, active ? 0x94A3B8 : 0x4A5270, 255);       /* minimise */
    if (active) {
        for (int k = -2; k <= 2; k++) { pset_(x + w - 22 + k, y + 17 + k, 0xFFFFFF); pset_(x + w - 22 + k, y + 17 - k, 0xFFFFFF); }
        hline(x + w - 46, y + 17, 5, 0x0F172A);
    }
    int ccx, ccy, ccw, cch; content_rect(id, &ccx, &ccy, &ccw, &cch);
    cx0 = imax(cx0, ccx); cy0 = imax(cy0, ccy); cx1 = imin(cx1, ccx + ccw); cy1 = imin(cy1, ccy + cch);
    if (cx0 < cx1 && cy0 < cy1) draw_content(id, ccx, ccy, ccw, cch);
    cx0 = sx0; cy0 = sy0; cx1 = sx1; cy1 = sy1;
}

/* ---- dialogs ---- */
static void dlg_buttons(int *okx, int *cax, int *by) {
    *by = dlg.y + dlg.h - 40;
    *okx = dlg.x + dlg.w - 108;
    *cax = dlg.cancel ? *okx - 96 : -1000;
}
static void draw_dialog(void) {
    if (!dlg.kind) return;
    int x = dlg.x, y = dlg.y, w = dlg.w, h = dlg.h;
    chamfer(x - 8, y + 2, w + 16, h + 16, 18, 0, 50);
    chamfer(x - 2, y - 2, w + 4, h + 4, 14, PRISM, 255);
    chamfer(x, y, w, h, 12, 0x181E30, 255);
    const Image *ic = dlg.kind == D_INFO ? &IC_README : dlg.kind == D_DELETE ? &IC_BIN : &IC_VAULT;
    image(ic, x + 18, y + 18, 255, false);
    text(&F_BOLD, x + 80, y + 22, dlg.l1, TXT);
    text(&F_REG, x + 80, y + 42, dlg.l2, TXT2);
    if (dlg.kind == D_SAVE_NOTE || dlg.kind == D_SAVE_PAINT) {
        int fx = x + 80, fy = y + 68;
        chamfer(fx, fy, 170, 28, 6, 0x6366F1, 255);
        chamfer(fx + 1, fy + 1, 168, 26, 5, 0x0E121D, 255);
        int e = text(&F_REG, fx + 10, fy + 6, dlg.f, TXT);
        if (caret_on) fill(e + 1, fy + 6, 2, 15, 0x5EEAD4);
        text(&F_REG, fx + 178, fy + 6, dlg.kind == D_SAVE_NOTE ? ".TXT" : ".BMP", TXT2);
    }
    int okx, cax, by; dlg_buttons(&okx, &cax, &by);
    if (dlg.cancel) button(cax, by, 88, 28, dlg.cancel, false, false);
    button(okx, by, 88, 28, dlg.ok, true, false);
}
static void dlg_open(int kind, const char *l1, const char *l2, const char *field, const char *ok, const char *cancel) {
    dlg.kind = kind;
    scat(dlg.l1, l1); scat(dlg.l2, l2);
    dlg.flen = 0; dlg.f[0] = 0;
    if (field) { dlg.flen = imin(slen(field), 8); memcpy(dlg.f, field, (size_t)dlg.flen); dlg.f[dlg.flen] = 0; }
    dlg.ok = ok; dlg.cancel = cancel;
    dlg.w = 440; dlg.h = (kind == D_SAVE_NOTE || kind == D_SAVE_PAINT) ? 150 : 124;
    dlg.x = W / 2 - dlg.w / 2; dlg.y = H / 3 - dlg.h / 2;
    damage(dlg.x - 10, dlg.y - 10, dlg.w + 20, dlg.h + 30);
}
static void dlg_close(void) { damage(dlg.x - 10, dlg.y - 10, dlg.w + 20, dlg.h + 30); dlg.kind = D_NONE; }

/* ---- Prism Bar (dock), launcher, desktop icons ---- */
static void layout_dock(void) {
    dock_w = 14 + 48 + 18 + 8 * 54 + 14 + 96;
    dock_x = W / 2 - dock_w / 2; dock_y = H - DOCK_H - 10;
}
static int dock_icon_x(int i) { return dock_x + 14 + 48 + 18 + i * 54; }
static void draw_dock(void) {
    int x = dock_x, y = dock_y, w = dock_w, h = DOCK_H;
    chamfer(x - 4, y + 2, w + 8, h + 8, 16, 0, 60);
    chamfer(x, y, w, h, 14, 0x3A4566, 230);
    chamfer(x + 1, y + 1, w - 2, h - 2, 13, 0x121726, 225);
    int sy1 = cy1; cy1 = imin(cy1, y + 2); chamfer(x, y, w, h, 14, PRISM, 200); cy1 = sy1;
    /* gem button */
    if (launcher_open) chamfer(x + 12, y + 8, 50, 48, 9, 0x6366F1, 110);
    image(&GEM_MED, x + 22, y + 16, 255, false);
    vline(x + 14 + 48 + 8, y + 14, h - 28, LINE);
    int top = top_win();
    for (int i = 0; i < 8; i++) {
        int id = DOCK_APPS[i], ix = dock_icon_x(i);
        image(APPS[id].ic, ix + 3, y + 6, 255, false);
        if (win[id].open) {
            if (id == top) { int sy = cy0; (void)sy; for (int k = 0; k < 18; k++) fill(ix + 18 + k, y + h - 7, 1, 3, prism(k, 18)); }
            else diamond(ix + 27, y + h - 6, 2, win[id].minimized ? 0x5A6380 : TXT2, 255);
        }
    }
    vline(x + w - 96 - 6, y + 14, h - 28, LINE);
    if (clock_h >= 0) {
        char t[6] = {(char)('0' + clock_h / 10), (char)('0' + clock_h % 10), ':',
                     (char)('0' + clock_m / 10), (char)('0' + clock_m % 10), 0};
        text_c(&F_BIG, x + w - 52, y + 9, t, TXT);
        static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char d[16], *p = utoa((u32)date_d, d); p = scat(p, " "); p = scat(p, MON[(date_mo + 11) % 12]); p = scat(p, " "); utoa((u32)date_y, p);
        text_c(&F_REG, x + w - 52, y + 37, d, TXT2);
    }
}
#define LW 372
#define LH 330
static void launcher_rect(int *x, int *y) { *x = dock_x; *y = dock_y - LH - 12; }
static void draw_launcher(void) {
    if (!launcher_open) return;
    int x, y; launcher_rect(&x, &y);
    chamfer(x - 6, y + 2, LW + 12, LH + 12, 20, 0, 60);
    chamfer(x - 2, y - 2, LW + 4, LH + 4, 16, PRISM, 255);
    chamfer(x, y, LW, LH, 14, 0x141A2A, 245);
    image(&GEM_SMALL, x + 18, y + 16, 255, false);
    text(&F_BOLD, x + 44, y + 17, "FacetOS", TXT);
    text(&F_REG, x + 106, y + 17, VERSION " Prism", TXT2);
    for (int i = 0; i < NWIN; i++) {
        int ix = x + 20 + (i % 3) * 112, iy = y + 50 + (i / 3) * 76;
        image(APPS[i].ic, ix + 32, iy, 255, false);
        text_c(&F_REG, ix + 56, iy + 52, APPS[i].name, TXT);
    }
    hline(x + 16, y + LH - 52, LW - 32, LINE);
    button(x + 20, y + LH - 40, 150, 28, "Restart", false, false);
    button(x + LW - 170, y + LH - 40, 150, 28, "Shut down", true, false);
}
static void desk_icon_pos(int i, int *x, int *y) { *x = 22; *y = 22 + i * 92; }
static void draw_desk_icons(void) {
    for (int i = 0; i < 3; i++) {
        int x, y; desk_icon_pos(i, &x, &y);
        int id = DESK_ICONS[i];
        if (dsel == i) chamfer(x - 4, y - 6, 88, 84, 10, 0x6366F1, 80);
        image(APPS[id].ic, x + 16, y, 255, false);
        int tw = text_w(&F_BOLD, APPS[id].name);
        text(&F_BOLD, x + 40 - tw / 2 + 1, y + 56, APPS[id].name, 0x000000);
        text(&F_BOLD, x + 40 - tw / 2, y + 55, APPS[id].name, 0xFFFFFF);
    }
}

/* ---- compose a region of the screen ---- */
static void compose(int x, int y, int w, int h) {
    clip_set(x, y, w, h);
    if (cx0 >= cx1 || cy0 >= cy1) return;
    for (int j = cy0; j < cy1; j++) copy32(&bb[j * MAXW + cx0], &bgbuf[j * MAXW + cx0], (u32)(cx1 - cx0));
    if (cx0 < 120) draw_desk_icons();
    int top = top_win();
    for (int i = 0; i < nz; i++) {
        struct Win *wd = &win[zord[i]];
        if (wd->minimized) continue;
        if (wd->x - 8 < cx1 && wd->x + wd->w + 8 > cx0 && wd->y - 8 < cy1 && wd->y + wd->h + 14 > cy0)
            draw_window(zord[i], zord[i] == top && !dlg.kind && !launcher_open);
    }
    if (cy1 > dock_y - 8) draw_dock();
    draw_launcher();
    draw_dialog();
    int ox = cx0, oy = cy0, ow = cx1 - cx0, oh = cy1 - cy0;
    clip_all();
    present(ox, oy, ow, oh);
}
static void flush(void) {
    if (!dmg) return;
    dmg = false;
    compose(dx0, dy0, dx1 - dx0, dy1 - dy0);
}
static void damage_launcher(void) { int x, y; launcher_rect(&x, &y); damage(x - 8, y - 8, LW + 16, LH + 24); }

/* ====================================================================
 *  Window animations
 * ==================================================================== */
static u32 snap_win[700 * 560], snap_bg[700 * 560];
static void anim_frame_wait(void) { sleep_ms(16); }
static void snap_take(u32 *dst, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= H) { for (int i = 0; i < w; i++) dst[j * w + i] = 0; continue; }
        for (int i = 0; i < w; i++) { int px = x + i; dst[j * w + i] = (px >= 0 && px < W) ? bb[py * MAXW + px] : 0; }
    }
}
static void snap_put(const u32 *src, int x, int y, int w, int h) {
    int i0 = imax(0, -x), i1 = imin(w, W - x);
    if (i1 <= i0) return;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py >= 0 && py < H) copy32(&bb[py * MAXW + x + i0], &src[j * w + i0], (u32)(i1 - i0));
    }
}
/* opening: a band of light sweeps across the new window */
static void anim_open(int id) {
    flush();
    if (!anims_on) return;
    struct Win *w = &win[id];
    int x = w->x, y = w->y, ww = imin(w->w, 700), hh = imin(w->h, 560);
    snap_take(snap_win, x, y, ww, hh);
    int n = 9;
    for (int k = 0; k <= n; k++) {
        snap_put(snap_win, x, y, ww, hh);
        int pos = (ww + hh / 2 + 60) * k / n - 30;
        for (int j = 0; j < hh; j++) {
            int c = pos - j / 2;
            for (int i = imax(0, c - 26); i < imin(ww, c + 26); i++) {
                int px = x + i, py = y + j;
                if (px < 0 || px >= W || py < 0 || py >= H) continue;
                if (!cham_cov(px, py, x, y, ww, hh, CHAMF)) continue;
                int d = i - c, a = 80 - iabs(d) * 80 / 26;
                u32 *p = &bb[py * MAXW + px];
                *p = blend(*p, d < 0 ? 0x5EEAD4 : 0xC4B5FD, (u32)a);
            }
        }
        blit(x, y, ww, hh);
        if (cur_x < x + ww && cur_x + CUR_W > x && cur_y < y + hh && cur_y + CUR_H > y) cursor_draw();
        anim_frame_wait();
    }
    snap_put(snap_win, x, y, ww, hh);
    present(x, y, ww, hh);
}
/* closing: the window shatters into triangles that drift apart and fade */
static void anim_close(int id) {
    struct Win *w = &win[id];
    int x = w->x, y = w->y, ww = imin(w->w, 680), hh = imin(w->h, 520);
    flush();
    snap_take(snap_win, x, y, ww, hh);
    w->minimized = true;
    int mx0 = x - 10, my0 = y - 10, bw = imin(ww + 20, 700), bh = imin(hh + 40, 560);
    compose(mx0, my0, bw, bh);
    w->minimized = false;
    snap_take(snap_bg, mx0, my0, bw, bh);
    const int cols = 5, rows = 4, n = 8;
    for (int k = 1; k <= n; k++) {
        snap_put(snap_bg, mx0, my0, bw, bh);
        u32 alpha = (u32)(255 - 230 * k / n);
        for (int c = 0; c < cols; c++) for (int r = 0; r < rows; r++) for (int half = 0; half < 2; half++) {
            int tx = c * ww / cols, ty = r * hh / rows, tw = (c + 1) * ww / cols - tx, th = (r + 1) * hh / rows - ty;
            int seed = (c * 7 + r * 13 + half * 5) % 11 - 5;
            int ox = ((c * 2 - cols + 1) * 5 + seed) * k * 4 / n, oy = ((r * 2 - rows + 1) * 3 + seed) * k * 4 / n + k * k * 2;
            for (int j = 0; j < th; j += 2) for (int i = 0; i < tw; i += 2) {
                bool in = half ? (i * th >= j * tw) : (i * th < j * tw);
                if (!in) continue;
                int sx = tx + i, sy = ty + j;
                if (!cham_cov(x + sx, y + sy, x, y, ww, hh, CHAMF)) continue;
                u32 src = snap_win[sy * ww + sx];
                int px = x + sx + ox, py = y + sy + oy;
                for (int b = 0; b < 4; b++) {
                    int qx = px + (b & 1), qy = py + (b >> 1);
                    if (qx < mx0 || qx >= mx0 + bw || qy < my0 || qy >= my0 + bh || qx < 0 || qx >= W || qy < 0 || qy >= H) continue;
                    u32 *p = &bb[qy * MAXW + qx];
                    *p = blend(*p, src, alpha);
                }
            }
        }
        blit(mx0, my0, bw, bh);
        if (cur_x < mx0 + bw && cur_x + CUR_W > mx0 && cur_y < my0 + bh && cur_y + CUR_H > my0) cursor_draw();
        anim_frame_wait();
    }
    snap_put(snap_bg, mx0, my0, bw, bh);
    present(mx0, my0, bw, bh);
}
static void anim_minimize(int id, bool restore) {
    if (!anims_on) return;
    flush();
    struct Win *w = &win[id];
    int slot = 0; for (int i = 0; i < 8; i++) if (DOCK_APPS[i] == id) slot = i;
    int tx = dock_icon_x(slot), ty = dock_y + 6, tw = 54, th = 52;
    int n = 9;
    for (int k = 0; k <= n; k++) {
        int t = restore ? n - k : k;
        outline_erase();
        ol_x = w->x + (tx - w->x) * t / n; ol_y = w->y + (ty - w->y) * t / n;
        ol_w = w->w + (tw - w->w) * t / n; ol_h = w->h + (th - w->h) * t / n;
        ol_on = true; outline_draw();
        anim_frame_wait();
    }
    outline_erase(); ol_on = false;
}

/* ====================================================================
 *  Files: save / open Notes (.TXT) and Paint (.BMP) in the Vault
 * ==================================================================== */
static void set_titles(void) {
    char *p = scat(notes_title, "Notes"); if (notes_file[0]) { p = scat(p, "  -  "); scat(p, notes_file); }
    p = scat(paint_title, "Paint"); if (paint_file[0]) { p = scat(p, "  -  "); scat(p, paint_file); }
    win[W_NOTES].title = notes_title; win[W_PAINT].title = paint_title;
    damage_win(W_NOTES); damage_win(W_PAINT);
}
static u32 bmp_encode(void) {
    u32 row = (CW + 3) & ~3u, img = row * CH, off = 54 + 48, size = off + img;
    u8 *b = filebuf;
    memset(b, 0, off);
    b[0] = 'B'; b[1] = 'M'; wr32(b + 2, size); wr32(b + 10, off);
    wr32(b + 14, 40); wr32(b + 18, CW); wr32(b + 22, CH); wr16(b + 26, 1); wr16(b + 28, 8);
    wr32(b + 34, img); wr32(b + 38, 2835); wr32(b + 42, 2835); wr32(b + 46, 12); wr32(b + 50, 12);
    for (int i = 0; i < 12; i++) { b[54 + i * 4] = (u8)PCOL[i]; b[55 + i * 4] = (u8)(PCOL[i] >> 8); b[56 + i * 4] = (u8)(PCOL[i] >> 16); }
    for (int y = 0; y < CH; y++) {
        u8 *r = b + off + (u32)(CH - 1 - y) * row;
        memcpy(r, canvas + y * CW, CW);
        for (u32 i = CW; i < row; i++) r[i] = 0;
    }
    return size;
}
static int nearest(u32 rgb) {
    int best = 0; u32 bd = 0xFFFFFFFF;
    for (int i = 0; i < 12; i++) {
        int dr = (int)((rgb >> 16) & 255) - (int)((PCOL[i] >> 16) & 255);
        int dg = (int)((rgb >> 8) & 255) - (int)((PCOL[i] >> 8) & 255);
        int db = (int)(rgb & 255) - (int)(PCOL[i] & 255);
        u32 d = (u32)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static bool bmp_decode(u32 size) {
    u8 *b = filebuf;
    if (size < 54 || b[0] != 'B' || b[1] != 'M' || rd32(b + 30) != 0) return false;
    u32 off = rd32(b + 10), hs = rd32(b + 14), bpp = rd16(b + 28);
    i32 w = (i32)rd32(b + 18), h = (i32)rd32(b + 22);
    bool topdown = h < 0; if (topdown) h = -h;
    if (w <= 0 || h <= 0 || (bpp != 8 && bpp != 24)) return false;
    u32 row = bpp == 8 ? ((u32)w + 3) & ~3u : ((u32)w * 3 + 3) & ~3u;
    if (off + row * (u32)h > size) return false;
    u8 map[256];
    if (bpp == 8) {
        u32 n = rd32(b + 46); if (!n || n > 256) n = 256;
        for (u32 i = 0; i < n; i++) { u8 *p = b + 14 + hs + i * 4; map[i] = (u8)nearest((u32)p[2] << 16 | (u32)p[1] << 8 | p[0]); }
    }
    memset(canvas, 0, sizeof canvas);
    for (int y = 0; y < CH && y < h; y++) {
        u8 *r = b + off + (u32)(topdown ? y : h - 1 - y) * row;
        for (int x = 0; x < CW && x < w; x++)
            canvas[y * CW + x] = bpp == 8 ? map[r[x]] : (u8)nearest((u32)r[x * 3 + 2] << 16 | (u32)r[x * 3 + 1] << 8 | r[x * 3]);
    }
    return true;
}
static void save_file(int kind, const char *name) {
    u8 n83[11]; int r;
    if (kind == D_SAVE_NOTE) { make83(name, "TXT", n83); r = vault_write(n83, (const u8 *)notes, (u32)nlen); }
    else { make83(name, "BMP", n83); u32 sz = bmp_encode(); r = vault_write(n83, filebuf, sz); }
    if (r) { dlg_open(D_INFO, "The Vault is full.", "Delete some files and try again.", 0, "OK", 0); return; }
    char *p = scat(kind == D_SAVE_NOTE ? notes_file : paint_file, name);
    scat(p, kind == D_SAVE_NOTE ? ".TXT" : ".BMP");
    set_titles();
    damage_win(W_VAULT);
    tone(1319); sleep_ms(45); tone(1976); sleep_ms(70); tone(0);
}
static void open_file(int i) {
    u32 sz = vault_read(flist[i].ent, filebuf, sizeof filebuf);
    int n = slen(flist[i].name);
    if (n > 4 && streq(flist[i].name + n - 4, ".TXT")) {
        nlen = 0;
        for (u32 k = 0; k < sz && nlen < (int)sizeof notes - 1; k++) {
            char c = (char)filebuf[k];
            if (c == '\r') continue;
            if (c == '\t') c = ' ';
            if (c == '\n' || (c >= 32 && c < 127)) notes[nlen++] = c;
        }
        scat(notes_file, flist[i].name);
        set_titles(); win_open(W_NOTES); damage_win(W_NOTES);
    } else if (n > 4 && streq(flist[i].name + n - 4, ".BMP")) {
        if (!bmp_decode(sz)) { dlg_open(D_INFO, "Paint can't open this picture.", "Use 8-bit or 24-bit BMP files up to 512 KB.", 0, "OK", 0); return; }
        scat(paint_file, flist[i].name);
        set_titles(); win_open(W_PAINT); damage_win(W_PAINT);
    } else dlg_open(D_INFO, "FacetOS can't open this file.", "Notes opens .TXT files, Paint opens .BMP files.", 0, "OK", 0);
}
static void ask_save(int which, bool force_ask) {
    if (which == W_NOTES) {
        if (notes_file[0] && !force_ask) { char n[9]; int k = 0; while (notes_file[k] != '.') { n[k] = notes_file[k]; k++; } n[k] = 0; save_file(D_SAVE_NOTE, n); return; }
        char def[9] = "NOTE1"; u8 n83[11];
        for (int k = 1; k < 100; k++) { char *p = scat(def, "NOTE"); utoa((u32)k, p); make83(def, "TXT", n83); if (vault_find(n83) < 0) break; }
        dlg_open(D_SAVE_NOTE, "Save this note as:", vault_mode == VAULT_DISK ? "It goes to the Vault on your FacetOS disk." : "Live mode: kept in the Vault until shutdown.", def, "Save", "Cancel");
    } else {
        if (paint_file[0] && !force_ask) { char n[9]; int k = 0; while (paint_file[k] != '.') { n[k] = paint_file[k]; k++; } n[k] = 0; save_file(D_SAVE_PAINT, n); return; }
        char def[9] = "PICTURE1"; u8 n83[11];
        for (int k = 1; k < 10; k++) { char *p = scat(def, "PICTURE"); utoa((u32)k, p); make83(def, "BMP", n83); if (vault_find(n83) < 0) break; }
        dlg_open(D_SAVE_PAINT, "Save this picture as:", vault_mode == VAULT_DISK ? "It goes to the Vault on your FacetOS disk." : "Live mode: kept in the Vault until shutdown.", def, "Save", "Cancel");
    }
}
static void dlg_ok(void) {
    int k = dlg.kind;
    char f[9]; scat(f, dlg.f);
    dlg_close();
    if ((k == D_SAVE_NOTE || k == D_SAVE_PAINT) && f[0]) save_file(k, f);
    if (k == D_DELETE && fsel >= 0) {
        if (streq(notes_file, flist[fsel].name)) { notes_file[0] = 0; set_titles(); }
        if (streq(paint_file, flist[fsel].name)) { paint_file[0] = 0; set_titles(); }
        vault_delete(flist[fsel].ent); fsel = -1; damage_win(W_VAULT);
        tone(300); sleep_ms(90); tone(0);
    }
}

/* ====================================================================
 *  Power
 * ==================================================================== */
static void shutdown(void) {
    flush();
    outw(0x604, 0x2000); outw(0xB004, 0x2000); outw(0x4004, 0x3400);   /* QEMU, Bochs, VirtualBox */
    clip_all();
    fill_a(0, 0, W, H, 0x000000, 170);
    int pw = 440, ph = 130, px = W / 2 - pw / 2, py = H / 2 - ph / 2;
    chamfer(px - 2, py - 2, pw + 4, ph + 4, 14, PRISM, 255);
    chamfer(px, py, pw, ph, 12, 0x141A2A, 255);
    image(&GEM_BIG, px + 14, py + 18, 255, false);
    text(&F_BOLD, px + 124, py + 46, "It's now safe to switch off", TXT);
    text(&F_BOLD, px + 124, py + 64, "your computer.", TXT);
    blit(0, 0, W, H);
    halt();
}
static void restart(void) {
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) ;
    outb(0x64, 0xFE);
    struct __attribute__((packed)) { u16 l; u32 b; } z = {0, 0};
    __asm__ volatile("lidt %0; int $3" :: "m"(z));
    halt();
}

/* ====================================================================
 *  Crash screen
 * ==================================================================== */
static const char *const EXC[20] = {"Divide by zero", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range",
    "Invalid instruction", "No math chip", "Double fault", "Coprocessor", "Invalid TSS", "Missing segment",
    "Stack fault", "General protection fault", "Page fault", "Reserved", "Math error", "Alignment check",
    "Machine check", "SIMD error"};
static void crash(struct Frame *f) {
    clip_all();
    fill_a(0, 0, W, H, 0x000000, 180);
    int pw = 480, ph = 150, px = W / 2 - pw / 2, py = H / 2 - ph / 2;
    chamfer(px - 2, py - 2, pw + 4, ph + 4, 14, 0xF43F5E, 255);
    chamfer(px, py, pw, ph, 12, 0x141A2A, 255);
    image(&GEM_BIG, px + 14, py + 26, 255, true);
    text(&F_BOLD, px + 124, py + 30, "FacetOS ran into a problem.", TXT);
    char s[80], *p = scat(s, f->num < 20 ? EXC[f->num] : "CPU error");
    p = scat(p, " at ");
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) *p++ = hx[(f->eip >> (i * 4)) & 15];
    *p = 0;
    text(&F_REG, px + 124, py + 52, s, TXT2);
    text(&F_REG, px + 124, py + 90, "Press any key to restart.", TXT);
    blit(0, 0, W, H);
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) (void)inb(0x60);
    for (;;) if ((inb(0x64) & 1) && !(inb(0x60) & 0x80)) restart();
}

/* ====================================================================
 *  Mouse
 * ==================================================================== */
static void paint_dot(int x, int y) {
    int r = psize;
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) {
        if (i * i + j * j > r * r + r / 2) continue;
        int a = x + i, b = y + j;
        if (a >= 0 && a < CW && b >= 0 && b < CH) canvas[b * CW + a] = (u8)pcolor;
    }
}
static void paint_line(int x0, int y0, int x1, int y1) {
    int sx0 = x0, sy0 = y0;
    int ddx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1, ddy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = ddx + ddy;
    for (;;) {
        paint_dot(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= ddy) { err += ddy; x0 += sx; }
        if (e2 <= ddx) { err += ddx; y0 += sy; }
    }
    int cx, cy, cw, ch; content_rect(W_PAINT, &cx, &cy, &cw, &ch);
    damage(cx + imin(sx0, x1) - psize - 1, cy + 41 + imin(sy0, y1) - psize - 1,
           iabs(x1 - sx0) + 2 * psize + 3, iabs(y1 - sy0) + 2 * psize + 3);
}
static void run_tbtn(int act) {
    switch (act) {
    case B_NEW: nlen = 0; notes_file[0] = 0; set_titles(); damage_win(W_NOTES); break;
    case B_OPEN: case B_POPEN: win_open(W_VAULT); break;
    case B_SAVE: ask_save(W_NOTES, false); break;
    case B_SAVEAS: ask_save(W_NOTES, true); break;
    case B_PSAVE: ask_save(W_PAINT, false); break;
    case B_CLEAR: memset(canvas, 0, sizeof canvas); paint_file[0] = 0; set_titles(); damage_win(W_PAINT); break;
    case B_VOPEN: if (fsel >= 0) open_file(fsel); break;
    case B_VDELETE:
        if (fsel >= 0) { char l1[64], *p = scat(l1, "Delete \""); p = scat(p, flist[fsel].name); scat(p, "\"?");
                         dlg_open(D_DELETE, l1, "This can't be undone.", 0, "Delete", "Cancel"); }
        break;
    case B_SHUFFLE: puzzle_shuffle(); damage_win(W_PUZZLE); break;
    }
}
static void content_click(int id, int lx, int ly, bool dbl) {
    for (int i = 0; i < NTBTN; i++) if (TBTNS[i].win == id) {
        int x, y, w, h; tbtn_rect(i, &x, &y, &w, &h);
        if (inside(mx, my, x, y, w, h)) {
            if ((TBTNS[i].act == B_VDELETE || TBTNS[i].act == B_VOPEN) && fsel < 0) return;
            pressed_btn = i; damage(x, y, w, h); flush();
            run_tbtn(TBTNS[i].act); return;
        }
    }
    int cx, cy, cw, ch; content_rect(id, &cx, &cy, &cw, &ch);
    if (id == W_VAULT) {
        for (int i = 0; i < imin(nfiles, 12); i++)
            if (inside(lx, ly, 18 + (i % 4) * 112 - 6, 50 + (i / 4) * 84 - 4, 100, 78)) {
                if (dbl && fsel == i) open_file(i);
                else { fsel = i; damage_win(id); }
                return;
            }
        if (fsel >= 0) { fsel = -1; damage_win(id); }
    } else if (id == W_PAINT) {
        if (ly < 41) {
            for (int i = 0; i < 12; i++) if (inside(lx, ly, 8 + i * 19, 10, 20, 20)) { pcolor = i; damage_win(id); }
            for (int i = 0; i < 3; i++) if (inside(lx, ly, 246 + i * 25, 8, 22, 24)) { psize = i * 3 + 1; damage_win(id); }
        } else { painting = true; plx = lx; ply = ly - 41; paint_line(plx, ply, plx, ply); }
    } else if (id == W_CALC) {
        static const char KEYCH[19] = {'C', 'N', '%', '/', '7', '8', '9', '*', '4', '5', '6', '-', '1', '2', '3', '+', '0', '.', '='};
        for (int i = 0; i < 19; i++) {
            int kx, ky, kw, kh; calc_key_rect(i, &kx, &ky, &kw, &kh);
            if (inside(lx, ly, kx, ky, kw, kh)) { calc_key(KEYCH[i]); c_pressed = i; damage_win(id); tone(1500); sleep_ms(4); tone(0); }
        }
    } else if (id == W_PUZZLE) {
        int c = (lx - 12) / 44, r = (ly - 10) / 44;
        if (lx < 12 || ly < 10 || c > 3 || r > 3) return;
        int t = r * 4 + c;
        int nb[4] = {t - 4, t + 4, (c > 0) ? t - 1 : -1, (c < 3) ? t + 1 : -1};
        for (int k = 0; k < 4; k++)
            if (nb[k] >= 0 && nb[k] < 16 && tiles[nb[k]] == 0) {
                tiles[nb[k]] = tiles[t]; tiles[t] = 0; moves++;
                tone(1200); sleep_ms(8); tone(0);
                damage_win(id); break;
            }
    } else if (id == W_SETTINGS) {
        for (int i = 0; i < 4; i++) if (inside(lx, ly, 20 + i * 104, 42, 92, 84) && theme != i) {
            theme = i; make_wallpaper(); damage(0, 0, W, H);
        }
        if (inside(lx, ly, cw - 76, 144, 52, 26)) { anims_on = !anims_on; damage_win(id); }
    }
}
static void mouse_down(void) {
    bool dbl = (ms_now - last_click_ms < 500) && iabs(mx - last_click_x) < 5 && iabs(my - last_click_y) < 5;
    last_click_ms = ms_now; last_click_x = mx; last_click_y = my;
    if (dlg.kind) {
        int okx, cax, by; dlg_buttons(&okx, &cax, &by);
        if (inside(mx, my, okx, by, 88, 28)) dlg_ok();
        else if (dlg.cancel && inside(mx, my, cax, by, 88, 28)) dlg_close();
        else if (!inside(mx, my, dlg.x, dlg.y, dlg.w, dlg.h)) beep();
        return;
    }
    if (launcher_open) {
        int lx, ly; launcher_rect(&lx, &ly);
        if (inside(mx, my, lx, ly, LW, LH)) {
            for (int i = 0; i < NWIN; i++) {
                int ix = lx + 20 + (i % 3) * 112, iy = ly + 50 + (i / 3) * 76;
                if (inside(mx, my, ix, iy - 4, 112, 72)) { launcher_open = false; damage_launcher(); damage_dock(); flush(); win_open(i); return; }
            }
            if (inside(mx, my, lx + 20, ly + LH - 40, 150, 28)) restart();
            if (inside(mx, my, lx + LW - 170, ly + LH - 40, 150, 28)) shutdown();
            return;
        }
        launcher_open = false; damage_launcher(); damage_dock();
        if (inside(mx, my, dock_x + 12, dock_y + 6, 50, 52)) return;
    }
    if (inside(mx, my, dock_x, dock_y, dock_w, DOCK_H)) {
        if (inside(mx, my, dock_x + 12, dock_y + 6, 50, 52)) { launcher_open = true; damage_launcher(); damage_dock(); tone(1760); sleep_ms(20); tone(0); return; }
        for (int i = 0; i < 8; i++) if (inside(mx, my, dock_icon_x(i), dock_y + 4, 54, 56)) {
            int id = DOCK_APPS[i];
            if (win[id].open && !win[id].minimized && top_win() == id) win_minimize(id);
            else win_open(id);
            return;
        }
        return;
    }
    int id = win_at(mx, my);
    if (id >= 0) {
        win_front(id);
        struct Win *w = &win[id];
        if (my < w->y + TB) {
            if (iabs(mx - (w->x + w->w - 22)) + iabs(my - (w->y + 17)) <= 9) win_close(id);
            else if (iabs(mx - (w->x + w->w - 44)) + iabs(my - (w->y + 17)) <= 9) win_minimize(id);
            else {
                drag_win = id; drag_dx = mx - w->x; drag_dy = my - w->y;
                ol_x = w->x; ol_y = w->y; ol_w = w->w; ol_h = w->h; ol_on = true;
                flush(); outline_draw();
            }
        } else {
            int cx, cy, cw, ch; content_rect(id, &cx, &cy, &cw, &ch);
            content_click(id, mx - cx, my - cy, dbl);
        }
        return;
    }
    for (int i = 0; i < 3; i++) {
        int x, y; desk_icon_pos(i, &x, &y);
        if (inside(mx, my, x, y - 6, 80, 80)) {
            if (dbl && dsel == i) win_open(DESK_ICONS[i]);
            else { dsel = i; damage(0, 0, 130, 300); }
            return;
        }
    }
    if (dsel >= 0) { dsel = -1; damage(0, 0, 130, 300); }
}
static void mouse_move(void) {
    if (drag_win >= 0 && mbtn) {
        struct Win *w = &win[drag_win];
        int nx = imax(60 - w->w, imin(W - 60, mx - drag_dx)), ny = imax(0, imin(H - TB - 20, my - drag_dy));
        if (nx != ol_x || ny != ol_y) { outline_erase(); ol_x = nx; ol_y = ny; outline_draw(); }
    }
    if (painting && mbtn) {
        int cx, cy, cw, ch; content_rect(W_PAINT, &cx, &cy, &cw, &ch);
        int nx = mx - cx, ny = my - cy - 41;
        paint_line(plx, ply, nx, ny);
        plx = nx; ply = ny;
    }
}
static void mouse_up(void) {
    if (drag_win >= 0) {
        outline_erase(); ol_on = false;
        struct Win *w = &win[drag_win];
        if (w->x != ol_x || w->y != ol_y) { damage_win(drag_win); w->x = ol_x; w->y = ol_y; damage_win(drag_win); }
        drag_win = -1;
    }
    if (c_pressed >= 0) { c_pressed = -1; damage_win(W_CALC); }
    if (pressed_btn >= 0) { int x, y, w, h; tbtn_rect(pressed_btn, &x, &y, &w, &h); pressed_btn = -1; damage(x, y, w, h); }
    painting = false;
}

/* ====================================================================
 *  Keyboard
 * ==================================================================== */
static void ps2_wait_w(void) { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return; }
static void ps2_wait_r(void) { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return; }
static void mouse_cmd(u8 v) { ps2_wait_w(); outb(0x64, 0xD4); ps2_wait_w(); outb(0x60, v); ps2_wait_r(); (void)inb(0x60); }
static void ps2_flush(void) { for (int i = 0; i < 64 && (inb(0x64) & 1); i++) (void)inb(0x60); }
static void ps2_init(void) {
    ps2_flush();
    ps2_wait_w(); outb(0x64, 0xA8);
    ps2_wait_w(); outb(0x64, 0x20); ps2_wait_r();
    u8 cfg = inb(0x60);
    cfg &= (u8)~0x30; cfg |= 0x03;
    ps2_wait_w(); outb(0x64, 0x60); ps2_wait_w(); outb(0x60, cfg);
    mouse_cmd(0xF6); mouse_cmd(0xF4);
    ps2_flush();
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
static bool shift, caps, ext;
static void on_key(u8 sc) {
    if (sc == 0xE0) { ext = true; return; }
    bool was_ext = ext; ext = false;
    if (sc == 0x2A || sc == 0x36) { shift = true; return; }
    if (sc == 0xAA || sc == 0xB6) { shift = false; return; }
    if (sc == 0x3A) { caps = !caps; return; }
    if (sc & 0x80) return;
    char c = 0;
    if (sc == 0x4A) c = '-';
    else if (sc == 0x4E) c = '+';
    else if (was_ext && sc == 0x35) c = '/';
    else if (was_ext && sc == 0x1C) c = '\n';
    else if (sc < 58) c = shift ? KMAP_S[sc] : KMAP[sc];
    if (caps && c >= 'a' && c <= 'z') c -= 32; else if (caps && c >= 'A' && c <= 'Z') c += 32;
    if (!c || c == '\t') return;
    if (dlg.kind) {
        if (c == 27) { if (dlg.cancel) dlg_close(); else dlg_ok(); }
        else if (c == '\n') dlg_ok();
        else if (dlg.kind == D_SAVE_NOTE || dlg.kind == D_SAVE_PAINT) {
            if (c == '\b') { if (dlg.flen) dlg.f[--dlg.flen] = 0; }
            else {
                if (c >= 'a' && c <= 'z') c -= 32;
                if (((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') && dlg.flen < 8) { dlg.f[dlg.flen++] = c; dlg.f[dlg.flen] = 0; }
                else beep();
            }
            caret_on = true; caret_ms = ms_now;
            damage(dlg.x, dlg.y, dlg.w, dlg.h);
        }
        return;
    }
    if (launcher_open) { if (c == 27) { launcher_open = false; damage_launcher(); damage_dock(); } return; }
    int t = top_win();
    if (t == W_CALC) {
        char k = c;
        if (c == '\n') k = '='; else if (c == '\b') k = 'B'; else if (c == 27 || c == 'c' || c == 'C') k = 'C';
        else if (c == ',') k = '.';
        if ((k >= '0' && k <= '9') || k == '.' || k == '+' || k == '-' || k == '*' || k == '/' || k == '=' || k == 'B' || k == 'C' || k == '%') {
            calc_key(k); damage_win(W_CALC);
        }
        return;
    }
    if (t != W_NOTES || c == 27) return;
    if (c == '\b') { if (nlen) nlen--; }
    else if (nlen < (int)sizeof notes - 1) notes[nlen++] = c;
    caret_on = true; caret_ms = ms_now;
    damage_win(W_NOTES);
}

/* ====================================================================
 *  Boot animation: a beam of light hits the gem and splits into a spectrum
 * ==================================================================== */
static void beam_line(int x0, int y0, int x1, int y1, u32 col, int width, u32 alpha) {
    if (x1 == x0) return;
    int sx = x0 < x1 ? 1 : -1;
    for (int x = x0; x != x1; x += sx) {
        int yc = y0 + (y1 - y0) * (x - x0) / (x1 - x0);
        for (int d = -width; d <= width; d++) {
            int a = (int)alpha * (width - iabs(d)) / width;
            if (a > 0) pblend(x, yc + d, col, (u32)(a * a / 255));
        }
    }
}
static void boot_animation(void) {
    clip_all();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) bgbuf[y * MAXW + x] = mix(0x0E1222, 0x04050A, y, H);
    memcpy(bb, bgbuf, sizeof(u32) * MAXW * (u32)H);
    blit(0, 0, W, H);
    int gx = W / 2, gy = H / 2 - 70;
    /* 1. a white beam travels in from the left */
    for (int k = 1; k <= 14; k++) {
        int xe = gx * k / 14;
        memcpy(&bb[(gy - 20) * MAXW], &bgbuf[(gy - 20) * MAXW], sizeof(u32) * MAXW * 40);
        beam_line(0, gy + 10, xe, gy, 0xFFFFFF, 3, 255);
        beam_line(0, gy + 10, xe, gy, 0x9FEFFF, 10, 70);
        blit(0, gy - 20, W, 40);
        sleep_ms(28);
    }
    /* 2. the gem lights up and the light splits into a spectrum */
    static const u32 chime[5] = {1047, 1319, 1568, 2093, 2637};
    static const u32 spec[6] = {0x2DD4BF, 0x38BDF8, 0x3B82F6, 0x6366F1, 0x8B5CF6, 0xEC4899};
    for (int k = 1; k <= 16; k++) {
        memcpy(bb, bgbuf, sizeof(u32) * MAXW * (u32)H);
        beam_line(0, gy + 10, gx, gy, 0xFFFFFF, 3, 255 - (u32)k * 8);
        beam_line(0, gy + 10, gx, gy, 0x9FEFFF, 10, 70);
        int len = (W - gx) * k / 16;
        for (int b = 0; b < 6; b++) {
            int ye = gy + (b - 2) * (H / 10) * k / 16 - 30;
            beam_line(gx, gy, gx + len, ye, spec[b], 5, 200);
        }
        image(&GEM_BIG, gx - 48, gy - 48, (u32)imin(255, k * 32), false);
        blit(0, 0, W, H);
        if (k <= 5) tone(chime[k - 1]);
        sleep_ms(k <= 5 ? 90 : 45);
    }
    tone(0);
    /* 3. beams fade, the name appears, facets light up as each part starts */
    const char *msg[10] = {"Starting interrupts", "Keyboard and mouse", "Checking memory", "Reading the clock",
                           "Warming up the speaker", "Looking for storage", "Notes", "Paint", "Calculator", "Ready"};
    int wy = gy + 66, py = wy + 66, n = 10;
    for (int i = 0; i <= n; i++) {
        if (i == 2) cpu_info();
        if (i == 3) read_clock();
        if (i == 5) { ata_init(); pci_scan(); vault_mount(); vault_list(); }
        memcpy(bb, bgbuf, sizeof(u32) * MAXW * (u32)H);
        if (i < 4) for (int b = 0; b < 6; b++) beam_line(gx, gy, W, gy + (b - 2) * (H / 10) - 30, spec[b], 5, (u32)(200 - i * 50));
        image(&GEM_BIG, gx - 48, gy - 48, 255, false);
        image(&WORDMARK, gx - WORDMARK.w / 2, wy, (u32)imin(255, 60 + i * 50), false);
        for (int d = 0; d < n; d++) {
            int dx = gx - (n - 1) * 13 + d * 26;
            if (d < i) diamond(dx, py, 7, prism(d, n), 255);
            else { diamond(dx, py, 7, 0x2A3148, 255); diamond(dx, py, 5, 0x0E1222, 255); }
        }
        const char *m = i < n ? msg[i] : (vault_mode == VAULT_DISK ? "FacetOS disk found - welcome back" : "Welcome to FacetOS");
        text_c(&F_REG, gx, py + 22, m, TXT2);
        blit(0, 0, W, H);
        sleep_ms(i < n ? 170 : 800);
    }
}

/* ====================================================================
 *  Main
 * ==================================================================== */
static void setup_windows(void) {
    win[W_SYSTEM]   = (struct Win){"System", W / 2 - 260, 50, 520, 430, false, false};
    win[W_VAULT]    = (struct Win){"Vault", 150, 90, 470, 340, false, false};
    win[W_NOTES]    = (struct Win){notes_title, 470, 70, 380, 330, false, false};
    win[W_PAINT]    = (struct Win){paint_title, 130, 110, CW + 2, TB + 1 + 41 + CH + CHAMF + 1, false, false};
    win[W_CALC]     = (struct Win){"Calculator", 660, 100, 238, TB + 1 + 302 + CHAMF, false, false};
    win[W_PUZZLE]   = (struct Win){"Puzzle", 600, 250, 204, TB + 1 + 222 + CHAMF, false, false};
    win[W_SETTINGS] = (struct Win){"Settings", 260, 140, 460, 290, false, false};
    win[W_README]   = (struct Win){"Read Me", 230, 80, 480, 360, false, false};
    win[W_BIN]      = (struct Win){"Bin", W / 2 - 180, 220, 360, 140, false, false};
    const char *hello = "Welcome to FacetOS Prism!\nType here, then press Save.";
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
    interrupts_init();
    ps2_init();
    sti();
    boot_animation();
    rng ^= ms_now * 2654435761u;
    setup_windows();
    layout_dock();
    make_wallpaper();
    cur_x = mx = W / 2; cur_y = my = H / 2;
    dmg = false;
    compose(0, 0, W, H);
    cursor_draw();
    win_open(W_README);

    u8 pkt[3]; int pi = 0;
    u32 last_clock = 0;
    for (;;) {
        int moved = 0;
        while (ev_tail != ev_head) {
            u16 ev = evq[ev_tail]; ev_tail = (ev_tail + 1) & 1023;
            u8 d = (u8)ev;
            if (ev & 0x100) {
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
        bool caret_view = top_win() == W_NOTES || dlg.kind == D_SAVE_NOTE || dlg.kind == D_SAVE_PAINT;
        if (caret_view && ms_now - caret_ms > 530) {
            caret_on = !caret_on; caret_ms = ms_now;
            if (dlg.kind) damage(dlg.x + 70, dlg.y + 60, 220, 40);
            else { int cx, cy, cw, ch; content_rect(W_NOTES, &cx, &cy, &cw, &ch); damage(cx, cy + 37, cw, ch - 37); }
        }
        if (ms_now - last_clock > 1000) {
            last_clock = ms_now;
            int oh = clock_h, om = clock_m;
            read_clock();
            if (oh != clock_h || om != clock_m) damage(dock_x + dock_w - 110, dock_y, 110, DOCK_H);
        }
        if (ev_tail == ev_head) flush();
        cli();
        if (ev_tail == ev_head) __asm__ volatile("sti; hlt"); else sti();
    }
}
