/* ==========================================================================
 *  WM -- window manager + desktop compositor
 *
 *  16 overlapping windows with true z-order, dark-glass chrome, title bar
 *  dragging, 8-way edge resizing, Aero-style snapping (top = maximize,
 *  left/right = half screen), focus-on-click (which also retargets the owning
 *  PID), minimise/maximise/restore/close, a taskbar with a Start menu and a
 *  system tray, procedural wallpapers, desktop icons, toast notifications and
 *  a flicker-free software mouse cursor drawn last.
 * ========================================================================== */

#define WM_MAX_WINDOWS 16
#define WM_TITLEBAR    30
#define WM_BORDER      4
#define WM_TASKBAR     44
#define WM_RESIZE_GRIP 6

enum { WIN_FREE = 0, WIN_OPEN, WIN_MIN, WIN_MAXIMIZED };

struct window {
    int   used;
    int   z;                    /* 0 = bottom, wm.count-1 = top               */
    int   id;                   /* unique id, also used as the handle         */
    int   win_state;            /* WIN_OPEN / WIN_MIN / WIN_MAXIMIZED         */
    wm_rect_t frame;            /* outer rectangle in screen space            */
    wm_rect_t restore;          /* geometry to restore from maximize          */
    char  title[96];
    const app_desc_t *desc;
    void *state;
    u32   pid;
    int   icon;
    u32   opened_ms;
    int   dirty;
    int   close_requested;
};

typedef struct {
    window_t win[WM_MAX_WINDOWS];
    int  count;                 /* number of live windows (z-order length)    */
    int  next_id;
    int  focused;               /* window id                                  */
    int  hover;                 /* window id under cursor                     */
    int  drag_mode;             /* 0 none, 1 move, 2 resize                   */
    int  drag_dir;              /* bit0 left, bit1 right, bit2 top, bit3 bot  */
    int  drag_id;
    int  drag_dx, drag_dy;
    wm_rect_t drag_origin;
    int  snap_preview;          /* 0 none, 1 left, 2 right, 3 top             */
    int  taskbar_hover;
    int  start_open;
    int  start_hover;
    int  start_page;
    int  tray_hover;
    int  resize_hover;
    int  input_win_id;          /* window currently receiving input          */
    int  mouse_px, mouse_py;
    int  mouse_down;
    int  mouse_consumed;
    int  desktop_icon_hover;
    int  desktop_selected;
    u32  last_click_ms;
    int  last_click_id;
    int  last_click_x, last_click_y;
    int  click_count;
    int  redraw_x0, redraw_y0, redraw_x1, redraw_y1;
    int  frames;
    u32  frame_ms;
    u32  fps_x100;
    int  quit_requested;
    int  reboot_requested;
    int  crash_requested;
    char status_text[128];
    u32  status_until;
    int  dblclick_id;
    int  dblclick_x, dblclick_y;
} wm_state_t;

static wm_state_t g_wm;

/* Two presses only form a double-click when they are close in time *and* in
   space.  Without the spatial test every second click anywhere inside a window
   was swallowed as a double-click, which made rapid entry on a keypad (or any
   two quick clicks on different buttons) drop events. */
#define WM_DBLCLICK_MS   420
#define WM_DBLCLICK_SLOP 8

/* True when the press at (x,y) repeats the previous one closely enough in both
   time and space to count as a double-click on `id`. */
static int wm_is_double_click(int id, int x, int y, u32 now) {
    int dx = x - g_wm.last_click_x;
    int dy = y - g_wm.last_click_y;
    if (id != g_wm.last_click_id) return 0;
    if (now - g_wm.last_click_ms >= WM_DBLCLICK_MS) return 0;
    if (dx > WM_DBLCLICK_SLOP || dx < -WM_DBLCLICK_SLOP) return 0;
    if (dy > WM_DBLCLICK_SLOP || dy < -WM_DBLCLICK_SLOP) return 0;
    return 1;
}

static void wm_remember_click(int id, int x, int y, u32 now) {
    g_wm.last_click_id = id;
    g_wm.last_click_ms = now;
    g_wm.last_click_x = x;
    g_wm.last_click_y = y;
}

static u32 g_wm_power_off_ms;
static int g_compositor_pid;

/* -------------------------------------------------------------------------- */
/*  small helpers                                                            */
/* -------------------------------------------------------------------------- */

static wm_rect_t wm_win_client_rect(window_t *w) {
    return (wm_rect_t){ w->frame.x + WM_BORDER, w->frame.y + WM_TITLEBAR,
                        w->frame.w - WM_BORDER * 2,
                        w->frame.h - WM_TITLEBAR - WM_BORDER };
}

static window_t *wm_by_id(int id);
static window_t *wm_focused_window(void);

/* The client rectangle of the window that is currently receiving input.
   Application mouse/keyboard handlers use this instead of guessing from the
   focus, which is wrong for background windows (wheel events, for example). */
wm_rect_t wm_input_client_rect(void) {
    window_t *w = wm_by_id(g_wm.input_win_id);
    if (!w) w = wm_focused_window();
    if (!w) return (wm_rect_t){ 0, 0, 0, 0 };
    return wm_win_client_rect(w);
}

static void wm_layout_window(window_t *w) {
    if (w->desc && w->desc->layout && w->state) {
        wm_rect_t c = wm_win_client_rect(w);
        w->desc->layout(w->state, c);
    }
}

void wm_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(g_wm.status_text, sizeof(g_wm.status_text), fmt, ap);
    va_end(ap);
    g_wm.status_until = now_ms() + 2600;
}

/* -------------------------------------------------------------------------- */
/*  z-order                                                                  */
/* -------------------------------------------------------------------------- */

static window_t *wm_by_id(int id) {
    int i;
    for (i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_wm.win[i].used && g_wm.win[i].id == id) return &g_wm.win[i];
    return NULL;
}

static void wm_reindex_z(void) {
    /* compact the used windows into [0, count) preserving relative order */
    window_t tmp[WM_MAX_WINDOWS];
    int i, n = 0;
    kmemset(tmp, 0, sizeof(tmp));
    for (i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_wm.win[i].used) tmp[n++] = g_wm.win[i];
    kmemcpy(g_wm.win, tmp, sizeof(g_wm.win));
    for (i = 0; i < n; i++) {
        g_wm.win[i].used = 1;
        g_wm.win[i].z = i;
    }
    for (i = n; i < WM_MAX_WINDOWS; i++) kmemset(&g_wm.win[i], 0, sizeof(window_t));
    g_wm.count = n;
}

static void wm_raise(int id) {
    int i, from = -1;
    window_t w;
    for (i = 0; i < g_wm.count; i++)
        if (g_wm.win[i].id == id) { from = i; break; }
    if (from < 0 || from == g_wm.count - 1) { g_wm.focused = id; return; }
    w = g_wm.win[from];
    for (i = from; i < g_wm.count - 1; i++) g_wm.win[i] = g_wm.win[i + 1];
    g_wm.win[g_wm.count - 1] = w;
    for (i = 0; i < g_wm.count; i++) g_wm.win[i].z = i;
    g_wm.focused = id;
}

static window_t *wm_top_at(int x, int y) {
    int i;
    for (i = g_wm.count - 1; i >= 0; i--) {
        window_t *w = &g_wm.win[i];
        if (w->used && w->win_state != WIN_MIN &&
            x >= w->frame.x && y >= w->frame.y &&
            x < w->frame.x + w->frame.w && y < w->frame.y + w->frame.h)
            return w;
    }
    return NULL;
}

static window_t *wm_focused_window(void) {
    return wm_by_id(g_wm.focused);
}

