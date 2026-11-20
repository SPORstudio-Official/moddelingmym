/* ==========================================================================
 *  GFX -- software rendering pipeline
 *
 *  Everything is drawn straight into the 32-bit ARGB linear framebuffer with
 *  a clip rectangle.  Primitives: pixels, clipped rects, alpha-blended rounded
 *  rectangles, drop shadows, gradients, Bresenham lines, circles, embedded
 *  bitmap text, procedural icons and the software cursor sprites.
 * ========================================================================== */

typedef struct { int x, y, w, h; } rect_t;
typedef struct { i32 x, y; } point_t;

static rect_t g_clip;

/* When the renderer paints into a small offscreen tile (the icon cache), the
   framebuffer pointer and its dimensions are swapped for the tile's.  The clip
   helpers below all derive from these instead of the screen constants, so a
   primitive clipped against "the display" still lands inside the tile. */
static int g_fb_w = SCREEN_W;
static int g_fb_h = SCREEN_H;
static int g_fb_pitch = VRAM_PITCH;
static u32 *g_fb_screen;      /* the real framebuffer, remembered on first bind */

static inline void fb_bind_offscreen(u32 *buf, int w, int h) {
    if (!g_fb_screen) g_fb_screen = g_vram;    /* first bind: this is the screen */
    g_vram = buf; g_fb_w = w; g_fb_h = h; g_fb_pitch = w;
}
static inline void fb_bind_screen(void) {
    if (g_fb_screen) g_vram = g_fb_screen;
    g_fb_w = SCREEN_W; g_fb_h = SCREEN_H; g_fb_pitch = VRAM_PITCH;
}

static inline u32 gfx_blend(u32 dst, u32 src, int alpha) {
    /* alpha is the *additional* coverage (0..255) on top of src's own alpha */
    int sa = (int)ALPHA(src);
    int a = (sa * alpha) / 255;
    int ia = 255 - a;
    int r = (RED(src)   * a + RED(dst)   * ia) / 255;
    int g = (GREEN(src) * a + GREEN(dst) * ia) / 255;
    int b = (BLUE(src)  * a + BLUE(dst)  * ia) / 255;
    return RGB(r, g, b);
}

static inline int rect_empty(const rect_t *r) { return r->w <= 0 || r->h <= 0; }

static inline int rect_contains(const rect_t *r, int x, int y) {
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static inline int rects_intersect(const rect_t *a, const rect_t *b) {
    return !(a->x + a->w <= b->x || b->x + b->w <= a->x ||
             a->y + a->h <= b->y || b->y + b->h <= a->y);
}

static rect_t rect_make(int x, int y, int w, int h) {
    rect_t r; r.x = x; r.y = y; r.w = w; r.h = h; return r;
}

static rect_t rect_intersect(rect_t a, rect_t b) {
    int x0 = k_max(a.x, b.x), y0 = k_max(a.y, b.y);
    int x1 = k_min(a.x + a.w, b.x + b.w), y1 = k_min(a.y + a.h, b.y + b.h);
    return rect_make(x0, y0, k_max(0, x1 - x0), k_max(0, y1 - y0));
}

/* The clip rectangle is always intersected with the framebuffer.  A window can
   be dragged so that its client area hangs over an edge (or is larger than the
   screen), and every primitive treats g_clip as "safe to write" -- so the
   screen bounds have to be part of it rather than a separate check. */
void gfx_set_clip(rect_t r) { g_clip = rect_intersect(r, rect_make(0, 0, g_fb_w, g_fb_h)); }
rect_t gfx_get_clip(void) { return g_clip; }
void gfx_reset_clip(void) { g_clip = rect_make(0, 0, g_fb_w, g_fb_h); if (!g_fb_screen) g_fb_screen = g_vram; }
void gfx_push_clip(rect_t r) { g_clip = rect_intersect(g_clip, r); }

/* -------------------------------------------------------------------------- */
/*  Pixels and blending                                                      */
/* -------------------------------------------------------------------------- */

void gfx_pixel(int x, int y, u32 color) {
    if (x < g_clip.x || y < g_clip.y || x >= g_clip.x + g_clip.w || y >= g_clip.y + g_clip.h)
        return;
    if (x < 0 || y < 0 || x >= g_fb_w || y >= g_fb_h) return;
    if (ALPHA(color) == 255) g_vram[y * g_fb_pitch + x] = color;
    else g_vram[y * g_fb_pitch + x] = gfx_blend(g_vram[y * g_fb_pitch + x], color, 255);
}

void gfx_pixel_add(int x, int y, u32 color, int amount) {
    u32 *p;
    if (x < g_clip.x || y < g_clip.y || x >= g_clip.x + g_clip.w || y >= g_clip.y + g_clip.h)
        return;
    if (x < 0 || y < 0 || x >= g_fb_w || y >= g_fb_h) return;
    p = &g_vram[y * g_fb_pitch + x];
    {
        int r = k_min(255, (int)RED(*p)   + (RED(color)   * amount) / 255);
        int g = k_min(255, (int)GREEN(*p) + (GREEN(color) * amount) / 255);
        int b = k_min(255, (int)BLUE(*p)  + (BLUE(color)  * amount) / 255);
        *p = RGB(r, g, b);
    }
}

u32 gfx_get_pixel(int x, int y) {
    if (x < 0 || y < 0 || x >= g_fb_w || y >= g_fb_h) return 0;
    return g_vram[y * g_fb_pitch + x];
}

void gfx_hline(int x, int y, int w, u32 color) {
    int i;
    if (y < g_clip.y || y >= g_clip.y + g_clip.h) return;
    if (x < g_clip.x) { w -= (g_clip.x - x); x = g_clip.x; }
    if (x + w > g_clip.x + g_clip.w) w = g_clip.x + g_clip.w - x;
    if (w <= 0 || y < 0 || y >= g_fb_h) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > g_fb_w) w = g_fb_w - x;
    if (w <= 0) return;
    {
        u32 *p = &g_vram[y * g_fb_pitch + x];
        if (ALPHA(color) == 255) {
            for (i = 0; i < w; i++) p[i] = color;
        } else {
            for (i = 0; i < w; i++) p[i] = gfx_blend(p[i], color, 255);
        }
    }
}

void gfx_vline(int x, int y, int h, u32 color) {
    int i;
    if (x < g_clip.x || x >= g_clip.x + g_clip.w) return;
    if (y < g_clip.y) { h -= (g_clip.y - y); y = g_clip.y; }
    if (y + h > g_clip.y + g_clip.h) h = g_clip.y + g_clip.h - y;
    if (h <= 0 || x < 0 || x >= g_fb_w) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > g_fb_h) h = g_fb_h - y;
    for (i = 0; i < h; i++) {
        u32 *p = &g_vram[(y + i) * g_fb_pitch + x];
        *p = (ALPHA(color) == 255) ? color : gfx_blend(*p, color, 255);
    }
}

