/* ==========================================================================
 *  HAL -- the "bare metal chassis".
 *
 *  On Windows this file is the *only* place that talks to Win32.  It provides
 *  exactly what a PC motherboard provides to an OS kernel:
 *
 *    - a contiguous physical RAM block            (64 MB)
 *    - a linear 32-bit ARGB framebuffer          (1280x720 DIB section)
 *    - a monotonic hardware timer                (~1 kHz)
 *    - raw keyboard / mouse hardware events      (no widgets, no controls)
 *
 *  Nothing above this layer ever calls a Win32 UI API.  All desktop content is
 *  produced by the AE compositor.
 * ========================================================================== */

/* 40_gfx.c draws the software pointer; the HAL composites it as the final step
   of a present so it never dirties the layers underneath. */
void gfx_composite_cursor(void);
void gfx_cursor_restore(void);
/* Allocator, declared here because the Win32 network chassis needs it before
   50_mm.c is concatenated.  The signatures must match 50_mm.c exactly (kfree
   returns int there, and the bytecode VM uses that return value). */
void *mm_kmalloc(u32 size);
int   mm_kfree(void *p);

typedef struct {
    int  type;          /* HAL_EV_*                                            */
    int  key;           /* virtual key code (keyboard)                         */
    int  ch;            /* translated character, 0 if none                     */
    int  mx, my;        /* mouse position in client pixels                     */
    int  wheel;         /* wheel delta (multiples of 120)                      */
    int  buttons;       /* bitmask: 1 left, 2 right, 4 middle                  */
    int  repeat;        /* key auto-repeat flag                                */
    u32  time_ms;
} hal_event_t;

enum {
    HAL_EV_NONE = 0,
    HAL_EV_KEYDOWN,
    HAL_EV_KEYUP,
    HAL_EV_CHAR,
    HAL_EV_MOUSEMOVE,
    HAL_EV_MOUSEDOWN,
    HAL_EV_MOUSEUP,
    HAL_EV_WHEEL,
    HAL_EV_FOCUS,
    HAL_EV_QUIT
};

/* physical RAM lives here (single contiguous block) */
u8 *g_phys_ram = NULL;
u32 *g_vram = NULL;

static u64 g_host_t0;
static int g_hal_running;
#if defined(AETHER_ON_WIN32) || defined(AETHER_WASM)
static u64 g_host_freq;
#endif
#ifdef AETHER_ON_WIN32
static int g_hal_mouse_buttons;
#endif

machine_t g_machine;

/* -------------------------------------------------------------------------- */
/*  WebAssembly chassis                                                       */
/*                                                                            */
/*  The module owns its own linear memory, and physical RAM is simply a slice  */
/*  of it: there is no VirtualAlloc to call, so the memory *is* the machine's   */
/*  address space.  The host provides the clock, presents the framebuffer to a */
/*  canvas and collects input; everything else is the kernel's own code.       */
/* -------------------------------------------------------------------------- */
#if defined(AETHER_WASM)

/* Grow linear memory by `pages` and return the byte address of the new region.
   memory.grow yields the previous page count, so the start of the fresh block is
   exactly that many pages into the address space. */
static void *hal_wasm_alloc_pages(u32 pages) {
    u32 prev = (u32)__builtin_wasm_memory_grow(0, (size_t)pages);
    if (prev == 0xFFFFFFFFu) return NULL;
    return (void *)(size_t)((u64)prev * 65536ULL);
}

#define HAL_QUEUE_MAX 256
static hal_event_t g_evq[HAL_QUEUE_MAX];
static int g_ev_head, g_ev_tail;
static int g_wasm_mouse_buttons;

/* The event struct crosses the boundary as raw little-endian bytes, so the
   shell only needs to agree on the field offsets.  Layout on wasm32:
   9 x 4-byte fields = 36 bytes (type, key, ch, mx, my, wheel, buttons,
   repeat, time_ms).

   A compiled-in assertion catches any future edit to hal_event_t that would
   silently desynchronise the shell's writer. */
#define HAL_EVENT_BYTES 36
typedef char hal_event_size_check[(sizeof(hal_event_t) == HAL_EVENT_BYTES) ? 1 : -1];

static void ev_push(const hal_event_t *e) {
    int next = (g_ev_head + 1) % HAL_QUEUE_MAX;
    if (next == g_ev_tail) return;              /* queue full: drop */
    g_evq[g_ev_head] = *e;
    g_ev_head = next;
}

int hal_init(void) {
    g_wasm_mouse_buttons = 0;
    g_ev_head = g_ev_tail = 0;

    /* Physical RAM: a stable 64 MB region of the module's linear memory.  It is
       zeroed by the wasm runtime when the memory is first grown, so unlike the
       Win32 chassis there is nothing to memset.

       memory.grow returns the *previous page count* (or -1), not an address, so
       the base of the new region is that count times the 64 KiB page size. */
    g_phys_ram = hal_wasm_alloc_pages(PHYS_RAM_SIZE / 65536);
    if (!g_phys_ram) return -1;

    /* Framebuffer: plain ARGB pixels, one u32 per pixel, drawn by the host. */
    g_vram = (u32 *)hal_wasm_alloc_pages((VRAM_PITCH * SCREEN_H * 4 + 65535) / 65536);
    if (!g_vram) return -1;

    g_host_t0 = js_now_us();
    g_host_freq = 1000000ULL;                   /* js_now_us is microseconds */
    g_hal_running = 1;

    kmemset(&g_machine, 0, sizeof(g_machine));
    g_machine.magic = 0x41455448u;
    g_machine.width = SCREEN_W;
    g_machine.height = SCREEN_H;
    g_machine.pitch = VRAM_PITCH;
    g_machine.bpp = 32;
    g_machine.total_ram = PHYS_RAM_SIZE;
    g_machine.kernel_bytes = KERNEL_SIZE;
    g_machine.heap_bytes = HEAP_SIZE;
    g_machine.disk_bytes = DISK_SIZE;
    g_machine.cpu_mhz = 2400;
    kstrcpy(g_machine.cpu_name, "Aether Virtual CPU (webassembly)");
    kstrcpy(g_machine.host_os, "web");
    js_set_title("AE");
    return 0;
}

void hal_shutdown(void) {
    g_hal_running = 0;
}

int  hal_running(void) { return g_hal_running; }
u32 *hal_vram(void) { return g_vram; }

void hal_set_title(const char *t) { js_set_title(t); }
void hal_open_external(const char *url) { js_open_external(url); }