/* -------------------------------------------------------------------------- */
/*  window lifecycle                                                         */
/* -------------------------------------------------------------------------- */

int wm_open_app(const app_desc_t *desc, const char *arg) {
    window_t *w;
    void *state = NULL;
    int slot = -1, i;
    u32 prev_owner = mm_current_pid();
    if (!desc) return -1;

    if (desc->single_instance) {
        for (i = 0; i < g_wm.count; i++) {
            if (g_wm.win[i].used && g_wm.win[i].desc == desc) {
                if (g_wm.win[i].win_state == WIN_MIN) g_wm.win[i].win_state = WIN_OPEN;
                wm_raise(g_wm.win[i].id);
                return g_wm.win[i].id;
            }
        }
    }
    if (g_wm.count >= WM_MAX_WINDOWS) {
        wm_status("window limit reached (%d)", WM_MAX_WINDOWS);
        return -1;
    }
    for (i = 0; i < WM_MAX_WINDOWS; i++)
        if (!g_wm.win[i].used) { slot = i; break; }
    if (slot < 0) return -1;
    w = &g_wm.win[slot];
    kmemset(w, 0, sizeof(*w));
    w->used = 1;
    w->id = ++g_wm.next_id;
    w->win_state = WIN_OPEN;
    w->desc = desc;
    w->win_state = WIN_OPEN;
    w->title[0] = 0;
    kstrncpy(w->title, desc->name, sizeof(w->title) - 1);
    w->icon = desc->icon;
    w->opened_ms = now_ms();
    {
        int ww = desc->def_w ? desc->def_w : 520;
        int wh = desc->def_h ? desc->def_h : 380;
        int cascade = (g_wm.next_id * 28) % 180;
        w->frame.x = 150 + cascade;
        w->frame.y = 60 + cascade / 2;
        w->frame.w = ww;
        w->frame.h = wh;
        if (w->frame.x + ww > SCREEN_W - 20) w->frame.x = (SCREEN_W - ww) / 2;
        if (w->frame.y + wh > SCREEN_H - WM_TASKBAR - 20)
            w->frame.y = (SCREEN_H - WM_TASKBAR - wh) / 2;
    }
    w->pid = proc_spawn(desc->name, 1, 16, PROC_FLAG_APP | PROC_FLAG_GUI, 0);
    /* The window state belongs to the process that owns the window, so the
       allocator can attribute it and release it if the process is killed. */
    mm_set_current_pid(w->pid);
    if (desc->create) state = desc->create(arg);
    w->state = state;
    mm_set_current_pid(prev_owner);
    if (desc->create && !state) {
        w->used = 0;
        proc_kill(w->pid, 9);
        wm_status("%s failed to start", desc->name);
        return -1;
    }
    w->close_requested = 0;
    g_wm.count = k_max(g_wm.count, slot + 1);
    /* put it on top */
    wm_reindex_z();
    wm_raise(w->id);
    wm_layout_window(w);
    {
        pcb_t *p = proc_pcb(w->pid);
        if (p) p->window_id = (u32)w->id;
    }
    klog("wm: opened '%s' as window %d (pid %u)", desc->name, w->id, (unsigned)w->pid);
    return w->id;
}

void wm_close_window(int id) {
    window_t *w = wm_by_id(id);
    if (!w) return;
    if (w->desc && w->desc->destroy) w->desc->destroy(w->state);
    if (w->pid) {
        pcb_t *p = proc_pcb(w->pid);
        if (p && p->state != PROC_ZOMBIE) proc_kill(w->pid, 0);
    }
    klog("wm: window %d ('%s') closed", w->id, w->title);
    kmemset(w, 0, sizeof(*w));
    wm_reindex_z();
    {
        int i;
        g_wm.focused = 0;
        for (i = g_wm.count - 1; i >= 0; i--) {
            if (g_wm.win[i].used && g_wm.win[i].win_state != WIN_MIN) {
                g_wm.focused = g_wm.win[i].id;
                break;
            }
        }
    }
}

/* Closes the window owned by a dying process. */
void wm_close_by_pid(u32 pid) {
    int i;
    for (i = 0; i < g_wm.count; i++)
        if (g_wm.win[i].used && g_wm.win[i].pid == pid)
            wm_close_window(g_wm.win[i].id);
}

void wm_minimize(int id) {
    window_t *w = wm_by_id(id);
    if (!w) return;
    w->win_state = WIN_MIN;
    if (g_wm.focused == id) {
        int i;
        g_wm.focused = 0;
        for (i = g_wm.count - 1; i >= 0; i--)
            if (g_wm.win[i].used && g_wm.win[i].win_state != WIN_MIN) {
                g_wm.focused = g_wm.win[i].id;
                break;
            }
    }
}

void wm_restore(int id) {
    window_t *w = wm_by_id(id);
    if (!w) return;
    if (w->win_state == WIN_MIN) w->win_state = WIN_OPEN;
    else if (w->win_state == WIN_MAXIMIZED) {
        w->frame = w->restore;
        w->win_state = WIN_OPEN;
        wm_layout_window(w);
    }
    wm_raise(id);
}

void wm_maximize(int id) {
    window_t *w = wm_by_id(id);
    if (!w) return;
    if (w->win_state == WIN_MAXIMIZED) { wm_restore(id); return; }
    w->restore = w->frame;
    w->frame.x = 0;
    w->frame.y = 0;
    w->frame.w = SCREEN_W;
    w->frame.h = SCREEN_H - WM_TASKBAR;
    w->win_state = WIN_MAXIMIZED;
    wm_layout_window(w);
}

int wm_window_count(void) {
    int i, n = 0;
    for (i = 0; i < g_wm.count; i++) if (g_wm.win[i].used) n++;
    return n;
}

window_t *wm_window_at_z(int z) {
    if (z < 0 || z >= g_wm.count) return NULL;
    return g_wm.win[z].used ? &g_wm.win[z] : NULL;
}

window_t *wm_get_window(int id) { return wm_by_id(id); }
wm_state_t *wm_state(void) { return &g_wm; }
int wm_focused_id(void) { return g_wm.focused; }
int compositor_pid(void) { return g_compositor_pid; }

/* -------------------------------------------------------------------------- */
/*  chrome hit testing                                                       */
/* -------------------------------------------------------------------------- */

enum { HIT_NONE = 0, HIT_CLIENT, HIT_TITLE, HIT_BTN_MIN, HIT_BTN_MAX, HIT_BTN_CLOSE,
       HIT_RESIZE_L, HIT_RESIZE_R, HIT_RESIZE_T, HIT_RESIZE_B,
       HIT_RESIZE_TL, HIT_RESIZE_TR, HIT_RESIZE_BL, HIT_RESIZE_BR };

static int wm_hit_test(window_t *w, int x, int y) {
    int gx = x - w->frame.x, gy = y - w->frame.y;
    int ww = w->frame.w, wh = w->frame.h;
    int bx0 = ww - 3 * 30 - 8;
    int grip = WM_RESIZE_GRIP;
    if (w->win_state == WIN_MAXIMIZED) {
        if (gy < WM_TITLEBAR) {
            if (gx >= bx0 && gx < bx0 + 28) return HIT_BTN_MIN;
            if (gx >= bx0 + 30 && gx < bx0 + 58) return HIT_BTN_MAX;
            if (gx >= bx0 + 60 && gx < bx0 + 88) return HIT_BTN_CLOSE;
            return HIT_TITLE;
        }
        return HIT_CLIENT;
    }
    if (gx < grip && gy < grip) return HIT_RESIZE_TL;
    if (gx > ww - grip && gy < grip) return HIT_RESIZE_TR;
    if (gx < grip && gy > wh - grip) return HIT_RESIZE_BL;
    if (gx > ww - grip && gy > wh - grip) return HIT_RESIZE_BR;
    if (gx < grip) return HIT_RESIZE_L;
    if (gx > ww - grip) return HIT_RESIZE_R;
    if (gy < grip) return HIT_RESIZE_T;
    if (gy > wh - grip) return HIT_RESIZE_B;
    if (gy < WM_TITLEBAR) {
        if (gx >= bx0 && gx < bx0 + 28) return HIT_BTN_MIN;
        if (gx >= bx0 + 30 && gx < bx0 + 58) return HIT_BTN_MAX;
        if (gx >= bx0 + 60 && gx < bx0 + 88) return HIT_BTN_CLOSE;
        return HIT_TITLE;
    }
    if (gx < WM_BORDER || gx > ww - WM_BORDER) return HIT_NONE;
    if (gy > wh - WM_BORDER) return HIT_NONE;
    return HIT_CLIENT;
}

