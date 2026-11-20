/* ==========================================================================
 *  APP: paint -- framebuffer studio
 *
 *  A real pixel canvas held in allocator memory: freehand brush, line, rect
 *  and filled-rect tools, an 18-swatch palette, four brush sizes, clear,
 *  save / load to the VFS (custom RLE image format), undo of the last stroke
 *  and a live coordinate readout.
 * ========================================================================== */

#define PAINT_W 400
#define PAINT_H 300
#define PAINT_SWATCHES 18
#define PAINT_UNDO_MAX 3

enum { TOOL_BRUSH = 0, TOOL_LINE, TOOL_RECT, TOOL_FILLRECT, TOOL_ERASER, TOOL_PICK, TOOL_COUNT };

typedef struct {
    u32 *pixels;                /* PAINT_W * PAINT_H ARGB                      */
    u32 *undo[PAINT_UNDO_MAX];
    int  undo_count;
    char path[VFS_MAX_PATH];
    u32  color;
    int  brush;
    int  tool;
    int  drawing;
    int  last_x, last_y;
    int  start_x, start_y;
    int  mouse_x, mouse_y;
    int  canvas_x, canvas_y;    /* canvas origin in screen space               */
    int  canvas_scale;
    int  swatch_hover;
    int  dirty;
    char status[128];
    int  status_kind;
    u32  strokes;
} paint_t;

static const u32 g_palette[PAINT_SWATCHES] = {
    0xFF000000, 0xFF3A3A3A, 0xFF808080, 0xFFC8C8C8, 0xFFFFFFFF, 0xFFE05A5A,
    0xFFF0C040, 0xFF60C070, 0xFF50B0E0, 0xFF9060D0, 0xFFF080C0, 0xFFA0703A,
    0xFF00E0C0, 0xFF204080, 0xFF80FF80, 0xFFFF8040, 0xFF604020, 0xFFF8F0A0
};

static void paint_push_undo(paint_t *p) {
    int i;
    if (p->undo_count < PAINT_UNDO_MAX) {
        if (!p->undo[p->undo_count]) return;
        kmemcpy(p->undo[p->undo_count], p->pixels, (size_t)PAINT_W * PAINT_H * 4);
        p->undo_count++;
    } else {
        u32 *first = p->undo[0];
        for (i = 0; i < PAINT_UNDO_MAX - 1; i++) p->undo[i] = p->undo[i + 1];
        p->undo[PAINT_UNDO_MAX - 1] = first;
        kmemcpy(p->undo[PAINT_UNDO_MAX - 1], p->pixels, (size_t)PAINT_W * PAINT_H * 4);
    }
}

static void paint_undo(paint_t *p) {
    if (p->undo_count <= 0) return;
    p->undo_count--;
    kmemcpy(p->pixels, p->undo[p->undo_count], (size_t)PAINT_W * PAINT_H * 4);
    p->dirty = 1;
    kstrcpy(p->status, "undo");
    p->status_kind = 0;
}

static void paint_plot(paint_t *p, int x, int y, u32 col) {
    int dx, dy;
    int r = p->brush / 2;
    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++) {
            int px = x + dx, py = y + dy;
            if (px < 0 || py < 0 || px >= PAINT_W || py >= PAINT_H) continue;
            if (dx * dx + dy * dy > r * r + 1) continue;
            p->pixels[py * PAINT_W + px] = col;
        }
    p->dirty = 1;
}

