/* ==========================================================================
 *  REGISTRY -- theme service, application registry, notifications, clipboard
 * ========================================================================== */

typedef struct { int x, y, w, h; } wm_rect_t;

/* Forward declarations for the window manager (80_wm.c). */
typedef struct window window_t;
struct app_desc;

typedef void (*app_paint_fn)(void *state, window_t *w, wm_rect_t client);
typedef void (*app_key_fn)(void *state, int key, int ch, int mods);
typedef void (*app_mouse_fn)(void *state, int event, int x, int y, int button, int wheel);
typedef void (*app_tick_fn)(void *state, u32 dt_ms);
typedef void (*app_layout_fn)(void *state, wm_rect_t client);

enum {
    APP_EV_MOUSE_MOVE = 0,
    APP_EV_MOUSE_DOWN,
    APP_EV_MOUSE_UP,
    APP_EV_MOUSE_DBLCLICK,
    APP_EV_WHEEL
};

enum { APP_CAT_SYSTEM = 0, APP_CAT_DEV = 1, APP_CAT_GRAPHICS = 2, APP_CAT_UTIL = 3 };

typedef struct app_desc {
    const char *id;             /* "sh", "sysmon", ...                       */
    const char *name;           /* window title / menu label                 */
    const char *subtitle;       /* menu description                          */
    int   icon;                 /* icon_* index (70_gfx)                     */
    int   def_w, def_h;
    int   min_w, min_h;
    int   category;
    int   on_desktop;           /* show a desktop shortcut                   */
    int   single_instance;      /* focus the existing window instead         */
    void *(*create)(const char *arg);
    void  (*destroy)(void *state);
    app_paint_fn paint;
    app_key_fn   key;
    app_mouse_fn mouse;
    app_tick_fn  tick;
    app_layout_fn layout;
} app_desc_t;

/* Each application module exposes one descriptor. */
extern const app_desc_t app_shell;
extern const app_desc_t app_sysmon;
extern const app_desc_t app_explorer;
extern const app_desc_t app_edit;
extern const app_desc_t app_hexview;
extern const app_desc_t app_paint;
extern const app_desc_t app_settings;
extern const app_desc_t app_calc;
extern const app_desc_t app_console;
extern const app_desc_t app_browser;

static const app_desc_t *const g_app_table[] = {
    &app_shell, &app_explorer, &app_edit, &app_sysmon,
    &app_hexview, &app_paint, &app_settings, &app_calc, &app_console, &app_browser
};
#define APP_COUNT ((int)(sizeof(g_app_table) / sizeof(g_app_table[0])))

const app_desc_t *app_by_id(const char *id) {
    int i;
    for (i = 0; i < APP_COUNT; i++)
        if (!kstrcmp(g_app_table[i]->id, id)) return g_app_table[i];
    return NULL;
}

const app_desc_t *app_by_index(int i) {
    if (i < 0 || i >= APP_COUNT) return NULL;
    return g_app_table[i];
}

/* -------------------------------------------------------------------------- */
/*  Themes                                                                   */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    u32 accent, accent_dim;
    u32 bg0, bg1;               /* desktop gradient                          */
    u32 win_bg, win_chrome, win_chrome_lo;
    u32 panel, panel_hi, panel_lo;
    u32 border, border_hi;
    u32 text, text_dim, text_mute;
    u32 ok, warn, err, info;
    u32 sel, sel_dim;
    u32 led;
} theme_t;

static theme_t g_themes[4];
static int g_theme_index;
static theme_t g_theme;
static int g_theme_ready;
static int g_scanlines;
static int g_crt_enabled;

