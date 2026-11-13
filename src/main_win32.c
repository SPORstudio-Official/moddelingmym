/* ===========================================================================
   DEAD AIR -- Win32 platform layer
   Window + software framebuffer presentation + waveOut audio thread.
   =========================================================================== */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include "da.h"

#ifndef WAVE_MAPPER
#define WAVE_MAPPER ((UINT)-1)
#endif

#define WIN_SCALE 2
#define AUD_BUFS 6
#define AUD_FRAMES 512

static HWND  g_hwnd;
static HDC   g_memdc;
static HBITMAP g_hbm;
static BITMAPINFO g_bmi;
static int   g_running = 1;
static int   g_fullscreen;
static Input g_in;
static double g_qpc_period;
static LARGE_INTEGER g_qpc_start;
static float g_time;
static int   g_view_w = FB_W * WIN_SCALE, g_view_h = FB_H * WIN_SCALE;
static int   g_offx, g_offy, g_dstw, g_dsth;
static RECT  g_saved_rect;
static int   g_rawmx, g_rawmy;
static DWORD g_saved_style;

/* --------------------------------------------------------- audio thread  */
static short   g_abuf[AUD_BUFS][AUD_FRAMES * 2];
static WAVEHDR g_ahdr[AUD_BUFS];
static HWAVEOUT g_hwo;
static HANDLE  g_aev;
static CRITICAL_SECTION g_acs;
static volatile int g_arun = 1;

static DWORD WINAPI audio_thread(LPVOID p) {
    (void)p;
    while (g_arun) {
        DWORD w = WaitForSingleObject(g_aev, 60);
        if (!g_arun) break;
        if (w != WAIT_OBJECT_0 && w != WAIT_TIMEOUT) continue;
        EnterCriticalSection(&g_acs);
        for (int i = 0; i < AUD_BUFS; i++) {
            if (!(g_ahdr[i].dwFlags & WHDR_INQUEUE)) {
                audio_mix(g_abuf[i], AUD_FRAMES);
                waveOutWrite(g_hwo, &g_ahdr[i], sizeof(WAVEHDR));
            }
        }
        LeaveCriticalSection(&g_acs);
    }
    return 0;
}

static void audio_start(void) {
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = MIX_SR;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    InitializeCriticalSection(&g_acs);
    g_aev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&g_hwo, WAVE_MAPPER, &wf, (DWORD_PTR)g_aev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
        return;
    for (int i = 0; i < AUD_BUFS; i++) {
        memset(&g_ahdr[i], 0, sizeof(WAVEHDR));
        g_ahdr[i].lpData = (LPSTR)g_abuf[i];
        g_ahdr[i].dwBufferLength = AUD_FRAMES * 4;
        waveOutPrepareHeader(g_hwo, &g_ahdr[i], sizeof(WAVEHDR));
    }
    CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
}

/* --------------------------------------------------------------- video   */
static void video_init(void) {
    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = FB_W;
    g_bmi.bmiHeader.biHeight = -FB_H;      /* top-down */
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(g_hwnd);
    g_memdc = CreateCompatibleDC(dc);
    ReleaseDC(g_hwnd, dc);
    void *bits = NULL;
    g_hbm = CreateDIBSection(g_memdc, &g_bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(g_memdc, g_hbm);
    SetBkMode(g_memdc, TRANSPARENT);

    FB fb;
    fb.px = (u32 *)bits;
    fb.w = FB_W;
    fb.h = FB_H;
    game_init(&fb);
}

static void video_present(void) {
    HDC dc = GetDC(g_hwnd);
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    float sx = (float)cw / (float)FB_W, sy = (float)ch / (float)FB_H;
    float s = sx < sy ? sx : sy;
    int dw = (int)(FB_W * s), dh = (int)(FB_H * s);
    int ox = (cw - dw) / 2, oy = (ch - dh) / 2;
    PatBlt(dc, 0, 0, cw, ch, BLACKNESS);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchBlt(dc, ox, oy, dw, dh, g_memdc, 0, 0, FB_W, FB_H, SRCCOPY);
    ReleaseDC(g_hwnd, dc);
}

static void toggle_fullscreen(void) {
    if (!g_fullscreen) {
        GetWindowRect(g_hwnd, &g_saved_rect);
        g_saved_style = GetWindowLongA(g_hwnd, GWL_STYLE);
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongA(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
        g_fullscreen = 1;
    } else {
        SetWindowLongA(g_hwnd, GWL_STYLE, g_saved_style);
        SetWindowPos(g_hwnd, HWND_TOP, g_saved_rect.left, g_saved_rect.top,
                     g_saved_rect.right - g_saved_rect.left,
                     g_saved_rect.bottom - g_saved_rect.top, SWP_FRAMECHANGED);
        g_fullscreen = 0;
    }
}

/* ---------------------------------------------- window -> framebuffer   */
static void map_mouse(void) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) { g_in.mouse_x = 0; g_in.mouse_y = 0; return; }
    float sx = (float)cw / (float)FB_W, sy = (float)ch / (float)FB_H;
    float s = sx < sy ? sx : sy;
    int dw = (int)(FB_W * s), dh = (int)(FB_H * s);
    int ox = (cw - dw) / 2, oy = (ch - dh) / 2;
    int fx = (int)(((float)(g_rawmx - ox) / s));
    int fy = (int)(((float)(g_rawmy - oy) / s));
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx > FB_W - 1) fx = FB_W - 1;
    if (fy > FB_H - 1) fy = FB_H - 1;
    g_in.mouse_x = fx;
    g_in.mouse_y = fy;
}