static int wm_cursor_for_hit(int hit) {
    switch (hit) {
    case HIT_RESIZE_L: case HIT_RESIZE_R: return CURSOR_HRESIZE;
    case HIT_RESIZE_T: case HIT_RESIZE_B: return CURSOR_VRESIZE;
    case HIT_RESIZE_TL: case HIT_RESIZE_TR: case HIT_RESIZE_BL: case HIT_RESIZE_BR: return CURSOR_MOVE;
    case HIT_CLIENT: return CURSOR_ARROW;
    case HIT_TITLE: return CURSOR_MOVE;
    default: return CURSOR_ARROW;
    }
}

/* -------------------------------------------------------------------------- */
/*  desktop icons                                                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *id;
    const char *label;
    int icon;
} desktop_icon_t;

static const desktop_icon_t g_desktop_icons[] = {
    { "sh",       "Terminal",  8  },
    { "explorer", "Files",     10 },
    { "edit",     "Code Studio", 9 },
    { "sysmon",   "Sys Monitor", 3 },
    { "hexview",  "Hex Viewer", 13 },
    { "paint",    "Paint",     12 },
    { "settings", "Settings",  6  }
};
#define DESKTOP_ICON_COUNT ((int)(sizeof(g_desktop_icons) / sizeof(g_desktop_icons[0])))
#define ICON_CELL_W 96
#define ICON_CELL_H 92
#define ICON_X0 18
#define ICON_Y0 52

static wm_rect_t desktop_icon_rect(int i) {
    int col = i % 7, row = i / 7;
    return (wm_rect_t){ ICON_X0 + col * ICON_CELL_W, ICON_Y0 + row * ICON_CELL_H,
                        ICON_CELL_W - 8, ICON_CELL_H - 8 };
}

/* -------------------------------------------------------------------------- */
/*  taskbar geometry                                                        */
/* -------------------------------------------------------------------------- */

#define TRAY_W 336
static wm_rect_t taskbar_rect(void) {
    return (wm_rect_t){ 0, SCREEN_H - WM_TASKBAR, SCREEN_W, WM_TASKBAR };
}
static wm_rect_t start_button_rect(void) {
    return (wm_rect_t){ 6, SCREEN_H - WM_TASKBAR + 6, 104, WM_TASKBAR - 12 };
}
static wm_rect_t task_tab_rect(int i) {
    return (wm_rect_t){ 118 + i * 152, SCREEN_H - WM_TASKBAR + 7, 144, WM_TASKBAR - 14 };
}
static wm_rect_t start_menu_rect(void) {
    /* tall enough for four category headers, nine programs and the footer */
    return (wm_rect_t){ 6, SCREEN_H - WM_TASKBAR - 440, 330, 434 };
}

/* -------------------------------------------------------------------------- */
/*  Start menu layout                                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    wm_rect_t r;
    int app_index;
    int action;             /* app 0, 1 reboot, 2 panic, 3 shutdown, 4 page */
} start_item_t;

static int start_menu_items(start_item_t *out, int maxitems) {
    int n = 0, i;
    int page = g_wm.start_page;
    wm_rect_t m = start_menu_rect();
    int y = m.y + 46;
    int limit = m.y + m.h - 42;
    if (page == 0) {
        /* categorised application list */
        int cat;
        for (cat = 0; cat < 4 && n < maxitems; cat++) {
            int any = 0;
            for (i = 0; i < APP_COUNT; i++) {
                const app_desc_t *d = app_by_index(i);
                if (d->category != cat) continue;
                if (!any) {
                    /* category header occupies a row without an item */
                    y += 20;
                    any = 1;
                }
                if (y + 26 > limit) { n = maxitems; break; }
                out[n].r = (wm_rect_t){ m.x + 10, y, m.w - 20, 26 };
                out[n].app_index = i;
                out[n].action = 0;
                n++;
                y += 26;
                if (n >= maxitems) break;
            }
        }
    } else {
        /* system page */
        static const char *labels[4] = { "Reboot OS", "Trigger Kernel Panic", "Shutdown", "Back to programs" };
        for (i = 0; i < 4; i++) {
            if (y + 30 > limit) break;
            out[n].r = (wm_rect_t){ m.x + 10, y, m.w - 20, 30 };
            out[n].app_index = -1;
            out[n].action = (i == 0) ? 1 : (i == 1) ? 2 : (i == 2) ? 3 : 4;
            n++;
            y += 34;
            (void)labels;
        }
    }
    return n;
}

/* -------------------------------------------------------------------------- */
/*  painting: wallpapers                                                    */
/* -------------------------------------------------------------------------- */

