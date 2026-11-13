/* ===========================================================================
   DEAD AIR -- software drawing primitives
   =========================================================================== */
#include "da.h"
#include "font_data.h"

void fb_clear(FB *fb, u32 c) {
    int n = fb->w * fb->h;
    for (int i = 0; i < n; i++) fb->px[i] = c;
}

void fb_px(FB *fb, int x, int y, u32 c) {
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h) return;
    fb->px[y * fb->w + x] = c;
}

void fb_blend_px(FB *fb, int x, int y, u32 c) {
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h) return;
    u32 a = (c >> 24) & 0xff;
    if (a == 0) return;
    u32 *p = &fb->px[y * fb->w + x];
    u32 d = *p;
    if (a == 255) { *p = c & 0x00ffffff; return; }
    int dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
    int sr = (c >> 16) & 0xff, sg = (c >> 8) & 0xff, sb = c & 0xff;
    int ia = 255 - (int)a;
    *p = (u32)(((dr * ia + sr * a) / 255) << 16 |
               ((dg * ia + sg * a) / 255) << 8 |
               ((db * ia + sb * a) / 255));
}

void fb_rect(FB *fb, int x, int y, int w, int h, u32 c) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > fb->w ? fb->w : x + w;
    int y1 = y + h > fb->h ? fb->h : y + h;
    c &= 0x00ffffff;
    for (int yy = y0; yy < y1; yy++) {
        u32 *row = fb->px + yy * fb->w;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

void fb_rect_a(FB *fb, int x, int y, int w, int h, u32 c) {
    if (w <= 0 || h <= 0) return;
    int a = (c >> 24) & 0xff;
    if (a == 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > fb->w ? fb->w : x + w;
    int y1 = y + h > fb->h ? fb->h : y + h;
    int sr = (c >> 16) & 0xff, sg = (c >> 8) & 0xff, sb = c & 0xff;
    int ia = 255 - a;
    for (int yy = y0; yy < y1; yy++) {
        u32 *row = fb->px + yy * fb->w;
        for (int xx = x0; xx < x1; xx++) {
            u32 d = row[xx];
            int dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
            row[xx] = (u32)(((dr * ia + sr * a) / 255) << 16 |
                            ((dg * ia + sg * a) / 255) << 8 |
                            ((db * ia + sb * a) / 255));
        }
    }
}

void fb_frame(FB *fb, int x, int y, int w, int h, u32 c) {
    fb_rect(fb, x, y, w, 1, c);
    fb_rect(fb, x, y + h - 1, w, 1, c);
    fb_rect(fb, x, y, 1, h, c);
    fb_rect(fb, x + w - 1, y, 1, h, c);
}

void fb_vgrad(FB *fb, int x, int y, int w, int h, u32 top, u32 bot) {
    if (h <= 0) return;
    for (int i = 0; i < h; i++) {
        float t = (float)i / (float)h;
        int r = (int)lerpf((float)((top >> 16) & 0xff), (float)((bot >> 16) & 0xff), t);
        int g = (int)lerpf((float)((top >> 8) & 0xff), (float)((bot >> 8) & 0xff), t);
        int b = (int)lerpf((float)(top & 0xff), (float)(bot & 0xff), t);
        fb_rect(fb, x, y + i, w, 1, rgb_(r, g, b));
    }
}

void fb_line(FB *fb, int x0, int y0, int x1, int y1, u32 c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        fb_blend_px(fb, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void fb_disc(FB *fb, int cx, int cy, int r, u32 c) {
    if (r <= 0) return;
    for (int y = -r; y <= r; y++) {
        int span = (int)sqrtf((float)(r * r - y * y));
        fb_rect(fb, cx - span, cy + y, span * 2 + 1, 1, c);
    }
}

void fb_ellipse(FB *fb, int cx, int cy, int rx, int ry, u32 c) {
    if (rx <= 0 || ry <= 0) return;
    for (int y = -ry; y <= ry; y++) {
        float t = 1.0f - (float)(y * y) / (float)(ry * ry);
        if (t <= 0) continue;
        int span = (int)((float)rx * sqrtf(t));
        fb_rect(fb, cx - span, cy + y, span * 2 + 1, 1, c);
    }
}

void fb_ellipse_a(FB *fb, int cx, int cy, int rx, int ry, u32 c) {
    if (rx <= 0 || ry <= 0) return;
    for (int y = -ry; y <= ry; y++) {
        float t = 1.0f - (float)(y * y) / (float)(ry * ry);
        if (t <= 0) continue;
        int span = (int)((float)rx * sqrtf(t));
        fb_rect_a(fb, cx - span, cy + y, span * 2 + 1, 1, c);
    }
}

void fb_ring(FB *fb, int cx, int cy, int r, u32 c) {
    if (r <= 0) return;
    int n = (int)(2.0f * 3.14159265f * (float)r) + 8;
    for (int i = 0; i < n; i++) {
        float a = (float)i / (float)n * 6.2831853f;
        fb_blend_px(fb, cx + (int)(cosf(a) * (float)r), cy + (int)(sinf(a) * (float)r), c);
    }
}

void fb_shade(FB *fb, int x, int y, int w, int h, float mul) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > fb->w ? fb->w : x + w;
    int y1 = y + h > fb->h ? fb->h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        u32 *row = fb->px + yy * fb->w;
        for (int xx = x0; xx < x1; xx++) {
            u32 d = row[xx];
            int r = (int)((float)((d >> 16) & 0xff) * mul);
            int g = (int)((float)((d >> 8) & 0xff) * mul);
            int b = (int)((float)(d & 0xff) * mul);
            row[xx] = (u32)((r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 |
                            (b > 255 ? 255 : b));
        }
    }
}

void fb_scanlines(FB *fb, int alpha) {
    u32 c = argb(alpha, 0, 0, 0);
    for (int y = 0; y < fb->h; y += 2) fb_rect_a(fb, 0, y, fb->w, 1, c);
}

static unsigned char g_vig[FB_W * FB_H];
static int g_vig_ready;

static void vignette_build(void) {
    int cx = FB_W / 2, cy = FB_H / 2;
    float maxd = sqrtf((float)(cx * cx + cy * cy));
    for (int y = 0; y < FB_H; y++)
        for (int x = 0; x < FB_W; x++) {
            float dx = (float)(x - cx), dy = (float)(y - cy);
            float d = sqrtf(dx * dx + dy * dy) / maxd;
            float v = d * d;
            int a = (int)(v * 255.0f);
            if (a > 235) a = 235;
            g_vig[y * FB_W + x] = (unsigned char)a;
        }
    g_vig_ready = 1;
}

void fb_vignette(FB *fb, float strength) {
    if (!g_vig_ready) vignette_build();
    float sc = strength;
    for (int y = 0; y < fb->h; y++) {
        for (int x = 0; x < fb->w; x++) {
            int a = (int)((float)g_vig[y * FB_W + x] * sc);
            if (a <= 1) continue;
            if (a > 235) a = 235;
            u32 *p = &fb->px[y * fb->w + x];
            u32 dc = *p;
            int dr = (dc >> 16) & 0xff, dg = (dc >> 8) & 0xff, db = dc & 0xff;
            int ia = 255 - a;
            *p = (u32)(((dr * ia) / 255) << 16 | ((dg * ia) / 255) << 8 |
                       ((db * ia) / 255));
        }
    }
}

void fb_grain(FB *fb, float amount, u32 *seed) {
    u32 s = *seed;
    int n = fb->w * fb->h;
    int step = amount > 0.10f ? 1 : 2;
    for (int i = 0; i < n; i += step) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        int d = (int)((s >> 24) & 0xff) - 128;
        int add = (int)((float)d * amount);
        u32 c = fb->px[i];
        int r = (int)((c >> 16) & 0xff) + add;
        int g = (int)((c >> 8) & 0xff) + add;
        int b = (int)(c & 0xff) + add;
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        fb->px[i] = (u32)(r << 16 | g << 8 | b);
    }
    *seed = s;
}

void fb_noise_block(FB *fb, int x, int y, int w, int h, float amount, u32 *seed) {
    u32 s = *seed;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= fb->h) continue;
        u32 *row = fb->px + yy * fb->w;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= fb->w) continue;
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            int d = (int)((s >> 24) & 0xff) - 128;
            int add = (int)((float)d * amount);
            u32 c = row[xx];
            int r = (int)((c >> 16) & 0xff) + add;
            int g = (int)((c >> 8) & 0xff) + add;
            int b = (int)(c & 0xff) + add;
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            row[xx] = (u32)(r << 16 | g << 8 | b);
        }
    }
    *seed = s;
}