static void paint_line(paint_t *p, int x0, int y0, int x1, int y1, u32 col) {
    int dx = k_abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -k_abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        paint_plot(p, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

static void paint_rect(paint_t *p, int x0, int y0, int x1, int y1, u32 col, int filled) {
    int x, y;
    int xa = k_min(x0, x1), xb = k_max(x0, x1);
    int ya = k_min(y0, y1), yb = k_max(y0, y1);
    if (filled) {
        for (y = ya; y <= yb; y++)
            for (x = xa; x <= xb; x++)
                if (x >= 0 && y >= 0 && x < PAINT_W && y < PAINT_H)
                    p->pixels[y * PAINT_W + x] = col;
        p->dirty = 1;
        return;
    }
    for (x = xa; x <= xb; x++) { paint_plot(p, x, ya, col); paint_plot(p, x, yb, col); }
    for (y = ya; y <= yb; y++) { paint_plot(p, xa, y, col); paint_plot(p, xb, y, col); }
}

/* Simple RLE image format: "APIX" | w | h | runs of (count, color). */
static int paint_save(paint_t *p) {
    u32 n = (u32)PAINT_W * PAINT_H;
    u32 runs = 0, i;
    u8 *buf;
    u32 cap, off = 0;
    if (!p->path[0]) kstrcpy(p->path, "/home/user/painting.apix");
    for (i = 0; i < n; i++) if (i == 0 || p->pixels[i] != p->pixels[i - 1]) runs++;
    cap = 16 + runs * 8;
    buf = (u8 *)mm_kmalloc(cap);
    if (!buf) return -1;
    kmemcpy(buf, "APIX", 4);
    kmemcpy(buf + 4, &(u32){ PAINT_W }, 4);
    kmemcpy(buf + 8, &(u32){ PAINT_H }, 4);
    kmemcpy(buf + 12, &runs, 4);
    off = 16;
    for (i = 0; i < n; ) {
        u32 c = p->pixels[i], r = 1;
        while (i + r < n && p->pixels[i + r] == c && r < 0xFFFFu) r++;
        kmemcpy(buf + off, &r, 4);
        kmemcpy(buf + off + 4, &c, 4);
        off += 8;
        i += r;
    }
    {
        int rc = vfs_write_file(p->path, buf, off);
        mm_kfree(buf);
        if (rc == 0) {
            ksnprintf(p->status, sizeof(p->status), "saved %u bytes (%u runs) to %s",
                      (unsigned)off, (unsigned)runs, p->path);
            p->status_kind = 1;
            p->dirty = 0;
            return 0;
        }
    }
    kstrcpy(p->status, "save failed");
    p->status_kind = 2;
    return -1;
}

static int paint_load(paint_t *p) {
    u32 len = 0, runs = 0, i = 0, off = 16;
    const char *err = NULL;
    char *data;
    if (!p->path[0]) kstrcpy(p->path, "/home/user/painting.apix");
    data = vfs_read_alloc(p->path, &len, &err);
    if (!data) {
        ksnprintf(p->status, sizeof(p->status), "load: %s", err ? err : "not found");
        p->status_kind = 2;
        return -1;
    }
    if (len < 16 || kstrncmp(data, "APIX", 4)) {
        mm_kfree(data);
        kstrcpy(p->status, "load: not an APIX image");
        p->status_kind = 2;
        return -1;
    }
    kmemcpy(&runs, data + 12, 4);
    paint_push_undo(p);
    {
        u32 pos = 0;
        for (i = 0; i < runs && off + 8 <= len; i++) {
            u32 count = 0, col = 0;
            kmemcpy(&count, data + off, 4);
            kmemcpy(&col, data + off + 4, 4);
            off += 8;
            while (count-- && pos < (u32)PAINT_W * PAINT_H)
                p->pixels[pos++] = col;
        }
    }
    mm_kfree(data);
    p->dirty = 1;
    ksnprintf(p->status, sizeof(p->status), "loaded %u runs from %s", (unsigned)runs, p->path);
    p->status_kind = 1;
    return 0;
}

static void paint_clear(paint_t *p, u32 color) {
    paint_push_undo(p);
    {
        int i;
        for (i = 0; i < PAINT_W * PAINT_H; i++) p->pixels[i] = color;
    }
    p->dirty = 1;
}

static void *paint_create(const char *arg) {
    paint_t *p = (paint_t *)mm_kmalloc(sizeof(paint_t));
    int i;
    if (!p) return NULL;
    kmemset(p, 0, sizeof(*p));
    p->pixels = (u32 *)mm_kmalloc((u32)PAINT_W * PAINT_H * 4);
    if (!p->pixels) { mm_kfree(p); return NULL; }
    for (i = 0; i < PAINT_UNDO_MAX; i++) {
        p->undo[i] = (u32 *)mm_kmalloc((u32)PAINT_W * PAINT_H * 4);
    }
    paint_clear(p, 0xFFFFFFFF);
    p->undo_count = 0;
    for (i = 0; i < PAINT_W * PAINT_H; i++) p->pixels[i] = 0xFFFFFFFF;
    p->color = 0xFF202020;
    p->brush = 3;
    p->tool = TOOL_BRUSH;
    p->canvas_scale = 1;
    if (arg && arg[0]) { kstrncpy(p->path, arg, sizeof(p->path) - 1); paint_load(p); }
    return p;
}

static void paint_destroy(void *state) {
    paint_t *p = (paint_t *)state;
    int i;
    if (!p) return;
    if (p->pixels) mm_kfree(p->pixels);
    for (i = 0; i < PAINT_UNDO_MAX; i++) if (p->undo[i]) mm_kfree(p->undo[i]);
    mm_kfree(p);
}

static void paint_paint(void *state, window_t *w, wm_rect_t c) {
    paint_t *p = (paint_t *)state;
    theme_t *t = theme_get();
    int toolbar_h = 34;
    int palette_h = 30;
    int x, y, i;
    int avail_w, avail_h, scale;

    gfx_fill_rect(c.x, c.y, c.w, c.h, RGB(18, 22, 30));

    /* toolbar */
    {
        static const char *names[TOOL_COUNT] = { "Brush", "Line", "Rect", "Fill", "Eraser", "Pick" };
        int bx = c.x + 6, by = c.y + 5;
        for (i = 0; i < TOOL_COUNT; i++) {
            int bw = gfx_text_w(names[i], 0) + 14;
            int active = p->tool == i;
            int hover = (g_wm.mouse_px >= bx && g_wm.mouse_px < bx + bw &&
                         g_wm.mouse_py >= by && g_wm.mouse_py < by + 24);
            gfx_round_rect(bx, by, bw, 24, 5, active ? t->accent : (hover ? t->sel : RGBA(38, 48, 66, 220)));
            gfx_text_center(bx + bw / 2, by + 5, names[i], active ? RGB(8, 14, 22) : t->text, 0);
            bx += bw + 4;
        }
        /* brush size */
        bx += 6;
        gfx_text(bx, by + 6, "size", t->text_mute, 0);
        bx += 30;
        for (i = 1; i <= 4; i++) {
            int active = p->brush == i * 2 - 1 || (i == 4 && p->brush == 9);
            int sz = i * 2 + 2;
            int hover = (g_wm.mouse_px >= bx && g_wm.mouse_px < bx + 20);
            gfx_round_rect(bx, by, 20, 24, 4, active ? t->accent : (hover ? t->sel : RGBA(38, 48, 66, 220)));
            gfx_fill_rect(bx + 10 - sz / 2, by + 12 - sz / 2, sz, sz, RGB(20, 26, 36));
            bx += 22;
        }
        /* actions */
        {
            const char *acts[4] = { "Save", "Load", "Undo", "Clear" };
            int ax = c.x + c.w - 236;
            for (i = 0; i < 4; i++) {
                int bw = 54;
                int hover = (g_wm.mouse_px >= ax && g_wm.mouse_px < ax + bw &&
                             g_wm.mouse_py >= by && g_wm.mouse_py < by + 24);
                gfx_round_rect(ax, by, bw, 24, 4,
                               hover ? (i == 3 ? RGBA(200, 70, 80, 220) : t->sel) : RGBA(38, 48, 66, 220));
                gfx_text_center(ax + bw / 2, by + 5, acts[i], t->text, 0);
                ax += bw + 3;
            }
        }
    }

    /* canvas */
    avail_w = c.w - 24;
    avail_h = c.h - toolbar_h - palette_h - 40;
    scale = k_max(1, k_min(avail_w / PAINT_W, avail_h / PAINT_H));
    if (scale > 3) scale = 3;
    p->canvas_scale = scale;
    x = c.x + (c.w - PAINT_W * scale) / 2;
    y = c.y + toolbar_h + 4;
    p->canvas_x = x;
    p->canvas_y = y;
    {
        int cx, cy;
        /* checkerboard border */
        gfx_fill_rect(x - 2, y - 2, PAINT_W * scale + 4, PAINT_H * scale + 4, RGBA(60, 70, 88, 255));
        for (cy = 0; cy < PAINT_H; cy++) {
            for (cx = 0; cx < PAINT_W; cx++) {
                u32 col = p->pixels[cy * PAINT_W + cx];
                if (scale == 1) {
                    gfx_pixel(x + cx, y + cy, col);
                } else {
                    gfx_fill_rect(x + cx * scale, y + cy * scale, scale, scale, col);
                }
            }
        }
        /* live shape preview */
        if (p->drawing && (p->tool == TOOL_LINE || p->tool == TOOL_RECT || p->tool == TOOL_FILLRECT)) {
            u32 col = (p->color & 0x00FFFFFF) | 0xC0000000;
            int lx = p->mouse_x - x, ly = p->mouse_y - y;
            if (scale > 1) { lx /= scale; ly /= scale; }
            if (p->tool == TOOL_LINE)
                gfx_line(x + p->start_x * scale, y + p->start_y * scale,
                         p->mouse_x, p->mouse_y, col);
            else {
                int xa = k_min(p->start_x, lx), xb = k_max(p->start_x, lx);
                int ya = k_min(p->start_y, ly), yb = k_max(p->start_y, ly);
                gfx_frame(x + xa * scale, y + ya * scale, (xb - xa + 1) * scale, (yb - ya + 1) * scale, col);
            }
        }
    }

    /* palette */
    {
        int py = c.y + c.h - palette_h - 22;
        int pw = 26;
        int px = c.x + 8;
        for (i = 0; i < PAINT_SWATCHES; i++) {
            int hover = (g_wm.mouse_px >= px && g_wm.mouse_px < px + pw - 2 &&
                         g_wm.mouse_py >= py && g_wm.mouse_py < py + 22);
            int selected = (p->color == g_palette[i]);
            gfx_round_rect(px, py, pw - 2, 22, 3, g_palette[i]);
            if (selected) gfx_round_rect_border(px - 1, py - 1, pw, 24, 3, t->accent, 2);
            else if (hover) gfx_round_rect_border(px, py, pw - 2, 22, 3, RGB(255, 255, 255), 1);
            px += pw;
        }
        /* current colour */
        gfx_text(px + 8, py + 5, "current", t->text_mute, 0);
        gfx_round_rect(px + 58, py, 24, 22, 3, p->color);
        gfx_round_rect_border(px + 58, py, 24, 22, 3, t->text_dim, 1);
    }

    /* status */
    {
        int sy = c.y + c.h - 20;
        char buf[200];
        gfx_fill_rect(c.x, sy, c.w, 20, RGBA(10, 14, 22, 240));
        if (p->status[0])
            gfx_text(c.x + 8, sy + 3, p->status,
                     p->status_kind == 1 ? t->ok : p->status_kind == 2 ? t->err : t->text_dim, 0);
        ksnprintf(buf, sizeof(buf), "x=%d y=%d  brush %d  strokes %u  %s",
                  p->mouse_x - p->canvas_x, p->mouse_y - p->canvas_y, p->brush,
                  (unsigned)p->strokes, p->path[0] ? p->path : "/home/user/painting.apix");
        gfx_text_right(c.x + c.w - 8, sy + 3, buf, t->text_mute, 0);
    }
}

static void paint_tick(void *state, u32 dt) { (void)state; (void)dt; }

static void paint_key(void *state, int key, int ch, int mods) {
    paint_t *p = (paint_t *)state;
    (void)mods;
    if (key == 0x09) p->tool = (p->tool + 1) % TOOL_COUNT;
    if (ch >= '1' && ch <= '4') p->brush = (ch - '1') * 2 + 1;
    if (ch == 'b') p->tool = TOOL_BRUSH;
    if (ch == 'l') p->tool = TOOL_LINE;
    if (ch == 'r') p->tool = TOOL_RECT;
    if (ch == 'f') p->tool = TOOL_FILLRECT;
    if (ch == 'e') p->tool = TOOL_ERASER;
    if (ch == 'u') paint_undo(p);
    if (ch == 's') paint_save(p);
    if (ch == 'o') paint_load(p);
    if (ch == 'c') paint_clear(p, 0xFFFFFFFF);
}

static void paint_mouse(void *state, int event, int x, int y, int button, int wheel) {
    paint_t *p = (paint_t *)state;
    wm_rect_t c = wm_input_client_rect();
    int scale = p->canvas_scale ? p->canvas_scale : 1;
    int cx = (x - p->canvas_x) / scale;
    int cy = (y - p->canvas_y) / scale;
    int in_canvas = (cx >= 0 && cy >= 0 && cx < PAINT_W && cy < PAINT_H);
    (void)button; (void)wheel;
    p->mouse_x = x;
    p->mouse_y = y;

    if (event == APP_EV_MOUSE_MOVE && p->drawing) {
        if (!in_canvas) return;
        if (p->tool == TOOL_BRUSH || p->tool == TOOL_ERASER) {
            u32 col = (p->tool == TOOL_ERASER) ? 0xFFFFFFFF : p->color;
            paint_line(p, p->last_x, p->last_y, cx, cy, col);
            p->last_x = cx;
            p->last_y = cy;
        }
        return;
    }
    if (event == APP_EV_MOUSE_UP) {
        if (p->drawing) {
            int lx = k_clamp(cx, 0, PAINT_W - 1), ly = k_clamp(cy, 0, PAINT_H - 1);
            if (p->tool == TOOL_LINE) paint_line(p, p->start_x, p->start_y, lx, ly, p->color);
            else if (p->tool == TOOL_RECT) paint_rect(p, p->start_x, p->start_y, lx, ly, p->color, 0);
            else if (p->tool == TOOL_FILLRECT) paint_rect(p, p->start_x, p->start_y, lx, ly, p->color, 1);
            p->drawing = 0;
            p->strokes++;
        }
        return;
    }
    if (event != APP_EV_MOUSE_DOWN) return;

    /* toolbar */
    if (y < c.y + 40) {
        int bx = c.x + 6;
        int i;
        static const char *names[TOOL_COUNT] = { "Brush", "Line", "Rect", "Fill", "Eraser", "Pick" };
        for (i = 0; i < TOOL_COUNT; i++) {
            int bw = gfx_text_w(names[i], 0) + 14;
            if (x >= bx && x < bx + bw) { p->tool = i; return; }
            bx += bw + 4;
        }
        bx += 6 + 30;
        for (i = 1; i <= 4; i++) {
            if (x >= bx && x < bx + 20) { p->brush = (i == 4) ? 9 : i * 2 - 1; return; }
            bx += 22;
        }
        {
            int ax = c.x + c.w - 236;
            for (i = 0; i < 4; i++) {
                if (x >= ax && x < ax + 54) {
                    if (i == 0) paint_save(p);
                    else if (i == 1) paint_load(p);
                    else if (i == 2) paint_undo(p);
                    else paint_clear(p, 0xFFFFFFFF);
                    return;
                }
                ax += 57;
            }
        }
        return;
    }
    /* palette */
    {
        int py = c.y + c.h - 52;
        int px = c.x + 8;
        int i;
        if (y >= py && y < py + 24) {
            for (i = 0; i < PAINT_SWATCHES; i++) {
                if (x >= px && x < px + 24) { p->color = g_palette[i]; return; }
                px += 26;
            }
        }
    }
    /* canvas */
    if (in_canvas) {
        paint_push_undo(p);
        p->drawing = 1;
        p->last_x = cx;
        p->last_y = cy;
        p->start_x = cx;
        p->start_y = cy;
        if (p->tool == TOOL_BRUSH) paint_plot(p, cx, cy, p->color);
        else if (p->tool == TOOL_ERASER) paint_plot(p, cx, cy, 0xFFFFFFFF);
        else if (p->tool == TOOL_PICK) p->color = p->pixels[cy * PAINT_W + cx] | 0xFF000000;
    }
}

const app_desc_t app_paint = {
    "paint", "Paint Studio", "pixel canvas", ICON_PAINT,
    720, 620, 460, 420, APP_CAT_GRAPHICS, 1, 0,
    paint_create, paint_destroy, paint_paint, paint_key, paint_mouse, paint_tick, NULL
};
