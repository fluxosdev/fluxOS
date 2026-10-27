/*
 * The panic screen: drawn on the framebuffer when the kernel stops, over
 * whatever was there, the desktop included. It says in words what
 * happened, then gives the details for whoever has to fix it: the panic
 * message, the processor and process, and the end of the kernel log, which
 * holds panic()'s call trace and, for an exception, the registers.
 *
 * Without a framebuffer the console's text is all there is.
 */
#include "kernel.h"
#include "drivers.h"
#include "proc.h"
#include "smp.h"
#include "../../user/desktop/shell/font.h"
#include <flux/font.h>
#include <flux/logo.h>

#define LOGO       64                   /* the logo, at half its size */
#define LINE_GAP   4                    /* between lines of wrapped text */
#define PAD        14                   /* inside the details panel */

#define BG         0x15171Cu
#define PANEL      0x1F2229u
#define RULE       0x2E323Bu
#define ACCENT     0xE5484Du
#define TEXT_WHITE 0xF2F2F2u
#define TEXT_GREY  0x9BA1A9u
#define TEXT_MONO  0xC9CDD3u
#define TEXT_DIM   0x7C828Cu

extern const struct pfont font_archivo_title, font_archivo_large, font_archivo_start;

static char logbuf[4096];

static uint32_t mix(uint32_t a, uint32_t b, uint32_t t, uint32_t n)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        int ca = (int)((a >> sh) & 0xFF), cb = (int)((b >> sh) & 0xFF);
        out |= (uint32_t)(ca + (cb - ca) * (int)t / (int)n) << sh;
    }
    return out;
}

static void put(int x, int y, uint32_t rgb)
{
    if (x >= 0 && y >= 0 && (uint32_t)x < fb.width && (uint32_t)y < fb.height)
        fb_pixel((uint32_t)x, (uint32_t)y, fb_pack(rgb));
}

