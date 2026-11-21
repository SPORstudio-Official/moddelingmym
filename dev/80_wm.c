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
    int  mouse_right_down;      /* tracked separately from the left button    */
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
    /* context menu (right-click) */
    int  ctx_open;
    int  ctx_kind;              /* CTX_* -- what was right-clicked            */
    int  ctx_target;            /* icon index / window id                    */
    int  ctx_x, ctx_y;          /* top-left of the popup                     */
    int  ctx_hover;             /* hovered item index, -1 none               */
    int  ctx_close_pending;
} wm_state_t;

static wm_state_t g_wm;

static void wm_cycle_focus(int dir);
static void wm_snap_window(int id, int side);

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
    { "browser",  "Browser",   ICON_BROWSER },
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
/*  painting: the backdrop cache                                            */
/* --------------------------------------------------------------------------
 *
 *  The wallpaper is static until the user changes it, but wm_render() redraws
 *  every frame.  Repainting the gradients, grid lines and radial glows from
 *  scratch costs ~23 ms per frame on the ceiling theme -- the single largest
 *  reason the desktop felt laggy, because every window move, keystroke and
 *  cursor pixel paid for a full-screen backdrop.
 *
 *  So the backdrop is rendered once into its own buffer and blitted afterwards.
 *  The cache is invalidated by a generation counter that the theme, wallpaper,
 *  accent and solid-colour setters bump, plus by any desktop resize.
 */
static void paint_wallpaper(void);

static u32  g_backdrop_buf[SCREEN_W * SCREEN_H];
static u32  g_backdrop_gen;          /* bumped to force a rebuild              */
static u32  g_backdrop_built;        /* generation currently in the buffer     */
static int  g_backdrop_valid;

/* Apps and the theme service call this whenever something that feeds the
   wallpaper changes.  Cheap enough to call unconditionally. */
void wm_invalidate_backdrop(void) { g_backdrop_gen++; g_backdrop_valid = 0; }