/* A wasm module must not block its host: the shell already paces frames with
   requestAnimationFrame, so sleeping is a no-op here. */
void hal_sleep_ms(int ms) { (void)ms; }

/* Presentation is a single call into the shell, which copies the framebuffer
   into the canvas.  The damaged rect is passed through so the shell can upload
   only the rows that changed -- copying the whole 3.5 MB surface every frame was
   the single most expensive thing the browser build did. */
void hal_present(int x, int y, int w, int h) {
    gfx_composite_cursor();    /* the pointer is the last thing on the screen */
    wm_cursor_presented();
    js_present_rect(x, y, w, h);
}

void hal_flush_all(void) { gfx_composite_cursor(); wm_cursor_presented(); js_present(); }
void hal_frame_boundary(u32 frame) { (void)frame; }

/* The desktop draws its own software pointer, so the host pointer stays put. */
void hal_capture_mouse(int on) { (void)on; }
void hal_get_native_mouse(int *x, int *y) { if (x) *x = 0; if (y) *y = 0; }
void hal_set_native_mouse(int x, int y) { (void)x; (void)y; }

void hal_beep(int freq, int ms) { js_beep(freq, ms); }
void hal_request_quit(void) { g_hal_running = 0; js_request_quit(); }

int hal_poll_event(hal_event_t *ev) {
    if (g_ev_tail == g_ev_head) {
        /* Nothing queued: ask the shell to decode its pending DOM events.  It
           fills exactly one hal_event_t and returns 1 if it produced one. */
        hal_event_t incoming;
        if (!js_poll_event(&incoming)) return 0;
        if (incoming.type == HAL_EV_NONE) return 0;
        *ev = incoming;
        return 1;
    }
    *ev = g_evq[g_ev_tail];
    g_ev_tail = (g_ev_tail + 1) % HAL_QUEUE_MAX;
    return 1;
}

void hal_wallclock(int *y, int *mo, int *d, int *hh, int *mm2, int *ss) {
    js_wallclock(y, mo, d, hh, mm2, ss);
}

void hal_dump_ppm(const char *path) { (void)path; }

/* -------------------------------------------------------------------------- */
/*  Headless (CI) implementation                                              */
/* -------------------------------------------------------------------------- */
#elif defined(AETHER_HEADLESS)

#include <time.h>
/* Declared by hand so <unistd.h> (which declares a conflicting host-level
 * syscall() prototype) is not pulled into this translation unit. */
extern int usleep(unsigned int usec);

u64 host_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000ULL + (u64)(ts.tv_nsec / 1000);
}

static FILE *g_script = NULL;
void hal_dump_ppm(const char *path);
/* Called once per presented frame; headless builds use it to honour the
   AETHER_DUMP_AT schedule, Win32 builds do nothing. */
void hal_frame_boundary(u32 frame);

/* ---- headless schedule ------------------------------------------------- */
static int  g_dump_frame[16];
static char g_dump_path[16][128];
static int  g_dump_count;
static int  g_dump_parsed;

static void dump_schedule_parse(void) {
    const char *spec;
    g_dump_parsed = 1;
    spec = getenv("AETHER_DUMP_AT");
    if (!spec) return;
    while (*spec && g_dump_count < 16) {
        int frame = 0;
        char path[128];
        int n = 0;
        while (*spec >= '0' && *spec <= '9') { frame = frame * 10 + (*spec - '0'); spec++; }
        if (*spec != ':') { while (*spec && *spec != ',') spec++; if (*spec) spec++; continue; }
        spec++;
        while (*spec && *spec != ',' && n < 127) path[n++] = *spec++;
        path[n] = 0;
        g_dump_frame[g_dump_count] = frame;
        kstrcpy(g_dump_path[g_dump_count], path);
        g_dump_count++;
        if (*spec == ',') spec++;
    }
}

static u32 g_dbg_max_frame;

void hal_frame_boundary(u32 frame) {
    int i;
    if (frame > g_dbg_max_frame) g_dbg_max_frame = frame;
    if (!g_dump_parsed) dump_schedule_parse();
    for (i = 0; i < g_dump_count; i++) {
        if ((u32)g_dump_frame[i] == frame) {
            g_dump_frame[i] = -1;
            gfx_composite_cursor();
            wm_cursor_presented();
            hal_dump_ppm(g_dump_path[i]);
        }
    }
}

int hal_init(void) {
    g_phys_ram = (u8 *)calloc(PHYS_RAM_SIZE, 1);
    g_vram = (u32 *)calloc((size_t)VRAM_PITCH * SCREEN_H, 4);
    if (!g_phys_ram || !g_vram) return -1;
    g_host_t0 = now_us();
    g_hal_running = 1;
    const char *sc = getenv("AETHER_SCRIPT");
    if (sc) g_script = fopen(sc, "r");
    kmemset(&g_machine, 0, sizeof(g_machine));
    g_machine.magic = 0x41455448u;
    g_machine.width = SCREEN_W;
    g_machine.height = SCREEN_H;
    g_machine.pitch = VRAM_PITCH;
    g_machine.bpp = 32;
    g_machine.total_ram = PHYS_RAM_SIZE;
    g_machine.kernel_bytes = KERNEL_SIZE;
    g_machine.heap_bytes = HEAP_SIZE;
    g_machine.disk_bytes = DISK_SIZE;
    g_machine.cpu_mhz = 2400;
    kstrcpy(g_machine.cpu_name, "Aether Virtual CPU (headless)");
    kstrcpy(g_machine.host_os, "linux");
    return 0;
}