static void fill(int x, int y, int w, int h, uint32_t rgb)
{
    if (w > 0 && h > 0)
        fb_fill((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, fb_pack(rgb));
}

static int text_width(const struct pfont *f, const char *s, int len)
{
    int w = 0;
    for (int i = 0; i < len; i++)
        if (s[i] >= 32 && s[i] < 127)
            w += f->glyphs[s[i] - 32].advance;
    return w;
}

/* len characters of s on the background bg, the text's top at y. */
static void draw_text(const struct pfont *f, int x, int y, const char *s, int len, uint32_t rgb, uint32_t bg)
{
    for (int i = 0; i < len; i++) {
        if (s[i] < 32 || s[i] >= 127)
            continue;
        const struct pglyph *g = &f->glyphs[s[i] - 32];
        for (int r = 0; r < f->height; r++)
            for (int c = 0; c < g->width; c++) {
                uint32_t cov = g->cov[r * g->width + c];
                if (cov)
                    put(x + g->xoff + c, y + r, mix(bg, rgb, cov, 15));
            }
        x += g->advance;
    }
}

/* s broken at spaces into lines no wider than w; returns the y below them. */
static int draw_wrapped(const struct pfont *f, int x, int y, int w, const char *s, uint32_t rgb)
{
    while (*s) {
        int len = 0, fit = 0, width = 0;
        while (s[len]) {
            width += text_width(f, s + len, 1);
            if (width > w)
                break;
            len++;
            if (!s[len] || s[len] == ' ')
                fit = len;
        }
        if (!s[len])
            fit = len;
        else if (!fit)
            fit = MAX(len, 1);         /* one word wider than the line */
        draw_text(f, x, y, s, fit, rgb, BG);
        y += f->height + LINE_GAP;
        s += fit;
        while (*s == ' ')
            s++;
    }
    return y;
}

/* A line of the 8x16 font, cut off at cols characters. */
static void draw_mono(int x, int y, const char *s, int len, int cols, uint32_t rgb)
{
    uint32_t fg = fb_pack(rgb), bg = fb_pack(PANEL);
    for (int i = 0; i < len && i < cols; i++)
        fb_glyph((uint32_t)(x + i * FONT_W), (uint32_t)y, font8x16_glyph((unsigned char)s[i]), FONT_H, fg, bg);
}

/* The logo scaled down 2:1, blended onto the background. */
static void draw_logo(int x, int y)
{
    for (int j = 0; j < LOGO; j++)
        for (int i = 0; i < LOGO; i++) {
            uint32_t sum[3] = { 0, 0, 0 }, alpha = 0;
            for (int k = 0; k < 4; k++) {
                uint32_t p = fluxos_logo[(2 * j + k / 2) * FLUXOS_LOGO_SIZE + 2 * i + k % 2], a = p >> 24;
                alpha += a;
                for (int c = 0; c < 3; c++)
                    sum[c] += ((p >> (16 - 8 * c)) & 0xFF) * a;
            }
            uint32_t rgb = 0;
            for (int c = 0; c < 3; c++) {
                uint32_t b = (BG >> (16 - 8 * c)) & 0xFF;
                rgb |= ((b * (4 * 255 - alpha) + sum[c]) / (4 * 255)) << (16 - 8 * c);
            }
            put(x + i, y + j, rgb);
        }
}

/*
 * The end of the kernel log, without its colour codes, as lines in place
 * in logbuf; returns how many, their starts and lengths in start[], len[].
 */
static int log_lines(int max, char **start, int *len)
{
    int n = klog_read(logbuf, sizeof(logbuf)), out = 0;
    for (int i = 0; i < n; i++) {
        char c = logbuf[i];
        if (c == 0x1B && i + 1 < n && logbuf[i + 1] == '[') {
            for (i += 2; i < n && !(logbuf[i] >= 0x40 && logbuf[i] <= 0x7E); i++)
                ;
            continue;
        }
        if (c == '\t')
            c = ' ';
        if (c == '\n' || (c >= 32 && c < 127))
            logbuf[out++] = c;
    }
    int at = 0;
    if (n == (int)sizeof(logbuf))       /* the first line is only the end of one */
        while (at < out && logbuf[at++] != '\n')
            ;
    while (out > at && logbuf[out - 1] == '\n')
        out--;

    /* Back from the end, a line at a time. */
    int count = 0;
    while (out > at && count < max) {
        int s = out;
        while (s > at && logbuf[s - 1] != '\n')
            s--;
        count++;
        start[max - count] = logbuf + s;
        len[max - count] = out - s;
        out = s - 1;
    }
    memmove(start, start + max - count, (uint32_t)count * sizeof(start[0]));
    memmove(len, len + max - count, (uint32_t)count * sizeof(len[0]));
    return count;
}

/* Draw the panic screen; 0 if there is no framebuffer to draw it on. */
int panic_screen(const char *msg)
{
    if (!fb_panic())
        return 0;
    int w = (int)fb.width, h = (int)fb.height;
    int m = MAX(24, MIN(w, h) / 12);
    int textw = w - 2 * m;
    char line[160];

    fill(0, 0, w, h, BG);
    fill(0, 0, w, 4, ACCENT);

    /* The logo, and beside it what happened, in words. */
    int y = m;
    draw_logo(m, y);
    int tx = m + LOGO + 20;
    int ty = y + (LOGO - font_archivo_large.height - LINE_GAP - font_archivo_title.height) / 2;
    const char *title = "fluxOS has stopped";
    draw_text(&font_archivo_large, tx, ty, title, (int)strlen(title), TEXT_WHITE, BG);
    ty += font_archivo_large.height + LINE_GAP;
    ty = draw_wrapped(&font_archivo_title, tx, ty, w - m - tx,
                      "Something went wrong inside the kernel, and the computer cannot go on. "
                      "Work that was not saved has been lost.", TEXT_GREY);
    y = MAX(y + LOGO, ty) + 24;

    /* The panic message, behind a bar of the accent colour. */
    int my = y;
    y = draw_wrapped(&font_archivo_start, m + 16, y, textw - 16, msg, TEXT_WHITE) - LINE_GAP;
    fill(m, my, 4, y - my, ACCENT);
    y += 20;

    /* The footer. */
    int fh = font_archivo_title.height;
    int fy = h - m / 2 - fh;
    const char *restart = "Press Enter to restart the computer.";
    draw_text(&font_archivo_title, m, fy, restart, (int)strlen(restart), TEXT_GREY, BG);
    snprintf(line, sizeof(line), "fluxOS %s", FLUXOS_RELEASE);
    draw_text(&font_archivo_title, w - m - text_width(&font_archivo_title, line, (int)strlen(line)), fy, line,
              (int)strlen(line), TEXT_DIM, BG);

    /* The details: where, then as much of the log as fits. */
    int py = y, ph = fy - 16 - py;
    int cols = (textw - 2 * PAD) / FONT_W;
    if (ph < 2 * PAD + 3 * FONT_H || cols < 20)
        return 1;
    fill(m, py, textw, ph, PANEL);
    int lx = m + PAD;
    y = py + PAD;

    struct proc *p = current;
    uint32_t up = ticks / HZ;
    if (p)
        snprintf(line, sizeof(line), "CPU %d, pid %d (%s), up %u:%02u:%02u", this_cpu()->index, p->pid, p->name,
                 up / 3600, up / 60 % 60, up % 60);
    else
        snprintf(line, sizeof(line), "CPU %d, during boot, up %u:%02u:%02u", this_cpu()->index, up / 3600,
                 up / 60 % 60, up % 60);
    draw_mono(lx, y, line, (int)strlen(line), cols, TEXT_MONO);
    y += FONT_H + PAD / 2;
    fill(lx, y, textw - 2 * PAD, 1, RULE);
    y += PAD / 2 + 1;

    char *start[64];
    int len[64];
    int rows = MIN((py + ph - PAD - y) / FONT_H, 64);
    int n = rows > 0 ? log_lines(rows, start, len) : 0;
    for (int i = 0; i < n; i++, y += FONT_H)
        draw_mono(lx, y, start[i], len[i], cols, TEXT_DIM);
    return 1;
}