/* ------------------------------------------------------------------ text  */
static void glyph(FB *fb, int x, int y, int gi, u32 c, int scale) {
    if (gi < 0 || gi >= FONT_COUNT) return;
    const unsigned char *rows = font_glyphs[gi];
    for (int ry = 0; ry < FONT_H; ry++) {
        unsigned char bits = rows[ry];
        if (!bits) continue;
        for (int rx = 0; rx < FONT_W; rx++) {
            if (bits & (1 << (FONT_W - 1 - rx))) {
                if (scale == 1) fb_blend_px(fb, x + rx, y + ry, c);
                else fb_rect_a(fb, x + rx * scale, y + ry * scale, scale, scale, c);
            }
        }
    }
}

int txt_w(const char *s, int scale) { return (int)strlen(s) * FONT_W * scale; }

void txt_draw(FB *fb, int x, int y, const char *s, u32 c, int scale) {
    int cx = x;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; y += FONT_H * scale + scale; continue; }
        glyph(fb, cx, y, (int)(unsigned char)*s - FONT_FIRST, c, scale);
        cx += FONT_W * scale;
    }
}

void txt_draw_glow(FB *fb, int x, int y, const char *s, u32 c, u32 glow, int scale) {
    int a = (glow >> 24) & 0xff;
    u32 g = (glow & 0x00ffffff) | ((u32)(a / 4) << 24);
    for (int oy = -1; oy <= 1; oy++)
        for (int ox = -1; ox <= 1; ox++)
            if (ox || oy) txt_draw(fb, x + ox, y + oy, s, g, scale);
    txt_draw(fb, x, y, s, c, scale);
}