void hal_shutdown(void) {
    fprintf(stderr, "[hal] headless run ended after %u frames\n", (unsigned)g_dbg_max_frame);
    g_hal_running = 0;
}
int  hal_running(void) { return g_hal_running; }
u32 *hal_vram(void) { return g_vram; }
void hal_set_title(const char *t) { (void)t; }
void hal_open_external(const char *url) {
    /* No host browser in a headless run: record it, which is also how the
       test harness asserts that the escape hatch was taken. */
    fprintf(stderr, "[hal] open external: %s\n", url ? url : "(null)");
}
void hal_sleep_ms(int ms) { usleep((unsigned int)(ms < 0 ? 0 : ms) * 1000u); }
void hal_present(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void hal_flush_all(void) {}
void hal_capture_mouse(int on) { (void)on; }
void hal_get_native_mouse(int *x, int *y) { *x = 0; *y = 0; }
void hal_set_native_mouse(int x, int y) { (void)x; (void)y; }
void hal_beep(int freq, int ms) { (void)freq; (void)ms; }
void hal_request_quit(void) { g_hal_running = 0; }

/* Script line: "<ms> <event> <args...>", for example "120 mousemove 640 500".
 * Lines are consumed only once the emulated machine has been running for the
 * given number of milliseconds; until then poll returns 0 so the frame loop
 * keeps compositing.  Supported verbs: key, keydown, keyup, mousemove, down,
 * up, click, wheel, dump <path>, sleep <ms>, quit. */
static char g_pending[256];
static int  g_pending_valid;
/* The script's 'mousemove' verb carries no button information, but a real
   pointer keeps whatever button was held down while it moves.  Tracking that
   here is what makes dragging possible in a scripted session (paint strokes,
   window moves, selections). */
static int  g_script_buttons;

static int script_next_line(char *out, size_t cap) {
    if (g_pending_valid) {
        kstrncpy(out, g_pending, (int)cap - 1);
        out[cap - 1] = 0;
        g_pending_valid = 0;
        return 1;
    }
    if (!g_script) return 0;
    if (!fgets(out, (int)cap, g_script)) return 0;
    return 1;
}

int hal_poll_event(hal_event_t *ev) {
    static char line[256];
    u32 t = 0;
    char what[32] = {0};
    for (;;) {
        u64 elapsed;
        if (!script_next_line(line, sizeof(line))) return 0;
        if (sscanf(line, "%u %31s", &t, what) < 2) continue;
        elapsed = (now_us() - g_host_t0) / 1000ULL;
        if ((u64)t > elapsed) {
            kstrncpy(g_pending, line, sizeof(g_pending) - 1);
            g_pending[sizeof(g_pending) - 1] = 0;
            g_pending_valid = 1;
            return 0;
        }
        break;
    }
    kmemset(ev, 0, sizeof(*ev));
    if (!kstrcmp(what, "key")) {
        int ch = 0;
        if (sscanf(line, "%*u %*s %d", &ch) == 1) {
            ev->type = HAL_EV_CHAR;
            ev->ch = ch;
            ev->key = ch;
            return 1;
        }
    } else if (!kstrcmp(what, "keydown") || !kstrcmp(what, "keyup")) {
        int k = 0;
        if (sscanf(line, "%*u %*s %d", &k) == 1) {
            ev->type = (what[3] == 'd') ? HAL_EV_KEYDOWN : HAL_EV_KEYUP;
            ev->key = k;
            return 1;
        }
    } else if (!kstrcmp(what, "mousemove")) {
        if (sscanf(line, "%*u %*s %d %d", &ev->mx, &ev->my) == 2) {
            ev->type = HAL_EV_MOUSEMOVE;
            ev->buttons = g_script_buttons;
            return 1;
        }
    } else if (!kstrcmp(what, "click") || !kstrcmp(what, "down") || !kstrcmp(what, "up") ||
               !kstrcmp(what, "rdown") || !kstrcmp(what, "rup") ||
               !kstrcmp(what, "rdrag") || !kstrcmp(what, "mdrag")) {
        if (sscanf(line, "%*u %*s %d %d", &ev->mx, &ev->my) == 2) {
            /* A release must carry no buttons, otherwise the emulated button
               state never clears and the next 'down' is not seen as a fresh
               press (the desktop only reacts to state transitions). */
            int release = (what[0] == 'u') || !kstrcmp(what, "rup");
            int right = !kstrcmp(what, "rdown") || !kstrcmp(what, "rup") ||
                        !kstrcmp(what, "rdrag");
            int middle = !kstrcmp(what, "mdrag");
            ev->type = release ? HAL_EV_MOUSEUP : HAL_EV_MOUSEDOWN;
            ev->buttons = release ? 0 : (right ? 2 : (middle ? 4 : 1));
            g_script_buttons = ev->buttons;
            return 1;
        }
    } else if (!kstrcmp(what, "wheel")) {
        int dz = 0;
        if (sscanf(line, "%*u %*s %d", &dz) == 1) {
            ev->type = HAL_EV_WHEEL;
            ev->wheel = dz;
            return 1;
        }
    } else if (!kstrcmp(what, "dump")) {
        char path[128] = "/tmp/aether_frame.ppm";
        if (sscanf(line, "%*u %*s %127s", path) == 1) { }
        hal_dump_ppm(path);
        return hal_poll_event(ev);
    } else if (!kstrcmp(what, "quit")) {
        hal_request_quit();
        return 0;
    } else if (!kstrcmp(what, "sleep")) {
        int ms = 0;
        if (sscanf(line, "%*u %*s %d", &ms) == 1 && ms > 0) hal_sleep_ms(ms);
        return hal_poll_event(ev);
    }
    return hal_poll_event(ev);
}

void hal_wallclock(int *y, int *mo, int *d, int *hh, int *mm2, int *ss) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    if (y) *y = tmv.tm_year + 1900;
    if (mo) *mo = tmv.tm_mon + 1;
    if (d) *d = tmv.tm_mday;
    if (hh) *hh = tmv.tm_hour;
    if (mm2) *mm2 = tmv.tm_min;
    if (ss) *ss = tmv.tm_sec;
}

void hal_dump_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[dump] cannot open %s\n", path); return; }
    fprintf(stderr, "[dump] writing %s\n", path);
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            u32 c = g_vram[y * VRAM_PITCH + x];
            u8 px[3] = { (u8)RED(c), (u8)GREEN(c), (u8)BLUE(c) };
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
}

/* -------------------------------------------------------------------------- */
/*  Win32 implementation                                                     */
/* -------------------------------------------------------------------------- */
#else /* AETHER_ON_WIN32 */

static HWND      g_hwnd;
static HDC       g_memdc;
static HBITMAP   g_dib;
static HGDIOBJ   g_oldbmp;
static BITMAPINFO g_bmi;
/* The window is created at exactly SCREEN_W x SCREEN_H and never resized, so
   client geometry is the framebuffer geometry; these are only used for the
   cursor-confinement rectangle. */
static int       g_client_w = SCREEN_W, g_client_h = SCREEN_H;
static int       g_mouse_capture;
static int       g_has_focus = 1;

#define HAL_QUEUE_MAX 512
static hal_event_t g_evq[HAL_QUEUE_MAX];
static volatile int g_ev_head, g_ev_tail;

static void ev_push(const hal_event_t *e) {
    int next = (g_ev_head + 1) % HAL_QUEUE_MAX;
    if (next == g_ev_tail) return;           /* queue full: drop */
    g_evq[g_ev_head] = *e;
    g_ev_head = next;
}

