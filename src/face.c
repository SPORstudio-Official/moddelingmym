/* ===========================================================================
   DEAD AIR -- the thing on the tape (procedural, drawn)
   =========================================================================== */
#include "da.h"

void draw_entity_face(FB *fb, int cx, int cy, float sc, float t, int alpha) {
    int w = (int)(62.0f * sc);
    int h = (int)(86.0f * sc);
    if (w < 8 || h < 10) return;
    u32 skin  = argb(alpha, 158, 154, 140);
    u32 skinL = argb(alpha, 190, 186, 171);
    u32 skinD = argb(alpha, 104, 100, 90);
    u32 dark  = argb(alpha, 9, 8, 8);
    u32 black = argb(alpha, 3, 3, 4);

    /* shoulders + neck */
    fb_ellipse_a(fb, cx, cy + (int)(h * 1.12f), (int)(w * 1.5f), (int)(h * 0.62f), argb(alpha, 16, 15, 15));
    fb_rect_a(fb, cx - w / 5, cy + h / 4, (w * 2) / 5, h * 3 / 4, skinD);

    /* skull silhouette: cranium merged with a heavy jaw */
    fb_ellipse(fb, cx, cy - (int)(h * 0.12f), w / 2, (int)(h * 0.42f), skinD);
    fb_ellipse(fb, cx, cy + (int)(h * 0.14f), (int)(w * 0.44f), (int)(h * 0.30f), skinD);
    /* lit front plane */
    fb_ellipse(fb, cx - (int)(w * 0.03f), cy - (int)(h * 0.13f), (int)(w * 0.44f), (int)(h * 0.38f), skin);
    fb_ellipse(fb, cx - (int)(w * 0.02f), cy + (int)(h * 0.14f), (int)(w * 0.36f), (int)(h * 0.26f), skin);
    /* highlight down the left cheek */
    fb_ellipse_a(fb, cx - (int)(w * 0.18f), cy - (int)(h * 0.10f), (int)(w * 0.16f), (int)(h * 0.26f),
                 argb(alpha / 3, 255, 252, 240));

    /* brow shelf */
    fb_ellipse_a(fb, cx, cy - (int)(h * 0.235f), (int)(w * 0.45f), (int)(h * 0.035f), argb(alpha, 48, 44, 40));

    /* eye sockets, deepening */
    int ey = cy - (int)(h * 0.115f);
    int ex_off = (int)(w * 0.215f);
    for (int k = 0; k < 2; k++) {
        int rx = (int)(w * (0.165f - k * 0.045f));
        int ry = (int)(h * (0.098f - k * 0.026f));
        fb_ellipse_a(fb, cx - ex_off, ey, rx, ry, argb(alpha, 34 - k * 12, 30 - k * 10, 28 - k * 9));
        fb_ellipse_a(fb, cx + ex_off, ey, rx, ry, argb(alpha, 34 - k * 12, 30 - k * 10, 28 - k * 9));
    }
    fb_ellipse_a(fb, cx - ex_off, ey, (int)(w * 0.085f), (int)(h * 0.045f), dark);
    fb_ellipse_a(fb, cx + ex_off, ey, (int)(w * 0.085f), (int)(h * 0.045f), dark);
    /* eyes */
    int jx = (int)(sinf(t * 17.0f) * 2.0f * sc);
    int jy = (int)(cosf(t * 13.0f) * 1.5f * sc);
    int er = (int)(w * 0.042f) + 1;
    fb_disc(fb, cx - ex_off + jx, ey + jy, er, argb(alpha, 226, 222, 206));
    fb_disc(fb, cx + ex_off + jx, ey + jy, er, argb(alpha, 226, 222, 206));
    fb_disc(fb, cx - ex_off + jx + er / 2, ey + jy + er / 2, er / 2 + 1, black);
    fb_disc(fb, cx + ex_off + jx + er / 2, ey + jy + er / 2, er / 2 + 1, black);

    /* nose cavity + philtrum */
    int ny = cy + (int)(h * 0.035f);
    fb_ellipse_a(fb, cx, ny, (int)(w * 0.055f), (int)(h * 0.045f), argb(alpha, 26, 22, 21));
    fb_line(fb, cx, ny, cx, ny - (int)(h * 0.11f), argb(alpha / 2, 52, 46, 42));

    /* mouth: hanging open, teeth irregular */
    int my = cy + (int)(h * 0.245f);
    int mw = (int)(w * 0.33f), mh = (int)(h * 0.072f);
    fb_ellipse_a(fb, cx, my, mw + 2, mh + 2, argb(alpha, 44, 32, 30));
    fb_ellipse_a(fb, cx, my, mw, mh, black);
    if (mh > 2) {
        int teeth = 6;
        for (int i = 0; i < teeth; i++) {
            int tx = cx - mw + 4 + ((mw * 2 - 8) * i) / teeth;
            int tw = (mw * 2) / (teeth * 2);
            if (tw < 1) tw = 1;
            int th = (int)(mh * (0.55f + 0.35f * (float)((i * 37) % 3))) + 1;
            fb_rect_a(fb, tx, my - mh + 2, tw, th, argb(alpha, 168, 163, 148));
        }
    }
    /* cracks and grime */
    for (int i = 0; i < 7; i++) {
        int y0 = cy - (int)(h * 0.38f) + (i * h) / 10;
        int x0 = cx - w / 3 + ((i * 41) % (2 * w / 3));
        fb_line(fb, x0, y0, x0 + (int)(7 * sc), y0 + (int)(5 * sc), argb(alpha / 2, 40, 36, 32));
    }
    fb_ellipse_a(fb, cx + (int)(w * 0.26f), cy - (int)(h * 0.30f), (int)(w * 0.16f), (int)(h * 0.20f),
                 argb(alpha / 4, 0, 0, 0));
}
