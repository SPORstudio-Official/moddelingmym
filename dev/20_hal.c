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
 *  produced by the AetherOS compositor.
 * ========================================================================== */

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
#ifdef AETHER_ON_WIN32
static u64 g_host_freq;
static int g_hal_mouse_buttons;
#endif

machine_t g_machine;

/* -------------------------------------------------------------------------- */
/*  Headless (CI) implementation                                              */
/* -------------------------------------------------------------------------- */
#ifdef AETHER_HEADLESS

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
            return 1;
        }
    } else if (!kstrcmp(what, "click") || !kstrcmp(what, "down") || !kstrcmp(what, "up")) {
        if (sscanf(line, "%*u %*s %d %d", &ev->mx, &ev->my) == 2) {
            ev->type = (what[0] == 'u') ? HAL_EV_MOUSEUP : HAL_EV_MOUSEDOWN;
            ev->buttons = 1;
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

void hal_present(int x, int y, int w, int h) {
    HDC dc;
    if (!g_hwnd || !g_memdc || !g_vram) return;
    dc = GetDC(g_hwnd);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w > 0 && h > 0)
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
    wc.lpszClassName = "AetherOSClass";
    wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    if (!RegisterClassExA(&wc)) return -2;

    style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    r.left = 0; r.top = 0; r.right = SCREEN_W; r.bottom = SCREEN_H;
    AdjustWindowRect(&r, style, FALSE);
    sw = r.right - r.left;
    sh = r.bottom - r.top;
    g_hwnd = CreateWindowExA(WS_EX_APPWINDOW, "AetherOSClass", "AetherOS",
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