static void ev_push_simple(int type, int key, int ch, int wheel) {
    hal_event_t e;
    kmemset(&e, 0, sizeof(e));
    e.type = type;
    e.key = key;
    e.ch = ch;
    e.wheel = wheel;
    ev_push(&e);
}

int hal_running(void) { return g_hal_running; }
u32 *hal_vram(void) { return g_vram; }
void hal_request_quit(void) { g_hal_running = 0; }
void hal_capture_mouse(int on) {
    g_mouse_capture = on;
    if (on) SetCapture(g_hwnd);
    else ReleaseCapture();
}
void hal_flush_all(void) { /* blitting happens in hal_present */ }

void hal_frame_boundary(u32 frame) { (void)frame; }
void hal_beep(int freq, int ms) {
    (void)freq; (void)ms;
    MessageBeep(MB_OK);
}

void hal_set_title(const char *t) {
    if (g_hwnd) SetWindowTextA(g_hwnd, t);
}

void hal_open_external(const char *url) {
    /* Hand the URL to the user's real browser.  The OS window is still the app;
       this is the escape hatch for pages that need a web engine. */
    if (url && *url) ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

void hal_sleep_ms(int ms) { Sleep((DWORD)(ms < 0 ? 0 : ms)); }

void hal_get_native_mouse(int *x, int *y) {
    POINT p = {0, 0};
    GetCursorPos(&p);
    ScreenToClient(g_hwnd, &p);
    if (x) *x = p.x;
    if (y) *y = p.y;
}

void hal_set_native_mouse(int x, int y) {
    POINT p = {x, y};
    ClientToScreen(g_hwnd, &p);
    SetCursorPos(p.x, p.y);
}

/* -------------------------------------------------------------------------- */
static void hal_create_backbuffer(void) {
    HDC wnddc = GetDC(g_hwnd);
    kmemset(&g_bmi, 0, sizeof(g_bmi));
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = SCREEN_W;
    g_bmi.bmiHeader.biHeight = -SCREEN_H;       /* top-down */
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;
    if (g_dib) { DeleteObject(g_dib); g_dib = NULL; }
    g_dib = CreateDIBSection(wnddc, &g_bmi, DIB_RGB_COLORS,
                             (void **)&g_vram, NULL, 0);
    if (!g_memdc) g_memdc = CreateCompatibleDC(wnddc);
    if (g_oldbmp) DeleteObject(g_oldbmp);
    g_oldbmp = SelectObject(g_memdc, g_dib);
    ReleaseDC(g_hwnd, wnddc);
}

/* Only the rect the compositor damaged is copied to the window each frame.  A
   full framebuffer copy is 3.5 MB of video traffic per frame; with the pointer
   parked, a frame that changed nothing now moves nothing at all. */
void hal_present(int x, int y, int w, int h) {
    HDC dc;
    if (!g_hwnd || !g_memdc || !g_vram) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    gfx_composite_cursor();
    wm_cursor_presented();
    dc = GetDC(g_hwnd);
    BitBlt(dc, x, y, w, h, g_memdc, x, y, SRCCOPY);
    ReleaseDC(g_hwnd, dc);
}

/* -------------------------------------------------------------------------- */
static LRESULT CALLBACK hal_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        g_hal_running = 0;
        ev_push_simple(HAL_EV_QUIT, 0, 0, 0);
        return 0;
    case WM_DESTROY:
        g_hal_running = 0;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);            /* our compositor draws the cursor */
            return TRUE;
        }
        break;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN: {
        hal_event_t e;
        kmemset(&e, 0, sizeof(e));
        e.type = HAL_EV_KEYDOWN;
        e.key = (int)wp;
        e.repeat = (int)((lp >> 30) & 1);
        ev_push(&e);
        if (msg == WM_SYSKEYDOWN && wp != VK_MENU) return 0;
        break;
    }
    case WM_SYSKEYUP:
    case WM_KEYUP:
        ev_push_simple(HAL_EV_KEYUP, (int)wp, 0, 0);
        return 0;
    case WM_CHAR:
    case WM_SYSCHAR: {
        hal_event_t e;
        kmemset(&e, 0, sizeof(e));
        e.type = HAL_EV_CHAR;
        e.ch = (int)wp;
        e.key = (int)wp;
        ev_push(&e);
        return 0;
    }
    case WM_MOUSEMOVE: {
        hal_event_t e;
        kmemset(&e, 0, sizeof(e));
        e.type = HAL_EV_MOUSEMOVE;
        e.mx = GET_X_LPARAM(lp);
        e.my = GET_Y_LPARAM(lp);
        e.buttons = g_hal_mouse_buttons;
        ev_push(&e);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
        hal_event_t e;
        int bit = (msg == WM_LBUTTONDOWN) ? 1 : (msg == WM_RBUTTONDOWN) ? 2 : 4;
        g_hal_mouse_buttons |= bit;
        SetCapture(hwnd);
        kmemset(&e, 0, sizeof(e));
        e.type = HAL_EV_MOUSEDOWN;
        e.mx = GET_X_LPARAM(lp);
        e.my = GET_Y_LPARAM(lp);
        e.buttons = g_hal_mouse_buttons;
        ev_push(&e);
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        hal_event_t e;
        int bit = (msg == WM_LBUTTONUP) ? 1 : (msg == WM_RBUTTONUP) ? 2 : 4;
        g_hal_mouse_buttons &= ~bit;
        if (!g_hal_mouse_buttons || !g_mouse_capture) ReleaseCapture();
        kmemset(&e, 0, sizeof(e));
        e.type = HAL_EV_MOUSEUP;
        e.mx = GET_X_LPARAM(lp);
        e.my = GET_Y_LPARAM(lp);
        e.buttons = g_hal_mouse_buttons;
        ev_push(&e);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        hal_event_t e;
        kmemset(&e, 0, sizeof(e));
        e.type = HAL_EV_WHEEL;
        e.wheel = GET_WHEEL_DELTA_WPARAM(wp);
        ev_push(&e);
        return 0;
    }
    case WM_KILLFOCUS:
    case WM_ACTIVATEAPP:
        if (msg == WM_KILLFOCUS || wp == 0) {
            g_has_focus = 0;
            g_hal_mouse_buttons = 0;
            ev_push_simple(HAL_EV_FOCUS, 0, 0, 0);
        } else {
            g_has_focus = 1;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int hal_init(void) {
    WNDCLASSEXA wc;
    RECT r;
    int sw, sh;
    HMODULE hinst = GetModuleHandleA(NULL);
    DWORD style;
    (void)g_has_focus;

    /* 1. physical RAM ------------------------------------------------------ */
    g_phys_ram = (u8 *)VirtualAlloc(NULL, PHYS_RAM_SIZE, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
    if (!g_phys_ram) return -1;
    kmemset(g_phys_ram, 0, PHYS_RAM_SIZE);

    /* 2. window + framebuffer ---------------------------------------------- */
    SetProcessDPIAware();
    kmemset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = hal_wndproc;
    wc.hInstance = hinst;
    wc.hCursor = NULL;
    wc.hbrBackground = NULL;
    wc.lpszClassName = "AEClass";
    wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    if (!RegisterClassExA(&wc)) return -2;

    style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    r.left = 0; r.top = 0; r.right = SCREEN_W; r.bottom = SCREEN_H;
    AdjustWindowRect(&r, style, FALSE);
    sw = r.right - r.left;
    sh = r.bottom - r.top;
    g_hwnd = CreateWindowExA(WS_EX_APPWINDOW, "AEClass", "AE",
                             style, (GetSystemMetrics(SM_CXSCREEN) - sw) / 2,
                             (GetSystemMetrics(SM_CYSCREEN) - sh) / 2,
                             sw, sh, NULL, NULL, hinst, NULL);
    if (!g_hwnd) return -3;
    hal_create_backbuffer();
    if (!g_vram) return -4;
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    SetCursor(NULL);
    ShowCursor(FALSE);

    /* 3. hardware timer ---------------------------------------------------- */
    timeBeginPeriod(1);
    QueryPerformanceFrequency((LARGE_INTEGER *)&g_host_freq);
    QueryPerformanceCounter((LARGE_INTEGER *)&g_host_t0);

    kmemset(&g_machine, 0, sizeof(g_machine));
    g_machine.magic = 0x41455448u;
    g_machine.width = SCREEN_W;
    g_machine.height = SCREEN_H;
    g_machine.pitch = VRAM_PITCH;
    g_machine.bpp = 32;
    g_machine.total_ram = PHYS_RAM_SIZE;
    g_machine.kernel_bytes = KERNEL_SIZE;
    g_machine.heap_bytes = HEAP_SIZE;
    g_machine.disk_bytes = DISK_SIZE;
    g_machine.cpu_mhz = 0;
    kstrcpy(g_machine.host_os, "windows");
    {
        SYSTEM_INFO si;
        char cpu[64];
        GetNativeSystemInfo(&si);
        ksnprintf(g_machine.host_os, sizeof(g_machine.host_os), "windows");
        /* Identify the host CPU through GetSystemInfo only: reading the
           registry would drag advapi32 in as an extra DLL dependency, and the
           OS is specified to need nothing beyond the four core libraries. */
        {
            const char *arch = "x86";
            switch (si.wProcessorArchitecture) {
            case PROCESSOR_ARCHITECTURE_AMD64: arch = "x86-64"; break;
            case PROCESSOR_ARCHITECTURE_ARM64: arch = "arm64";  break;
            case PROCESSOR_ARCHITECTURE_ARM:   arch = "arm";    break;
            case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86";    break;
            default: break;
            }
            ksnprintf(cpu, sizeof(cpu), "%s host processor (%u cores)",
                      arch, (unsigned)si.dwNumberOfProcessors);
            kstrncpy(g_machine.cpu_name, cpu, sizeof(g_machine.cpu_name) - 1);
            g_machine.cpu_mhz = 2400;
        }
    }
    {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        g_client_w = rc.right - rc.left;
        g_client_h = rc.bottom - rc.top;
    }
    g_hal_running = 1;
    return 0;
}

void hal_shutdown(void) {
    g_hal_running = 0;
    ShowCursor(TRUE);
    timeEndPeriod(1);
    if (g_memdc) { SelectObject(g_memdc, g_oldbmp); DeleteDC(g_memdc); g_memdc = NULL; }
    if (g_dib) { DeleteObject(g_dib); g_dib = NULL; }
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = NULL; }
}

void hal_wallclock(int *y, int *mo, int *d, int *hh, int *mm2, int *ss) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (y) *y = st.wYear;
    if (mo) *mo = st.wMonth;
    if (d) *d = st.wDay;
    if (hh) *hh = st.wHour;
    if (mm2) *mm2 = st.wMinute;
    if (ss) *ss = st.wSecond;
}