static void paint_wallpaper(void) {
    int i, j;
    theme_t *t = theme_get();
    switch (wallpaper_index()) {
    case 0: { /* cyber grid */
        gfx_gradient_v(0, 0, SCREEN_W, SCREEN_H - WM_TASKBAR, RGB(6, 10, 20), RGB(14, 26, 44));
        for (i = 0; i < 24; i++) {
            int y = 60 + i * i / 2 + 30;
            if (y > SCREEN_H - WM_TASKBAR) break;
            int alpha = 18 + i * 4;
            for (j = 0; j < SCREEN_W; j += 64)
                gfx_pixel(j + (i * 7) % 64, y, RGBA(64, 200, 255, k_min(90, alpha)));
            gfx_hline(0, y, SCREEN_W, RGBA(60, 180, 240, k_min(80, alpha / 2)));
        }
        for (i = 0; i < SCREEN_W; i += 64) gfx_vline(i, 0, SCREEN_H - WM_TASKBAR, RGBA(50, 140, 200, 18));
        gfx_radial_glow(SCREEN_W / 2, 240, 380, RGBA(30, 120, 200, 60));
        break;
    }
    case 1: { /* nebula */
        gfx_gradient_v(0, 0, SCREEN_W, SCREEN_H - WM_TASKBAR, RGB(6, 8, 18), RGB(16, 20, 42));
        gfx_radial_glow(300, 200, 420, RGBA(60, 90, 220, 55));
        gfx_radial_glow(980, 420, 460, RGBA(160, 50, 200, 60));
        gfx_radial_glow(720, 120, 320, RGBA(40, 180, 200, 40));
        gfx_radial_glow(150, 520, 340, RGBA(200, 60, 130, 40));
        for (i = 0; i < 460; i++) {
            int x = (int)(krandom() % SCREEN_W);
            int y = (int)(krandom() % (SCREEN_H - WM_TASKBAR));
            int b = 40 + (int)(krandom() % 200);
            u32 c = RGBA(b, b, k_min(255, b + 20), b);
            gfx_pixel(x, y, c);
            if ((i % 23) == 0) gfx_pixel(x + 1, y, RGBA(b / 2, b / 2, b, 120));
        }
        break;
    }
    case 2: { /* slate */
        gfx_gradient_v(0, 0, SCREEN_W, SCREEN_H - WM_TASKBAR, RGB(40, 44, 50), RGB(22, 24, 28));
        for (i = 0; i < SCREEN_H; i += 4) gfx_hline(0, i, SCREEN_W, RGBA(255, 255, 255, 6));
        for (i = 0; i < SCREEN_W; i += 4) gfx_vline(i, 0, SCREEN_H - WM_TASKBAR, RGBA(0, 0, 0, 8));
        gfx_radial_glow(SCREEN_W / 2, SCREEN_H / 3, 560, RGBA(200, 220, 255, 22));
        break;
    }
    default: { /* synthwave */
        gfx_gradient_v(0, 0, SCREEN_W, SCREEN_H - WM_TASKBAR, RGB(28, 6, 44), RGB(120, 20, 80));
        gfx_radial_glow(SCREEN_W / 2, SCREEN_H / 2, 300, RGBA(255, 180, 60, 90));
        gfx_fill_circle(SCREEN_W / 2, SCREEN_H / 2 + 40, 130, RGBA(255, 200, 90, 200));
        for (i = 0; i < 9; i++)
            gfx_fill_rect(SCREEN_W / 2 - 140, SCREEN_H / 2 + 40 + i * 16, 280, 5, RGBA(180, 20, 90, 220));
        for (i = 0; i < 40; i++) {
            int y = SCREEN_H / 2 + 160 + i * i / 3;
            if (y > SCREEN_H - WM_TASKBAR) break;
            gfx_hline(0, y, SCREEN_W, RGBA(255, 100, 200, 40 + i * 3));
        }
        for (i = -20; i < 40; i++) {
            int x = SCREEN_W / 2 + i * 40;
            gfx_line(SCREEN_W / 2 + i * 10, SCREEN_H / 2 + 160, x, SCREEN_H - WM_TASKBAR,
                     RGBA(255, 120, 220, 60));
        }
        break;
    }
    }
    if (gfx_scanlines_enabled()) {
        int lvl = gfx_scanlines_state() ? 26 : 14;
        for (i = 0; i < SCREEN_H; i += 2) gfx_hline(0, i, SCREEN_W, RGBA(0, 0, 0, lvl));
    }
    (void)t;
}