static void theme_build(void) {
    /* 0 -- Midnight (default): deep blue glass */
    g_themes[0].name = "Midnight Glass";
    g_themes[0].accent = RGB(79, 195, 247);
    g_themes[0].accent_dim = RGB(41, 121, 168);
    g_themes[0].bg0 = RGB(9, 14, 26);
    g_themes[0].bg1 = RGB(22, 32, 52);
    g_themes[0].win_bg = RGBA(16, 22, 34, 246);
    g_themes[0].win_chrome = RGBA(30, 42, 62, 250);
    g_themes[0].win_chrome_lo = RGBA(20, 28, 44, 250);
    g_themes[0].panel = RGBA(12, 17, 28, 244);
    g_themes[0].panel_hi = RGBA(44, 60, 86, 255);
    g_themes[0].panel_lo = RGBA(6, 9, 16, 255);
    g_themes[0].border = RGBA(70, 96, 132, 230);
    g_themes[0].border_hi = RGBA(120, 170, 210, 255);
    g_themes[0].text = RGB(230, 238, 248);
    g_themes[0].text_dim = RGB(150, 166, 186);
    g_themes[0].text_mute = RGB(96, 110, 130);
    g_themes[0].ok = RGB(96, 220, 150);
    g_themes[0].warn = RGB(250, 196, 92);
    g_themes[0].err = RGB(248, 104, 112);
    g_themes[0].info = RGB(112, 190, 255);
    g_themes[0].sel = RGBA(79, 195, 247, 90);
    g_themes[0].sel_dim = RGBA(79, 195, 247, 34);
    g_themes[0].led = RGB(255, 96, 96);

    /* 1 -- Amber terminal */
    g_themes[1] = g_themes[0];
    g_themes[1].name = "Amber Phosphor";
    g_themes[1].accent = RGB(255, 176, 64);
    g_themes[1].accent_dim = RGB(140, 92, 24);
    g_themes[1].bg0 = RGB(16, 12, 8);
    g_themes[1].bg1 = RGB(34, 24, 12);
    g_themes[1].win_bg = RGBA(20, 16, 12, 246);
    g_themes[1].win_chrome = RGBA(40, 30, 18, 250);
    g_themes[1].panel = RGBA(16, 12, 8, 244);
    g_themes[1].text = RGB(255, 226, 178);
    g_themes[1].text_dim = RGB(190, 150, 96);
    g_themes[1].border = RGBA(120, 88, 40, 230);
    g_themes[1].border_hi = RGB(255, 190, 90);

    /* 2 -- Slate (classic unix) */
    g_themes[2] = g_themes[0];
    g_themes[2].name = "Unix Slate";
    g_themes[2].accent = RGB(190, 205, 220);
    g_themes[2].accent_dim = RGB(110, 122, 138);
    g_themes[2].bg0 = RGB(36, 38, 42);
    g_themes[2].bg1 = RGB(56, 60, 66);
    g_themes[2].win_bg = RGBA(30, 32, 36, 250);
    g_themes[2].win_chrome = RGBA(58, 62, 68, 252);
    g_themes[2].panel = RGBA(26, 28, 32, 248);
    g_themes[2].text = RGB(226, 230, 236);
    g_themes[2].text_dim = RGB(160, 168, 178);
    g_themes[2].border = RGBA(96, 102, 112, 235);
    g_themes[2].border_hi = RGB(200, 210, 222);
    g_themes[2].sel = RGBA(190, 205, 220, 80);

    /* 3 -- Synthwave */
    g_themes[3] = g_themes[0];
    g_themes[3].name = "Synthwave";
    g_themes[3].accent = RGB(255, 92, 168);
    g_themes[3].accent_dim = RGB(150, 40, 110);
    g_themes[3].bg0 = RGB(24, 8, 34);
    g_themes[3].bg1 = RGB(56, 18, 68);
    g_themes[3].win_bg = RGBA(28, 12, 38, 246);
    g_themes[3].win_chrome = RGBA(56, 20, 68, 250);
    g_themes[3].panel = RGBA(22, 10, 32, 246);
    g_themes[3].text = RGB(255, 226, 246);
    g_themes[3].text_dim = RGB(206, 150, 200);
    g_themes[3].border = RGBA(150, 60, 140, 230);
    g_themes[3].border_hi = RGB(255, 120, 200);
    g_themes[3].sel = RGBA(255, 92, 168, 90);
}

void theme_apply(int index) {
    if (!g_theme_ready) { theme_build(); g_theme_ready = 1; }
    if (index < 0 || index > 3) index = 0;
    g_theme_index = index;
    g_theme = g_themes[index];
}

theme_t *theme_get(void) { return &g_theme; }
int theme_index(void) { return g_theme_index; }
const char *theme_name(int i) {
    if (!g_theme_ready) { theme_build(); g_theme_ready = 1; }
    if (i < 0 || i > 3) return "";
    return g_themes[i].name;
}

int  gfx_scanlines_enabled(void) { return g_crt_enabled; }
void gfx_set_scanlines(int on) { g_crt_enabled = on ? 1 : 0; }
int  gfx_scanlines_state(void) { return g_scanlines; }
void gfx_set_scanline_level(int level) { g_scanlines = k_clamp(level, 0, 3); }