int hal_poll_event(hal_event_t *ev) {
    MSG msg;
    int had = 0;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (msg.message == WM_QUIT) { g_hal_running = 0; }
    }
    if (g_ev_tail != g_ev_head) {
        *ev = g_evq[g_ev_tail];
        g_ev_tail = (g_ev_tail + 1) % HAL_QUEUE_MAX;
        had = 1;
    }
    return had;
}

#endif /* AETHER_HEADLESS */

/* -------------------------------------------------------------------------- */
/*  Networking                                                               */
/*                                                                            */
/*  A kernel module asks the chassis for bytes; how they are obtained is the   */
/*  chassis' problem.  Each platform supplies the same four calls plus the      */
/*  blocking wrapper:                                                          */
/*                                                                            */
/*      slot = net_begin(url)      start a request, returns a slot id          */
/*      net_poll(slot)             0 pending, 1 ready, <0 NET_ERR_*            */
/*      net_finish(slot, dst, cap) copy the body out and release the slot      */
/*      net_abort(slot)            give up on a slot                            */
/*                                                                            */
/*  Asynchronous by design: on Win32 the request runs on its own thread so the  */
/*  UI keeps compositing (the frame loop must never block on a socket), and on  */
/*  wasm the *host* performs the fetch.  The headless CI build has no network. */
/* -------------------------------------------------------------------------- */

int net_available(void) {
#if defined(AETHER_WASM) || defined(AETHER_ON_WIN32)
    return 1;
#else
    return 0;
#endif
}

#if defined(AETHER_WASM)

/* Start a request.  Returns a slot id, or NET_ERR_*.  Deliberately does NOT
 * wait: the wasm module shares a thread with the shell's UI, so blocking here
 * would freeze the window for as long as the network took.  Callers poll. */
int net_begin(const char *url) {
    return js_http_get(url);
}

/* The browser host implements GET only (fetch() with no options), so the POST
 * form reports itself unavailable rather than sending a body-less request that
 * the search server would reject.  The Win32 image is the chassis that can do
 * JSON-RPC; see the AETHER_ON_WIN32 branch below. */