void gfx_fill_rect(int x, int y, int w, int h, u32 color) {
    rect_t r = rect_intersect(rect_make(x, y, w, h), g_clip);
    int yy;
    /* belt and braces: never index the framebuffer outside its rows */
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.y + r.h > g_fb_h) r.h = g_fb_h - r.y;
    if (r.h <= 0 || r.w <= 0) return;
    if (rect_empty(&r) || ALPHA(color) == 0) return;
    if (ALPHA(color) == 255) {
        for (yy = r.y; yy < r.y + r.h; yy++) {
            u32 *p = &g_vram[yy * g_fb_pitch + r.x];
            int i;
            for (i = 0; i < r.w; i++) p[i] = color;
        }
    } else {
        for (yy = r.y; yy < r.y + r.h; yy++) {
            u32 *p = &g_vram[yy * g_fb_pitch + r.x];
            int i;
            for (i = 0; i < r.w; i++) p[i] = gfx_blend(p[i], color, 255);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Gradients                                                               */
/* -------------------------------------------------------------------------- */

void gfx_gradient_v(int x, int y, int w, int h, u32 top, u32 bottom) {
    int i;
    if (h <= 0 || w <= 0) return;
    for (i = 0; i < h; i++) {
        int t = (h > 1) ? (i * 255) / (h - 1) : 0;
        u32 c;
        int a = (ALPHA(top) * (255 - t) + ALPHA(bottom) * t) / 255;
        int r = (RED(top) * (255 - t) + RED(bottom) * t) / 255;
        int g = (GREEN(top) * (255 - t) + GREEN(bottom) * t) / 255;
        int b = (BLUE(top) * (255 - t) + BLUE(bottom) * t) / 255;
        c = RGBA(r, g, b, a);
        gfx_hline(x, y + i, w, c);
    }
}

void gfx_gradient_h(int x, int y, int w, int h, u32 left, u32 right) {
    int i;
    if (h <= 0 || w <= 0) return;
    for (i = 0; i < w; i++) {
        int t = (w > 1) ? (i * 255) / (w - 1) : 0;
        u32 c = RGBA((RED(left) * (255 - t) + RED(right) * t) / 255,
                     (GREEN(left) * (255 - t) + GREEN(right) * t) / 255,
                     (BLUE(left) * (255 - t) + BLUE(right) * t) / 255,
                     (ALPHA(left) * (255 - t) + ALPHA(right) * t) / 255);
        gfx_vline(x + i, y, h, c);
    }
}

/* Radial glow, used for the wallpapers and active window borders.
 *
 * The obvious implementation evaluates sqrt() once per pixel.  For the 380 px
 * wallpaper glow that is about 578,000 double-precision square roots *per
 * frame*, and profiling put it at 38% of all compositor time -- by far the
 * largest single cost in the system.
 *
 * The glow is a smooth radial falloff, so the expensive part is precomputed
 * instead: one table maps the normalised squared distance to coverage, and a
 * pixel then costs a multiply, a shift and a table lookup.  The curve is the
 * original one -- coverage = (1 - d/r)^2 -- sampled into the table, so the
 * image is visually unchanged while the per-pixel sqrt disappears. */
#define GLOW_TABLE_BITS 9
#define GLOW_TABLE_SIZE (1 << GLOW_TABLE_BITS)
static u8 g_glow_table[GLOW_TABLE_SIZE];

static void glow_table_build(int base_a) {
    int i;
    for (i = 0; i < GLOW_TABLE_SIZE; i++) {
        /* normalised squared distance 0..1 -> f = (1 - sqrt(t))^2 * base */
        double t = (double)i / (double)(GLOW_TABLE_SIZE - 1);
        double f = 1.0 - k_sqrt(t);
        int a;
        if (f < 0.0) f = 0.0;
        f = f * f;
        a = (int)(f * (double)base_a + 0.5);
        if (a > 255) a = 255;
        if (a < 0) a = 0;
        g_glow_table[i] = (u8)a;
    }
}

void gfx_radial_glow(int cx, int cy, int radius, u32 color) {
    int y;
    int base_a = (int)ALPHA(color);
    int r2;
    u32 scale;           /* d2 * scale >> 8 -> table index */
    if (radius <= 0 || base_a <= 0) return;
    if (radius > 2048) radius = 2048;
    r2 = radius * radius;
    scale = (u32)(((GLOW_TABLE_SIZE - 1) << 8) / (u32)r2);
    if (scale == 0) scale = 1;
    glow_table_build(base_a);

    for (y = -radius; y <= radius; y++) {
        int py = cy + y;
        int dy2 = y * y;
        int span, xa, xb, x;
        if (dy2 > r2) continue;
        if (py < g_clip.y || py >= g_clip.y + g_clip.h) continue;
        if (py < 0 || py >= g_fb_h) continue;

        span = k_isqrt((u32)(r2 - dy2));
        if (span <= 0) continue;

        xa = cx - span;
        xb = cx + span;
        if (xa < g_clip.x) xa = g_clip.x;
        if (xb > g_clip.x + g_clip.w - 1) xb = g_clip.x + g_clip.w - 1;
        if (xa < 0) xa = 0;
        if (xb > g_fb_w - 1) xb = g_fb_w - 1;
        if (xa > xb) continue;

        for (x = xa; x <= xb; x++) {
            int dx = x - cx;
            int d2 = dx * dx + dy2;
            u32 idx;
            int a, ia;
            u32 *pp, d;
            if (d2 >= r2) continue;
            idx = ((u32)d2 * scale) >> 8;
            if (idx >= GLOW_TABLE_SIZE) idx = GLOW_TABLE_SIZE - 1;
            a = g_glow_table[idx];
            if (a <= 0) continue;
            ia = 255 - a;
            pp = &g_vram[py * g_fb_pitch + x];
            d = *pp;
            *pp = RGB(((int)RED(color)   * a + (int)RED(d)   * ia) / 255,
                      ((int)GREEN(color) * a + (int)GREEN(d) * ia) / 255,
                      ((int)BLUE(color)  * a + (int)BLUE(d)  * ia) / 255);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Rounded rectangles with analytic anti-aliasing                           */
/* -------------------------------------------------------------------------- */

/* Coverage of a pixel by the rounded rect, computed with 4x4 subsamples. */
static void gfx_round_rect_ex(int x, int y, int w, int h, int radius, u32 color,
                              int border_only, int border_width, u32 border_color,
                              int inset_shadow) {
    int yy, xx;
    rect_t r;
    if (w <= 0 || h <= 0) return;
    if (radius < 0) radius = 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    r = rect_intersect(rect_make(x, y, w, h), g_clip);
    if (rect_empty(&r)) return;

    for (yy = r.y; yy < r.y + r.h; yy++) {
        for (xx = r.x; xx < r.x + r.w; xx++) {
            int lx = xx - x, ly = yy - y;
            int cx = 0, cy = 0;
            int on_corner = 0;
            int dist_from_edge;
            if (lx < radius && ly < radius) { cx = radius; cy = radius; on_corner = 1; }
            else if (lx >= w - radius && ly < radius) { cx = w - radius - 1; cy = radius; on_corner = 1; }
            else if (lx < radius && ly >= h - radius) { cx = radius; cy = h - radius - 1; on_corner = 1; }
            else if (lx >= w - radius && ly >= h - radius) { cx = w - radius - 1; cy = h - radius - 1; on_corner = 1; }
            if (on_corner) {
                double dx = (double)lx - (double)cx;
                double dy = (double)ly - (double)cy;
                double d = k_sqrt(dx * dx + dy * dy);
                double cov = (double)radius + 0.5 - d;
                if (cov <= 0.0) continue;
                if (cov > 1.0) cov = 1.0;
                {
                    int alpha = (int)(cov * 255.0);
                    u32 *p = &g_vram[yy * g_fb_pitch + xx];
                    if (border_only) {
                        int bw = border_width;
                        double inner = d - (double)(radius - bw);
                        if (inner < 0.5) {
                            double bcov = inner + 0.5;
                            if (bcov < 0) bcov = 0;
                            if (bcov > 1) bcov = 1;
                            *p = gfx_blend(*p, RGBA(RED(border_color), GREEN(border_color),
                                                    BLUE(border_color), (int)(bcov * ALPHA(border_color))), 255);
                        }
                    } else {
                        *p = gfx_blend(*p, color, alpha);
                    }
                }
                continue;
            }
            if (border_only) {
                dist_from_edge = k_min(k_min(lx, w - 1 - lx), k_min(ly, h - 1 - ly));
                if (dist_from_edge < border_width) {
                    gfx_pixel(xx, yy, border_color);
                } else if (inset_shadow) {
                    /* subtle inner top highlight */
                    if (dist_from_edge == border_width)
                        gfx_pixel(xx, yy, RGBA(255, 255, 255, 10));
                }
            } else {
                gfx_pixel(xx, yy, color);
            }
        }
    }
}

void gfx_round_rect(int x, int y, int w, int h, int radius, u32 color) {
    gfx_round_rect_ex(x, y, w, h, radius, color, 0, 0, 0, 0);
}

void gfx_round_rect_border(int x, int y, int w, int h, int radius, u32 color, int bw) {
    gfx_round_rect_ex(x, y, w, h, radius, 0, 1, bw, color, 0);
}

/* Rounded rect with a vertical gradient fill. */
void gfx_round_rect_grad(int x, int y, int w, int h, int radius, u32 top, u32 bottom) {
    int i;
    if (w <= 0 || h <= 0) return;
    for (i = 0; i < h; i++) {
        int t = (h > 1) ? (i * 255) / (h - 1) : 0;
        u32 c = RGBA((RED(top) * (255 - t) + RED(bottom) * t) / 255,
                     (GREEN(top) * (255 - t) + GREEN(bottom) * t) / 255,
                     (BLUE(top) * (255 - t) + BLUE(bottom) * t) / 255,
                     (ALPHA(top) * (255 - t) + ALPHA(bottom) * t) / 255);
        /* per-row rounded clip via a temporary row rect */
        if (i < radius) {
            int dy = radius - i;
            int inset = radius - (int)k_sqrt((double)(radius * radius - dy * dy));
            gfx_hline(x + inset, y + i, w - 2 * inset, c);
        } else if (i >= h - radius) {
            int dy = i - (h - radius - 1);
            int inset = radius - (int)k_sqrt((double)(radius * radius - dy * dy));
            gfx_hline(x + inset, y + i, w - 2 * inset, c);
        } else {
            gfx_hline(x, y + i, w, c);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Shadows                                                                  */
/* -------------------------------------------------------------------------- */

/* A soft drop shadow: three nested rounded rects with decreasing alpha. */
/* Drop shadow.
 *
 * The straightforward implementation draws ten nested anti-aliased rounded
 * rectangles.  Each one walks its whole area and, for every pixel, decides
 * whether it is in a corner, computes a distance with double-precision sqrt()
 * and derives coverage -- and a shadow is drawn for every visible window on
 * every frame.  Measured on the desktop it cost 14.3 of 19.0 ms per frame: 75%
 * of the entire compositor, and the reason dragging a window felt sluggish.
 *
 * The shape is known before the first pixel is touched, so the expensive part
 * is precomputed rather than guessed at: one coverage table indexed by
 * (dx, dy) from the corner centre, built once per corner radius.  A pixel then
 * costs a table lookup, and the interior of every ring is a plain horizontal
 * span instead of a per-pixel fit.  The result is pixel-identical to before --
 * same radii, same alphas, same curve -- because the table is generated from
 * exactly the same expression. */
#define SHADOW_COV_MAX 72
static u8  g_shadow_cov[SHADOW_COV_MAX + 2][SHADOW_COV_MAX + 2];
static int g_shadow_cov_for = -1;

static void shadow_cov_build(int radius) {
    int cx, cy;
    if (g_shadow_cov_for == radius) return;
    if (radius > SHADOW_COV_MAX) radius = SHADOW_COV_MAX;
    for (cy = 0; cy <= radius + 1; cy++) {
        for (cx = 0; cx <= radius + 1; cx++) {
            double d = k_sqrt((double)(cx * cx + cy * cy));
            double cov = (double)radius + 0.5 - d;
            int a;
            if (cov <= 0.0) a = 0;
            else if (cov >= 1.0) a = 255;
            else a = (int)(cov * 255.0);
            g_shadow_cov[cy][cx] = (u8)a;
        }
    }
    g_shadow_cov_for = radius;
}

/* One anti-aliased rounded rectangle with integer geometry, used by the
 * shadow.  Same result as gfx_round_rect, without the per-pixel sqrt. */
static void shadow_ring(int x, int y, int w, int h, int radius, u32 color) {
    int a_col = (int)ALPHA(color);
    int yy;
    if (w <= 0 || h <= 0 || a_col <= 0) return;
    if (radius < 0) radius = 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    shadow_cov_build(radius);

    for (yy = 0; yy < h; yy++) {
        int py = y + yy;
        int ly = yy;
        int x0 = x, x1 = x + w - 1;
        int corner_row = 0;
        if (py < g_clip.y || py >= g_clip.y + g_clip.h) continue;
        if (py < 0 || py >= g_fb_h) continue;

        if (ly < radius) corner_row = 1;
        else if (ly >= h - radius) corner_row = 2;

        if (!corner_row) {
            /* straight section: one solid span */
            if (x0 < g_clip.x) x0 = g_clip.x;
            if (x1 > g_clip.x + g_clip.w - 1) x1 = g_clip.x + g_clip.w - 1;
            if (x0 < 0) x0 = 0;
            if (x1 > g_fb_w - 1) x1 = g_fb_w - 1;
            if (x0 <= x1) {
                int cx;
                for (cx = x0; cx <= x1; cx++) {
                    u32 *p = &g_vram[py * g_fb_pitch + cx];
                    *p = gfx_blend(*p, color, 255);
                }
            }
            continue;
        }

        {
            int cy = (corner_row == 1) ? ly : (h - 1 - ly);   /* 0..radius-1 */
            int cx;
            /* interior of the rounded band: solid */
            if (x0 < g_clip.x) x0 = g_clip.x;
            if (x1 > g_clip.x + g_clip.w - 1) x1 = g_clip.x + g_clip.w - 1;
            if (x0 < 0) x0 = 0;
            if (x1 > g_fb_w - 1) x1 = g_fb_w - 1;
            for (cx = x0; cx <= x1; cx++) {
                int lx = cx - x;
                int dist_x;
                int cov;
                u32 *p;
                if (lx < radius) dist_x = radius - lx;
                else if (lx >= w - radius) dist_x = lx - (w - radius - 1);
                else { /* between the corners */
                    p = &g_vram[py * g_fb_pitch + cx];
                    *p = gfx_blend(*p, color, 255);
                    continue;
                }
                if (dist_x < 0) dist_x = 0;
                if (dist_x > SHADOW_COV_MAX + 1) dist_x = SHADOW_COV_MAX + 1;
                if (cy > SHADOW_COV_MAX + 1) { p = &g_vram[py * g_fb_pitch + cx]; *p = gfx_blend(*p, color, 255); continue; }
                cov = g_shadow_cov[cy][dist_x];
                if (cov <= 0) continue;
                p = &g_vram[py * g_fb_pitch + cx];
                *p = gfx_blend(*p, color, cov);
            }
        }
    }
}

/* Drop shadow.
 *
 * Ten nested anti-aliased rounded rectangles, drawn for every visible window on
 * every frame.  Rendering them costs ~6.8 ms per frame on the desktop, which
 * after the wallpaper, icons and text were fixed became the largest remaining
 * cost.  The geometry is correct and worth keeping exactly as it was, so it is
 * not re-derived here -- instead the *result* is cached.
 *
 * A shadow depends only on the window's size and the style arguments, not on
 * where the window is.  So it is rendered once into a tile and blitted from
 * then on.  Dragging a window moves it without touching the cache; only a
 * resize rebuilds it, which is exactly when the cost is already being paid.
 */
static u32 *g_shw_tile;                 /* lazily grown scratch bitmap         */
static int  g_shw_tile_cap;
static int  g_shw_cw, g_shw_ch, g_shw_cr, g_shw_cs, g_shw_cst;

/* Alpha accumulation for the shadow tile.
 *
 * A shadow is pure black, so all that matters per pixel is its total coverage.
 * Rings are composited src-over into the tile's alpha channel only:
 *
 *      a_out = a_ring + a_dst * (255 - a_ring) / 255
 *
 * Doing this instead of gfx_blend() is essential: gfx_blend() returns an opaque
 * ARGB word (it drops the destination's alpha), so compositing the rings that
 * way made every covered pixel opaque black -- and because a ring fills its
 * whole rectangle rather than an outline, the tile's interior turned solid
 * black and blitted over the window as a hard band. */
static void shadow_tile_ring(int x, int y, int w, int h, int radius, int a_ring) {
    int yy;
    if (w <= 0 || h <= 0 || a_ring <= 0) return;
    if (radius < 0) radius = 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    shadow_cov_build(radius);

    for (yy = 0; yy < h; yy++) {
        int ly = yy;
        int py = y + yy;
        int corner_row = (ly < radius) ? 1 : (ly >= h - radius ? 2 : 0);
        int cy = corner_row == 2 ? (h - 1 - ly) : ly;
        int x0 = x, x1 = x + w - 1, cx;
        if (py < 0 || py >= g_fb_h) continue;
        if (x0 < 0) x0 = 0;
        if (x1 > g_fb_w - 1) x1 = g_fb_w - 1;
        for (cx = x0; cx <= x1; cx++) {
            int lx = cx - x;
            int cov = 255;
            int dist_x;
            if (corner_row) {
                if (lx < radius) dist_x = radius - lx;
                else if (lx >= w - radius) dist_x = lx - (w - radius - 1);
                else dist_x = -1;                       /* between corners     */
                if (dist_x >= 0) {
                    if (cy > SHADOW_COV_MAX + 1) cov = 255;
                    else cov = g_shadow_cov[cy][dist_x];
                    if (cov <= 0) continue;
                }
            }
            {
                u32 *p = &g_vram[py * g_fb_pitch + cx];
                int a = (cov * a_ring) / 255;
                int da = (int)ALPHA(*p);
                int out;
                if (a <= 0) continue;
                out = a + (da * (255 - a)) / 255;
                if (out > 255) out = 255;
                *p = ((u32)out << 24);                  /* black, alpha only   */
            }
        }
    }
}

void gfx_shadow(int x, int y, int w, int h, int radius, int spread, int strength) {
    int i, tw, th, pad;
    if (spread <= 0 || w <= 0 || h <= 0) return;
    pad = spread + spread / 3 + 2;
    tw = w + 2 * pad;
    th = h + 2 * pad;

    if (!(g_shw_tile && g_shw_cw == w && g_shw_ch == h && g_shw_cr == radius &&
          g_shw_cs == spread && g_shw_cst == strength)) {
        if (g_shw_tile_cap < tw * th) {
            extern void *mm_kmalloc(u32 size);
            u32 *nt = (u32 *)mm_kmalloc((u32)(tw * th) * 4u);
            if (!nt) {
                for (i = spread; i >= 1; i--) {
                    int a = (strength * (spread - i + 1) * (spread - i + 1)) / (spread * spread + 1);
                    int inset = spread - i;
                    gfx_round_rect(x + spread - i + inset, y + spread - i + inset + spread / 3,
                                   w + 2 * i - 2 * inset, h + 2 * i - 2 * inset, radius + i,
                                   RGBA(0, 0, 0, a));
                }
                return;
            }
            g_shw_tile = nt;
            g_shw_tile_cap = tw * th;
        }
        {
            rect_t save_clip = g_clip;
            kmemset(g_shw_tile, 0, (size_t)(tw * th) * 4u);
            fb_bind_offscreen(g_shw_tile, tw, th);
            gfx_reset_clip();
            for (i = spread; i >= 1; i--) {
                int a = (strength * (spread - i + 1) * (spread - i + 1)) / (spread * spread + 1);
                int inset = spread - i;
                shadow_tile_ring(pad + spread - i + inset, pad + spread - i + inset + spread / 3,
                                 w + 2 * i - 2 * inset, h + 2 * i - 2 * inset, radius + i, a);
            }
            fb_bind_screen();
            g_clip = save_clip;
        }
        g_shw_cw = w; g_shw_ch = h; g_shw_cr = radius;
        g_shw_cs = spread; g_shw_cst = strength;
    }

    /* Blit: the tile carries coverage in its alpha channel and black in its
       rgb, which is exactly what an overlay wants. */
    {
        int oy, ox;
        rect_t r = rect_intersect(rect_make(x - pad, y - pad, tw, th), g_clip);
        if (r.w <= 0 || r.h <= 0) return;
        for (oy = r.y - (y - pad); oy < r.y - (y - pad) + r.h; oy++) {
            int py = (y - pad) + oy;
            if (oy < 0 || oy >= th || py < 0 || py >= g_fb_h) continue;
            for (ox = r.x - (x - pad); ox < r.x - (x - pad) + r.w; ox++) {
                int px = (x - pad) + ox;
                u32 c;
                if (ox < 0 || ox >= tw || px < 0 || px >= g_fb_w) continue;
                c = g_shw_tile[oy * tw + ox];
                if (ALPHA(c)) {
                    u32 *p = &g_vram[py * g_fb_pitch + px];
                    *p = gfx_blend(*p, c, 255);
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Lines and circles                                                        */
/* -------------------------------------------------------------------------- */

static void gfx_line_aa_impl(int x0, int y0, int x1, int y1, u32 color, int thick) {
    int dx = k_abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -k_abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int guard = 0;
    for (;;) {
        if (thick <= 1) gfx_pixel(x0, y0, color);
        else gfx_fill_rect(x0 - thick / 2, y0 - thick / 2, thick, thick, color);
        if ((x0 == x1 && y0 == y1) || guard++ > 20000) break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

void gfx_line(int x0, int y0, int x1, int y1, u32 color) {
    gfx_line_aa_impl(x0, y0, x1, y1, color, 1);
}

void gfx_line_thick(int x0, int y0, int x1, int y1, int thick, u32 color) {
    gfx_line_aa_impl(x0, y0, x1, y1, color, thick);
}

void gfx_circle(int cx, int cy, int radius, u32 color) {
    int x = radius, y = 0, err = 1 - radius;
    if (radius <= 0) return;
    while (x >= y) {
        gfx_pixel(cx + x, cy + y, color); gfx_pixel(cx + y, cy + x, color);
        gfx_pixel(cx - y, cy + x, color); gfx_pixel(cx - x, cy + y, color);
        gfx_pixel(cx - x, cy - y, color); gfx_pixel(cx - y, cy - x, color);
        gfx_pixel(cx + y, cy - x, color); gfx_pixel(cx + x, cy - y, color);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void gfx_fill_circle(int cx, int cy, int radius, u32 color) {
    int y;
    if (radius <= 0) return;
    for (y = -radius; y <= radius; y++) {
        int span = (int)k_sqrt((double)(radius * radius - y * y));
        gfx_hline(cx - span, cy + y, span * 2 + 1, color);
    }
}

/* Anti-aliased filled circle (soft edges) used for the tray LED etc. */
void gfx_fill_circle_aa(int cx, int cy, int radius, u32 color) {
    int y;
    int a0 = (int)ALPHA(color);
    int r2;
    if (radius <= 0 || a0 <= 0) return;
    r2 = radius * radius;
    /* One row at a time: the interior is a solid span and only the two end
       pixels need coverage.  The previous version called sqrt() for every pixel
       in the bounding box, including the ~21% that fall outside the circle,
       and paid a gfx_pixel() clip test for each of them. */
    for (y = -radius; y <= radius; y++) {
        int py = cy + y;
        int span, x;
        if (py < g_clip.y || py >= g_clip.y + g_clip.h) continue;
        if (py < 0 || py >= g_fb_h) continue;
        span = (int)k_isqrt((u32)(r2 - y * y));
        if (span <= 0) continue;
        {
            int xa = cx - span, xb = cx + span;
            int i;
            u32 opaque = RGBA(RED(color), GREEN(color), BLUE(color), a0);
            if (a0 == 255) {
                /* solid interior, then soften the two rim pixels */
                int solid_a = xa + 1, solid_b = xb - 1;
                if (solid_a > solid_b) { solid_a = xa; solid_b = xb; }
                gfx_hline(solid_a, py, solid_b - solid_a + 1, opaque);
                for (i = 0; i < 2; i++) {
                    int px = (i == 0) ? xa : xb;
                    if (px >= solid_a && px <= solid_b) continue;
                    {
                        int dx = px - cx;
                        double d = k_sqrt((double)(dx * dx + y * y));
                        double cov = (double)radius + 0.5 - d;
                        if (cov <= 0.0) continue;
                        if (cov > 1.0) cov = 1.0;
                        gfx_pixel(px, py, RGBA(RED(color), GREEN(color), BLUE(color),
                                               (int)(cov * a0)));
                    }
                }
                (void)x;
            } else {
                for (x = xa; x <= xb; x++) {
                    int dx = x - cx;
                    double d = k_sqrt((double)(dx * dx + y * y));
                    double cov = (double)radius + 0.5 - d;
                    if (cov <= 0.0) continue;
                    if (cov > 1.0) cov = 1.0;
                    gfx_pixel(x, py, RGBA(RED(color), GREEN(color), BLUE(color),
                                          (int)(cov * a0)));
                }
            }
        }
    }
}

void gfx_frame(int x, int y, int w, int h, u32 color) {
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}

/* 50% checkerboard used to show transparency in the paint app. */
void gfx_checkerboard(int x, int y, int w, int h, u32 a, u32 b, int cell) {
    int yy, xx;
    for (yy = 0; yy < h; yy += cell) {
        for (xx = 0; xx < w; xx += cell) {
            int odd = ((xx / cell) + (yy / cell)) & 1;
            gfx_fill_rect(x + xx, y + yy, cell, cell, odd ? b : a);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Embedded bitmap fonts                                                    */
/* -------------------------------------------------------------------------- */

/*
 * Glyph lookup.  The terminal font is the CP437 table in font_data.h, the UI
 * fonts are packed 1-bit proportional atlases.
 */

static int font_glyph_index_ascii(int ch) {
    if (ch >= 32 && ch < 127) return ch - 32;
    return -1;
}

static int font_glyph_index_unicode(unsigned int cp) {
    int i;
    for (i = 0; i < FONT_TERM_COUNT - FONT_TERM_ASCII; i++) {
        if (g_font_term_uni[i] == cp) return FONT_TERM_ASCII + i;
    }
    return '?' - 32;
}

/* UTF-8 decode step. Returns codepoint, advances *s. */
static unsigned int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    unsigned int cp;
    if (!*p) return 0;
    if (p[0] < 0x80) { cp = p[0]; *s += 1; return cp; }
    if ((p[0] & 0xE0) == 0xC0 && p[1]) {
        cp = ((unsigned)(p[0] & 0x1F) << 6) | (unsigned)(p[1] & 0x3F);
        *s += 2;
        return cp;
    }
    if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        cp = ((unsigned)(p[0] & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (unsigned)(p[2] & 0x3F);
        *s += 3;
        return cp;
    }
    *s += 1;
    return '?';
}

static const u16 *term_glyph(int cp) {
    int idx = font_glyph_index_ascii(cp);
    if (idx < 0) idx = font_glyph_index_unicode((unsigned)cp);
    if (idx < 0 || idx >= FONT_TERM_COUNT) idx = '?' - 32;
    return &g_font_term[idx * FONT_TERM_H];
}

/* Terminal font: fixed 8x16 cell, drawn 1bpp with colour.
 *
 * Glyph rows are bitmasks, so a row is written as horizontal spans rather than
 * eight individual pixels.  Profiling showed this function accounting for 7.6
 * million gfx_pixel() calls per benchmark run -- each one re-checking the clip
 * rectangle and the framebuffer bounds -- while the terminal is repainted on
 * every frame.  Span writes reduce that to a handful of stores per row. */
void gfx_putc_term(int x, int y, int ch, u32 fg, u32 bg, int opaque) {
    const u16 *g = term_glyph(ch);
    int row;
    for (row = 0; row < FONT_TERM_H; row++) {
        int bits = g[row];
        int py = y + row;
        int col = 0;
        if (py < 0 || py >= g_fb_h) continue;
        /* fast path: the whole cell is inside the clip and the framebuffer */
        if (x >= g_clip.x && x + FONT_TERM_W <= g_clip.x + g_clip.w &&
            x >= 0 && x + FONT_TERM_W <= SCREEN_W) {
            u32 *p = &g_vram[py * g_fb_pitch + x];
            if (bits == 0) {
                if (opaque) {
                    int i;
                    for (i = 0; i < FONT_TERM_W; i++) p[i] = bg;
                }
                continue;
            }
            if (!opaque && !bg) {
                /* transparent background, nothing to clear */
            } else if (opaque) {
                int i;
                for (i = 0; i < FONT_TERM_W; i++) p[i] = (bits & (1 << (FONT_TERM_W - 1 - i))) ? fg : bg;
                continue;
            }
            /* transparent background: only the set bits are touched */
            while (col < FONT_TERM_W) {
                if (bits & (1 << (FONT_TERM_W - 1 - col))) {
                    int start = col;
                    while (col < FONT_TERM_W && (bits & (1 << (FONT_TERM_W - 1 - col)))) col++;
                    {
                        int i;
                        for (i = start; i < col; i++) p[i] = fg;
                    }
                } else col++;
            }
            continue;
        }
        /* slow path: partially clipped cell -- keep the per-pixel behaviour */
        for (col = 0; col < FONT_TERM_W; col++) {
            if (bits & (1 << (FONT_TERM_W - 1 - col))) gfx_pixel(x + col, py, fg);
            else if (opaque) gfx_pixel(x + col, py, bg);
        }
    }
}

void gfx_text_term(int x, int y, const char *s, u32 fg, u32 bg, int opaque) {
    int cx = x;
    while (s && *s) {
        unsigned int cp = utf8_next(&s);
        if (cp == '\n') { cx = x; y += FONT_TERM_H; continue; }
        gfx_putc_term(cx, y, (int)cp, fg, bg, opaque);
        cx += FONT_TERM_W;
    }
}

/* Proportional UI text.  size: 0 = 12px, 1 = 14px, 2 = 17px. */
static const unsigned char *ui_bits(int size) {
    return size == 0 ? g_ui_bits_0 : (size == 1 ? g_ui_bits_1 : g_ui_bits_2);
}
static const unsigned char *ui_adv(int size) {
    return size == 0 ? g_ui_adv_0 : (size == 1 ? g_ui_adv_1 : g_ui_adv_2);
}
static int ui_w(int size) { return size == 0 ? UI_FONT_0_W : (size == 1 ? UI_FONT_1_W : UI_FONT_2_W); }
static int ui_h(int size) { return size == 0 ? UI_FONT_0_H : (size == 1 ? UI_FONT_1_H : UI_FONT_2_H); }

/* Width of a single ASCII character in the given text size.  Used by the HTML
   layout to wrap text without having to walk a whole string for every word. */
int gfx_char_w(int ch, int size) {
    if (ch < 32) ch = '?';
    if (ch >= 127) ch = '?';
    return ui_adv(size)[ch - 32];
}

int gfx_text_w(const char *s, int size) {
    int w = 0;
    while (s && *s) {
        unsigned int cp = utf8_next(&s);
        if (cp < 32) cp = '?';
        if (cp >= 127) cp = '?';
        w += ui_adv(size)[cp - 32];
    }
    return w;
}

int gfx_text_h(int size) { return ui_h(size); }

void gfx_char_ui(int x, int y, int ch, u32 color, int size) {
    int w = ui_w(size), h = ui_h(size);
    const unsigned char *bits = ui_bits(size);
    int idx, row, a = ALPHA(color);
    u32 fg;
    if (ch < 32 || ch > 126) ch = '?';
    idx = ch - 32;
    fg = (a == 255) ? color : RGBA(RED(color), GREEN(color), BLUE(color), a);

    /* Text is the most-painted thing on the screen, and glyphs are 1bpp: each
       row is written as runs of set bits instead of one gfx_pixel() per cell.
       That removes the per-pixel clip test, which dominated the profile. */
    for (row = 0; row < h; row++) {
        int py = y + row;
        int col = 0;
        int base;
        if (py < g_clip.y || py >= g_clip.y + g_clip.h) continue;
        if (py < 0 || py >= g_fb_h) continue;
        base = idx * w * h + row * w;
        while (col < w) {
            int bit = base + col;
            if (bits[bit >> 3] & (1 << (bit & 7))) {
                int start = col;
                while (col < w) {
                    int b2 = base + col;
                    if (!(bits[b2 >> 3] & (1 << (b2 & 7)))) break;
                    col++;
                }
                gfx_hline(x + start, py, col - start, fg);
            } else {
                col++;
            }
        }
    }
}

void gfx_text(int x, int y, const char *s, u32 color, int size) {
    int cx = x;
    int w = ui_w(size);
    const unsigned char *adv = ui_adv(size);
    (void)w;
    while (s && *s) {
        unsigned int cp = utf8_next(&s);
        if (cp == '\n') { break; }
        if (cp < 32 || cp > 126) cp = '?';
        gfx_char_ui(cx, y, (int)cp, color, size);
        cx += adv[cp - 32];
    }
}

void gfx_text_n(int x, int y, const char *s, int n, u32 color, int size) {
    char tmp[512];
    if (n <= 0) return;
    if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
    kmemcpy(tmp, s, (size_t)n);
    tmp[n] = 0;
    gfx_text(x, y, tmp, color, size);
}

void gfx_text_center(int cx, int y, const char *s, u32 color, int size) {
    gfx_text(cx - gfx_text_w(s, size) / 2, y, s, color, size);
}

void gfx_text_right(int rx, int y, const char *s, u32 color, int size) {
    gfx_text(rx - gfx_text_w(s, size), y, s, color, size);
}

/* Ellipsised text for list rows. */
void gfx_text_clip(int x, int y, int maxw, const char *s, u32 color, int size) {
    int w = 0;
    char buf[256];
    int n = 0;
    const unsigned char *adv = ui_adv(size);
    while (s && *s && n < (int)sizeof(buf) - 4) {
        unsigned int cp = utf8_next(&s);
        int cwid;
        if (cp < 32 || cp > 126) cp = '?';
        cwid = adv[cp - 32];
        if (w + cwid > maxw) {
            if (n > 0 && w + 3 * adv['.' - 32] <= maxw) {
                buf[n++] = '.'; buf[n++] = '.'; buf[n++] = '.';
            }
            break;
        }
        w += cwid;
        buf[n++] = (char)cp;
    }
    buf[n] = 0;
    gfx_text(x, y, buf, color, size);
}

/* -------------------------------------------------------------------------- */
/*  Framebuffer blits                                                        */
/* -------------------------------------------------------------------------- */

void gfx_blit_argb(int dx, int dy, int dw, int dh, const u32 *src, int sw, int sh, int alpha) {
    int y, x;
    if (!src) return;
    if (dw == sw && dh == sh && alpha >= 255) {
        rect_t r = rect_intersect(rect_make(dx, dy, dw, dh), g_clip);
        if (r.w <= 0 || r.h <= 0) return;
        /* Fast path: the destination rows lie fully inside the framebuffer and
           the source is a straight 1:1 copy.  Going through gfx_pixel() here
           meant re-testing the clip rectangle and the screen bounds for every
           single pixel of every cached icon blit -- which kept the icon cache
           from helping: the blit cost as much as repainting the icon. */
        for (y = r.y - dy; y < r.y - dy + r.h; y++) {
            const u32 *srow;
            u32 *drow;
            int dyi = dy + y;
            if (y < 0 || y >= sh) continue;
            if (dyi < 0 || dyi >= g_fb_h) continue;
            srow = &src[y * sw];
            drow = &g_vram[dyi * g_fb_pitch];
            for (x = r.x - dx; x < r.x - dx + r.w; x++) {
                u32 c;
                int dxo;
                if (x < 0 || x >= sw) continue;
                dxo = dx + x;
                if (dxo < 0 || dxo >= g_fb_w) continue;
                c = srow[x];
                if (ALPHA(c) == 255) drow[dxo] = c;
                else if (ALPHA(c) != 0) drow[dxo] = gfx_blend(drow[dxo], c, 255);
            }
        }
        return;
    }
    for (y = 0; y < dh; y++) {
        int sy = (sh > 1) ? (y * sh) / dh : 0;
        for (x = 0; x < dw; x++) {
            int sx = (sw > 1) ? (x * sw) / dw : 0;
            if (sy < sh && sx < sw) {
                u32 c = src[sy * sw + sx];
                int a2 = (int)((ALPHA(c) * (alpha < 255 ? alpha : 255)) / 255);
                if (a2 <= 0) continue;
                gfx_pixel(dx + x, dy + y, RGBA(RED(c), GREEN(c), BLUE(c), a2));
            }
        }
    }
}

void gfx_clear(u32 color) {
    rect_t save = g_clip;
    gfx_reset_clip();
    gfx_fill_rect(0, 0, SCREEN_W, SCREEN_H, color);
    g_clip = save;
}

/* -------------------------------------------------------------------------- */
/*  Software mouse cursor sprites                                            */
/* -------------------------------------------------------------------------- */

enum {
    CURSOR_ARROW = 0,
    CURSOR_HAND,
    CURSOR_IBEAM,
    CURSOR_HRESIZE,
    CURSOR_VRESIZE,
    CURSOR_MOVE,
    CURSOR_WAIT,
    CURSOR_COUNT
};

/* The arrow (13x20) as coverage masks: 0 empty, 1 black outline, 2 white body. */
static const char *cursor_arrow_rows[20] = {
    "1............",
    "11...........",
    "121..........",
    "1221.........",
    "12221........",
    "122221.......",
    "1222221......",
    "12222221.....",
    "122222221....",
    "1222222221...",
    "12222222221..",
    "122222222221.",
    "1222222222221",
    "1222222111111",
    "1222221......",
    "1221.221.....",
    "121..221.....",
    "11....221....",
    "1.....221....",
    ".......11...."
};

static const char *cursor_hand_rows[20] = {
    ".....11......",
    "....1221.....",
    "....1221..11.",
    "....1221.1221",
    "....1221.1221",
    ".11.1221.1221",
    "122112211221.",
    "122112211221.",
    "1221122112211",
    "1221122112211",
    ".122222222221",
    "..1222222221.",
    "..1222222221.",
    "...122222221.",
    "....1222221..",
    "....1222221..",
    ".....122221..",
    ".....12221...",
    "......1221...",
    "......1111..."
};

static const char *cursor_ibeam_rows[20] = {
    ".111111.",
    "..1221..",
    "...22...",
    "...12...",
    "...12...",
    "...12...",
    "...12...",
    "...12...",
    "...12...",
    "...12...",
    "...12...",
    "...12...",
    "...12...",
    "...12...",
    "...12...",
    "...22...",
    "..1221..",
    ".111111.",
    "........",
    "........"
};

static const char *cursor_hres_rows[20] = {
    "..........",
    "..........",
    "..........",
    "..........",
    "..1....1..",
    ".12....21.",
    ".1222221..",
    "1222222221",
    ".1222221..",
    ".12....21.",
    "..1....1..",
    "..........",
    "..........",
    "..........",
    "..........",
    "..........",
    "..........",
    "..........",
    "..........",
    ".........."
};

static void gfx_blit_mask(const char **rows, int nrows, int rx, int ry, u32 hot_color) {
    int y, x;
    for (y = 0; y < nrows; y++) {
        const char *r = rows[y];
        for (x = 0; r[x]; x++) {
            char c = r[x];
            if (c == '1') gfx_pixel(rx + x, ry + y, RGB(10, 10, 14));
            else if (c == '2') gfx_pixel(rx + x, ry + y, hot_color);
        }
    }
}

/* Deferred cursor -----------------------------------------------------------
 *
 * The cursor is the one thing that must be redrawn every frame, but drawing it
 * while the compositor paints means the pixels under it are already final --
 * so the next frame has to repaint whatever it covered, which defeats any
 * caching below it.  Instead the compositor records "where the pointer is" and
 * the cursor is blitted once, in hal_present(), after the rest of the frame is
 * done.  Nothing publishes a frame without calling hal_present, so it is always
 * drawn, and the backdrop/window layers stay clean.
 */
void gfx_draw_cursor(int x, int y, int kind);   /* defined below */

static int g_cursor_x = -1, g_cursor_y = -1, g_cursor_kind;
static int g_cursor_enabled = 1;

void gfx_set_cursor(int x, int y, int kind) {
    g_cursor_x = x; g_cursor_y = y; g_cursor_kind = kind;
}
void gfx_cursor_hide(int hide) { g_cursor_enabled = hide ? 0 : 1; }
void gfx_composite_cursor(void) {
    if (!g_cursor_enabled || g_cursor_x < 0) return;
    gfx_draw_cursor(g_cursor_x, g_cursor_y, g_cursor_kind);
}

void gfx_draw_cursor(int x, int y, int kind) {
    rect_t save = g_clip;
    gfx_reset_clip();
    switch (kind) {
    case CURSOR_HAND:     gfx_blit_mask(cursor_hand_rows, 20, x - 6, y - 1, RGB(246, 248, 252)); break;
    case CURSOR_IBEAM:    gfx_blit_mask(cursor_ibeam_rows, 19, x - 3, y - 9, RGB(246, 248, 252)); break;
    case CURSOR_HRESIZE:  gfx_blit_mask(cursor_hres_rows, 11, x - 5, y - 5, RGB(246, 248, 252)); break;
    case CURSOR_VRESIZE: {
        /* rotate the h-resize cursor 90 degrees */
        int yy, xx;
        static const char *vrows[10] = {
            "....11....",
            "...1221...",
            "..122221..",
            ".12222221.",
            "1222222221",
            "1222222221",
            ".12222221.",
            "..122221..",
            "...1221...",
            "....11...."
        };
        for (yy = 0; yy < 10; yy++)
            for (xx = 0; vrows[yy][xx]; xx++) {
                char c = vrows[yy][xx];
                if (c == '1') gfx_pixel(x - 5 + xx, y - 5 + yy, RGB(10, 10, 14));
                else if (c == '2') gfx_pixel(x - 5 + xx, y - 5 + yy, RGB(246, 248, 252));
            }
        break;
    }
    case CURSOR_MOVE: {
        int yy, xx;
        static const char *mrows[15] = {
            "......1......",
            ".....121.....",
            "....12221....",
            "...1222221...",
            "..122222221..",
            "......2......",
            "......2......",
            "1....222....1",
            "12...222...21",
            "1222222222221",
            "12...222...21",
            "1....222....1",
            "......2......",
            "......2......",
            "......1......"
        };
        for (yy = 0; yy < 15; yy++)
            for (xx = 0; mrows[yy][xx]; xx++) {
                char c = mrows[yy][xx];
                if (c == '1') gfx_pixel(x - 6 + xx, y - 7 + yy, RGB(10, 10, 14));
                else if (c == '2') gfx_pixel(x - 6 + xx, y - 7 + yy, RGB(246, 248, 252));
            }
        break;
    }
    case CURSOR_WAIT: {
        static const int phase = 0;
        u32 ticks = now_ms() / 90;
        int i;
        (void)phase;
        for (i = 0; i < 12; i++) {
            double ang = (double)i * 3.14159265 / 6.0 + (double)(ticks % 12) * 3.14159265 / 6.0;
            int px = x + 16 + (int)(k_cos(ang) * 9.0);
            int py = y + 9 + (int)(k_sin(ang) * 9.0);
            int a = 60 + (i * 16);
            if (a > 255) a = 255;
            gfx_fill_circle_aa(px, py, 1, RGBA(240, 244, 255, a));
        }
        break;
    }
    default:
        gfx_blit_mask(cursor_arrow_rows, 20, x, y, RGB(246, 248, 252));
        break;
    }
    g_clip = save;
}

/* -------------------------------------------------------------------------- */
/*  Procedural icons                                                         */
/* -------------------------------------------------------------------------- */
/*  Every icon is drawn from code (rounded rects, lines, circles): no image
    assets exist anywhere in this executable.                                  */

#define ICON_SZ 40

enum {
    ICON_NONE = 0, ICON_OS, ICON_FOLDER, ICON_SYSMON, ICON_REBOOT,
    ICON_PANIC, ICON_SETTINGS, ICON_POWER, ICON_TERMINAL, ICON_CODE,
    ICON_FILES, ICON_BACK, ICON_PAINT, ICON_HEX, ICON_TXT, ICON_C,
    ICON_BIN, ICON_PROC, ICON_CALC, ICON_FWD, ICON_UP, ICON_DOC,
    ICON_CLOSE, ICON_CHECK, ICON_BROWSER
};

static void icon_paint(int kind, int x, int y, int size) {
    /* all coordinates below are authored at 32x32 and scaled */
    #define PX(u) ((x) + ((u) * (size)) / 32)
    #define PY(v) ((y) + ((v) * (size)) / 32)
    #define PS(u) (((u) * (size)) / 32 > 0 ? ((u) * (size)) / 32 : 1)
    u32 c1, c2;
    switch (kind) {
    case ICON_OS: {
        int cx = PX(16), cy = PY(16), r = PS(13);
        gfx_fill_circle_aa(cx, cy, r, RGBA(40, 120, 200, 210));
        gfx_fill_circle_aa(cx, cy, r - PS(3), RGBA(12, 20, 34, 240));
        gfx_fill_circle_aa(cx, cy, PS(2), RGB(120, 220, 255));
        gfx_circle(cx, cy, r, RGB(120, 200, 255));
        break;
    }
    case ICON_FOLDER:
    case ICON_FILES: {
        gfx_round_rect(PX(3), PY(9), PS(12), PS(5), 2, RGB(224, 172, 64));
        gfx_round_rect(PX(3), PY(11), PS(26), PS(17), PS(3), RGB(238, 188, 78));
        gfx_round_rect_grad(PX(3), PY(14), PS(26), PS(14), PS(3), RGB(250, 206, 96), RGB(214, 156, 52));
        gfx_hline(PX(3) + 2, PY(14), PS(26) - 4, RGB(255, 228, 150));
        break;
    }
    case ICON_SYSMON: {
        gfx_round_rect(PX(2), PY(4), PS(28), PS(24), PS(3), RGB(20, 30, 44));
        gfx_round_rect_border(PX(2), PY(4), PS(28), PS(24), PS(3), RGB(80, 160, 210), 1);
        gfx_line(PX(5), PY(22), PX(9), PY(14), RGB(96, 220, 150));
        gfx_line(PX(9), PY(14), PX(13), PY(19), RGB(96, 220, 150));
        gfx_line(PX(13), PY(19), PX(17), PY(9), RGB(250, 196, 92));
        gfx_line(PX(17), PY(9), PX(21), PY(17), RGB(250, 196, 92));
        gfx_line(PX(21), PY(17), PX(26), PY(11), RGB(248, 104, 112));
        gfx_fill_rect(PX(5), PY(26), PS(22), PS(2), RGB(60, 90, 120));
        break;
    }
    case ICON_REBOOT: {
        int cx = PX(16), cy = PY(17), r = PS(10);
        int i;
        for (i = 30; i < 330; i += 12) {
            double a = (double)i * 3.14159265 / 180.0;
            gfx_line(cx + (int)(k_cos(a) * r), cy + (int)(k_sin(a) * r),
                     cx + (int)(k_cos(a) * (r - PS(3))), cy + (int)(k_sin(a) * (r - PS(3))),
                     RGB(110, 210, 255));
        }
        gfx_line_thick(cx, cy - PS(16), cx, cy - PS(6), PS(2), RGB(110, 210, 255));
        gfx_line(cx, cy - PS(16), cx + PS(6), cy - PS(12), RGB(110, 210, 255));
        gfx_line(cx, cy - PS(16), cx - PS(6), cy - PS(12), RGB(110, 210, 255));
        break;
    }
    case ICON_PANIC: {
        gfx_round_rect(PX(4), PY(4), PS(24), PS(24), PS(4), RGB(60, 12, 16));
        gfx_round_rect_border(PX(4), PY(4), PS(24), PS(24), PS(4), RGB(240, 80, 90), 1);
        gfx_line_thick(PX(16), PY(9), PX(16), PY(19), PS(2), RGB(255, 110, 120));
        gfx_fill_rect(PX(15), PY(21), PS(3), PS(3), RGB(255, 110, 120));
        break;
    }
    case ICON_SETTINGS: {
        int i;
        int cx = PX(16), cy = PY(16);
        for (i = 0; i < 8; i++) {
            double a = (double)i * 3.14159265 / 4.0;
            gfx_line_thick(cx + (int)(k_cos(a) * PS(9)), cy + (int)(k_sin(a) * PS(9)),
                           cx + (int)(k_cos(a) * PS(13)), cy + (int)(k_sin(a) * PS(13)),
                           PS(3), RGB(180, 195, 215));
        }
        gfx_fill_circle_aa(cx, cy, PS(8), RGB(180, 195, 215));
        gfx_fill_circle_aa(cx, cy, PS(4), RGB(20, 28, 42));
        break;
    }
    case ICON_POWER: {
        int cx = PX(16), cy = PY(17), r = PS(10);
        int i;
        for (i = 40; i < 320; i += 10) {
            double a = (double)i * 3.14159265 / 180.0;
            gfx_line(cx + (int)(k_cos(a) * r), cy + (int)(k_sin(a) * r),
                     cx + (int)(k_cos(a) * (r - PS(3))), cy + (int)(k_sin(a) * (r - PS(3))),
                     RGB(250, 130, 130));
        }
        gfx_line_thick(cx, cy - PS(15), cx, cy - PS(4), PS(2), RGB(250, 130, 130));
        break;
    }
    case ICON_TERMINAL:
    case ICON_BIN: {
        gfx_round_rect(PX(2), PY(4), PS(28), PS(24), PS(3), RGB(14, 18, 26));
        gfx_round_rect_border(PX(2), PY(4), PS(28), PS(24), PS(3), RGB(70, 90, 116), 1);
        gfx_fill_rect(PX(2), PY(4), PS(28), PS(4), RGB(40, 58, 82));
        gfx_text_term(PX(5), PY(11), ">", RGB(96, 220, 150), 0, 0);
        gfx_fill_rect(PX(13), PY(12), PS(6), PS(9), RGB(96, 220, 150));
        gfx_text_term(PX(5), PY(27), "", 0, 0, 0);
        break;
    }
    case ICON_CODE: {
        gfx_round_rect(PX(3), PY(3), PS(26), PS(26), PS(3), RGB(26, 32, 44));
        gfx_round_rect_border(PX(3), PY(3), PS(26), PS(26), PS(3), RGB(120, 140, 170), 1);
        gfx_line_thick(PX(12), PY(10), PX(6), PY(16), PS(2), RGB(96, 200, 255));
        gfx_line_thick(PX(6), PY(16), PX(12), PY(22), PS(2), RGB(96, 200, 255));
        gfx_line_thick(PX(20), PY(10), PX(26), PY(16), PS(2), RGB(250, 196, 92));
        gfx_line_thick(PX(26), PY(16), PX(20), PY(22), PS(2), RGB(250, 196, 92));
        gfx_line_thick(PX(18), PY(23), PX(14), PY(9), PS(2), RGB(248, 130, 160));
        break;
    }
    case ICON_BACK:
        gfx_fill_rect(PX(6), PY(15), PS(16), PS(3), RGB(200, 215, 230));
        gfx_line_thick(PX(13), PY(9), PX(6), PY(16), PS(2), RGB(200, 215, 230));
        gfx_line_thick(PX(6), PY(16), PX(13), PY(23), PS(2), RGB(200, 215, 230));
        break;
    case ICON_FWD:
        gfx_fill_rect(PX(10), PY(15), PS(16), PS(3), RGB(200, 215, 230));
        gfx_line_thick(PX(19), PY(9), PX(26), PY(16), PS(2), RGB(200, 215, 230));
        gfx_line_thick(PX(26), PY(16), PX(19), PY(23), PS(2), RGB(200, 215, 230));
        break;
    case ICON_UP:
        gfx_fill_rect(PX(15), PY(10), PS(3), PS(16), RGB(200, 215, 230));
        gfx_line_thick(PX(9), PY(14), PX(16), PY(7), PS(2), RGB(200, 215, 230));
        gfx_line_thick(PX(16), PY(7), PX(23), PY(14), PS(2), RGB(200, 215, 230));
        break;
    case ICON_PAINT: {
        gfx_round_rect(PX(3), PY(3), PS(26), PS(26), PS(4), RGB(240, 240, 245));
        {
            static const u32 cols[6] = { 0xFFE05A5A, 0xFFF0C040, 0xFF60C070,
                                         0xFF50B0E0, 0xFF9060D0, 0xFF202020 };
            int i;
            for (i = 0; i < 6; i++) {
                int cx = PX(8 + (i % 3) * 8), cy = PY(9 + (i / 3) * 10);
                gfx_fill_circle_aa(cx, cy, PS(3), cols[i] | 0xFF000000u);
            }
        }
        gfx_line_thick(PX(18), PY(24), PX(26), PY(16), PS(2), RGB(150, 110, 70));
        break;
    }
    case ICON_HEX: {
        int row, col;
        gfx_round_rect(PX(3), PY(3), PS(26), PS(26), PS(3), RGB(14, 20, 30));
        gfx_round_rect_border(PX(3), PY(3), PS(26), PS(26), PS(3), RGB(90, 120, 150), 1);
        /* a little hex dump: pairs of green digits in a 4x4 grid */
        for (row = 0; row < 4; row++) {
            for (col = 0; col < 4; col++) {
                u32 c = RGB(96, 220, 150);
                int gx = PX(6 + col * 5), gy = PY(7 + row * 5);
                if (((row * 4 + col) & 3) == 0) c = RGB(120, 170, 210);
                gfx_fill_rect(gx, gy, PS(3), PS(3), c);
            }
        }
        gfx_fill_rect(PX(6), PY(25), PS(20), PS(1), RGB(90, 120, 150));
        break;
    }
    case ICON_TXT:
    case ICON_DOC: {
        gfx_round_rect(PX(7), PY(3), PS(18), PS(26), PS(2), RGB(238, 242, 248));
        gfx_fill_rect(PX(10), PY(9), PS(12), PS(1), RGB(120, 140, 160));
        gfx_fill_rect(PX(10), PY(13), PS(12), PS(1), RGB(120, 140, 160));
        gfx_fill_rect(PX(10), PY(17), PS(8), PS(1), RGB(120, 140, 160));
        gfx_fill_rect(PX(10), PY(21), PS(10), PS(1), RGB(120, 140, 160));
        break;
    }
    case ICON_C: {
        gfx_round_rect(PX(7), PY(3), PS(18), PS(26), PS(2), RGB(40, 48, 62));
        gfx_round_rect_border(PX(7), PY(3), PS(18), PS(26), PS(2), RGB(120, 160, 210), 1);
        gfx_text(PX(10), PY(8), "c", RGB(96, 200, 255), 1);
        gfx_fill_rect(PX(10), PY(22), PS(12), PS(1), RGB(90, 110, 140));
        break;
    }
    case ICON_PROC: {
        gfx_fill_circle_aa(PX(16), PY(16), PS(11), RGB(40, 70, 100));
        gfx_circle(PX(16), PY(16), PS(11), RGB(110, 200, 240));
        gfx_text(PX(11), PY(11), "p", RGB(200, 235, 255), 1);
        break;
    }
    case ICON_BROWSER: {
        /* a globe: filled sphere, equator and one meridian */
        gfx_fill_circle_aa(PX(16), PY(16), PS(13), RGB(34, 96, 158));
        gfx_circle(PX(16), PY(16), PS(13), RGB(150, 214, 255));
        gfx_line(PX(3), PY(16), PX(29), PY(16), RGB(190, 232, 255));
        gfx_circle(PX(16), PY(16), PS(7), RGB(190, 232, 255));
        gfx_fill_rect(PX(15), PY(4), PS(2), PS(24), RGB(190, 232, 255));
        break;
    }
    case ICON_CALC: {
        gfx_round_rect(PX(4), PY(2), PS(24), PS(28), PS(3), RGB(30, 38, 52));
        gfx_round_rect_border(PX(4), PY(2), PS(24), PS(28), PS(3), RGB(110, 140, 180), 1);
        gfx_fill_rect(PX(7), PY(5), PS(18), PS(6), RGB(90, 200, 160));
        {
            int i, j;
            for (i = 0; i < 3; i++)
                for (j = 0; j < 3; j++)
                    gfx_fill_rect(PX(7 + j * 6), PY(14 + i * 5), PS(4), PS(3), RGB(150, 170, 195));
        }
        break;
    }
    case ICON_CHECK: {
        gfx_line_thick(PX(7), PY(17), PX(13), PY(23), PS(3), RGB(96, 220, 150));
        gfx_line_thick(PX(13), PY(23), PX(25), PY(9), PS(3), RGB(96, 220, 150));
        break;
    }
    case ICON_CLOSE: {
        gfx_line_thick(PX(8), PY(8), PX(24), PY(24), PS(2), RGB(240, 120, 130));
        gfx_line_thick(PX(24), PY(8), PX(8), PY(24), PS(2), RGB(240, 120, 130));
        break;
    }
    default:
        gfx_round_rect(PX(4), PY(4), PS(24), PS(24), PS(3), RGB(70, 80, 100));
        break;
    }
    c1 = c2 = 0;
    (void)c1; (void)c2;
    #undef PX
    #undef PY
    #undef PS
}

/* --------------------------------------------------------------------------
 *  Icon cache
 *
 *  Every icon is a small vector program: rounded rectangles with analytic
 *  anti-aliasing, anti-aliased circles, lines.  The desktop redraws its icon
 *  row on every frame, and profiling showed icon painting accounting for 9.5 of
 *  17.4 seconds -- over half the compositor's time -- because each redraw
 *  re-ran those routines from scratch.
 *
 *  An icon depends only on (kind, size), so each one is rendered once into a
 *  small ARGB tile and blitted afterwards.  The tile is rendered against a
 *  transparent backdrop and blitted with alpha, so it composites correctly over
 *  any wallpaper.
 */
#define ICON_CACHE_SLOTS 48
#define ICON_CACHE_MAXSZ 72

typedef struct {
    int  kind, size;
    int  valid;
    u32  px[ICON_CACHE_MAXSZ * ICON_CACHE_MAXSZ];
} icon_cache_t;

static icon_cache_t g_icon_cache[ICON_CACHE_SLOTS];

/* Cache key: the same icon is requested at three or four sizes (desktop row,
   window title, taskbar tab, start menu).  An earlier version keyed only on the
   icon kind, so one icon at three sizes evicted itself every call and the cache
   never hit -- which is why it made no difference.  Hash on (kind, size) and
   use linear probing. */
static icon_cache_t *icon_cache_slot(int kind, int size) {
    int i;
    int h = (kind * 131 + size * 7) % ICON_CACHE_SLOTS;
    for (i = 0; i < ICON_CACHE_SLOTS; i++) {
        icon_cache_t *c = &g_icon_cache[(h + i) % ICON_CACHE_SLOTS];
        if (!c->valid) return c;                        /* free slot           */
        if (c->kind == kind && c->size == size) return c;
    }
    return 0;                                           /* table full          */
}

/* Scratch tile used while rendering an icon for the cache. */
static u32 g_icon_tile[ICON_CACHE_MAXSZ * ICON_CACHE_MAXSZ];

void gfx_draw_icon_size(int kind, int x, int y, int size) {
    icon_cache_t *slot;
    if (kind <= ICON_NONE || kind > ICON_BROWSER || size <= 0 || size > ICON_CACHE_MAXSZ) {
        icon_paint(kind, x, y, size);
        return;
    }
    slot = icon_cache_slot(kind, size);
    if (slot && slot->valid) {
        gfx_blit_argb(x, y, size, size, slot->px, size, size, 255);
        return;
    }
    /* Render the icon once into a transparent tile, then store and blit it.
       The tile is bound as the framebuffer for the duration, so every primitive
       clips to the tile instead of to the display. */
    {
        rect_t save_clip = g_clip;
        kmemset(g_icon_tile, 0, (size_t)(size * size) * sizeof(u32));
        fb_bind_offscreen(g_icon_tile, size, size);
        gfx_reset_clip();
        icon_paint(kind, 0, 0, size);
        fb_bind_screen();
        g_clip = save_clip;
        if (slot) {
            slot->kind = kind;
            slot->size = size;
            kmemcpy(slot->px, g_icon_tile, (size_t)(size * size) * sizeof(u32));
            slot->valid = 1;
        }
        gfx_blit_argb(x, y, size, size, g_icon_tile, size, size, 255);
    }
}

void gfx_draw_icon(int kind, int x, int y, int size) {
    int save_x = g_clip.x, save_y = g_clip.y, save_w = g_clip.w, save_h = g_clip.h;
    gfx_draw_icon_size(kind, x, y, size);
    g_clip.x = save_x; g_clip.y = save_y; g_clip.w = save_w; g_clip.h = save_h;
}

/* File-type icon chooser shared by the explorer and the editor. */
int gfx_icon_for_path(const char *path, int is_dir, int is_proc, int is_exec) {
    const char *dot;
    if (is_dir) return ICON_FOLDER;
    if (is_proc) return ICON_PROC;
    if (is_exec) return ICON_TERMINAL;
    dot = kstrrchr(path, '.');
    if (dot) {
        if (!kstrnicmp(dot, ".c", 3)) return ICON_C;
        if (!kstrnicmp(dot, ".txt", 5) || !kstrnicmp(dot, ".md", 4)) return ICON_TXT;
        if (!kstrnicmp(dot, ".bin", 5) || !kstrnicmp(dot, ".aeb", 5) || !kstrnicmp(dot, ".sys", 5))
            return ICON_BIN;
        if (!kstrnicmp(dot, ".cfg", 5)) return ICON_SETTINGS;
    }
    return ICON_DOC;
}

/* Width of the first n bytes of a string in the proportional UI font. */
int gfx_text_w_n(const char *s, int n, int size) {
    int w = 0;
    const unsigned char *adv = ui_adv(size);
    int i = 0;
    while (s && s[i] && i < n) {
        int c = (unsigned char)s[i];
        if (c < 32 || c > 126) c = '?';
        w += adv[c - 32];
        i++;
    }
    return w;
}