/* Wallpaper selection.  Six built-ins, and the last one is a flat colour the
   user picks, so the desktop can be made to look like nothing else in the box. */
#define WALLPAPER_COUNT 6
static int g_wallpaper = 1;
int  wallpaper_index(void) { return g_wallpaper; }
void wallpaper_set(int i) { g_wallpaper = k_clamp(i, 0, WALLPAPER_COUNT - 1); }
int  wallpaper_count(void) { return WALLPAPER_COUNT; }
const char *wallpaper_name(int i) {
    static const char *n[WALLPAPER_COUNT] = {
        "Cyber Grid", "Deep Space Nebula", "Classic Unix Slate",
        "Sunset Synthwave", "Aurora Drift", "Solid Colour"
    };
    if (i < 0 || i >= WALLPAPER_COUNT) return "";
    return n[i];
}

/* ---- customisation state ------------------------------------------------
 *  Everything the user can change is kept in one struct and saved to
 *  /etc/desktop.cfg on a single line, so the settings survive a reboot and can
 *  be inspected or edited by hand from the shell (`cat /etc/desktop.cfg`).
 * ---------------------------------------------------------------------- */
typedef struct {
    int theme;              /* 0..THEME_COUNT-1                            */
    int wallpaper;          /* 0..WALLPAPER_COUNT-1                        */
    u32 solid;              /* custom wallpaper colour (0xRRGGBB)          */
    int accent;             /* 0 = from theme, 1 = custom                  */
    u32 accent_color;
    int font_scale;         /* 0 compact, 1 normal, 2 large                */
    int crt;                /* scanline overlay                            */
    int taskbar_h;
    int transparency;       /* 0..100, window alpha                        */
    int rounded;            /* corner radius scale, 0..2                   */
} desktop_cfg_t;

static desktop_cfg_t g_cfg = { 0, 1, 0x101828u, 0, 0, 1, 0, 0, 88, 1 };
static int g_cfg_loaded;

void desktop_cfg_load(void);
void desktop_cfg_save(void);

desktop_cfg_t *desktop_cfg(void) {
    if (!g_cfg_loaded) desktop_cfg_load();
    return &g_cfg;
}

/* Parse "key=value" pairs separated by ';' from /etc/desktop.cfg. */
void desktop_cfg_load(void) {
    char buf[512];
    int n;
    g_cfg_loaded = 1;
    {
        u32 out_len = 0;
        if (vfs_read_text("/etc/desktop.cfg", buf, sizeof(buf) - 1, &out_len) != 0) return;
        n = (int)out_len;
        buf[n] = 0;
    }
    {
        char *p = buf;
        while (*p) {
            char key[32];
            int kn = 0, val = 0, neg = 0;
            while (*p == ' ' || *p == '\n' || *p == ';') p++;
            if (!*p) break;
            while (*p && *p != '=' && *p != ';' && kn < 31) key[kn++] = *p++;
            key[kn] = 0;
            if (*p == '=') p++;
            if (*p == '-') { neg = 1; p++; }
            while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
            while (*p && *p != ';') p++;
            if (neg) val = -val;
            if (!kstrcmp(key, "theme"))      g_cfg.theme = k_clamp(val, 0, 3);
            else if (!kstrcmp(key, "wallpaper")) g_cfg.wallpaper = k_clamp(val, 0, WALLPAPER_COUNT - 1);
            else if (!kstrcmp(key, "solid"))     g_cfg.solid = (u32)val;
            else if (!kstrcmp(key, "accent"))    g_cfg.accent = val ? 1 : 0;
            else if (!kstrcmp(key, "accent_color")) g_cfg.accent_color = (u32)val;
            else if (!kstrcmp(key, "font_scale")) g_cfg.font_scale = k_clamp(val, 0, 2);
            else if (!kstrcmp(key, "crt"))       g_cfg.crt = val ? 1 : 0;
            else if (!kstrcmp(key, "taskbar_h")) g_cfg.taskbar_h = k_clamp(val, 0, 40);
            else if (!kstrcmp(key, "transparency")) g_cfg.transparency = k_clamp(val, 0, 100);
            else if (!kstrcmp(key, "rounded"))   g_cfg.rounded = k_clamp(val, 0, 2);
        }
    }
    /* apply what affects global state immediately */
    theme_apply(g_cfg.theme);
    wallpaper_set(g_cfg.wallpaper);
    gfx_set_scanlines(g_cfg.crt);
    if (g_cfg.accent) theme_get()->accent = g_cfg.accent_color;
}