int net_begin_post(const char *url, const char *ct, const char *b) {
    (void)url; (void)ct; (void)b;
    return NET_ERR_UNSUPPORTED;
}

int net_begin_full(const char *url, const char *method, const char *ctype,
                   const char *body, const char *hdrs) {
    (void)method; (void)ctype; (void)hdrs;
    if (body && *body) return NET_ERR_UNSUPPORTED;
    return js_http_get(url);
}

/* 0 = still waiting, 1 = ready to read, negative NET_ERR_* = failed. */
int net_poll(int slot) {
    int state = js_http_state(slot);
    if (state == 0) return 0;
    if (state == 2) return NET_ERR_IO;
    return 1;
}

/* Copy the finished body out and release the slot.  Returns the byte count, or
 * a negative NET_ERR_* code. */
int net_finish(int slot, char *dst, int cap) {
    int len = js_http_len(slot);
    int take;
    if (len < 0) { js_http_release(slot); return NET_ERR_IO; }
    /* A body bigger than the destination used to be an outright failure, which
       is why large pages reported "the page is too large to display" and showed
       nothing at all.  Reading the part that fits and marking the result as
       truncated is far more useful: the reader still gets the article.  The cap
       is a real limit (the wasm module has 64 MB of linear memory), but it is a
       limit on the *copy*, not a reason to refuse the page. */
    take = len;
    if (take > cap - 1) take = cap - 1;
    if (take > AETHER_HTTP_MAX_BODY) take = AETHER_HTTP_MAX_BODY;
    if (take > 0) js_http_read(slot, dst, take);
    dst[take > 0 ? take : 0] = 0;
    js_http_release(slot);
    return take;
}

/* The WebAssembly chassis keeps no response headers: its fetch() wrapper only
 * surfaces the body.  MCP over the browser therefore reports no session id and
 * the server treats each call as a fresh one, which is allowed. */
int net_headers(int slot, char *dst, int cap) {
    (void)slot;
    if (dst && cap > 0) dst[0] = 0;
    return 0;
}

void net_abort(int slot) { js_http_release(slot); }

#elif defined(AETHER_ON_WIN32)

/* --------------------------------------------------------------------------
 *  Win32 network chassis -- real HTTP, no browser involved
 *
 *  WinHTTP ships with every supported Windows version, so the OS can fetch a
 *  page with nothing installed.  The one rule the frame loop depends on is that
 *  a request must never block it, so each request runs on its own thread:
 *
 *      net_begin   starts the worker and returns the slot immediately
 *      worker      opens the session, follows redirects, reads the body
 *      net_poll    reports the worker's state (lock-free: two volatile words)
 *      net_finish  copies the body out and joins the thread
 *
 *  TLS is verified against the system root store (the default) -- the proxies
 *  the browser falls back to are ordinary HTTPS endpoints, nothing special is
 *  needed.
 * ------------------------------------------------------------------------ */
#define NET_MAX_SLOTS 6
#define NET_MAX_REDIRECTS 6

typedef struct {
    int  used;
    volatile int state;        /* 0 pending, 1 ready, 2 failed            */
    volatile int status;       /* HTTP status code when ready             */
    int  len;                  /* body length, valid when ready           */
    char *body;                /* kmalloc'd body, freed by finish/abort   */
    volatile int refs;         /* worker still running?                   */
    HANDLE thread;
    char url[1024];
    /* POST support: an MCP search is a JSON-RPC call, so the chassis needs to
       send a body and a content type, not just GET a URL.  NULL method means
       GET, which keeps every existing caller unchanged. */
    const char *method;
    const char *ctype;
    const char *req_body;
    const char *hdrs;          /* extra request headers, "Name: value\r\n"... */
    char        rhdr[768];     /* raw response headers, CRLF separated         */
} net_slot_t;

static net_slot_t g_net[NET_MAX_SLOTS];
static CRITICAL_SECTION g_net_lock;
static int g_net_lock_ready;

static void net_lock_init(void) {
    if (!g_net_lock_ready) { InitializeCriticalSection(&g_net_lock); g_net_lock_ready = 1; }
}

/* Fetch one URL into a fresh buffer.  Returns the byte count, or NET_ERR_*.
 *
 * Kept out of line and with a small frame: on Win64 any function whose frame
 * exceeds 4 KB needs a stack probe (___chkstk_ms), and the freestanding kernel
 * link has no provider for it.  The big buffers this routine needs are
 * therefore file scope, not automatic. */
static wchar_t g_net_wurl[1200];
static wchar_t g_net_whost[300];
static wchar_t g_net_wpath[2100];
static wchar_t g_net_wloc[2048];
static wchar_t g_net_whdr[512];
static wchar_t g_net_whdr2[900];
static wchar_t g_net_wraw[768];
static CRITICAL_SECTION g_fetch_lock;
static int g_fetch_lock_ready;
static char    g_net_next[1024];
static char    g_net_tmp[1024];

static int winhttp_fetch_inner(const char *url, const char *method, const char *ctype,
                               const char *req_body, const char *hdrs,
                               char *rhdr, int rhcap, char **out_body);

/* The URL, host, path and location buffers this routine needs are file scope (a
 * large stack frame needs ___chkstk_ms, which the freestanding link has no
 * provider for).  File scope plus six worker threads means two requests in
 * flight at once would overwrite each other mid-parse, so fetches take turns:
 * the UI thread still never blocks, only the workers queue. */
static int winhttp_fetch(const char *url, const char *method, const char *ctype,
                         const char *req_body, const char *hdrs,
                         char *rhdr, int rhcap, char **out_body) {
    int rc;
    if (!g_fetch_lock_ready) { InitializeCriticalSection(&g_fetch_lock); g_fetch_lock_ready = 1; }
    EnterCriticalSection(&g_fetch_lock);
    rc = winhttp_fetch_inner(url, method, ctype, req_body, hdrs, rhdr, rhcap, out_body);
    LeaveCriticalSection(&g_fetch_lock);
    return rc;
}

