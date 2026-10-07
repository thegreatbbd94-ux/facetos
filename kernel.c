/*
 * FacetOS 1.1 - a tiny 32-bit desktop operating system that draws every pixel itself.
 * Classic desktop style, original art. Boots via Multiboot2 (Limine / GRUB) on any x86 PC,
 * including 32-bit-only emulators like v86.
 *
 * New in 1.1: interrupts, an ATA disk driver, a FAT16 file system (save and open files),
 * dialogs and a Calculator.
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

#define VERSION "1.1"

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
static bool ata_probe(u16 io, u16 ctl, int slave) {
    if (inb(io + 7) == 0xFF) return false;      /* nothing on this bus */
    ata_io = io; ata_ctl = ctl;
    outb(ctl, 0x02);                            /* no disk interrupts, we poll */
    outb(io + 6, (u8)(0xA0 | slave << 4)); ata_delay();
    outb(io + 2, 0); outb(io + 3, 0); outb(io + 4, 0); outb(io + 5, 0);
    outb(io + 7, 0xEC);                         /* IDENTIFY */
    if (inb(io + 7) == 0) return false;
    for (u32 i = 0; i < 1000000 && (inb(io + 7) & 0x80); i++) ;
    if (inb(io + 4) || inb(io + 5)) return false;   /* CD drive (ATAPI), not a disk */
    if (!ata_wait(true)) return false;
    u16 id[256]; void *p = id; u32 n = 256;
    __asm__ volatile("rep insw" : "+D"(p), "+c"(n) : "d"(io) : "memory");
    ata_sectors = id[60] | (u32)id[61] << 16;
    ata_slave = slave;
    return ata_sectors > 0;
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
    for (int b = 0; b < 2 && !ata_ok; b++)
        for (int s = 0; s < 2 && !ata_ok; s++)
            ata_ok = ata_probe(ios[b], ctls[b], s);
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
static bool disk_blank(void) {
    for (u32 s = 0; s < 64; s++) {
        if (!ata_read(s, sec)) return false;
        for (int i = 0; i < 512; i++) if (sec[i]) return false;
    }
    return true;
}
static bool fs_format(void) {
    u32 tot = ata_sectors > 1048576 ? 1048576 : ata_sectors;     /* use at most 512 MB */
    if (tot < 8192) return false;
    u32 spc = 1;
    while (tot / spc > 65000) spc *= 2;
    u32 fsz = 1, cl = 0;
    for (int k = 0; k < 4; k++) { cl = (tot - 1 - 2 * fsz - 32) / spc; fsz = ((cl + 2) * 2 + 511) / 512; }
    cl = (tot - 1 - 2 * fsz - 32) / spc;
    if (cl < 4085 || fsz > 256) return false;
    memset(sec, 0, 512);
    sec[0] = 0xEB; sec[1] = 0x3C; sec[2] = 0x90;
    memcpy(sec + 3, "FACETOS ", 8);
    wr16(sec + 11, 512); sec[13] = (u8)spc; wr16(sec + 14, 1); sec[16] = 2; wr16(sec + 17, ROOT_ENTS);
    if (tot < 65536) wr16(sec + 19, tot); else wr32(sec + 32, tot);
    sec[21] = 0xF8; wr16(sec + 22, fsz); wr16(sec + 24, 63); wr16(sec + 26, 255);
    sec[36] = 0x80; sec[38] = 0x29; wr32(sec + 39, rng ^ ms_now);
    memcpy(sec + 43, "FACETDISK  ", 11); memcpy(sec + 54, "FAT16   ", 8);
    sec[62] = 0xCD; sec[63] = 0x18;            /* boot code: "not bootable, try the next drive" */
    sec[64] = 0xF4; sec[65] = 0xEB; sec[66] = 0xFD;
    sec[510] = 0x55; sec[511] = 0xAA;
    if (!ata_write(0, sec)) return false;
    memset(fatc, 0, sizeof fatc); fatc[0] = 0xFFF8; fatc[1] = 0xFFFF;
    memset(rootc, 0, sizeof rootc);
    memcpy(rootc, "FACETDISK  ", 11); rootc[11] = 0x08;
    fs.spc = (u8)spc; fs.total = tot; fs.fatsz = fsz; fs.fat_lba = 1;
    fs.root_lba = 1 + 2 * fsz; fs.data_lba = fs.root_lba + 32; fs.nclust = cl;
    memset(fat_dirty, 1, fsz); memset(root_dirty, 1, 32);
    fs.ok = true;
    fs_flush();
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
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static bool streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
enum { W_ABOUT, W_HD, W_FILES, W_NOTES, W_PAINT, W_CALC, W_PUZZLE, W_README, W_TRASH, NWIN };
struct Win { const char *title; int x, y, w, h; bool open, shaded; };
static struct Win win[NWIN];
static int zord[NWIN], nz;

struct DIcon { const char *label; const Image *img; int win; int x, y; };
static struct DIcon dicons[] = {
    {"Facet HD", &IC_DISK, W_HD, 0, 0}, {"Facet Disk", &IC_FLOPPY, W_FILES, 0, 0},
    {"Notes", &IC_NOTES, W_NOTES, 0, 0}, {"Paint", &IC_PAINT, W_PAINT, 0, 0},
    {"Calculator", &IC_CALC, W_CALC, 0, 0}, {"Puzzle", &IC_PUZZLE, W_PUZZLE, 0, 0},
    {"Read Me", &IC_README, W_README, 0, 0}, {"Trash", &IC_TRASH, W_TRASH, 0, 0},
};
#define NDICON 8
static int dsel = -1;
static struct DIcon hd_items[] = {
    {"Notes", &IC_NOTES, W_NOTES, 0, 0}, {"Paint", &IC_PAINT, W_PAINT, 0, 0},
    {"Calculator", &IC_CALC, W_CALC, 0, 0}, {"Puzzle", &IC_PUZZLE, W_PUZZLE, 0, 0},
    {"Read Me", &IC_README, W_README, 0, 0}, {"System Info", &IC_COMPUTER, W_ABOUT, 0, 0},
};
#define NHD 6
static int hd_sel = -1, fsel = -1;

enum { A_SEP, A_ABOUT, A_NOTES, A_PAINT, A_CALC, A_PUZZLE, A_README, A_NEWNOTE, A_FILES, A_SAVE, A_SAVEAS,
       A_DELETE, A_CLOSE, A_CLRNOTES, A_CLRCANVAS, A_SHUFFLE, A_T0, A_T1, A_T2, A_T3, A_T4, A_TRASH,
       A_RESTART, A_SHUTDOWN };
struct MItem { const char *label; int act; };
struct Menu { const char *title; int n; struct MItem it[9]; int x, w; };
static struct Menu menus[] = {
    {0, 7, {{"About FacetOS", A_ABOUT}, {"", A_SEP}, {"Calculator", A_CALC}, {"Notes", A_NOTES},
            {"Paint", A_PAINT}, {"Puzzle", A_PUZZLE}, {"Read Me", A_README}}, 0, 0},
    {"File", 8, {{"New Note", A_NEWNOTE}, {"Open Facet Disk", A_FILES}, {"", A_SEP}, {"Save", A_SAVE},
                 {"Save As...", A_SAVEAS}, {"Delete File...", A_DELETE}, {"", A_SEP}, {"Close Window", A_CLOSE}}, 0, 0},
    {"Edit", 3, {{"Clear Notes", A_CLRNOTES}, {"Clear Canvas", A_CLRCANVAS}, {"Shuffle Puzzle", A_SHUFFLE}}, 0, 0},
    {"View", 5, {{"Ocean", A_T0}, {"Sunset", A_T1}, {"Graphite", A_T2}, {"Meadow", A_T3}, {"Classic", A_T4}}, 0, 0},
    {"Special", 4, {{"Empty Trash", A_TRASH}, {"", A_SEP}, {"Restart", A_RESTART}, {"Shut Down", A_SHUTDOWN}}, 0, 0},
};
#define NMENU 5
static int menu_open = -1, menu_hover = -1; static bool menu_sticky;

static int mx, my, mbtn;
static int drag_win = -1, drag_dx, drag_dy;
static u32 last_click_ms; static int last_click_x, last_click_y;
static bool caret_on = true; static u32 caret_ms;
static u8 filebuf[512 * 1024];             /* used when loading/saving files */

/* Notes */
static char notes[4096]; static int nlen;
static char notes_file[13], notes_title[40] = "Notes";
/* Paint */
#define CW 418
#define CH 262
static u8 canvas[CW * CH];
static const u32 PCOL[12] = {0xFFFFFF, 0x1E2230, 0xE63946, 0xFF8C3C, 0xFCC419, 0x2EC464,
                             0x28B4C8, 0x3884FF, 0x8B5CF6, 0xF06EA0, 0x8B5A2B, 0x9AA0AA};
static int pcolor = 1, psize = 1; static bool painting; static int plx, ply;
static char paint_file[13], paint_title[40] = "Paint";
/* Puzzle */
static u8 tiles[16]; static int moves;
/* Calculator: numbers are fixed-point, 4 decimal places */
#define CSCALE 10000
#define CMAX ((i64)999999999999 * CSCALE)
static i64 c_acc, c_cur; static int c_op; static char c_ent[16]; static int c_elen;
static bool c_entering, c_err; static int c_pressed = -1;
static const char *CKEYS[19] = {"C", "+/-", "%", "/", "7", "8", "9", "*", "4", "5", "6", "-",
                                "1", "2", "3", "+", "0", ".", "="};
/* Dialog */
enum { D_NONE, D_SAVE_NOTE, D_SAVE_PAINT, D_DELETE, D_INFO };
static struct { int kind; char l1[64], l2[64]; char f[9]; int flen; const char *ok, *cancel; int x, y, w, h; } dlg;

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
    if (k == 'N') { c_cur = -c_cur; if (c_entering) { c_entering = false; } return; }
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
    *x = 10 + col * 50; *y = 66 + row * 42; *w = 44; *h = 36;
    if (i == 16) *w = 94;                  /* wide 0 */
    if (i == 17) *x = 110;
    if (i == 18) *x = 160;
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
static bool ends_with(const char *s, const char *e) { int a = slen(s), b = slen(e); return a >= b && !memcmp(s + a - b, e, (size_t)b); }
static const Image *file_icon(const char *name) {
    if (ends_with(name, ".TXT")) return &IC_NOTES;
    if (ends_with(name, ".BMP")) return &IC_PAINT;
    return &IC_README;
}

static const char *const README[] = {
    "Welcome to FacetOS " VERSION "!",
    "",
    "FacetOS is a tiny operating system that draws every",
    "single pixel on this screen by itself - no Linux,",
    "no libraries. Just one C file and some pixel art.",
    "",
    "New in 1.1: saving! Use File > Save in Notes or",
    "Paint. Your files live on the Facet Disk.",
    "",
    "Tips:",
    "  - Double-click icons to open them.",
    "  - Drag windows by their striped title bar.",
    "  - Double-click a title bar to roll the window up.",
    "  - The Calculator works with your keyboard too.",
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
    char s[96], *p;
    switch (id) {
    case W_ABOUT: {
        gradient(x, y, w, h, 0xFFFFFF, 0xECEEF2);
        image(&GEM_BIG, x + 18, y + 14, 255, false);
        image(&WORDMARK, x + 110, y + 22, 255, false);
        text(&F_REG, x + 112, y + 70, "Version " VERSION "  \"Saver\"", 0x5A6070);
        p = scat(s, "Built-in memory:  "); p = utoa(mem_mb, p); scat(p, " MB");
        text(&F_REG, x + 20, y + 104, s, 0x1E2230);
        p = scat(s, "Processor:  "); p = scat(p, cpu_vendor); scat(p, " (32-bit)");
        text(&F_REG, x + 20, y + 122, s, 0x1E2230);
        p = scat(s, "Display:  "); p = utoa((u32)W, p); p = scat(p, " x "); p = utoa((u32)H, p); scat(p, ", millions of colors");
        text(&F_REG, x + 20, y + 140, s, 0x1E2230);
        if (fs.ok) { p = scat(s, "Facet Disk:  "); p = utoa(fs.total / 2048, p); p = scat(p, " MB, "); p = utoa((u32)nfiles, p); scat(p, nfiles == 1 ? " file" : " files"); }
        else scat(s, "Facet Disk:  not attached");
        text(&F_REG, x + 20, y + 158, s, 0x1E2230);
        p = scat(s, "Started by:  "); scat(p, loader);
        text(&F_REG, x + 20, y + 176, s, 0x1E2230);
        u32 used = ((u32)_kernel_end - 0x200000u) / (1024 * 1024) + 1;
        text(&F_BOLD, x + 20, y + 202, "FacetOS", 0x1E2230);
        p = utoa(used, s); scat(p, " MB used");
        text(&F_REG, x + w - 20 - text_w(&F_REG, s), y + 202, s, 0x5A6070);
        int bw = w - 40, fw = mem_mb ? (int)(bw * used / mem_mb) : 10;
        fw = imax(fw, 6);
        rrect(x + 20, y + 222, bw, 12, 4, 0xC9CDD6, 255);
        rrect(x + 20, y + 222, fw, 12, 4, ACCENT, 255);
        break; }
    case W_HD: {
        gradient(x, y, w, 20, 0xF4F5F7, 0xDCDFE4);
        hline(x, y + 20, w, 0x9AA0AA);
        text_c(&F_REG, x + w / 2, y + 3, "6 items, built into FacetOS", 0x3A3F4A);
        fill(x, y + 21, w, h - 21, 0xFFFFFF);
        for (int i = 0; i < NHD; i++) {
            hd_items[i].x = x + 10 + (i % 4) * 84; hd_items[i].y = y + 32 + (i / 4) * 68;
            draw_icon(&hd_items[i], hd_items[i].x, hd_items[i].y, hd_sel == i, false);
        }
        break; }
    case W_FILES: {
        gradient(x, y, w, 20, 0xF4F5F7, 0xDCDFE4);
        hline(x, y + 20, w, 0x9AA0AA);
        if (fs.ok) { p = utoa((u32)nfiles, s); p = scat(p, nfiles == 1 ? " file, " : " files, "); p = utoa(free_kb, p); scat(p, " KB available"); }
        else scat(s, "No disk");
        text_c(&F_REG, x + w / 2, y + 3, s, 0x3A3F4A);
        fill(x, y + 21, w, h - 21, 0xFFFFFF);
        if (!fs.ok) {
            image(&IC_FLOPPY, x + 20, y + 40, 120, false);
            text(&F_BOLD, x + 68, y + 40, "No Facet Disk attached.", 0x1E2230);
            text(&F_REG, x + 68, y + 60, "Attach facetos-disk.img as a hard disk", 0x5A6070);
            text(&F_REG, x + 68, y + 78, "and restart FacetOS to save files.", 0x5A6070);
            break;
        }
        if (!nfiles) { text_c(&F_REG, x + w / 2, y + 90, "No files yet. Save something in Notes or Paint!", 0x8A909A); break; }
        int shown = imin(nfiles, 16);
        for (int i = 0; i < shown; i++) {
            struct DIcon ic = {flist[i].name, file_icon(flist[i].name), 0, x + 14 + (i % 4) * 104, y + 30 + (i / 4) * 66};
            draw_icon(&ic, ic.x, ic.y, fsel == i, false);
        }
        if (nfiles > shown) { p = scat(s, "+ "); p = utoa((u32)(nfiles - shown), p); scat(p, " more"); text(&F_REG, x + 8, y + h - 18, s, 0x8A909A); }
        break; }
    case W_NOTES: {
        fill(x, y, w, h, 0xFFF6B8);
        for (int ly = y + 22; ly < y + h; ly += 18) hline(x + 6, ly, w - 12, 0xF0E08A);
        int px = x + 8, py = y + 6, right = x + w - 8;
        for (int i = 0; i <= nlen; i++) {
            if (i == nlen) { if (caret_on && top_win() == W_NOTES && !dlg.kind) fill(px, py + 1, 2, 15, 0x1E2230); break; }
            char ch = notes[i];
            if (ch == '\n') { px = x + 8; py += 18; continue; }
            if (py > y + h) break;
            char str[2] = {ch, 0};
            int cw = text_w(&F_REG, str);
            if (ch != ' ' && (i == 0 || notes[i - 1] == ' ' || notes[i - 1] == '\n')) {   /* word wrap */
                int ww = 0;
                for (int j = i; j < nlen && notes[j] != ' ' && notes[j] != '\n'; j++) { char t2[2] = {notes[j], 0}; ww += text_w(&F_REG, t2); }
                if (px + ww > right && px > x + 8 && ww <= right - x - 8) { px = x + 8; py += 18; }
            }
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
        int ox = x, oy = y + 31;
        int i0 = imax(cx0 - ox, 0), i1 = imin(cx1 - ox, CW), j0 = imax(cy0 - oy, 0), j1 = imin(cy1 - oy, CH);
        for (int j = j0; j < j1; j++) {
            u32 *row = &bb[(oy + j) * MAXW + ox];
            const u8 *src = &canvas[j * CW];
            for (int i = i0; i < i1; i++) row[i] = PCOL[src[i]];
        }
        break; }
    case W_CALC: {
        gradient(x, y, w, h, 0x5C6270, 0x30343E);
        rrect(x + 10, y + 10, w - 20, 46, 6, 0x101418, 255);
        rrect(x + 11, y + 11, w - 22, 44, 5, 0xCFE3C3, 255);
        if (c_err) scat(s, "Error");
        else if (c_entering) scat(s, c_ent);
        else fmt_fixed(c_cur, s);
        const Font *f = text_w(&F_BIG, s) > w - 40 ? &F_BOLD : &F_BIG;
        text(f, x + w - 20 - text_w(f, s), y + (f == &F_BIG ? 20 : 26), s, 0x1C2A1C);
        if (c_op) { char o[2] = {(char)c_op, 0}; text(&F_BOLD, x + 18, y + 14, o, 0x3C5A3C); }
        for (int i = 0; i < 19; i++) {
            int kx, ky, kw, kh; calc_key_rect(i, &kx, &ky, &kw, &kh);
            kx += x; ky += y;
            bool op = (i % 4 == 3 && i < 16) || i == 18, top = i < 3;
            u32 c1 = op ? 0x6B7BEA : top ? 0xC8CCD4 : 0xF6F7F9, c2 = op ? 0x3A48B8 : top ? 0x9DA3AE : 0xD5D8DE;
            if (i == c_pressed) { u32 t = c1; c1 = c2; c2 = t; }
            rrect(kx, ky + 2, kw, kh, 7, 0x000000, 70);
            rrect(kx, ky, kw, kh, 7, 0x1A1D24, 255);
            rrect(kx + 1, ky + 1, kw - 2, kh - 2, 6, c2, 255);
            rrect(kx + 1, ky + 1, kw - 2, kh / 2 + 2, 6, c1, 255);
            rrect(kx + 3, ky + 2, kw - 6, 6, 3, 0xFFFFFF, 50);
            text_c(&F_BOLD, kx + kw / 2, ky + 10, CKEYS[i], op ? 0xFFFFFF : 0x1E2230);
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
        text(&F_REG, x + 70, y + 44, "Deleted files are gone for good.", 0x5A6070);
        break;
    }
}

static void draw_window(int id, bool active) {
    struct Win *wd = &win[id];
    int x = wd->x, y = wd->y, w = wd->w, h = win_h(id);
    fill_a(x + w, y + 4, 4, h, 0, 40); fill_a(x + 4, y + h, w - 4, 4, 0, 40);
    fill_a(x + w, y + 6, 2, h - 2, 0, 30); fill_a(x + 6, y + h, w - 6, 2, 0, 30);
    if (active) {
        gradient(x, y, w, TB, 0xF7F7F9, 0xD5D8DE);
        for (int k = 0; k < 6; k++) {
            hline(x + 6, y + 5 + k * 2, w - 12, 0xFFFFFF);
            hline(x + 6, y + 6 + k * 2, w - 12, 0xA9AEB8);
        }
        bevel(x + 8, y + 5, 13, 13, false);
        bevel(x + w - 21, y + 5, 13, 13, false);
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

/* ---- dialogs ---- */
static void dlg_buttons(int *okx, int *cax, int *by) {
    *by = dlg.y + dlg.h - 36;
    *okx = dlg.x + dlg.w - 100;
    *cax = dlg.cancel ? *okx - 92 : -1000;
}
static void draw_dialog(void) {
    if (!dlg.kind) return;
    int x = dlg.x, y = dlg.y, w = dlg.w, h = dlg.h;
    soft_shadow(x, y, w, h, 10);
    rrect(x, y, w, h, 10, 0x3A3E46, 255);
    rrect(x + 1, y + 1, w - 2, h - 2, 9, 0xF4F5F7, 255);
    const Image *ic = dlg.kind == D_INFO ? &IC_README : dlg.kind == D_DELETE ? &IC_TRASH : &IC_FLOPPY;
    image(ic, x + 18, y + 18, 255, false);
    text(&F_BOLD, x + 66, y + 20, dlg.l1, 0x1E2230);
    text(&F_REG, x + 66, y + 40, dlg.l2, 0x5A6070);
    if (dlg.kind == D_SAVE_NOTE || dlg.kind == D_SAVE_PAINT) {
        int fx = x + 66, fy = y + 64;
        fill(fx, fy, 160, 24, 0xFFFFFF); frame(fx, fy, 160, 24, 0x5A5E66); frame(fx + 1, fy + 1, 158, 22, 0xC8CCD4);
        int e = text(&F_REG, fx + 6, fy + 5, dlg.f, 0x1E2230);
        if (caret_on) fill(e, fy + 5, 2, 15, 0x1E2230);
        text(&F_REG, fx + 168, fy + 5, dlg.kind == D_SAVE_NOTE ? ".TXT" : ".BMP", 0x8A909A);
    }
    int okx, cax, by; dlg_buttons(&okx, &cax, &by);
    if (dlg.cancel) { bevel(cax, by, 80, 24, false); text_c(&F_REG, cax + 40, by + 5, dlg.cancel, 0x1E2230); }
    rrect(okx - 3, by - 3, 86, 30, 9, 0x1E2230, 255);                     /* default button ring */
    rrect(okx, by, 80, 24, 7, dlg.kind == D_DELETE ? 0xD63A44 : ACCENT, 255);
    text_c(&F_BOLD, okx + 40, by + 5, dlg.ok, 0xFFFFFF);
}
static void dlg_open(int kind, const char *l1, const char *l2, const char *field, const char *ok, const char *cancel) {
    dlg.kind = kind;
    scat(dlg.l1, l1); scat(dlg.l2, l2);
    dlg.flen = 0; dlg.f[0] = 0;
    if (field) { dlg.flen = imin(slen(field), 8); memcpy(dlg.f, field, (size_t)dlg.flen); dlg.f[dlg.flen] = 0; }
    dlg.ok = ok; dlg.cancel = cancel;
    dlg.w = 420; dlg.h = (kind == D_SAVE_NOTE || kind == D_SAVE_PAINT) ? 140 : 120;
    dlg.x = W / 2 - dlg.w / 2; dlg.y = H / 3 - dlg.h / 2;
    damage(dlg.x - 6, dlg.y - 6, dlg.w + 14, dlg.h + 14);
}
static void dlg_close(void) { damage(dlg.x - 6, dlg.y - 6, dlg.w + 14, dlg.h + 14); dlg.kind = D_NONE; }

/* ---- menu bar ---- */
static bool item_enabled(int act) {
    int t = top_win();
    if (act == A_SAVE || act == A_SAVEAS) return t == W_NOTES || t == W_PAINT;
    if (act == A_DELETE) return t == W_FILES && fsel >= 0 && fs.ok;
    if (act == A_CLOSE) return t >= 0;
    return true;
}
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
        if (py >= yy && py < yy + ih) return (menus[m].it[i].act == A_SEP || !item_enabled(menus[m].it[i].act)) ? -1 : i;
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
        bool en = item_enabled(it->act), hi = en && (i == menu_hover);
        if (hi) fill(x + 1, yy, w - 2, 20, ACCENT);
        u32 col = hi ? 0xFFFFFF : en ? 0x1E2230 : 0xA4A9B2;
        text(&F_REG, x + 24, yy + 3, it->label, col);
        if (it->act >= A_T0 && it->act <= A_T4 && it->act - A_T0 == theme) {
            for (int k = 0; k < 4; k++) fill(x + 8 + k, yy + 9 + k, 2, 2, col);
            for (int k = 0; k < 7; k++) fill(x + 12 + k, yy + 12 - k, 2, 2, col);
        }
        yy += 20;
    }
}

/* ---- compose a region of the screen ---- */
static void layout_icons(void) {
    for (int i = 0; i < NDICON; i++) {
        dicons[i].x = W - 96; dicons[i].y = MB + 12 + i * 66;
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
            draw_window(zord[i], i == nz - 1 && !dlg.kind);
    }
    draw_dialog();
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
 *  Crash screen (shown if the CPU hits an error)
 * ==================================================================== */
static const char *const EXC[20] = {"Divide by zero", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range",
    "Invalid instruction", "No math chip", "Double fault", "Coprocessor", "Invalid TSS", "Missing segment",
    "Stack fault", "General protection fault", "Page fault", "Reserved", "Math error", "Alignment check",
    "Machine check", "SIMD error"};
static void restart(void);
static void crash(struct Frame *f) {
    clip_all();
    fill_a(0, 0, W, H, 0x000000, 160);
    int pw = 460, ph = 150, px = W / 2 - pw / 2, py = H / 2 - ph / 2;
    rrect(px, py, pw, ph, 10, 0x3A3E46, 255);
    rrect(px + 1, py + 1, pw - 2, ph - 2, 9, 0xF4F5F7, 255);
    image(&GEM_BIG, px + 14, py + 30, 255, true);
    text(&F_BOLD, px + 100, py + 24, "Sorry, FacetOS ran into a problem.", 0x1E2230);
    char s[80], *p = scat(s, f->num < 20 ? EXC[f->num] : "CPU error");
    p = scat(p, " at address ");
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) *p++ = hx[(f->eip >> (i * 4)) & 15];
    *p = 0;
    text(&F_REG, px + 100, py + 46, s, 0x5A6070);
    text(&F_REG, px + 100, py + 80, "Press any key to restart.", 0x1E2230);
    blit(0, 0, W, H);
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) (void)inb(0x60);
    for (;;) if ((inb(0x64) & 1) && !(inb(0x60) & 0x80)) restart();
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
 *  Files: save / open Notes (.TXT) and Paint (.BMP)
 * ==================================================================== */
static void set_titles(void) {
    char *p = scat(notes_title, "Notes"); if (notes_file[0]) { p = scat(p, " - "); scat(p, notes_file); }
    p = scat(paint_title, "Paint"); if (paint_file[0]) { p = scat(p, " - "); scat(p, paint_file); }
    win[W_NOTES].title = notes_title; win[W_PAINT].title = paint_title;
    damage_win(W_NOTES); damage_win(W_PAINT);
}
static void no_disk(void) {
    dlg_open(D_INFO, "No Facet Disk found.", "Attach facetos-disk.img as a hard disk, then restart.", 0, "OK", 0);
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
    u8 n83[11];
    int r;
    if (kind == D_SAVE_NOTE) { make83(name, "TXT", n83); r = fs_write(n83, (const u8 *)notes, (u32)nlen); }
    else { make83(name, "BMP", n83); u32 sz = bmp_encode(); r = fs_write(n83, filebuf, sz); }
    if (r == 1) { no_disk(); return; }
    if (r == 2) { dlg_open(D_INFO, "The Facet Disk is full.", "Delete some files and try again.", 0, "OK", 0); return; }
    char *p = scat(kind == D_SAVE_NOTE ? notes_file : paint_file, name);
    scat(p, kind == D_SAVE_NOTE ? ".TXT" : ".BMP");
    set_titles();
    damage_win(W_FILES);
    tone(880); sleep_ms(50); tone(1320); sleep_ms(70); tone(0);
}
static void open_file(int i) {
    int ent = flist[i].ent;
    u32 sz = fs_read(ent, filebuf, sizeof filebuf);
    if (ends_with(flist[i].name, ".TXT")) {
        nlen = 0;
        for (u32 k = 0; k < sz && nlen < (int)sizeof notes - 1; k++) {
            char c = (char)filebuf[k];
            if (c == '\r') continue;
            if (c == '\t') c = ' ';
            if (c == '\n' || (c >= 32 && c < 127)) notes[nlen++] = c;
        }
        scat(notes_file, flist[i].name);
        set_titles(); win_open(W_NOTES); damage_win(W_NOTES);
    } else if (ends_with(flist[i].name, ".BMP")) {
        if (!bmp_decode(sz)) { dlg_open(D_INFO, "Paint can't open this picture.", "Use 8-bit or 24-bit BMP files up to 512 KB.", 0, "OK", 0); return; }
        scat(paint_file, flist[i].name);
        set_titles(); win_open(W_PAINT); damage_win(W_PAINT);
    } else dlg_open(D_INFO, "FacetOS can't open this file.", "Notes opens .TXT files, Paint opens .BMP files.", 0, "OK", 0);
}
static void ask_save(bool force_ask) {
    int t = top_win();
    if (!fs.ok) { no_disk(); return; }
    if (t == W_NOTES) {
        if (notes_file[0] && !force_ask) { char n[9]; int k = 0; while (notes_file[k] != '.') { n[k] = notes_file[k]; k++; } n[k] = 0; save_file(D_SAVE_NOTE, n); return; }
        char def[9] = "NOTE1"; u8 n83[11];
        for (int k = 1; k < 100; k++) { char *p = scat(def, "NOTE"); utoa((u32)k, p); make83(def, "TXT", n83); if (fs_find(n83) < 0) break; }
        dlg_open(D_SAVE_NOTE, "Save this note as:", "Up to 8 letters or numbers.", def, "Save", "Cancel");
    } else if (t == W_PAINT) {
        if (paint_file[0] && !force_ask) { char n[9]; int k = 0; while (paint_file[k] != '.') { n[k] = paint_file[k]; k++; } n[k] = 0; save_file(D_SAVE_PAINT, n); return; }
        char def[9] = "PICTURE1"; u8 n83[11];
        for (int k = 1; k < 10; k++) { char *p = scat(def, "PICTURE"); utoa((u32)k, p); make83(def, "BMP", n83); if (fs_find(n83) < 0) break; }
        dlg_open(D_SAVE_PAINT, "Save this picture as:", "Up to 8 letters or numbers.", def, "Save", "Cancel");
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
        fs_delete(flist[fsel].ent); fsel = -1; damage_win(W_FILES);
        tone(300); sleep_ms(90); tone(0);
    }
}

/* ====================================================================
 *  Actions
 * ==================================================================== */
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
    struct __attribute__((packed)) { u16 l; u32 b; } z = {0, 0};
    __asm__ volatile("lidt %0; int $3" :: "m"(z));
    halt();
}
static void run_action(int a) {
    switch (a) {
    case A_ABOUT: win_open(W_ABOUT); break;
    case A_NOTES: win_open(W_NOTES); break;
    case A_PAINT: win_open(W_PAINT); break;
    case A_CALC: win_open(W_CALC); break;
    case A_PUZZLE: win_open(W_PUZZLE); break;
    case A_README: win_open(W_README); break;
    case A_NEWNOTE: nlen = 0; notes_file[0] = 0; set_titles(); win_open(W_NOTES); damage_win(W_NOTES); break;
    case A_FILES: win_open(W_FILES); break;
    case A_SAVE: ask_save(false); break;
    case A_SAVEAS: ask_save(true); break;
    case A_DELETE: {
        char l1[64], *p = scat(l1, "Delete \""); p = scat(p, flist[fsel].name); scat(p, "\"?");
        dlg_open(D_DELETE, l1, "This can't be undone.", 0, "Delete", "Cancel");
        break; }
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
    damage(cx + imin(sx0, x1) - psize - 1, cy + 31 + imin(sy0, y1) - psize - 1,
           iabs(x1 - sx0) + 2 * psize + 3, iabs(y1 - sy0) + 2 * psize + 3);
}

static void content_click(int id, int lx, int ly, bool dbl) {
    int cx, cy, cw, ch; content_rect(id, &cx, &cy, &cw, &ch);
    if (id == W_HD) {
        for (int i = 0; i < NHD; i++)
            if (inside(mx, my, hd_items[i].x + 16, hd_items[i].y, 48, 54)) {
                if (dbl && hd_sel == i) { hd_sel = -1; damage_win(id); win_open(hd_items[i].win); }
                else { hd_sel = i; damage_win(id); }
                return;
            }
        if (hd_sel >= 0) { hd_sel = -1; damage_win(id); }
    } else if (id == W_FILES && fs.ok) {
        for (int i = 0; i < imin(nfiles, 16); i++)
            if (inside(lx, ly, 14 + (i % 4) * 104 + 16, 30 + (i / 4) * 66, 48, 54)) {
                if (dbl && fsel == i) open_file(i);
                else { fsel = i; damage_win(id); }
                return;
            }
        if (fsel >= 0) { fsel = -1; damage_win(id); }
    } else if (id == W_PAINT) {
        if (ly < 31) {
            for (int i = 0; i < 12; i++) if (inside(lx, ly, 8 + i * 20, 6, 18, 18)) { pcolor = i; damage_win(id); }
            for (int i = 0; i < 3; i++) if (inside(lx, ly, 258 + i * 26, 5, 22, 20)) { psize = i * 3 + 1; damage_win(id); }
            if (inside(lx, ly, cw - 62, 5, 54, 20)) { memset(canvas, 0, sizeof canvas); damage_win(id); }
        } else {
            painting = true; plx = lx; ply = ly - 31;
            paint_line(plx, ply, plx, ply);
        }
    } else if (id == W_CALC) {
        for (int i = 0; i < 19; i++) {
            int kx, ky, kw, kh; calc_key_rect(i, &kx, &ky, &kw, &kh);
            if (inside(lx, ly, kx, ky, kw, kh)) {
                static const char KEYCH[19] = {'C', 'N', '%', '/', '7', '8', '9', '*', '4', '5', '6', '-', '1', '2', '3', '+', '0', '.', '='};
                calc_key(KEYCH[i]); c_pressed = i; damage_win(id);
                tone(1500); sleep_ms(4); tone(0);
            }
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
    if (dlg.kind) {                                   /* a dialog is open: only its buttons work */
        int okx, cax, by; dlg_buttons(&okx, &cax, &by);
        if (inside(mx, my, okx, by, 80, 24)) dlg_ok();
        else if (dlg.cancel && inside(mx, my, cax, by, 80, 24)) dlg_close();
        else if (!inside(mx, my, dlg.x, dlg.y, dlg.w, dlg.h)) beep();
        return;
    }
    if (menu_open >= 0 && menu_sticky) {
        int it = menu_item_at(menu_open, mx, my), m = menu_open;
        close_menu();
        if (it >= 0) { run_action(menus[m].it[it].act); return; }
        int t2 = menu_title_at(mx, my);
        if (t2 < 0 || t2 == m) return;
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
    if (c_pressed >= 0) { c_pressed = -1; damage_win(W_CALC); }
    painting = false;
}

/* ====================================================================
 *  Keyboard + mouse
 * ==================================================================== */
static void ps2_wait_w(void) { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return; }
static void ps2_wait_r(void) { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return; }
static void mouse_cmd(u8 v) { ps2_wait_w(); outb(0x64, 0xD4); ps2_wait_w(); outb(0x60, v); ps2_wait_r(); (void)inb(0x60); }
static void ps2_flush(void) { for (int i = 0; i < 64 && (inb(0x64) & 1); i++) (void)inb(0x60); }
static void ps2_init(void) {                       /* runs with interrupts off */
    ps2_flush();
    ps2_wait_w(); outb(0x64, 0xA8);
    ps2_wait_w(); outb(0x64, 0x20); ps2_wait_r();
    u8 cfg = inb(0x60);
    cfg &= (u8)~0x30;                              /* keyboard + mouse clocks on */
    cfg |= 0x03;                                   /* keyboard + mouse interrupts on */
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
 *  Boot animation
 * ==================================================================== */
static char disk_msg[64];
static void boot_animation(void) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) bgbuf[y * MAXW + x] = mix(0x2B3550, 0x0B0F1C, y, H);
    clip_all();
    memcpy(bb, bgbuf, sizeof(u32) * MAXW * (u32)H);
    blit(0, 0, W, H);
    int gx = W / 2 - GEM_BIG.w / 2, gy = H / 2 - GEM_BIG.h / 2 - 20;
    static const u32 chime[] = {523, 659, 784, 1047};
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

    int pw = 440, ph = 230, px = W / 2 - pw / 2, py = H / 2 - ph / 2 - 30;
    memcpy(bb, bgbuf, sizeof(u32) * MAXW * (u32)H);
    soft_shadow(px, py, pw, ph, 14);
    rrect(px, py, pw, ph, 14, 0x1E2230, 255);
    rrect(px + 1, py + 1, pw - 2, ph - 2, 13, 0xF6F7F9, 255);
    rrect(px + 1, py + 1, pw - 2, 120, 13, 0xFFFFFF, 255);
    hline(px + 1, py + 120, pw - 2, 0xDDE0E6);
    image(&GEM_BIG, px + 40, py + 24, 255, false);
    image(&WORDMARK, px + 130, py + 38, 255, false);
    text(&F_REG, px + 132, py + 84, "Version " VERSION, 0x6A7080);
    int by = py + 176, bx = px + 50, bw = pw - 100;
    blit(0, 0, W, H);

    const Image *ext_icons[] = {&IC_MOUSE, &IC_KEYBOARD, &IC_COMPUTER, &IC_CLOCK, &IC_SOUND,
                                &IC_FLOPPY, &IC_NOTES, &IC_PAINT, &IC_CALC, &IC_PUZZLE};
    const char *msg[] = {"Starting interrupts...", "Starting keyboard and mouse...", "Checking memory...",
                         "Reading the clock...", "Warming up the speaker...", "Looking for Facet Disk...",
                         "Loading Notes...", "Loading Paint...", "Loading Calculator...", "Almost ready..."};
    int n = 10;
    for (int i = 0; i <= n; i++) {
        const char *m = i < n ? msg[i] : "Welcome to FacetOS";
        if (i == 5) {
            fill(px + 2, by - 32, pw - 4, 20, 0xF6F7F9);
            text_c(&F_REG, W / 2, by - 30, m, 0x3A3F4A); blit(px, py, pw, ph);
            ata_init();
            if (!ata_ok) scat(disk_msg, "No Facet Disk attached");
            else if (fs_mount()) scat(disk_msg, "Facet Disk is ready");
            else if (disk_blank()) {
                fill(px + 2, by - 32, pw - 4, 20, 0xF6F7F9);
                text_c(&F_REG, W / 2, by - 30, "Setting up a new Facet Disk...", 0x3A3F4A); blit(px, py, pw, ph);
                scat(disk_msg, fs_format() ? "New Facet Disk is ready" : "Could not set up the disk");
            } else scat(disk_msg, "Disk is not a Facet Disk - not touching it");
            m = disk_msg;
        }
        if (i == 2) cpu_info();
        if (i == 3) read_clock();
        fill(px + 2, by - 32, pw - 4, 20, 0xF6F7F9);
        text_c(&F_REG, W / 2, by - 30, m, 0x3A3F4A);
        rrect(bx, by, bw, 14, 7, 0x8A909A, 255);
        rrect(bx + 1, by + 1, bw - 2, 12, 6, 0xE3E6EB, 255);
        int fw = (bw - 2) * i / n;
        if (fw > 12) { rrect(bx + 1, by + 1, fw, 12, 6, ACCENT, 255); rrect(bx + 3, by + 2, fw - 4, 4, 2, 0xFFFFFF, 70); }
        if (i < n) image(ext_icons[i], 20 + i * 44, H - 52, 255, false);
        blit(px, py, pw, ph);
        if (i < n) blit(20 + i * 44, H - 52, 32, 32);
        sleep_ms(i == 5 ? 600 : i < n ? 200 : 900);
    }
}

/* ====================================================================
 *  Main
 * ==================================================================== */
static void setup_windows(void) {
    win[W_ABOUT]  = (struct Win){"About FacetOS", W / 2 - 220, MB + 70, 440, 270, false, false};
    win[W_HD]     = (struct Win){"Facet HD", 30, MB + 30, 360, 200, false, false};
    win[W_FILES]  = (struct Win){"Facet Disk", 60, MB + 250, 440, 320, false, false};
    win[W_NOTES]  = (struct Win){notes_title, 420, MB + 60, 320, 260, false, false};
    win[W_PAINT]  = (struct Win){paint_title, 120, MB + 120, CW + 2, TB + 31 + CH + 1, false, false};
    win[W_CALC]   = (struct Win){"Calculator", 560, MB + 120, 222, TB + 282, false, false};
    win[W_PUZZLE] = (struct Win){"Puzzle", 560, MB + 260, 198, TB + 212, false, false};
    win[W_README] = (struct Win){"Read Me", 200, MB + 90, 420, 290, false, false};
    win[W_TRASH]  = (struct Win){"Trash", W / 2 - 150, H / 2 - 40, 300, 104, false, false};
    const char *hello = "Welcome to FacetOS!\nType here, then choose File > Save.";
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
    layout_menus();
    boot_animation();
    rng ^= ms_now * 2654435761u;
    setup_windows();
    layout_icons();
    make_wallpaper();
    win_open(W_HD);
    if (fs.ok) win_open(W_FILES);
    cur_x = mx = W / 2; cur_y = my = H / 2;
    dmg = false;
    compose(0, 0, W, H);
    cursor_draw();

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
            if (dlg.kind) damage(dlg.x + 60, dlg.y + 60, 200, 32);
            else { int cx, cy, cw, ch; content_rect(W_NOTES, &cx, &cy, &cw, &ch); damage(cx, cy, cw, ch); }
        }
        if (ms_now - last_clock > 1000) {
            last_clock = ms_now;
            int oh = clock_h, om = clock_m;
            read_clock();
            if (oh != clock_h || om != clock_m) damage(W - 140, 0, 140, MB);
        }
        if (ev_tail == ev_head) flush();
        cli();
        if (ev_tail == ev_head) __asm__ volatile("sti; hlt"); else sti();
    }
}