static void paint_backdrop_cached(void) {
    int y;
    if (!g_backdrop_valid || g_backdrop_built != g_backdrop_gen) {
        /* Render into the cache by temporarily pointing the swap buffer at it. */
        u32 *save = g_vram;
        g_vram = g_backdrop_buf;
        gfx_reset_clip();
        paint_wallpaper();
        g_vram = save;
        g_backdrop_built = g_backdrop_gen;
        g_backdrop_valid = 1;
    }
    /* Blit.  Full-width rows are a straight copy, which the compiler turns into
       a vector move; the clip is respected so a wallpaper redraw inside a
       window's paint does not leak outside its frame. */
    {
        int x0 = g_clip.x, x1 = g_clip.x + g_clip.w;
        int y0 = g_clip.y, y1 = g_clip.y + g_clip.h;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > SCREEN_W) x1 = SCREEN_W;
        if (y1 > SCREEN_H) y1 = SCREEN_H;
        if (x0 >= x1 || y0 >= y1) return;
        for (y = y0; y < y1; y++) {
            kmemcpy(&g_vram[y * VRAM_PITCH + x0], &g_backdrop_buf[y * SCREEN_W + x0],
                    (size_t)(x1 - x0) * sizeof(u32));
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  context menus (right click)                                             */
/* --------------------------------------------------------------------------
 *
 *  A right click opens a popup whose contents depend on what was clicked:
 *  the desktop, a desktop icon, a window title bar, a window client area, or
 *  the taskbar.  The menu is described as data, painted by one routine and
 *  hit-tested by another, so adding an item is a one-line change and the
 *  drawing and clicking can never disagree about where an item is.
 */
#define CTX_MAX_ITEMS 12
#define CTX_ITEM_H    26
#define CTX_W         212
#define CTX_PAD       5

enum { CTX_NONE = 0, CTX_DESKTOP, CTX_ICON, CTX_WINDOW, CTX_TASKBAR };

/* Menu actions.  Kept as an enum so a menu entry can never reference an action
   that no handler implements -- a switch in ctx_run() covers every value. */
enum {
    CTX_ACT_NONE = 0,
    CTX_ACT_SEPARATOR,
    CTX_ACT_OPEN,
    CTX_ACT_NEW_TERMINAL,
    CTX_ACT_NEW_EDITOR,
    CTX_ACT_NEW_FILES,
    CTX_ACT_NEW_BROWSER,
    CTX_ACT_APPEARANCE,
    CTX_ACT_SYSMON,
    CTX_ACT_SHELL_REF,
    CTX_ACT_MINIMIZE,
    CTX_ACT_MAXIMIZE,
    CTX_ACT_RESTORE,
    CTX_ACT_SNAP_LEFT,
    CTX_ACT_SNAP_RIGHT,
    CTX_ACT_CLOSE,
    CTX_ACT_ALWAYS_TOP,
    CTX_ACT_ABOUT,
    CTX_ACT_REFRESH_DESKTOP
};

typedef struct {
    const char *label;
    const char *accel;          /* right-aligned hint, e.g. "Alt+F4"         */
    int  action;                /* CTX_ACT_*, 0 = separator                   */
    int  enabled;
    int  checked;
} ctx_item_t;

static void ctx_items(int kind, ctx_item_t *out, int *n, char *title, int title_cap);

/* Height of the title strip, 0 when the menu has no title. */
static int ctx_title_h(void) {
    return g_wm.ctx_kind == CTX_ICON || g_wm.ctx_kind == CTX_WINDOW ? 24 : 0;
}

/* Pixel rect of menu item i, in screen space.  The title offset belongs here,
   not in the painter: hit-testing and drawing have to agree, and a version that
   added the offset only while painting made every item click one row high. */
static wm_rect_t ctx_item_rect(int i) {
    return (wm_rect_t){ g_wm.ctx_x + 1,
                        g_wm.ctx_y + CTX_PAD + ctx_title_h() + i * CTX_ITEM_H,
                        CTX_W - 2, CTX_ITEM_H };
}

static void ctx_open(int kind, int target, int x, int y) {
    ctx_item_t items[CTX_MAX_ITEMS];
    int n = 0, i, h = 0;
    char title[128];
    ctx_items(kind, items, &n, title, sizeof(title));
    for (i = 0; i < n; i++) h += CTX_ITEM_H;
    h += CTX_PAD * 2 + ctx_title_h();

    /* Keep the popup on screen: flip or clamp rather than letting it hang off
       the edge, where the user could not reach the last items. */
    if (x + CTX_W > SCREEN_W) x = SCREEN_W - CTX_W - 4;
    if (y + h > SCREEN_H - WM_TASKBAR) y = SCREEN_H - WM_TASKBAR - h - 4;
    if (x < 4) x = 4;
    if (y < 4) y = 4;

    g_wm.ctx_open = 1;
    g_wm.ctx_kind = kind;
    g_wm.ctx_target = target;
    g_wm.ctx_x = x;
    g_wm.ctx_y = y;
    g_wm.ctx_hover = -1;
    g_wm.ctx_close_pending = 0;
    g_wm.start_open = 0;
}

static void ctx_close(void) {
    g_wm.ctx_open = 0;
    g_wm.ctx_kind = CTX_NONE;
    g_wm.ctx_hover = -1;
}

/* The contents of each menu. */
static void ctx_items(int kind, ctx_item_t *out, int *n, char *title, int title_cap) {
    int i = 0;
    title[0] = 0;
    switch (kind) {
    case CTX_DESKTOP:
        out[i++] = (ctx_item_t){ "New terminal",      "",         CTX_ACT_NEW_TERMINAL, 1, 0 };
        out[i++] = (ctx_item_t){ "New text editor",   "",         CTX_ACT_NEW_EDITOR,   1, 0 };
        out[i++] = (ctx_item_t){ "New file manager",  "",         CTX_ACT_NEW_FILES,    1, 0 };
        out[i++] = (ctx_item_t){ "New browser",       "",         CTX_ACT_NEW_BROWSER,  1, 0 };
        out[i++] = (ctx_item_t){ "", "", CTX_ACT_SEPARATOR, 0, 0 };
        out[i++] = (ctx_item_t){ "Appearance...",     "",         CTX_ACT_APPEARANCE,   1, 0 };
        out[i++] = (ctx_item_t){ "System monitor",    "",         CTX_ACT_SYSMON,       1, 0 };
        out[i++] = (ctx_item_t){ "Shell reference",   "",         CTX_ACT_SHELL_REF,    1, 0 };
        out[i++] = (ctx_item_t){ "", "", CTX_ACT_SEPARATOR, 0, 0 };
        out[i++] = (ctx_item_t){ "About AE",    "",         CTX_ACT_ABOUT,        1, 0 };
        break;

    case CTX_ICON: {
        const desktop_icon_t *di = (g_wm.ctx_target >= 0 && g_wm.ctx_target < DESKTOP_ICON_COUNT)
                                 ? &g_desktop_icons[g_wm.ctx_target] : 0;
        ksnprintf(title, (size_t)title_cap, "%s", di ? di->label : "Icon");
        out[i++] = (ctx_item_t){ "Open",              "double-click", CTX_ACT_OPEN,        1, 0 };
        out[i++] = (ctx_item_t){ "", "", CTX_ACT_SEPARATOR, 0, 0 };
        out[i++] = (ctx_item_t){ "Appearance...",     "",         CTX_ACT_APPEARANCE,   1, 0 };
        out[i++] = (ctx_item_t){ "System monitor",    "",         CTX_ACT_SYSMON,       1, 0 };
        out[i++] = (ctx_item_t){ "", "", CTX_ACT_SEPARATOR, 0, 0 };
        out[i++] = (ctx_item_t){ "About AE",    "",         CTX_ACT_ABOUT,        1, 0 };
        break;
    }

    case CTX_WINDOW: {
        window_t *w = wm_by_id(g_wm.ctx_target);
        int maxed = w && w->win_state == WIN_MAXIMIZED;
        ksnprintf(title, (size_t)title_cap, "%s", w ? w->title : "Window");
        out[i++] = (ctx_item_t){ "Restore",       "",  CTX_ACT_RESTORE,    maxed, 0 };
        out[i++] = (ctx_item_t){ "Minimize",      "",  CTX_ACT_MINIMIZE,   w != 0, 0 };
        out[i++] = (ctx_item_t){ maxed ? "Unmaximize" : "Maximize", "", CTX_ACT_MAXIMIZE, w != 0, 0 };
        out[i++] = (ctx_item_t){ "", "", CTX_ACT_SEPARATOR, 0, 0 };
        out[i++] = (ctx_item_t){ "Snap left",     "<-", CTX_ACT_SNAP_LEFT,  w != 0, 0 };
        out[i++] = (ctx_item_t){ "Snap right",    "->", CTX_ACT_SNAP_RIGHT, w != 0, 0 };
        out[i++] = (ctx_item_t){ "", "", CTX_ACT_SEPARATOR, 0, 0 };
        out[i++] = (ctx_item_t){ "Close",         "Alt+Q", CTX_ACT_CLOSE,  w != 0, 0 };
        break;
    }

    case CTX_TASKBAR:
        out[i++] = (ctx_item_t){ "New terminal",   "",  CTX_ACT_NEW_TERMINAL, 1, 0 };
        out[i++] = (ctx_item_t){ "System monitor", "",  CTX_ACT_SYSMON,       1, 0 };
        out[i++] = (ctx_item_t){ "", "", CTX_ACT_SEPARATOR, 0, 0 };
        out[i++] = (ctx_item_t){ "Appearance...",  "",  CTX_ACT_APPEARANCE,   1, 0 };
        break;
    default:
        break;
    }
    *n = i;
}

/* Is the popup at (x,y)? */
static int ctx_contains(int x, int y) {
    ctx_item_t items[CTX_MAX_ITEMS];
    int n = 0, h = CTX_PAD * 2, i;
    char title[128];
    if (!g_wm.ctx_open) return 0;
    ctx_items(g_wm.ctx_kind, items, &n, title, sizeof(title));
    for (i = 0; i < n; i++) h += CTX_ITEM_H;
    h += ctx_title_h();
    return x >= g_wm.ctx_x && x < g_wm.ctx_x + CTX_W &&
           y >= g_wm.ctx_y && y < g_wm.ctx_y + h;
}

/* Which item is under (x,y)?  -1 for none, -2 for the popup body. */
static int ctx_hit(int x, int y) {
    ctx_item_t items[CTX_MAX_ITEMS];
    int n = 0, i;
    char title[128];
    if (!ctx_contains(x, y)) return -1;
    ctx_items(g_wm.ctx_kind, items, &n, title, sizeof(title));
    for (i = 0; i < n; i++) {
        wm_rect_t r = ctx_item_rect(i);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
            if (items[i].action == CTX_ACT_SEPARATOR) return -2;
            return items[i].enabled ? i : -2;
        }
    }
    return -2;
}

/* Snap a window to half the desktop.  Shared by the menu and the drag gesture
   so both produce exactly the same geometry. */
static void wm_snap_window(int id, int side) {
    window_t *w = wm_by_id(id);
    if (!w) return;
    w->restore = (w->win_state == WIN_MAXIMIZED || w->win_state == WIN_OPEN) ? w->frame : w->restore;
    if (side == 1) w->frame = (wm_rect_t){ 0, 0, SCREEN_W / 2, SCREEN_H - WM_TASKBAR };
    else           w->frame = (wm_rect_t){ SCREEN_W / 2, 0, SCREEN_W / 2, SCREEN_H - WM_TASKBAR };
    w->win_state = WIN_OPEN;
    wm_layout_window(w);
}

static void ctx_run(int action) {
    int target = g_wm.ctx_target;
    int kind = g_wm.ctx_kind;
    ctx_close();
    switch (action) {
    case CTX_ACT_NONE:
    case CTX_ACT_SEPARATOR:
        break;
    case CTX_ACT_OPEN: {
        if (kind == CTX_ICON && target >= 0 && target < DESKTOP_ICON_COUNT) {
            const app_desc_t *d = app_by_id(g_desktop_icons[target].id);
            if (d) wm_open_app(d, NULL);
        }
        break;
    }
    case CTX_ACT_NEW_TERMINAL: { const app_desc_t *d = app_by_id("sh");       if (d) wm_open_app(d, NULL); break; }
    case CTX_ACT_NEW_EDITOR:   { const app_desc_t *d = app_by_id("edit");     if (d) wm_open_app(d, NULL); break; }
    case CTX_ACT_NEW_FILES:    { const app_desc_t *d = app_by_id("explorer"); if (d) wm_open_app(d, NULL); break; }
    case CTX_ACT_NEW_BROWSER:  { const app_desc_t *d = app_by_id("browser");  if (d) wm_open_app(d, NULL); break; }
    case CTX_ACT_APPEARANCE:   { const app_desc_t *d = app_by_id("settings"); if (d) wm_open_app(d, NULL); break; }
    case CTX_ACT_SYSMON:       { const app_desc_t *d = app_by_id("sysmon");   if (d) wm_open_app(d, NULL); break; }
    case CTX_ACT_SHELL_REF: {
        const app_desc_t *d = app_by_id("browser");
        if (d) wm_open_app(d, "ae://commands");
        break;
    }
    case CTX_ACT_MINIMIZE:   wm_minimize(target); break;
    case CTX_ACT_MAXIMIZE: {
        window_t *w = wm_by_id(target);
        if (w && w->win_state == WIN_MAXIMIZED) wm_restore(target);
        else wm_maximize(target);
        break;
    }
    case CTX_ACT_RESTORE:    wm_restore(target); break;
    case CTX_ACT_SNAP_LEFT:  wm_snap_window(target, 1); break;
    case CTX_ACT_SNAP_RIGHT: wm_snap_window(target, 2); break;
    case CTX_ACT_CLOSE:      wm_close_window(target); break;
    case CTX_ACT_ABOUT: {
        notify_ex("AE " AE_VERSION_STR " (" AE_CODENAME ") -- right-click anywhere", 0);
        break;
    }
    case CTX_ACT_REFRESH_DESKTOP:
        wm_invalidate_backdrop();
        break;
    default:
        break;
    }
}

/* Paint the popup.  Drawn last in wm_render(), so it floats above everything. */
static void paint_context_menu(void) {
    ctx_item_t items[CTX_MAX_ITEMS];
    int n = 0, i, h = CTX_PAD * 2;
    char title[128];
    theme_t *t = theme_get();
    int has_title;

    if (!g_wm.ctx_open) return;
    ctx_items(g_wm.ctx_kind, items, &n, title, sizeof(title));
    has_title = title[0] != 0;
    for (i = 0; i < n; i++) h += CTX_ITEM_H;
    if (has_title) h += ctx_title_h();

    gfx_shadow(g_wm.ctx_x, g_wm.ctx_y, CTX_W, h, 8, 8, 150);
    gfx_round_rect(g_wm.ctx_x, g_wm.ctx_y, CTX_W, h, 8, RGBA(22, 28, 40, 246));
    gfx_round_rect_border(g_wm.ctx_x, g_wm.ctx_y, CTX_W, h, 8, RGBA(90, 130, 175, 180), 1);

    {
        int y = g_wm.ctx_y + CTX_PAD;
        if (has_title) {
            gfx_text_clip(g_wm.ctx_x + 12, y + 4, CTX_W - 24, title, t->text_dim, 0);
            gfx_hline(g_wm.ctx_x + 8, y + 22, CTX_W - 16, RGBA(70, 90, 120, 120));
        }
        for (i = 0; i < n; i++) {
            wm_rect_t r = ctx_item_rect(i);
            if (items[i].action == CTX_ACT_SEPARATOR) {
                gfx_hline(g_wm.ctx_x + 10, r.y + CTX_ITEM_H / 2, CTX_W - 20, RGBA(70, 90, 120, 110));
                continue;
            }
            if (i == g_wm.ctx_hover && items[i].enabled)
                gfx_round_rect(r.x + 3, r.y + 1, r.w - 6, r.h - 2, 5, RGBA(60, 120, 180, 90));
            gfx_text_clip(r.x + 14, r.y + 6, r.w - 60, items[i].label,
                          items[i].enabled ? t->text : t->text_mute, 0);
            if (items[i].accel[0]) {
                gfx_text_right(r.x + r.w - 10, r.y + 6, items[i].accel, t->text_mute, 0);
            }
        }
        (void)y;
    }
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
    case 4: { /* aurora drift */
        gfx_gradient_v(0, 0, SCREEN_W, SCREEN_H - WM_TASKBAR, RGB(4, 12, 16), RGB(8, 30, 34));
        gfx_radial_glow(260, 180, 460, RGBA(40, 220, 170, 70));
        gfx_radial_glow(820, 320, 520, RGBA(40, 140, 230, 60));
        gfx_radial_glow(560, 90, 380, RGBA(150, 60, 230, 45));
        /* vertical curtains */
        for (i = 0; i < SCREEN_W; i += 3) {
            int h = 120 + (int)(88 * k_sin((double)i / 90.0)) + (int)(40 * k_sin((double)i / 31.0));
            int a = 14 + (int)(10 * k_sin((double)i / 140.0));
            if (a < 4) a = 4;
            gfx_vline(i, h, SCREEN_H - WM_TASKBAR, RGBA(40, 200, 170, a));
        }
        for (i = 0; i < 260; i++) {
            int x = (int)(krandom() % SCREEN_W);
            int y = (int)(krandom() % (SCREEN_H - WM_TASKBAR));
            int b = 60 + (int)(krandom() % 180);
            gfx_pixel(x, y, RGBA(b, b, b, b / 2));
        }
        break;
    }
    case 5: { /* solid user colour */
        u32 c = desktop_cfg()->solid;
        /* A subtle vertical shade keeps icons legible on any flat colour. */
        gfx_fill_rect(0, 0, SCREEN_W, SCREEN_H - WM_TASKBAR, c);
        gfx_gradient_v(0, 0, SCREEN_W, SCREEN_H - WM_TASKBAR,
                       RGBA(255, 255, 255, 10), RGBA(0, 0, 0, 46));
        gfx_radial_glow(SCREEN_W / 2, SCREEN_H / 3, 600, RGBA(255, 255, 255, 10));
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
    gfx_text(m.x + 42, m.y + 8, "AE", t->text, 2);
    gfx_text(m.x + 42, m.y + 27, AE_VERSION_STR "  --  " AE_CODENAME, t->text_dim, 0);
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
    paint_backdrop_cached();
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

    /* the context popup floats above the desktop and every window */
    if (g_wm.ctx_open) {
        paint_context_menu();
        mouse_over_start = 1;
    }

    /* cursor */
    {
        window_t *w = wm_top_at(g_wm.mouse_px, g_wm.mouse_py);
        int kind = CURSOR_ARROW;
        if (!mouse_over_start && w) kind = wm_cursor_for_hit(wm_hit_test(w, g_wm.mouse_px, g_wm.mouse_py));
        else if (!mouse_over_start && g_wm.resize_hover) kind = g_wm.resize_hover;
        /* Recorded, not drawn: hal_present() composites it last so the layers
           underneath stay valid between frames. */
        gfx_set_cursor(g_wm.mouse_px, g_wm.mouse_py, kind);
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

    /* An open context menu owns the pointer: hovering highlights an item, and
       nothing underneath receives motion while it is up. */
    if (g_wm.ctx_open) {
        g_wm.ctx_hover = ctx_hit(x, y);
        return;
    }

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
        /* While a button is held the window that received the press keeps
           receiving the moves, even when the pointer leaves its frame: a paint
           stroke or a drag must not stop the moment it crosses an edge. */
        if (g_wm.mouse_down && g_wm.input_win_id) {
            window_t *holder = wm_by_id(g_wm.input_win_id);
            if (holder && holder->used && holder->state) w = holder;
        }
        if (w) wm_dispatch_mouse(APP_EV_MOUSE_MOVE, x, y, buttons & 1, 0);
    }
    /* hover-driven repaint for the taskbar */
}

void wm_on_mouse_down(int x, int y, int button) {
    window_t *w;
    g_wm.mouse_px = x;
    g_wm.mouse_py = y;
    /* Only the left button drives mouse_down.  Setting it for a right press and
       then returning early on the right release left the flag stuck at 1, so the
       next left press was not seen as a fresh press and clicks stopped working
       entirely after opening a context menu. */
    if (button == 1) g_wm.mouse_down = 1;
    g_wm.dblclick_id = 0;

    /* ---- right button: context menu -------------------------------------- */
    if (button == 2) {
        g_wm.mouse_right_down = 1;
        if (g_wm.ctx_open) {
            /* a click inside the popup activates; anywhere else dismisses */
            int item = ctx_hit(x, y);
            if (item >= 0) {
                ctx_item_t items[CTX_MAX_ITEMS];
                int n = 0;
                char title[128];
                ctx_items(g_wm.ctx_kind, items, &n, title, sizeof(title));
                if (item < n) { ctx_run(items[item].action); return; }
            }
            if (item == -2) return;          /* clicked the body / separator  */
            ctx_close();
            /* fall through so the click also acts on whatever is beneath */
        }
        {
            window_t *rw = wm_top_at(x, y);
            /* taskbar */
            if (y >= SCREEN_H - WM_TASKBAR) {
                if (x >= SCREEN_W - TRAY_W - 60) { ctx_open(CTX_TASKBAR, 0, x, y); return; }
                ctx_open(CTX_TASKBAR, 0, x, y);
                return;
            }
            if (rw) {
                int hit = wm_hit_test(rw, x, y);
                if (hit != HIT_NONE) {
                    wm_raise(rw->id);
                    g_wm.focused = rw->id;
                    ctx_open(CTX_WINDOW, rw->id, x, y);
                    return;
                }
            }
            /* desktop icon */
            {
                int i;
                for (i = 0; i < DESKTOP_ICON_COUNT; i++) {
                    wm_rect_t r = desktop_icon_rect(i);
                    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                        g_wm.desktop_selected = i + 1;
                        ctx_open(CTX_ICON, i, x, y);
                        return;
                    }
                }
            }
            g_wm.desktop_selected = 0;
            ctx_open(CTX_DESKTOP, 0, x, y);
            return;
        }
    }

    /* A left click behaves exactly like a right click while a menu is up: an
       item activates, the body is swallowed, and anything outside dismisses.
       Without this the menu only responded to the right button, so a normal
       left click -- what people actually do -- just made it vanish. */
    if (g_wm.ctx_open) {
        int item = ctx_hit(x, y);
        if (item >= 0) {
            ctx_item_t items[CTX_MAX_ITEMS];
            int n = 0;
            char title[128];
            ctx_items(g_wm.ctx_kind, items, &n, title, sizeof(title));
            if (item < n) { ctx_run(items[item].action); return; }
        }
        if (item == -2) return;
        ctx_close();
    }

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

    /* Desktop icons.
       A window that covers an icon must win the click: testing the icons first
       meant that any click inside a window overlapping an icon cell was
       swallowed by the icon underneath (which is why the System Monitor's tab
       strip did not respond while the window was placed over the desktop
       icons).  Topmost window first, then the icon layer. */
    w = wm_top_at(x, y);
    if (!w) {
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
        return;
    }

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
        case HIT_TITLE: {
            /* Double-clicking the title bar maximizes (and restores again),
               the same gesture every other desktop uses.  It has to be tested
               before the drag is armed: pressing twice on a title bar that
               never moves is exactly what a double-click looks like. */
            u32 now = now_ms();
            if (wm_is_double_click(-w->id, x, y, now)) {
                g_wm.last_click_id = 0;
                wm_maximize(w->id);
                return;
            }
            wm_remember_click(-w->id, x, y, now);
            g_wm.drag_mode = 1;
            g_wm.drag_id = w->id;
            g_wm.drag_dx = x - w->frame.x;
            g_wm.drag_dy = y - w->frame.y;
            return;
        }
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
    if (button == 2) {            /* the popup was already handled on press  */
        g_wm.mouse_right_down = 0;
        return;
    }
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

/* Move the highlight in an open popup, skipping separators and disabled items. */
static void ctx_move(int dir) {
    ctx_item_t items[CTX_MAX_ITEMS];
    int n = 0, i, cur = g_wm.ctx_hover;
    char title[128];
    ctx_items(g_wm.ctx_kind, items, &n, title, sizeof(title));
    if (n <= 0) return;
    if (cur < 0) cur = dir > 0 ? -1 : n;
    for (i = 0; i < n; i++) {
        cur += dir;
        if (cur < 0) cur = n - 1;
        if (cur >= n) cur = 0;
        if (items[cur].action != CTX_ACT_SEPARATOR && items[cur].enabled) break;
    }
    g_wm.ctx_hover = cur;
}

void wm_on_key(int key, int ch, int mods) {
    window_t *w = wm_focused_window();

    /* An open context menu takes the keyboard: arrows move, Enter activates,
       Escape dismisses.  This is what makes the menu usable without a mouse. */
    if (g_wm.ctx_open) {
        if (key == 0x1B) { ctx_close(); return; }
        if (key == 0x28 || key == 0x09) { ctx_move(1); return; }
        if (key == 0x26) { ctx_move(-1); return; }
        if (key == 0x0D || key == 0x20) {
            if (g_wm.ctx_hover >= 0) {
                ctx_item_t items[CTX_MAX_ITEMS];
                int n = 0;
                char title[128];
                ctx_items(g_wm.ctx_kind, items, &n, title, sizeof(title));
                if (g_wm.ctx_hover < n) ctx_run(items[g_wm.ctx_hover].action);
            }
            return;
        }
        return;                    /* swallow everything else while it is up */
    }

    if (g_wm.start_open && key == 0x1B) { g_wm.start_open = 0; return; }
    /* System-wide hotkeys that must work even with nothing focused. */

    /* Ctrl+Escape or the Windows key opens the Start menu, as on Windows. */
    if ((key == 0x1B && (mods & MOD_CTRL)) || key == 0x5B || key == 0x5C) {
        g_wm.start_open = !g_wm.start_open;
        g_wm.start_page = 0;
        return;
    }
    /* Alt+Tab cycles to the next window; Alt+Shift+Tab goes backwards. */
    if (key == 0x09 && (mods & MOD_ALT)) {
        wm_cycle_focus((mods & MOD_SHIFT) ? -1 : 1);
        return;
    }
    /* Ctrl+F1 or F1 with nothing focused: keyboard shortcut reference. */
    if (key == 0x70 && (mods & MOD_CTRL)) {
        const app_desc_t *d = app_by_id("browser");
        if (d) wm_open_app(d, "ae://keys");
        return;
    }

    if (!w) {
        if (ch == 't' || ch == 'T') {
            const app_desc_t *d = app_by_id("sh");
            if (d) wm_open_app(d, NULL);
        } else if (key == 0x0D) {
            const app_desc_t *d = app_by_id("sh");
            if (d && !g_wm.count) wm_open_app(d, NULL);
        } else if (ch == 'b' || ch == 'B') {
            const app_desc_t *d = app_by_id("browser");
            if (d) wm_open_app(d, NULL);
        } else if (ch == 'f' || ch == 'F') {
            const app_desc_t *d = app_by_id("explorer");
            if (d) wm_open_app(d, NULL);
        }
        return;
    }
    if (w->win_state == WIN_MIN) return;
    g_wm.input_win_id = w->id;
    proc_wake_ticks(w->pid, 6);
    if (w->desc && w->desc->key && w->state) w->desc->key(w->state, key, ch, mods);
    /* global shortcuts, applied after the app has seen the key */
    if (mods & MOD_ALT) {
        if (ch == 'q' || ch == 'Q') wm_close_window(w->id);
        else if (ch == 'm' || ch == 'M') wm_minimize(w->id);
        else if (key == 0x09) wm_cycle_focus((mods & MOD_SHIFT) ? -1 : 1);
        else if (key == 0x73) wm_close_window(w->id);          /* Alt+F4       */
        else if (key == 0x0D) wm_maximize(w->id);              /* Alt+Enter    */
        else if (key == 0x25) wm_snap_window(w->id, 1);        /* Alt+Left     */
        else if (key == 0x27) wm_snap_window(w->id, 2);        /* Alt+Right    */
    }
}

/* Alt+Tab: raise the next window in z-order and give it focus. */
static void wm_cycle_focus(int dir) {
    int i, n = 0, idx = -1;
    int ids[WM_MAX_WINDOWS];
    if (g_wm.count == 0) return;
    for (i = 0; i < g_wm.count; i++) {
        if (!g_wm.win[i].used || g_wm.win[i].win_state == WIN_MIN) continue;
        ids[n++] = g_wm.win[i].id;
        if (g_wm.win[i].id == g_wm.focused) idx = n - 1;
    }
    if (n == 0) return;
    if (idx < 0) idx = 0;
    idx = (idx + dir + n) % n;
    wm_raise(ids[idx]);
    g_wm.focused = ids[idx];
    {
        pcb_t *p = proc_pcb(wm_by_id(ids[idx]) ? wm_by_id(ids[idx])->pid : 0);
        if (p) p->state = PROC_READY;
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

/* Release every open window before the window table is wiped.  reboot used to
   call wm_init() directly, which zeroed g_wm and left each window's heap state
   allocated for the rest of the session; worse, any code that still held a
   window id from the previous generation could paint through a stale pointer. */
void wm_shutdown(void) {
    int i;
    for (i = g_wm.count - 1; i >= 0; i--) {
        if (!g_wm.win[i].used) continue;
        if (g_wm.win[i].desc && g_wm.win[i].desc->destroy)
            g_wm.win[i].desc->destroy(g_wm.win[i].state);
        g_wm.win[i].state = NULL;
        kmemset(&g_wm.win[i], 0, sizeof(g_wm.win[i]));
    }
    g_wm.count = 0;
    wm_reindex_z();
    g_wm.focused = 0;
    g_wm.ctx_open = 0;
    g_wm.start_open = 0;
    g_wm.drag_mode = 0;
    g_wm.mouse_down = 0;
    g_wm.mouse_right_down = 0;
}

void wm_init(void) {
    kmemset(&g_wm, 0, sizeof(g_wm));
    g_wm.next_id = 100;
    g_wm.mouse_px = SCREEN_W / 2;
    g_wm.mouse_py = SCREEN_H / 2;
    g_wm.focused = 0;
    g_wm.start_page = 0;
    g_wm_power_off_ms = 0;
    /* Restore the user's desktop preferences from /etc/desktop.cfg.  This also
       applies the saved theme, wallpaper, accent colour and CRT setting, so a
       machine comes back exactly as it was left. */
    desktop_cfg_load();
    g_compositor_pid = (int)proc_spawn("compositor", 1, 24, PROC_FLAG_KERNEL | PROC_FLAG_GUI, 0);
    {
        pcb_t *cp = proc_pcb((u32)g_compositor_pid);
        if (cp) { cp->work_budget = 4; cp->duty_ticks = 6; cp->rest_ticks = 54; cp->flags |= PROC_FLAG_DAEMON; }
    }
    klog("wm: compositor online (pid %d), %d window slots, %d desktop icons",
         g_compositor_pid, WM_MAX_WINDOWS, DESKTOP_ICON_COUNT);
}
