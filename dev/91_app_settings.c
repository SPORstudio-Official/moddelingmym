/* ==========================================================================
 *  APP: settings -- control panel & kernel debugger
 *
 *  Wallpaper picker (live preview thumbnails), theme picker, CRT scanline
 *  toggle, kernel log viewer, a visual map of the allocator block chain, and
 *  a button that triggers a genuine kernel panic with a full register dump.
 * ========================================================================== */

typedef struct {
    int  tab;                   /* 0 appearance, 1 kernel log, 2 memory, 3 about */
    int  hover;
    int  log_scroll;
    int  mem_scroll;
    u32  flash_until;
    char status[160];
    int  status_kind;
    int  selected_theme;
} settings_t;

static void *settings_create(const char *arg) {
    settings_t *s = (settings_t *)mm_kmalloc(sizeof(settings_t));
    (void)arg;
    if (!s) return NULL;
    kmemset(s, 0, sizeof(*s));
    s->tab = 0;
    s->selected_theme = theme_index();
    return s;
}

static void settings_destroy(void *state) { if (state) mm_kfree(state); }

static void settings_paint(void *state, window_t *w, wm_rect_t c) {
    settings_t *s = (settings_t *)state;
    theme_t *t = theme_get();
    static const char *tabs[4] = { "Appearance", "Kernel Log", "Memory", "About" };
    int i;

    gfx_fill_rect(c.x, c.y, c.w, c.h, t->win_bg);

    /* tab strip */
    {
        int bx = c.x + 8, by = c.y + 8;
        for (i = 0; i < 4; i++) {
            int bw = gfx_text_w(tabs[i], 1) + 24;
            int active = s->tab == i;
            int hover = (g_wm.mouse_px >= bx && g_wm.mouse_px < bx + bw &&
                         g_wm.mouse_py >= by && g_wm.mouse_py < by + 26);
            gfx_round_rect(bx, by, bw, 26, 6, active ? t->accent : (hover ? t->sel : RGBA(36, 46, 64, 220)));
            gfx_text_center(bx + bw / 2, by + 6, tabs[i], active ? RGB(8, 14, 22) : t->text, 1);
            bx += bw + 5;
        }
    }

    {
        int x = c.x + 14;
        int y = c.y + 48;
        int cw = c.w - 28;
        switch (s->tab) {
        case 0: {
            gfx_text(x, y, "Wallpaper", t->text_mute, 0);
            y += 20;
            for (i = 0; i < wallpaper_count(); i++) {
                int percol = 3;
                int tw = (cw - 20) / percol;
                int th = 54;
                int tx = x + (i % percol) * (tw + 10);
                int row = i / percol;
                int active = wallpaper_index() == i;
                int sy = y + row * (th + 20);
                int hover = (g_wm.mouse_px >= tx && g_wm.mouse_px < tx + tw &&
                             g_wm.mouse_py >= sy && g_wm.mouse_py < sy + th);
                /* preview: tiny procedural swatch */
                if (i == 5) gfx_fill_rect(tx, sy, tw, th, desktop_cfg()->solid);
                else if (i == 4) { gfx_fill_rect(tx, sy, tw, th, RGB(6, 20, 24));
                    gfx_fill_circle(tx + tw / 2, sy + th / 2, 14, RGBA(40, 200, 170, 90));
                    gfx_fill_circle(tx + tw / 3, sy + th / 2, 10, RGBA(50, 140, 230, 90)); }
                else if (i == 0) gfx_fill_rect(tx, sy, tw, th, RGB(10, 18, 32));
                else if (i == 1) gfx_fill_rect(tx, sy, tw, th, RGB(16, 18, 40));
                else if (i == 2) gfx_fill_rect(tx, sy, tw, th, RGB(44, 48, 54));
                else gfx_fill_rect(tx, sy, tw, th, RGB(60, 16, 60));
                if (i == 0) {
                    int k;
                    for (k = 0; k < 5; k++) gfx_hline(tx, sy + 9 + k * 9, tw, RGBA(60, 180, 240, 50));
                } else if (i == 1) {
                    gfx_fill_circle(tx + tw / 2, sy + th / 2, 16, RGBA(90, 120, 240, 90));
                    gfx_fill_circle(tx + tw / 3, sy + th / 3, 10, RGBA(180, 70, 220, 80));
                } else if (i == 3) {
                    gfx_fill_circle(tx + tw / 2, sy + th / 2, 14, RGBA(255, 190, 70, 200));
                    gfx_fill_rect(tx, sy + th - 10, tw, 10, RGBA(180, 20, 90, 200));
                }
                if (active) gfx_round_rect_border(tx - 2, sy - 2, tw + 4, th + 4, 4, t->accent, 2);
                else if (hover) gfx_round_rect_border(tx, sy, tw, th, 3, t->border_hi, 1);
                gfx_text_center(tx + tw / 2, sy + th + 3, wallpaper_name(i), t->text_dim, 0);
            }
            y += 2 * 74 + 8;
            gfx_text(x, y, "Colour theme", t->text_mute, 0);
            y += 20;
            for (i = 0; i < 4; i++) {
                int tw = (cw - 30) / 4;
                int tx = x + i * (tw + 10);
                int active = theme_index() == i;
                int hover = (g_wm.mouse_px >= tx && g_wm.mouse_px < tx + tw &&
                             g_wm.mouse_py >= y && g_wm.mouse_py < y + 34);
                gfx_round_rect(tx, y, tw, 34, 6, active ? t->sel : (hover ? RGBA(80, 100, 140, 60) : RGBA(30, 38, 52, 200)));
                gfx_round_rect(tx + 8, y + 8, 18, 18, 4, i == 0 ? RGB(79, 195, 247) :
                               i == 1 ? RGB(255, 176, 64) : i == 2 ? RGB(190, 205, 220) : RGB(255, 92, 168));
                gfx_text(tx + 32, y + 11, theme_name(i), t->text, 0);
                if (active) gfx_round_rect_border(tx, y, tw, 34, 6, t->accent, 1);
            }
            y += 50;
            /* scanlines toggle */
            {
                int hover = (g_wm.mouse_px >= x && g_wm.mouse_px < x + 200 &&
                             g_wm.mouse_py >= y && g_wm.mouse_py < y + 26);
                gfx_round_rect(x, y, 200, 26, 6, hover ? t->sel : RGBA(30, 38, 52, 200));
                gfx_text(x + 10, y + 6, "CRT scanline shader", t->text, 0);
                gfx_round_rect(x + 150, y + 5, 40, 16, 8, gfx_scanlines_enabled() ? t->ok : RGBA(70, 80, 100, 255));
                gfx_fill_circle_aa(x + (gfx_scanlines_enabled() ? 180 : 160), y + 13, 6, RGB(240, 246, 255));
            }
            y += 38;
            gfx_text(x, y, "Keyboard: Ctrl+` toggles the shader at any time.", t->text_mute, 0);
            y += 24;
            {
                int hover = (g_wm.mouse_px >= x && g_wm.mouse_px < x + 220 &&
                             g_wm.mouse_py >= y && g_wm.mouse_py < y + 30);
                gfx_round_rect(x, y, 220, 30, 6, hover ? RGB(210, 70, 80) : RGB(150, 44, 54));
                gfx_text_center(x + 110, y + 7, "Trigger Kernel Panic", RGB(255, 235, 235), 1);
            }
            break;
        }
        case 1: {
            u32 len = 0;
            const char *log = klog_data(&len);
            int lines = 0, li;
            int vis = (c.h - 110) / 16;
            const char *p = log;
            u32 starts[512];
            gfx_text(x, y, "Kernel ring buffer (dmesg)", t->text_mute, 0);
            y += 20;
            gfx_round_rect(x - 4, y - 4, cw + 8, c.h - 100, 5, RGBA(6, 9, 14, 240));
            starts[0] = 0;
            while (*p && lines < 511) {
                if (*p == '\n') { starts[++lines] = (u32)(p - log) + 1; }
                p++;
            }
            {
                int first = k_max(0, lines - vis + 1 + s->log_scroll);
                for (li = 0; li < vis && first + li <= lines; li++) {
                    u32 st = starts[first + li];
                    char line[220];
                    int n = 0;
                    u32 k = st;
                    while (k < len && log[k] != '\n' && n < 210) line[n++] = log[k++];
                    line[n] = 0;
                    gfx_text_term(x, y + li * 16, line,
                                  kstrncmp(line, "PANIC", 5) == 0 ? t->err :
                                  kstrncmp(line, "[  OK  ]", 8) == 0 ? t->ok : t->text_dim, 0, 0);
                }
            }
            s->log_scroll = k_clamp(s->log_scroll, 0, k_max(0, lines - vis + 1));
            break;
        }
        case 2: {
            mm_report_t rep;
            mm_verify(&rep);
            char buf[200];
            gfx_text(x, y, "Physical memory map", t->text_mute, 0);
            y += 20;
            {
                int bars[3] = { 0 };
                gfx_round_rect(x, y, cw, 22, 4, RGBA(0, 0, 0, 140));
                gfx_fill_rect(x + 1, y + 1, (cw - 2) * 1 / 64, 20, RGB(120, 80, 200));
                gfx_fill_rect(x + 1 + (cw - 2) / 64, y + 1, (cw - 2) * 31 / 64, 20, RGB(60, 120, 200));
                gfx_fill_rect(x + 1 + (cw - 2) * 32 / 64, y + 1, (cw - 2) * 32 / 64, 20, RGB(50, 140, 90));
                y += 28;
                gfx_text(x, y, "kernel 1 MiB   heap 31 MiB   NVRAM ramdisk 32 MiB", t->text_mute, 0);
                y += 24;
                (void)bars;
            }
            ksnprintf(buf, sizeof(buf), "heap: %u / %u B used, %u blocks (%u used, %u free)",
                      (unsigned)rep.bytes_used, (unsigned)mm_total(),
                      (unsigned)rep.blocks_total, (unsigned)rep.blocks_used, (unsigned)rep.blocks_free);
            gfx_text(x, y, buf, t->text, 0);
            y += 18;
            ksnprintf(buf, sizeof(buf), "largest free block %u B   fragmentation %u%%   integrity %s",
                      (unsigned)rep.largest_free, (unsigned)rep.frag_pct,
                      rep.errors ? "CORRUPT" : "verified");
            gfx_text(x, y, buf, rep.errors ? t->err : t->ok, 0);
            y += 26;
            gfx_text(x, y, "allocator block chain (scroll with the wheel)", t->text_mute, 0);
            y += 18;
            gfx_round_rect(x, y, cw, c.h - (y - c.y) - 60, 5, RGBA(6, 9, 14, 240));
            {
                u32 idx = 0;
                mm_block_t *b;
                int by = y + 6;
                int bx = x + 6;
                int rows = 0;
                while (mm_block_at(idx, &b)) {
                    int bw = k_max(2, (int)(b->size / 2048));
                    u32 col;
                    if (idx < (u32)s->mem_scroll) { idx++; continue; }
                    if (bw > cw - 20) bw = cw - 20;
                    if (bx + bw > x + cw - 8) { bx = x + 6; by += 14; rows++; }
                    if (by > y + c.h - (y - c.y) - 70 + 20) break;
                    col = (b->flags & MM_FLAG_FREE) ? RGBA(60, 70, 90, 255) : RGBA(80, 200, 140, 220);
                    gfx_fill_rect(bx, by, bw, 10, col);
                    bx += bw + 1;
                    idx++;
                }
                (void)rows;
            }
            break;
        }
        default: {
            gfx_text(x, y, "AE " AE_VERSION_STR " \"" AE_CODENAME "\"", t->text, 2);
            y += 34;
            gfx_text(x, y, "A complete operating system in a single executable.", t->text_dim, 1);
            y += 30;
            gfx_text(x, y, "Components:", t->text_mute, 0);
            y += 18;
            {
                static const char *items[] = {
                    "HAL chassis (Win32 provides only RAM, VRAM, timer and IRQs)",
                    "Boundary-tag physical memory manager with coalescing + magic checks",
                    "Inode VFS: superblock, bitmaps, 12 direct + 1 indirect blocks",
                    "Preemptive round-robin scheduler with 64 PCBs and a syscall table",
                    "AetherC compiler (lexer, parser, code generator) + stack bytecode VM",
                    "Software compositor: 16 z-ordered windows, 4 procedural wallpapers",
                    "Seven applications -- all pixels and fonts generated in software"
                };
                int k;
                for (k = 0; k < 7; k++) {
                    gfx_draw_icon(ICON_CHECK, x, y - 2, 14);
                    gfx_text(x + 20, y, items[k], t->text_dim, 0);
                    y += 20;
                }
            }
            y += 12;
            gfx_text(x, y, "Build: gcc -O2 -std=c11 aether_os.c -o AE.exe", t->text_mute, 0);
            y += 18;
            gfx_text(x, y, "-lgdi32 -luser32 -lkernel32 -lwinmm -mwindows -static", t->text_mute, 0);
            break;
        }
        }
    }

    /* status bar */
    {
        int sy = c.y + c.h - 22;
        gfx_fill_rect(c.x, sy, c.w, 22, RGBA(8, 12, 18, 240));
        if (s->status[0])
            gfx_text(c.x + 8, sy + 4, s->status, s->status_kind == 1 ? t->ok :
                     s->status_kind == 2 ? t->err : t->text_dim, 0);
        else
            gfx_text(c.x + 8, sy + 4, "kernel debugger ready", t->text_mute, 0);
    }
}