void desktop_cfg_save(void) {
    char buf[512];
    ksnprintf(buf, sizeof(buf),
              "theme=%d;wallpaper=%d;solid=%u;accent=%d;accent_color=%u;"
              "font_scale=%d;crt=%d;taskbar_h=%d;transparency=%d;rounded=%d;",
              g_cfg.theme, g_cfg.wallpaper, (unsigned)g_cfg.solid,
              g_cfg.accent, (unsigned)g_cfg.accent_color,
              g_cfg.font_scale, g_cfg.crt, g_cfg.taskbar_h,
              g_cfg.transparency, g_cfg.rounded);
    vfs_write_string("/etc/desktop.cfg", buf);
}

/* Convenience setters used by the Control Panel and the shell, each of which
   saves immediately so a change is never lost. */
void desktop_set_theme(int i) { desktop_cfg()->theme = k_clamp(i, 0, 3); theme_apply(g_cfg.theme); desktop_cfg_save(); }
void desktop_set_wallpaper(int i) { desktop_cfg()->wallpaper = k_clamp(i, 0, WALLPAPER_COUNT - 1); wallpaper_set(g_cfg.wallpaper); desktop_cfg_save(); }
void desktop_set_solid(u32 c) { desktop_cfg()->solid = c; desktop_cfg()->wallpaper = WALLPAPER_COUNT - 1; wallpaper_set(g_cfg.wallpaper); desktop_cfg_save(); }
void desktop_set_accent(int on, u32 c) { desktop_cfg()->accent = on; desktop_cfg()->accent_color = c; if (on) theme_get()->accent = c; else theme_apply(g_cfg.theme); desktop_cfg_save(); }
void desktop_set_crt(int on) { desktop_cfg()->crt = on ? 1 : 0; gfx_set_scanlines(g_cfg.crt); desktop_cfg_save(); }
void desktop_set_font_scale(int i) { desktop_cfg()->font_scale = k_clamp(i, 0, 2); desktop_cfg_save(); }
void desktop_set_transparency(int v) { desktop_cfg()->transparency = k_clamp(v, 0, 100); desktop_cfg_save(); }
void desktop_reset(void) {
    g_cfg.theme = 0; g_cfg.wallpaper = 1; g_cfg.solid = 0x101828u;
    g_cfg.accent = 0; g_cfg.accent_color = 0x4FC3F7u; g_cfg.font_scale = 1;
    g_cfg.crt = 0; g_cfg.taskbar_h = 0; g_cfg.transparency = 88; g_cfg.rounded = 1;
    theme_apply(0); wallpaper_set(1); gfx_set_scanlines(0);
    desktop_cfg_save();
}

/* -------------------------------------------------------------------------- */
/*  Notifications (toast messages drawn by the desktop)                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    char text[128];
    u32  expire_ms;
    int  kind;              /* 0 info, 1 ok, 2 warn, 3 error               */
    int  used;
} toast_t;

#define TOAST_MAX 6
static toast_t g_toasts[TOAST_MAX];

void notify_ex(const char *text, int kind) {
    int i, slot = -1;
    for (i = 0; i < TOAST_MAX; i++) {
        if (!g_toasts[i].used) { slot = i; break; }
    }
    if (slot < 0) slot = 0;
    kstrncpy(g_toasts[slot].text, text, sizeof(g_toasts[slot].text) - 1);
    g_toasts[slot].expire_ms = now_ms() + 4200;
    g_toasts[slot].kind = kind;
    g_toasts[slot].used = 1;
}

void notify(const char *text) { notify_ex(text, 0); }
toast_t *toast_slot(int i) {
    if (i < 0 || i >= TOAST_MAX) return NULL;
    return g_toasts[i].used ? &g_toasts[i] : NULL;
}

/* -------------------------------------------------------------------------- */
/*  Clipboard                                                                */
/* -------------------------------------------------------------------------- */

static char g_clipboard[8192];
void clipboard_set(const char *s) { kstrncpy(g_clipboard, s ? s : "", sizeof(g_clipboard) - 1); }
const char *clipboard_get(void) { return g_clipboard; }