static void paint_desktop_icons(void) {
    int i;
    for (i = 0; i < DESKTOP_ICON_COUNT; i++) {
        wm_rect_t r = desktop_icon_rect(i);
        const desktop_icon_t *ic = &g_desktop_icons[i];
        int hover = (g_wm.desktop_icon_hover == i + 1);
        int selected = (g_wm.desktop_selected == i + 1);
        if (hover || selected) {
            gfx_round_rect(r.x, r.y, r.w, r.h, 10,
                           selected ? RGBA(79, 195, 247, 70) : RGBA(255, 255, 255, 22));
            gfx_round_rect_border(r.x, r.y, r.w, r.h, 10, RGBA(140, 200, 240, 120), 1);
        }
        gfx_draw_icon(ic->icon, r.x + r.w / 2 - ICON_SZ / 2, r.y + 8, ICON_SZ);
        {
            theme_t *t = theme_get();
            int tw = gfx_text_w(ic->label, 0);
            gfx_text(r.x + (r.w - tw) / 2, r.y + r.h - 20, ic->label,
                     hover ? t->text : t->text_dim, 0);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  painting: window chrome                                                 */
/* -------------------------------------------------------------------------- */

static void paint_window(window_t *w) {
    theme_t *t = theme_get();
    wm_rect_t f = w->frame;
    int focused = (w->id == g_wm.focused);
    int i;

    if (w->win_state == WIN_MIN) return;

    /* drop shadow */
    gfx_shadow(f.x, f.y, f.w, f.h, 10, 10, focused ? 190 : 130);

    /* frame background */
    gfx_round_rect(f.x, f.y, f.w, f.h, 10, t->win_bg);

    /* title bar */
    {
        int th = WM_TITLEBAR;
        u32 c0 = focused ? t->win_chrome : t->win_chrome_lo;
        u32 c1 = c0;
        for (i = 0; i < th; i++) {
            u32 cc = RGBA((RED(c0) * (th - i) + RED(c1) * i) / th,
                          (GREEN(c0) * (th - i) + GREEN(c1) * i) / th,
                          (BLUE(c0) * (th - i) + BLUE(c1) * i) / th,
                          ALPHA(c0));
            if (i < 10) {
                int inset = 10 - (int)k_sqrt((double)(100 - (10 - i) * (10 - i)));
                gfx_hline(f.x + inset, f.y + i, f.w - 2 * inset, cc);
            } else {
                gfx_hline(f.x, f.y + i, f.w, cc);
            }
        }
        /* accent line under the title bar */
        gfx_gradient_h(f.x + 10, f.y + th - 1, f.w - 20, 1,
                       focused ? t->accent : RGBA(80, 90, 110, 160),
                       RGBA(0, 0, 0, 0));
    }

    /* title icon + text */
    gfx_draw_icon(w->icon, f.x + 10, f.y + 7, 16);
    {
        char title[128];
        ksnprintf(title, sizeof(title), "%s", w->title);
        gfx_text_clip(f.x + 32, f.y + 7, f.w - 32 - 110, title,
                      focused ? t->text : t->text_dim, 1);
    }

    /* window buttons */
    {
        int bx = f.x + f.w - 3 * 30 - 8;
        int by = f.y + 5, bw = 28, bh = WM_TITLEBAR - 10;
        int hovered;
        hovered = (g_wm.mouse_down && g_wm.hover == w->id &&
                   g_wm.mouse_px >= bx && g_wm.mouse_px < bx + 88 &&
                   g_wm.mouse_py >= by && g_wm.mouse_py < by + bh);
        (void)hovered;
        /* minimize */
        gfx_round_rect(bx, by, bw, bh, 6, RGBA(255, 255, 255, 16));
        gfx_hline(bx + 9, by + bh / 2 + 2, 10, t->text);
        /* maximize */
        gfx_round_rect(bx + 30, by, bw, bh, 6, RGBA(255, 255, 255, 16));
        if (w->win_state == WIN_MAXIMIZED) {
            gfx_frame(bx + 38, by + 8, 9, 9, t->text);
            gfx_frame(bx + 41, by + 11, 9, 9, t->text);
        } else {
            gfx_frame(bx + 39, by + 9, 10, 9, t->text);
        }
        /* close */
        gfx_round_rect(bx + 60, by, bw, bh, 6, RGBA(232, 80, 90, 70));
        gfx_line(bx + 69, by + 9, bx + 79, by + bh - 9, t->text);
        gfx_line(bx + 79, by + 9, bx + 69, by + bh - 9, t->text);
    }

    /* client area */
    {
        wm_rect_t c = wm_win_client_rect(w);
        gfx_set_clip(rect_make(c.x, c.y, c.w, c.h));
        if (w->desc && w->desc->paint && w->state)
            w->desc->paint(w->state, w, c);
        gfx_reset_clip();
    }

    /* border glow */
    gfx_round_rect_border(f.x, f.y, f.w, f.h, 10,
                          focused ? t->border_hi : t->border, 1);
    if (focused) {
        gfx_round_rect_border(f.x - 1, f.y - 1, f.w + 2, f.h + 2, 11,
                              RGBA(RED(t->accent), GREEN(t->accent), BLUE(t->accent), 60), 1);
    }
}

/* -------------------------------------------------------------------------- */
/*  painting: taskbar, tray, start menu                                     */
/* -------------------------------------------------------------------------- */

static void paint_taskbar(void) {
    theme_t *t = theme_get();
    wm_rect_t r = taskbar_rect();
    int i;

    gfx_gradient_v(r.x, r.y, r.w, r.h, RGBA(24, 32, 48, 250), RGBA(10, 14, 22, 252));
    gfx_hline(0, r.y, SCREEN_W, t->border);
    gfx_hline(0, r.y + 1, SCREEN_W, RGBA(255, 255, 255, 12));

    /* start button */
    {
        wm_rect_t b = start_button_rect();
        int hover = g_wm.start_open || (g_wm.mouse_px >= b.x && g_wm.mouse_px < b.x + b.w &&
                                       g_wm.mouse_py >= b.y && g_wm.mouse_py < b.y + b.h);
        gfx_round_rect_grad(b.x, b.y, b.w, b.h, 8,
                            hover ? t->accent_dim : RGBA(46, 62, 88, 255),
                            hover ? RGBA(20, 60, 88, 255) : RGBA(22, 30, 44, 255));
        gfx_round_rect_border(b.x, b.y, b.w, b.h, 8, hover ? t->accent : t->border, 1);
        /* aether glyph: a diamond in a ring */
        {
            int cx = b.x + 20, cy = b.y + b.h / 2;
            gfx_circle(cx, cy, 8, hover ? t->accent : t->text);
            gfx_line(cx, cy - 5, cx + 5, cy, t->accent_dim);
            gfx_line(cx + 5, cy, cx, cy + 5, t->accent_dim);
            gfx_line(cx, cy + 5, cx - 5, cy, t->accent_dim);
            gfx_line(cx - 5, cy, cx, cy - 5, t->accent_dim);
        }
        gfx_text(b.x + 36, b.y + (b.h - gfx_text_h(1)) / 2, "Start", t->text, 1);
    }

    /* running window tabs */
    for (i = 0; i < g_wm.count && i < 7; i++) {
        window_t *w = &g_wm.win[i];
        wm_rect_t tr = task_tab_rect(i);
        int active = (w->id == g_wm.focused && w->win_state != WIN_MIN);
        int hover = (g_wm.mouse_px >= tr.x && g_wm.mouse_px < tr.x + tr.w &&
                     g_wm.mouse_py >= tr.y && g_wm.mouse_py < tr.y + tr.h);
        if (!w->used) continue;
        gfx_round_rect_grad(tr.x, tr.y, tr.w, tr.h, 6,
                            active ? RGBA(44, 70, 104, 255) : (hover ? RGBA(36, 48, 68, 255) : RGBA(24, 32, 46, 255)),
                            active ? RGBA(24, 40, 60, 255) : RGBA(16, 22, 32, 255));
        if (active)
            gfx_round_rect(tr.x + 3, tr.y + tr.h - 3, tr.w - 6, 2, 1, t->accent);
        gfx_draw_icon(w->icon, tr.x + 6, tr.y + (tr.h - 14) / 2, 14);
        gfx_text_clip(tr.x + 26, tr.y + (tr.h - gfx_text_h(0)) / 2, tr.w - 32,
                      w->title, active ? t->text : t->text_dim, 0);
        if (w->win_state == WIN_MIN)
            gfx_hline(tr.x + 26, tr.y + tr.h / 2, tr.w - 32, RGBA(255, 255, 255, 60));
    }

    /* system tray */
    {
        int tx = SCREEN_W - TRAY_W;
        wm_rect_t tray = { tx, r.y, TRAY_W, r.h };
        u32 cpu = proc_cpu_total_percent();
        u32 inodes_used = 0, blocks_used = 0;
        int y0 = tray.y + 8;
        char buf[64];
        vfs_usage(&inodes_used, &blocks_used, NULL);
        gfx_round_rect(tray.x, tray.y + 4, tray.w - 6, tray.h - 8, 7, RGBA(10, 16, 26, 230));
        gfx_round_rect_border(tray.x, tray.y + 4, tray.w - 6, tray.h - 8, 7, RGBA(60, 80, 108, 200), 1);

        /* CPU bar */
        gfx_text(tray.x + 10, y0, "CPU", theme_get()->text_mute, 0);
        gfx_round_rect(tray.x + 42, y0 + 2, 46, 8, 3, RGBA(0, 0, 0, 120));
        gfx_round_rect(tray.x + 42, y0 + 2, 2 + (int)(44 * cpu / 100), 8, 3,
                       cpu > 80 ? theme_get()->err : (cpu > 50 ? theme_get()->warn : theme_get()->ok));
        ksnprintf(buf, sizeof(buf), "%u%%", (unsigned)cpu);
        gfx_text(tray.x + 92, y0, buf, theme_get()->text_dim, 0);

        /* RAM bar */
        gfx_text(tray.x + 122, y0, "RAM", theme_get()->text_mute, 0);
        gfx_round_rect(tray.x + 154, y0 + 2, 46, 8, 3, RGBA(0, 0, 0, 120));
        gfx_round_rect(tray.x + 154, y0 + 2, 2 + (int)(44 * mm_heap_pct() / 100), 8, 3,
                       mm_heap_pct() > 85 ? theme_get()->err : theme_get()->accent);
        /* Show committed KiB: the heap is 31 MiB, so a bare percentage would
           read 0% for a whole session and tell the user nothing. */
        ksnprintf(buf, sizeof(buf), "%uK", (unsigned)(mm_committed() / 1024));
        gfx_text(tray.x + 204, y0, buf, theme_get()->text_dim, 0);

        /* disk activity LED */
        {
            u32 last = vfs_state()->last_activity_tick;
            int active = (irq_ticks() - last) < 180;
            gfx_fill_circle_aa(tray.x + 234, y0 + 6, 4, active ? theme_get()->led : RGBA(80, 40, 40, 255));
            if (active) gfx_fill_circle_aa(tray.x + 234, y0 + 6, 7, RGBA(255, 60, 60, 50));
        }

        /* clock */
        {
            int Y, Mo, D, hh, mi, ss;
            hal_wallclock(&Y, &Mo, &D, &hh, &mi, &ss);
            ksnprintf(buf, sizeof(buf), "%02d:%02d:%02d", hh, mi, ss);
            gfx_text_right(tray.x + tray.w - 12, y0, buf, theme_get()->text, 0);
            ksnprintf(buf, sizeof(buf), "%04d-%02d-%02d  up %us", Y, Mo, D,
                      (unsigned)(irq_ticks() / (irq_hz() ? irq_hz() : 1000)));
            gfx_text_right(tray.x + tray.w - 12, y0 + 14, buf, theme_get()->text_mute, 0);
        }
    }
}

static void paint_start_menu(void) {
    theme_t *t = theme_get();
    wm_rect_t m = start_menu_rect();
    start_item_t items[48];
    int n = start_menu_items(items, 48);
    int i;
    int y;

    gfx_shadow(m.x, m.y, m.w, m.h, 12, 14, 200);
    gfx_round_rect(m.x, m.y, m.w, m.h, 12, RGBA(14, 20, 32, 252));
    gfx_round_rect_border(m.x, m.y, m.w, m.h, 12, t->border_hi, 1);

    /* header */
    gfx_gradient_v(m.x + 1, m.y + 1, m.w - 2, 44, RGBA(40, 62, 92, 255), RGBA(20, 30, 46, 255));
    gfx_draw_icon(1, m.x + 12, m.y + 10, 22);
    gfx_text(m.x + 42, m.y + 8, "AetherOS", t->text, 2);
    gfx_text(m.x + 42, m.y + 27, AETHER_VERSION_STR "  --  " AETHER_CODENAME, t->text_dim, 0);
    (void)n;

    y = m.y + 46;
    if (g_wm.start_page == 0) {
        int list_limit = m.y + m.h - 42;
        int cat;
        static const char *cat_names[4] = { "SYSTEM", "DEVELOPMENT", "GRAPHICS", "UTILITIES" };
        for (cat = 0; cat < 4; cat++) {
            int any = 0;
            for (i = 0; i < APP_COUNT && i < 48; i++) {
                const app_desc_t *d = app_by_index(i);
                if (d->category != cat) continue;
                if (!any) {
                    gfx_text(m.x + 12, y + 4, cat_names[cat], t->text_mute, 0);
                    y += 20;
                    any = 1;
                }
                if (y + 26 > list_limit) break;
                {
                    wm_rect_t ir = { m.x + 8, y, m.w - 16, 26 };
                    int hover = (g_wm.mouse_px >= ir.x && g_wm.mouse_px < ir.x + ir.w &&
                                 g_wm.mouse_py >= ir.y && g_wm.mouse_py < ir.y + ir.h);
                    if (hover) gfx_round_rect(ir.x, ir.y, ir.w, ir.h, 6, t->sel);
                    gfx_draw_icon(d->icon, ir.x + 6, ir.y + 5, 16);
                    gfx_text(ir.x + 30, ir.y + 4, d->name, t->text, 1);
                    gfx_text_right(ir.x + ir.w - 6, ir.y + 5, d->subtitle ? d->subtitle : "",
                                   t->text_mute, 0);
                }
                y += 26;
            }
        }
    } else {
        static const char *labels[4] = { "Reboot OS", "Trigger Kernel Panic", "Shutdown", "Back to programs" };
        int icons[4] = { 4, 5, 7, 11 };
        for (i = 0; i < 4; i++) {
            int hover = (g_wm.mouse_px >= m.x + 10 && g_wm.mouse_px < m.x + m.w - 10 &&
                         g_wm.mouse_py >= y && g_wm.mouse_py < y + 30);
            if (y + 30 > m.y + m.h - 42) break;
            if (hover) gfx_round_rect(m.x + 10, y, m.w - 20, 30, 6, t->sel);
            gfx_draw_icon(icons[i], m.x + 20, y + 7, 16);
            gfx_text(m.x + 46, y + 7, labels[i], t->text, 1);
            y += 34;
        }

    }

    /* footer */
    gfx_hline(m.x + 8, m.y + m.h - 34, m.w - 16, RGBA(255, 255, 255, 18));
    gfx_text(m.x + 12, m.y + m.h - 26, "Uptime", t->text_mute, 0);
    {
        char buf[64];
        u32 s = irq_ticks() / (irq_hz() ? irq_hz() : 1000);
        ksnprintf(buf, sizeof(buf), "%02u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
        gfx_text_right(m.x + m.w - 12, m.y + m.h - 26, buf, t->text_dim, 0);
    }
}

static void paint_toasts(void) {
    int i, y = SCREEN_H - WM_TASKBAR - 8;
    for (i = 0; i < TOAST_MAX; i++) {
        toast_t *tst = toast_slot(i);
        u32 now = now_ms();
        int app = 0;
        char lines[3][64];
        int nl = 0, p = 0, li = 0;
        int bw, h;
        if (!tst) continue;
        if ((i32)(tst->expire_ms - now) < 0) { tst->used = 0; continue; }
        if (tst->expire_ms - now < 500) app = (int)((tst->expire_ms - now) * 255 / 500);
        /* wrap into up to 3 lines of 44 chars */
        {
            int k;
            for (k = 0; k < 3; k++) lines[k][0] = 0;
            for (k = 0; tst->text[k] && nl < 63 && li < 3; k++) {
                lines[li][nl++] = tst->text[k];
                lines[li][nl] = 0;
                if (nl >= 40 && tst->text[k] == ' ') { li++; nl = 0; }
            }
        }
        bw = 300;
        h = 16 + li * 16 + (li ? 16 : 0);
        (void)p;
        y -= h + 6;
        {
            theme_t *t = theme_get();
            u32 col = tst->kind == 1 ? t->ok : tst->kind == 2 ? t->warn : tst->kind == 3 ? t->err : t->accent;
            gfx_shadow(SCREEN_W - bw - 20, y, bw, h, 8, 8, 120);
            gfx_round_rect(SCREEN_W - bw - 20, y, bw, h, 8,
                           RGBA(18, 24, 36, app ? (u32)app : 246));
            gfx_round_rect_border(SCREEN_W - bw - 20, y, bw, h, 8, col, 1);
            gfx_round_rect(SCREEN_W - bw - 20, y, 3, h, 1, col);
            gfx_text_clip(SCREEN_W - bw - 8, y + 6, bw - 20, lines[0], t->text, 0);
            if (li > 0) gfx_text_clip(SCREEN_W - bw - 8, y + 22, bw - 20, lines[1], t->text_dim, 0);
            if (li > 1) gfx_text_clip(SCREEN_W - bw - 8, y + 38, bw - 20, lines[2], t->text_dim, 0);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  the compositor frame                                                    */
/* -------------------------------------------------------------------------- */

static void paint_status_bubble(void) {
    if (!g_wm.status_text[0]) return;
    if ((i32)(g_wm.status_until - now_ms()) < 0) { g_wm.status_text[0] = 0; return; }
    {
        theme_t *t = theme_get();
        int w = gfx_text_w(g_wm.status_text, 1) + 28;
        int x = (SCREEN_W - w) / 2, y = SCREEN_H - WM_TASKBAR - 46;
        gfx_round_rect(x, y, w, 28, 8, RGBA(12, 18, 28, 235));
        gfx_round_rect_border(x, y, w, 28, 8, t->accent, 1);
        gfx_text(x + 14, y + 6, g_wm.status_text, t->text, 1);
    }
}

void wm_render(void) {
    int i;
    theme_t *t = theme_get();
    int mouse_over_start = 0;

    gfx_reset_clip();
    paint_wallpaper();
    paint_desktop_icons();

    /* windows, bottom to top */
    for (i = 0; i < g_wm.count; i++)
        if (g_wm.win[i].used) paint_window(&g_wm.win[i]);

    /* snap preview */
    if (g_wm.snap_preview) {
        wm_rect_t p = { 0, 0, 0, 0 };
        if (g_wm.snap_preview == 1) p = (wm_rect_t){ 0, 0, SCREEN_W / 2, SCREEN_H - WM_TASKBAR };
        else if (g_wm.snap_preview == 2) p = (wm_rect_t){ SCREEN_W / 2, 0, SCREEN_W / 2, SCREEN_H - WM_TASKBAR };
        else if (g_wm.snap_preview == 3) p = (wm_rect_t){ 0, 0, SCREEN_W, SCREEN_H - WM_TASKBAR };
        gfx_round_rect(p.x + 6, p.y + 6, p.w - 12, p.h - 12, 10, RGBA(79, 195, 247, 40));
        gfx_round_rect_border(p.x + 6, p.y + 6, p.w - 12, p.h - 12, 10, t->accent, 2);
    }

    paint_toasts();
    paint_status_bubble();
    paint_taskbar();

    if (g_wm.start_open) {
        paint_start_menu();
        mouse_over_start = 1;
    }

    /* cursor */
    {
        window_t *w = wm_top_at(g_wm.mouse_px, g_wm.mouse_py);
        int kind = CURSOR_ARROW;
        if (!mouse_over_start && w) kind = wm_cursor_for_hit(wm_hit_test(w, g_wm.mouse_px, g_wm.mouse_py));
        else if (!mouse_over_start && g_wm.resize_hover) kind = g_wm.resize_hover;
        gfx_draw_cursor(g_wm.mouse_px, g_wm.mouse_py, kind);
    }
    g_wm.frames++;
}

/* -------------------------------------------------------------------------- */
/*  input handling                                                          */
/* -------------------------------------------------------------------------- */

static void wm_dispatch_mouse(int event, int x, int y, int button, int wheel) {
    window_t *w = wm_top_at(x, y);
    /* apps receive absolute screen coordinates so they can hit-test directly
       against the client rect they were painted with. */
    if (w && w->desc && w->desc->mouse && w->state) {
        g_wm.input_win_id = w->id;
        proc_wake_ticks(w->pid, 6);       /* schedule the app to service the event */
        w->desc->mouse(w->state, event, x, y, button, wheel);
    }
}

static void wm_start_action(int app_index, int action) {
    if (action == 0 && app_index >= 0) {
        const app_desc_t *d = app_by_index(app_index);
        g_wm.start_open = 0;
        if (d) wm_open_app(d, NULL);
        return;
    }
    switch (action) {
    case 1: g_wm.reboot_requested = 1; g_wm.start_open = 0; break;
    case 2: g_wm.crash_requested = 1; g_wm.start_open = 0; break;
    case 3: g_wm.quit_requested = 1; g_wm.start_open = 0; break;
    case 4: g_wm.start_page = 0; break;
    default: break;
    }
}

static void wm_click_start_menu(int x, int y) {
    start_item_t items[48];
    int n = start_menu_items(items, 48);
    int i;
    for (i = 0; i < n; i++) {
        if (x >= items[i].r.x && x < items[i].r.x + items[i].r.w &&
            y >= items[i].r.y && y < items[i].r.y + items[i].r.h) {
            wm_start_action(items[i].app_index, items[i].action);
            return;
        }
    }
    /* corner buttons: title bar area toggles the system page */
    if (y < start_menu_rect().y + 44) {
        g_wm.start_page = g_wm.start_page ? 0 : 1;
        return;
    }
    g_wm.start_open = 0;
}

void wm_on_mouse_move(int x, int y, int buttons) {
    g_wm.mouse_px = x;
    g_wm.mouse_py = y;
    if (buttons & 1) g_wm.mouse_down = 1;

    /* desktop icons */
    {
        int i, hov = 0;
        for (i = 0; i < DESKTOP_ICON_COUNT; i++) {
            wm_rect_t r = desktop_icon_rect(i);
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) { hov = i + 1; break; }
        }
        g_wm.desktop_icon_hover = hov;
    }

    if (g_wm.drag_mode == 1) {
        window_t *w = wm_by_id(g_wm.drag_id);
        if (w) {
            int nx = x - g_wm.drag_dx, ny = y - g_wm.drag_dy;
            w->frame.x = k_clamp(nx, -w->frame.w + 120, SCREEN_W - 120);
            w->frame.y = k_clamp(ny, 0, SCREEN_H - WM_TASKBAR - 40);
            wm_layout_window(w);
            g_wm.snap_preview = 0;
            if (y <= 2) g_wm.snap_preview = 3;
            else if (x <= 2) g_wm.snap_preview = 1;
            else if (x >= SCREEN_W - 3) g_wm.snap_preview = 2;
        }
        return;
    }
    if (g_wm.drag_mode == 2) {
        window_t *w = wm_by_id(g_wm.drag_id);
        if (w) {
            wm_rect_t o = g_wm.drag_origin;
            int nx = o.x, ny = o.y, nw = o.w, nh = o.h;
            int minw = w->desc ? w->desc->min_w : 260;
            int minh = w->desc ? w->desc->min_h : 180;
            if (g_wm.drag_dir & 1) { nx = x; nw = o.x + o.w - x; }
            if (g_wm.drag_dir & 2) { nw = x - o.x; }
            if (g_wm.drag_dir & 4) { ny = y; nh = o.y + o.h - y; }
            if (g_wm.drag_dir & 8) { nh = y - o.y; }
            if (nw < minw) { if (g_wm.drag_dir & 1) nx = o.x + o.w - minw; nw = minw; }
            if (nh < minh) { if (g_wm.drag_dir & 4) ny = o.y + o.h - minh; nh = minh; }
            if (ny < 0) { nh += ny; ny = 0; }
            w->frame.x = nx;
            w->frame.y = ny;
            w->frame.w = nw;
            w->frame.h = nh;
            wm_layout_window(w);
        }
        return;
    }

    {
        window_t *w = wm_top_at(x, y);
        g_wm.hover = w ? w->id : 0;
        g_wm.resize_hover = w ? wm_cursor_for_hit(wm_hit_test(w, x, y)) : 0;
        if (g_wm.resize_hover == CURSOR_ARROW) g_wm.resize_hover = 0;
        if (w) wm_dispatch_mouse(APP_EV_MOUSE_MOVE, x, y, buttons & 1, 0);
    }
    /* hover-driven repaint for the taskbar */
}

void wm_on_mouse_down(int x, int y, int button) {
    window_t *w;
    g_wm.mouse_px = x;
    g_wm.mouse_py = y;
    g_wm.mouse_down = 1;
    g_wm.dblclick_id = 0;

    /* start menu first */
    if (g_wm.start_open) {
        wm_rect_t m = start_menu_rect();
        if (x >= m.x && x < m.x + m.w && y >= m.y && y < m.y + m.h) {
            wm_click_start_menu(x, y);
            return;
        }
        g_wm.start_open = 0;
    }

    /* taskbar */
    if (y >= SCREEN_H - WM_TASKBAR) {
        wm_rect_t sb = start_button_rect();
        if (x >= sb.x && x < sb.x + sb.w && y >= sb.y && y < sb.y + sb.h) {
            g_wm.start_open = !g_wm.start_open;
            g_wm.start_page = 0;
            return;
        }
        {
            int i;
            for (i = 0; i < g_wm.count && i < 7; i++) {
                wm_rect_t tr = task_tab_rect(i);
                if (!g_wm.win[i].used) continue;
                if (x >= tr.x && x < tr.x + tr.w && y >= tr.y && y < tr.y + tr.h) {
                    if (g_wm.win[i].win_state == WIN_MIN) wm_restore(g_wm.win[i].id);
                    else if (g_wm.focused == g_wm.win[i].id) wm_minimize(g_wm.win[i].id);
                    else wm_raise(g_wm.win[i].id);
                    return;
                }
            }
        }
        /* tray clock click -> sysmon */
        if (x >= SCREEN_W - TRAY_W) {
            const app_desc_t *d = app_by_id("sysmon");
            if (d) wm_open_app(d, NULL);
            return;
        }
        return;
    }

    /* desktop icons */
    {
        int i;
        for (i = 0; i < DESKTOP_ICON_COUNT; i++) {
            wm_rect_t r = desktop_icon_rect(i);
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                u32 now = now_ms();
                const app_desc_t *d = app_by_id(g_desktop_icons[i].id);
                g_wm.desktop_selected = i + 1;
                if (wm_is_double_click(i + 1, x, y, now)) {
                    if (d) wm_open_app(d, NULL);
                    g_wm.last_click_id = 0;
                } else {
                    wm_remember_click(i + 1, x, y, now);
                }
                return;
            }
        }
        g_wm.desktop_selected = 0;
    }

    w = wm_top_at(x, y);
    if (!w) return;
    wm_raise(w->id);
    g_wm.focused = w->id;
    {
        pcb_t *p = proc_pcb(w->pid);
        if (p) p->state = PROC_READY;
    }

    {
        int hit = wm_hit_test(w, x, y);
        switch (hit) {
        case HIT_BTN_MIN: wm_minimize(w->id); return;
        case HIT_BTN_MAX: wm_maximize(w->id); return;
        case HIT_BTN_CLOSE: wm_close_window(w->id); return;
        case HIT_TITLE:
            g_wm.drag_mode = 1;
            g_wm.drag_id = w->id;
            g_wm.drag_dx = x - w->frame.x;
            g_wm.drag_dy = y - w->frame.y;
            return;
        case HIT_RESIZE_L: case HIT_RESIZE_R: case HIT_RESIZE_T: case HIT_RESIZE_B:
        case HIT_RESIZE_TL: case HIT_RESIZE_TR: case HIT_RESIZE_BL: case HIT_RESIZE_BR:
            g_wm.drag_mode = 2;
            g_wm.drag_id = w->id;
            g_wm.drag_origin = w->frame;
            g_wm.drag_dir = 0;
            if (hit == HIT_RESIZE_L || hit == HIT_RESIZE_TL || hit == HIT_RESIZE_BL) g_wm.drag_dir |= 1;
            if (hit == HIT_RESIZE_R || hit == HIT_RESIZE_TR || hit == HIT_RESIZE_BR) g_wm.drag_dir |= 2;
            if (hit == HIT_RESIZE_T || hit == HIT_RESIZE_TL || hit == HIT_RESIZE_TR) g_wm.drag_dir |= 4;
            if (hit == HIT_RESIZE_B || hit == HIT_RESIZE_BL || hit == HIT_RESIZE_BR) g_wm.drag_dir |= 8;
            return;
        case HIT_CLIENT: {
            u32 now = now_ms();
            wm_rect_t c = wm_win_client_rect(w);
            if (wm_is_double_click(w->id, x, y, now)) {
                g_wm.dblclick_id = w->id;
                g_wm.dblclick_x = x - c.x;
                g_wm.dblclick_y = y - c.y;
                g_wm.click_count = 2;
                /* a third press starts a fresh sequence rather than
                   re-firing the double-click */
                g_wm.last_click_id = 0;
            } else {
                wm_remember_click(w->id, x, y, now);
                g_wm.click_count = 1;
            }
            if (g_wm.dblclick_id == w->id)
                wm_dispatch_mouse(APP_EV_MOUSE_DBLCLICK, x, y, 0, 0);
            else
                wm_dispatch_mouse(APP_EV_MOUSE_DOWN, x, y, button, 0);
            return;
        }
        default: break;
        }
    }
}

void wm_on_mouse_up(int x, int y, int button) {
    g_wm.mouse_down = 0;
    if (g_wm.drag_mode == 1) {
        window_t *w = wm_by_id(g_wm.drag_id);
        int snap = g_wm.snap_preview;
        if (w && snap) {
            w->restore = w->frame;
            if (snap == 1) w->frame = (wm_rect_t){ 0, 0, SCREEN_W / 2, SCREEN_H - WM_TASKBAR };
            else if (snap == 2) w->frame = (wm_rect_t){ SCREEN_W / 2, 0, SCREEN_W / 2, SCREEN_H - WM_TASKBAR };
            else if (snap == 3) w->frame = (wm_rect_t){ 0, 0, SCREEN_W, SCREEN_H - WM_TASKBAR };
            w->win_state = (snap == 3) ? WIN_MAXIMIZED : WIN_OPEN;
            wm_layout_window(w);
        }
        g_wm.snap_preview = 0;
    }
    if (g_wm.drag_mode != 0) {
        window_t *w = wm_by_id(g_wm.drag_id);
        if (w) wm_dispatch_mouse(APP_EV_MOUSE_UP, x, y, button, 0);
    } else {
        wm_dispatch_mouse(APP_EV_MOUSE_UP, x, y, button, 0);
    }
    g_wm.drag_mode = 0;
    g_wm.drag_id = 0;
}

void wm_on_wheel(int x, int y, int dz) {
    window_t *w = wm_top_at(x, y);
    if (w) wm_dispatch_mouse(APP_EV_WHEEL, x, y, 0, dz);
}

void wm_on_key(int key, int ch, int mods) {
    window_t *w = wm_focused_window();
    if (g_wm.start_open && key == 0x1B) { g_wm.start_open = 0; return; }
    if (!w) {
        if (ch == 't' || ch == 'T') {
            const app_desc_t *d = app_by_id("sh");
            if (d) wm_open_app(d, NULL);
        } else if (key == 0x0D) {
            const app_desc_t *d = app_by_id("sh");
            if (d && !g_wm.count) wm_open_app(d, NULL);
        }
        return;
    }
    if (w->win_state == WIN_MIN) return;
    g_wm.input_win_id = w->id;
    proc_wake_ticks(w->pid, 6);
    if (w->desc && w->desc->key && w->state) w->desc->key(w->state, key, ch, mods);
    /* global shortcuts */
    if (mods & MOD_ALT) {
        if (ch == 'q' || ch == 'Q') wm_close_window(w->id);
        else if (ch == 'm' || ch == 'M') wm_minimize(w->id);
        else if (key == 0x09) wm_raise((g_wm.count > 1) ? g_wm.win[(g_wm.count - 2 + g_wm.count) % g_wm.count].id : w->id);
    }
}

/* -------------------------------------------------------------------------- */
/*  frame update                                                            */
/* -------------------------------------------------------------------------- */

void wm_update(u32 dt_ms) {
    int i;
    /* The compositor redraws every frame, so keep it on the run queue. */
    if (g_compositor_pid > 0) proc_wake_ticks((u32)g_compositor_pid, 4);
    for (i = 0; i < g_wm.count; i++) {
        window_t *w = &g_wm.win[i];
        if (!w->used || !w->desc || !w->desc->tick || !w->win_state) continue;
        if (w->win_state == WIN_MIN) continue;
        w->desc->tick(w->state, dt_ms);
    }
    /* reap windows whose process died */
    for (i = 0; i < g_wm.count; i++) {
        window_t *w = &g_wm.win[i];
        if (!w->used || !w->pid) continue;
        if (!proc_pcb(w->pid)) {
            klog("wm: window %d lost its process, closing", w->id);
            wm_close_window(w->id);
            i = -1;
        }
    }
    /* fps meter */
    g_wm.frame_ms = dt_ms;
    if (dt_ms > 0) g_wm.fps_x100 = 100000u / dt_ms;
}

int wm_take_quit(void) { int r = g_wm.quit_requested; g_wm.quit_requested = 0; return r; }
int wm_take_reboot(void) { int r = g_wm.reboot_requested; g_wm.reboot_requested = 0; return r; }
int wm_take_crash(void) { int r = g_wm.crash_requested; g_wm.crash_requested = 0; return r; }
u32 wm_fps_x100(void) { return g_wm.fps_x100; }
int wm_frame_ms(void) { return (int)g_wm.frame_ms; }
int wm_total_frames(void) { return g_wm.frames; }

void wm_init(void) {
    kmemset(&g_wm, 0, sizeof(g_wm));
    g_wm.next_id = 100;
    g_wm.mouse_px = SCREEN_W / 2;
    g_wm.mouse_py = SCREEN_H / 2;
    g_wm.focused = 0;
    g_wm.start_page = 0;
    g_wm_power_off_ms = 0;
    theme_apply(0);
    g_compositor_pid = (int)proc_spawn("compositor", 1, 24, PROC_FLAG_KERNEL | PROC_FLAG_GUI, 0);
    {
        pcb_t *cp = proc_pcb((u32)g_compositor_pid);
        if (cp) { cp->work_budget = 4; cp->duty_ticks = 6; cp->rest_ticks = 54; cp->flags |= PROC_FLAG_DAEMON; }
    }
    klog("wm: compositor online (pid %d), %d window slots, %d desktop icons",
         g_compositor_pid, WM_MAX_WINDOWS, DESKTOP_ICON_COUNT);
}