static void settings_tick(void *state, u32 dt) { (void)state; (void)dt; }

static void settings_key(void *state, int key, int ch, int mods) {
    settings_t *s = (settings_t *)state;
    (void)mods;
    if (key == 0x09 || ch == '\t') s->tab = (s->tab + 1) % 4;
    if (key == 0x26) { if (s->tab == 1) s->log_scroll--; else s->mem_scroll = k_max(0, s->mem_scroll - 4); }
    if (key == 0x28) { if (s->tab == 1) s->log_scroll++; else s->mem_scroll += 4; }
}

static void settings_mouse(void *state, int event, int x, int y, int button, int wheel) {
    settings_t *s = (settings_t *)state;
    wm_rect_t c = wm_input_client_rect();
    (void)button;
    if (event == APP_EV_WHEEL) {
        if (s->tab == 1) s->log_scroll = k_max(0, s->log_scroll - wheel);
        else s->mem_scroll = k_max(0, s->mem_scroll - wheel);
        return;
    }
    if (event != APP_EV_MOUSE_DOWN) return;
    if (y < c.y + 42) {
        static const char *tabs[4] = { "Appearance", "Kernel Log", "Memory", "About" };
        int bx = c.x + 8;
        int i;
        for (i = 0; i < 4; i++) {
            int bw = gfx_text_w(tabs[i], 1) + 24;
            if (x >= bx && x < bx + bw) { s->tab = i; return; }
            bx += bw + 5;
        }
        return;
    }
    if (s->tab == 0) {
        int cw = c.w - 28;
        int gx0 = c.x + 14;
        int wy = c.y + 68;
        int i;
        if (y >= wy && y < wy + 2 * 74) {
            for (i = 0; i < wallpaper_count(); i++) {
                int percol = 3;
                int tw = (cw - 20) / percol;
                int th = 54;
                int tx = gx0 + (i % percol) * (tw + 10);
                int sy = wy + (i / percol) * (th + 20);
                if (x >= tx && x < tx + tw && y >= sy && y < sy + th) {
                    desktop_set_wallpaper(i);
                    ksnprintf(s->status, sizeof(s->status), "wallpaper set to %s", wallpaper_name(i));
                    s->status_kind = 1;
                    return;
                }
            }
        }
        {
            int ty = wy + 2 * 74 + 36;
            if (y >= ty && y < ty + 34) {
                for (i = 0; i < 4; i++) {
                    int tw = (cw - 30) / 4;
                    int tx = gx0 + i * (tw + 10);
                    if (x >= tx && x < tx + tw) {
                        desktop_set_theme(i);
                        ksnprintf(s->status, sizeof(s->status), "theme applied: %s", theme_name(i));
                        s->status_kind = 1;
                        return;
                    }
                }
            }
            ty += 50;
            if (y >= ty && y < ty + 26) {
                desktop_set_crt(!gfx_scanlines_enabled());
                ksnprintf(s->status, sizeof(s->status), "CRT shader %s",
                          gfx_scanlines_enabled() ? "enabled" : "disabled");
                s->status_kind = 1;
                return;
            }
            ty += 38;
            if (y >= ty && y < ty + 30 && x >= gx0 && x < gx0 + 220) {
                wm_state()->crash_requested = 1;
                return;
            }
        }
    }
}

const app_desc_t app_settings = {
    "settings", "Control Panel", "themes + kernel", ICON_SETTINGS,
    760, 560, 520, 380, APP_CAT_SYSTEM, 1, 1,
    settings_create, settings_destroy, settings_paint, settings_key, settings_mouse,
    settings_tick, NULL
};