static int winhttp_fetch_inner(const char *url, const char *method, const char *ctype,
                               const char *req_body, const char *hdrs,
                               char *rhdr, int rhcap, char **out_body) {
    HINTERNET session = NULL, conn = NULL, req = NULL;
    URL_COMPONENTS uc;
    wchar_t *wurl = g_net_wurl, *whost = g_net_whost, *wpath = g_net_wpath;
    int redirects = 0;
    int i;

    *out_body = NULL;
    if (!url || !*url) return NET_ERR_IO;

    for (;;) {
        DWORD status = 0, sz;
        if (redirects++ > NET_MAX_REDIRECTS) return NET_ERR_IO;

        for (i = 0; url[i] && i < 1199; i++) wurl[i] = (wchar_t)(unsigned char)url[i];
        wurl[i] = 0;

        /* Crack the URL as wide text -- URL_COMPONENTS is a wide-character
           structure, so the narrow buffer must be converted first. */
        kmemset(&uc, 0, sizeof(uc));
        uc.dwStructSize = sizeof(uc);
        uc.lpszScheme = whost;   uc.dwSchemeLength = 15;      /* reuse the buffer */
        uc.lpszHostName = whost; uc.dwHostNameLength = 299;
        uc.lpszUrlPath = wpath;  uc.dwUrlPathLength = 2099;
        if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return NET_ERR_IO;

        session = WinHttpOpen(L"AE/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return NET_ERR_IO;

        {
            /* CrackUrl wrote the host into whost starting at offset 0 only if
               scheme came first; copy the components out explicitly instead. */
            int k = 0;
            for (i = 0; i < (int)uc.dwHostNameLength && k < 299; i++) whost[k++] = wurl[(int)(uc.lpszHostName - wurl) + i];
            whost[k] = 0;
        }
        conn = WinHttpConnect(session, whost, uc.nPort, 0);
        if (!conn) { WinHttpCloseHandle(session); return NET_ERR_IO; }

        {
            int n = 0;
            if (uc.dwUrlPathLength == 0) { wpath[n++] = L'/'; }
            else
                for (i = 0; i < (int)uc.dwUrlPathLength && n < 2090; i++)
                    wpath[n++] = wurl[(int)(uc.lpszUrlPath - wurl) + i];
            wpath[n] = 0;
            if (wpath[0] != L'/') {
                int m;
                for (m = n; m > 0; m--) wpath[m] = wpath[m - 1];
                wpath[0] = L'/';
            }
        }

        req = WinHttpOpenRequest(conn,
                                 (method && method[0] == 'P') ? L"POST" : L"GET",
                                 wpath, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return NET_ERR_IO; }

        WinHttpSetTimeouts(req, 8000, 8000, 12000, 30000);
        {
            /* A JSON-RPC call needs both a body and a content type.  The header
               and body buffers are wide/byte respectively; WinHTTP takes the
               request body as raw bytes and its length in bytes. */
            wchar_t *whdr = g_net_whdr2;
            const char *body = req_body ? req_body : "";
            int hlen = 0, i;
            int blen = (int)kstrlen(body);
            if (ctype && ctype[0]) {
                static const wchar_t ct[] = L"Content-Type: ";
                for (i = 0; ct[i]; i++) whdr[hlen++] = ct[i];
                for (i = 0; ctype[i] && hlen < 200; i++) whdr[hlen++] = (wchar_t)(unsigned char)ctype[i];
                whdr[hlen++] = L'\r'; whdr[hlen++] = L'\n';
            }
            /* MCP streams replies as server-sent events unless the client also
               advertises JSON, so ask for both and accept either. */
            if (hdrs) {
                for (i = 0; hdrs[i] && hlen < 620; i++) {
                    whdr[hlen++] = (hdrs[i] == '\n') ? L'\n' : (wchar_t)(unsigned char)hdrs[i];
                }
            }
            whdr[hlen] = 0;
            if (!WinHttpSendRequest(req,
                                    hlen ? whdr : WINHTTP_NO_ADDITIONAL_HEADERS,
                                    hlen ? (DWORD)-1L : 0,
                                    blen ? (LPVOID)body : WINHTTP_NO_REQUEST_DATA,
                                    (DWORD)blen, (DWORD)blen, 0) ||
                !WinHttpReceiveResponse(req, NULL)) {
                WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
                return NET_ERR_IO;
            }
        }

        /* Keep the response headers: MCP hands back its session id there, and
           without it the follow-up tools/call is rejected. */
        if (rhdr && rhcap > 1) {
            DWORD rsz = (DWORD)sizeof(g_net_wraw);
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                    g_net_wraw, &rsz, WINHTTP_NO_HEADER_INDEX)) {
                int k = 0;
                const wchar_t *wp2 = g_net_wraw;
                while (wp2[k] && k < rhcap - 1) { rhdr[k] = (char)wp2[k]; k++; }
                rhdr[k] = 0;
            } else rhdr[0] = 0;
        }

        sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);

        /* Follow redirects here rather than relying on the automatic policy: the
           reader proxies redirect on purpose and a cross-host hop must work. */
        if (status >= 300 && status < 400) {
            wchar_t *wloc = g_net_wloc;
            DWORD lsz = sizeof(g_net_wloc);
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                    wloc, &lsz, WINHTTP_NO_HEADER_INDEX) && lsz > 2) {
                char *next = g_net_next;
                int k = 0, rc;
                for (i = 0; wloc[i] && k < 1023; i++) next[k++] = (char)wloc[i];
                next[k] = 0;
                WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
                kstrncpy(g_net_tmp, next, sizeof(g_net_tmp) - 1);
                g_net_tmp[sizeof(g_net_tmp) - 1] = 0;
                url = g_net_tmp;
                req = NULL; conn = NULL; session = NULL;
                /* loop again with the new location */
                {
                    char *copy = (char *)mm_kmalloc((u32)kstrlen(url) + 1);
                    if (!copy) return NET_ERR_IO;
                    kstrcpy(copy, url);
                    rc = winhttp_fetch_inner(copy, method, ctype, req_body, hdrs, rhdr, rhcap, out_body);
                    mm_kfree(copy);
                    return rc;
                }
            }
        }
        if (status < 200 || status >= 300) {
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
            return NET_ERR_IO;
        }

        /* Read the whole body, up to the transfer cap. */
        {
            char *buf = NULL;
            int cap = 0, len = 0, result = NET_ERR_IO;
            for (;;) {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(req, &avail)) break;
                if (avail == 0) { result = len; break; }
                if (len >= AETHER_HTTP_MAX_BODY) { result = len; break; }
                if (len + (int)avail + 1 > cap) {
                    int ncap = cap ? cap : 65536;
                    char *nb;
                    while (ncap < len + (int)avail + 1) ncap *= 2;
                    if (ncap > AETHER_HTTP_MAX_BODY + 1) ncap = AETHER_HTTP_MAX_BODY + 1;
                    nb = (char *)mm_kmalloc((u32)ncap);
                    if (!nb) break;
                    if (buf) { kmemcpy(nb, buf, (size_t)len); mm_kfree(buf); }
                    buf = nb;
                    cap = ncap;
                }
                if (!WinHttpReadData(req, buf + len, avail, &got) || got == 0) break;
                len += (int)got;
            }
            if (buf) { buf[len] = 0; *out_body = buf; result = len; }
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
            return result;
        }
    }
}