/* --------------------------------------------------------------- window  */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE:
        g_running = 0;
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        g_running = 0;
        return 0;
    case WM_MOUSEMOVE:
        g_rawmx = (int)(short)LOWORD(l);
        g_rawmy = (int)(short)HIWORD(l);
        break;
    case WM_LBUTTONDOWN:
        SetCapture(h);
        g_in.mouse_down = 1;
        g_in.mouse_pressed = 1;
        g_rawmx = (int)(short)LOWORD(l);
        g_rawmy = (int)(short)HIWORD(l);
        break;
    case WM_LBUTTONUP:
        ReleaseCapture();
        g_in.mouse_down = 0;
        g_in.mouse_released = 1;
        break;
    case WM_KEYDOWN: {
        int vk = (int)(w & 0xff);
        if (vk == VK_F11) { toggle_fullscreen(); return 0; }
        if (vk == VK_ESCAPE) { /* handled by the game as pause */ }
        if (vk > 0 && vk < 128) {
            if (!(l & 0x40000000)) g_in.key_pressed[vk] = 1;
            g_in.key_down[vk] = 1;
        }
        if (vk == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) { g_running = 0; PostQuitMessage(0); }
        return 0;
    }
    case WM_KEYUP: {
        int vk = (int)(w & 0xff);
        if (vk > 0 && vk < 128) g_in.key_down[vk] = 0;
        return 0;
    }
    case WM_SYSKEYDOWN:
        if ((w & 0xff) == VK_F4) { g_running = 0; PostQuitMessage(0); return 0; }
        if ((w & 0xff) == VK_F11) { toggle_fullscreen(); return 0; }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        video_present();
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcA(h, m, w, l);
}

static double now_seconds(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)(c.QuadPart - g_qpc_start.QuadPart) * g_qpc_period;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd;
    SetProcessDPIAware();

    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "DeadAirWnd";
    wc.hIcon = LoadIconA(inst, "APPICON");
    RegisterClassA(&wc);

    RECT r = { 0, 0, g_view_w, g_view_h };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExA(0, "DeadAirWnd", "DEAD AIR", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_period = 1.0 / (double)f.QuadPart;
    QueryPerformanceCounter(&g_qpc_start);

    video_init();
    audio_start();

    double last = now_seconds();
    while (g_running) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        double t = now_seconds();
        float dt = (float)(t - last);
        last = t;
        if (dt < 0.0005f) dt = 0.0005f;
        if (dt > 0.05f) dt = 0.05f;
        g_time += dt;

        map_mouse();
        g_in.dt = dt;
        g_in.time = g_time;
        game_frame(&g_in);
        g_in.mouse_pressed = 0;
        g_in.mouse_released = 0;
        memset(g_in.key_pressed, 0, sizeof g_in.key_pressed);

        if (game_should_quit()) { g_running = 0; break; }
        video_present();
        Sleep(1);
    }

    g_arun = 0;
    if (g_aev) SetEvent(g_aev);
    Sleep(120);
    if (g_hwo) waveOutReset(g_hwo);
    DestroyWindow(g_hwnd);
    return 0;
}