void txt_draw_shadow(FB *fb, int x, int y, const char *s, u32 c, int scale) {
    txt_draw(fb, x + scale, y + scale, s, argb(150, 0, 0, 0), scale);
    txt_draw(fb, x, y, s, c, scale);
}

void txt_center(FB *fb, int cx, int y, const char *s, u32 c, int scale) {
    txt_draw(fb, cx - txt_w(s, scale) / 2, y, s, c, scale);
}

void txt_wrap(FB *fb, int x, int y, int maxchars, const char *s, u32 c, int scale,
              int linegap) {
    char buf[512];
    int n = 0, line = 0, wcount = 0;
    int i = 0;
    while (s[i] && n < 500) {
        /* measure next word */
        int j = i, wl = 0;
        while (s[j] && s[j] != ' ' && s[j] != '\n') { j++; wl++; }
        if (wcount > 0 && line + 1 + wl > maxchars) {
            buf[n++] = '\n'; line = 0; wcount = 0;
        } else if (wcount > 0) {
            buf[n++] = ' '; line++;
        }
        for (int k = i; k < j; k++) { buf[n++] = s[k]; line++; }
        wcount++;
        if (s[j] == '\n') { buf[n++] = '\n'; line = 0; wcount = 0; j++; }
        i = j;
        if (s[i] == ' ') i++;
    }
    buf[n] = 0;
    txt_draw(fb, x, y, buf, c, scale);
    (void)linegap;
}

/* soft radial glow, additive-ish */
void fb_glow(FB *fb, int cx, int cy, int r, u32 col, int intensity) {
    if (r <= 0 || intensity <= 0) return;
    int cr = (col >> 16) & 0xff, cg = (col >> 8) & 0xff, cb = col & 0xff;
    float rf = (float)r;
    for (int y = -r; y <= r; y++) {
        int py = cy + y;
        if (py < 0 || py >= fb->h) continue;
        u32 *row = fb->px + py * fb->w;
        for (int x = -r; x <= r; x++) {
            int px = cx + x;
            if (px < 0 || px >= fb->w) continue;
            float d = sqrtf((float)(x * x + y * y)) / rf;
            if (d >= 1.0f) continue;
            float f = (1.0f - d);
            f = f * f * (float)intensity / 255.0f;
            u32 dc = row[px];
            int dr = (dc >> 16) & 0xff, dg = (dc >> 8) & 0xff, db = dc & 0xff;
            dr = (int)((float)dr + (float)cr * f);
            dg = (int)((float)dg + (float)cg * f);
            db = (int)((float)db + (float)cb * f);
            if (dr > 255) dr = 255;
            if (dg > 255) dg = 255;
            if (db > 255) db = 255;
            row[px] = (u32)(dr << 16 | dg << 8 | db);
        }
    }
}