static DWORD WINAPI net_worker(LPVOID arg) {
    net_slot_t *s = (net_slot_t *)arg;
    char *body = NULL;
    int n = winhttp_fetch(s->url, s->method, s->ctype, s->req_body, s->hdrs,
                          s->rhdr, (int)sizeof(s->rhdr), &body);
    if (n >= 0) {
        s->body = body;
        s->len = n;
        s->state = 1;
    } else {
        s->state = 2;
    }
    return 0;
}

int net_begin(const char *url) { return net_begin_full(url, "GET", NULL, NULL, NULL); }

/* Start a POST (used for JSON-RPC, i.e. MCP).  The three strings are borrowed
 * for the lifetime of the request, so callers pass buffers that outlive it --
 * the browser keeps them in its own state for exactly this reason. */
int net_begin_post(const char *url, const char *ctype, const char *body) {
    return net_begin_full(url, "POST", ctype, body, NULL);
}

int net_begin_full(const char *url, const char *method, const char *ctype,
                   const char *body, const char *hdrs) {
    int i;
    net_lock_init();
    if (!url || !*url) return NET_ERR_IO;
    EnterCriticalSection(&g_net_lock);
    for (i = 0; i < NET_MAX_SLOTS; i++) {
        if (!g_net[i].used) {
            net_slot_t *s = &g_net[i];
            kmemset(s, 0, sizeof(*s));
            s->used = 1;
            kstrncpy(s->url, url, sizeof(s->url) - 1);
            s->method = method ? method : "GET";
            s->ctype = ctype;
            s->req_body = body;
            s->hdrs = hdrs;
            s->thread = CreateThread(NULL, 0, net_worker, s, 0, NULL);
            if (!s->thread) { s->used = 0; i = NET_MAX_SLOTS; break; }
            LeaveCriticalSection(&g_net_lock);
            return i;
        }
    }
    LeaveCriticalSection(&g_net_lock);
    return NET_ERR_IO;
}

int net_poll(int slot) {
    if (slot < 0 || slot >= NET_MAX_SLOTS || !g_net[slot].used) return NET_ERR_IO;
    if (g_net[slot].state == 0) return 0;
    if (g_net[slot].state == 2) return NET_ERR_IO;
    return 1;
}

int net_finish(int slot, char *dst, int cap) {
    net_slot_t *s;
    int take;
    if (slot < 0 || slot >= NET_MAX_SLOTS || !g_net[slot].used) return NET_ERR_IO;
    s = &g_net[slot];
    if (s->thread) { WaitForSingleObject(s->thread, 1000); CloseHandle(s->thread); s->thread = NULL; }
    if (s->state != 1 || !s->body) { net_abort(slot); return NET_ERR_IO; }
    take = s->len;
    if (take > cap - 1) take = cap - 1;
    kmemcpy(dst, s->body, (size_t)take);
    dst[take] = 0;
    mm_kfree(s->body);
    s->body = NULL;
    s->used = 0;
    return take;
}

void net_abort(int slot) {
    if (slot < 0 || slot >= NET_MAX_SLOTS) return;
    {
        net_slot_t *s = &g_net[slot];
        if (s->thread) { WaitForSingleObject(s->thread, 1000); CloseHandle(s->thread); s->thread = NULL; }
        if (s->body) { mm_kfree(s->body); s->body = NULL; }
        s->used = 0;
    }
}

/* Copy the response headers of a finished request.  The MCP client needs the
 * Mcp-Session-Id from handshake to authorise the tools/call that follows;
 * ordinary page fetches ignore headers entirely. */
int net_headers(int slot, char *dst, int cap) {
    int n;
    if (slot < 0 || slot >= NET_MAX_SLOTS || !dst || cap <= 0) return -1;
    n = (int)kstrlen(g_net[slot].rhdr);
    if (n > cap - 1) n = cap - 1;
    kmemcpy(dst, g_net[slot].rhdr, (size_t)n);
    dst[n] = 0;
    return n;
}

#else   /* headless: no network in the CI chassis */

int net_begin(const char *url) { (void)url; return NET_ERR_UNSUPPORTED; }
int net_begin_post(const char *url, const char *ct, const char *b) { (void)url; (void)ct; (void)b; return NET_ERR_UNSUPPORTED; }
int net_begin_full(const char *u, const char *m, const char *ct, const char *b, const char *h) { (void)u; (void)m; (void)ct; (void)b; (void)h; return NET_ERR_UNSUPPORTED; }
int net_poll(int slot) { (void)slot; return NET_ERR_UNSUPPORTED; }
int net_finish(int slot, char *dst, int cap) { (void)slot; (void)dst; (void)cap; return NET_ERR_UNSUPPORTED; }
void net_abort(int slot) { (void)slot; }
int net_headers(int slot, char *dst, int cap) { (void)slot; if (dst && cap) dst[0] = 0; return 0; }

#endif

/* Blocking convenience wrapper.  Only safe before the frame loop is running
 * (i.e. during boot); the browser app uses the three-call form instead. */
int net_fetch(const char *url, char *dst, int cap) {
    int slot = net_begin(url);
    u64 deadline;
    if (slot < 0) return slot;
    deadline = now_us() + 20000000ULL;
    while (now_us() < deadline) {
        int r = net_poll(slot);
        if (r != 0) {
            if (r < 0) { net_abort(slot); return r; }
            return net_finish(slot, dst, cap);
        }
    }
    net_abort(slot);
    return NET_ERR_TIMEOUT;
}

/* -------------------------------------------------------------------------- */
/*  Common HAL helpers                                                       */
/* -------------------------------------------------------------------------- */

void hal_phys_ram_bounds(u8 **base, u32 *size) {
    *base = g_phys_ram;
    *size = PHYS_RAM_SIZE;
}

/* The host timer is exposed to the kernel as a microsecond counter; IRQ0 is
 * derived from this by the interrupt controller module. */
u64 hal_ticks_us(void) { return now_us(); }

u32 hal_uptime_ms(void) { return now_ms(); }
