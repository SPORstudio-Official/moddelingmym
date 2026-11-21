/* ==========================================================================
 *  PRELUDE -- compilation environment, portable types, kernel-wide constants
 * ========================================================================== */

#ifndef AETHER_OS_C
#define AETHER_OS_C 1
#endif

/* --------------------------------------------------------------------------
 *  Target platform
 * -------------------------------------------------------------------------- */
#ifdef _WIN32
#  define AETHER_ON_WIN32 1
#endif
#ifdef AETHER_WASM
/* WebAssembly chassis: compiled freestanding into a .wasm module that the
   Electron shell instantiates.  It is *not* Win32 and it has no libc at all --
   exactly like the Windows build, which links -nostdlib.  The module talks to
   its host through a handful of imported functions (js_*), and the host does
   the blitting, input collection and wall clock. */
#  undef AETHER_ON_WIN32
#endif
#ifdef AETHER_HEADLESS
/* Headless (Linux CI) chassis: no window, events are injected from a script. */
#  undef AETHER_ON_WIN32
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
/* The Windows and WebAssembly builds deliberately do not link a C runtime: the
   kernel ships its own string/memory/formatting primitives (10_util.c) and its
   own maths (gfx + bytecode), so the executable depends on nothing but the four
   Win32 system libraries (or, on wasm, on the shell's js_* imports).  The
   headless CI build still uses the host libc for file I/O and the clock. */
#if !defined(AETHER_ON_WIN32) && !defined(AETHER_WASM)
#  include <string.h>
#  include <stdlib.h>
#  include <stdio.h>
#  include <math.h>
#endif

#ifdef AETHER_ON_WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <windowsx.h>       /* GET_X_LPARAM / GET_Y_LPARAM                */
#  include <mmsystem.h>       /* timeBeginPeriod / timeEndPeriod            */
#  include <shellapi.h>       /* ShellExecuteA for hal_open_external        */
#  include <winhttp.h>        /* the OS fetches pages itself, no browser    */
/* The Win32 headers define macros whose names collide with kernel vocabulary.
   Nothing in AE uses the Win32 meaning, and leaving them defined breaks
   the syscall enum and the modifier bitmask, so they are removed here. */
#  undef MOD_ALT
#  undef MOD_CONTROL
#  undef MOD_SHIFT
#  undef MOD_WIN
#  undef SYS_OPEN
#  undef SYS_READ
#  undef SYS_WRITE
#  undef SYS_CLOSE
#  undef RGB
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

#ifdef AETHER_WASM
/* --------------------------------------------------------------------------
 *  WebAssembly shell interface
 *
 *  The wasm module cannot open a window or read a clock by itself, so the
 *  Electron shell provides these.  Each one is a plain scalar function; the
 *  host implementation lives in app/aether-host.js.  Nothing else crosses the
 *  boundary, which keeps the module fully self-contained.
 * ------------------------------------------------------------------------ */
__attribute__((import_module("env"), import_name("js_now_us")))
u64 js_now_us(void);
__attribute__((import_module("env"), import_name("js_present")))
void js_present(void);
/* Present only a damaged rectangle.  The host uploads the rows in [y, y+h) of
   [x, x+w) instead of the whole 3.5 MB surface, which is what makes the
   browser build cheap when only a pointer-sized region changed. */
__attribute__((import_module("env"), import_name("js_present_rect")))
void js_present_rect(int x, int y, int w, int h);
__attribute__((import_module("env"), import_name("js_poll_event")))
int js_poll_event(void *out);          /* writes 40 bytes -> 1 if an event    */
__attribute__((import_module("env"), import_name("js_wallclock")))
void js_wallclock(int *y, int *mo, int *d, int *hh, int *mm, int *ss);
__attribute__((import_module("env"), import_name("js_set_title")))
void js_set_title(const char *t);
__attribute__((import_module("env"), import_name("js_beep")))
void js_beep(int freq, int ms);
__attribute__((import_module("env"), import_name("js_log")))
void js_log(const char *msg);
__attribute__((import_module("env"), import_name("js_request_quit")))
void js_request_quit(void);

/* ---- networking ---------------------------------------------------------
 *  The browser app needs real HTTP.  A wasm module cannot open a socket, so
 *  the host performs the request and hands back the body.  Requests are
 *  asynchronous in JavaScript but the kernel runs a synchronous frame loop, so
 *  the host starts a fetch(), buffers the result, and the kernel polls:
 *
 *      slot = js_http_get(url)        -> id (>0), or -1 if the host refused
 *      js_http_state(slot)            -> 0 pending, 1 done, 2 failed
 *      js_http_status(slot)           -> HTTP status code
 *      js_http_len(slot) / js_http_read(slot, dst, max)
 *
 *  Buffers are owned by the host and released by js_http_release().  The cap
 *  keeps a huge page from exhausting the module's linear memory.
 * ---------------------------------------------------------------------- */

__attribute__((import_module("env"), import_name("js_http_get")))
int  js_http_get(const char *url);
__attribute__((import_module("env"), import_name("js_http_state")))
int  js_http_state(int slot);
__attribute__((import_module("env"), import_name("js_http_status")))
int  js_http_status(int slot);
__attribute__((import_module("env"), import_name("js_http_len")))
int  js_http_len(int slot);
__attribute__((import_module("env"), import_name("js_http_read")))
int  js_http_read(int slot, void *dst, int max);
__attribute__((import_module("env"), import_name("js_http_release")))
void js_http_release(int slot);
__attribute__((import_module("env"), import_name("js_open_external")))
void js_open_external(const char *url);
#endif

/* Open a URL outside the OS window, in a real browser on the host.  Every
 * chassis provides this: the wasm build forwards it to window.open(), the
 * standalone Windows build to ShellExecute(), and the headless build logs it.
 * Apps call this and never the js_* import directly, or they stop linking on
 * the chassis that has no JavaScript host. */
void hal_open_external(const char *url);

#define AETHER_VERSION_MAJOR 1
#define AETHER_VERSION_MINOR 0
#define AE_VERSION_STR "1.0.0-ae"
#define AE_CODENAME "Nebula"

/* --------------------------------------------------------------------------
 *  Hardware geometry (the emulated machine)
 * -------------------------------------------------------------------------- */
#define SCREEN_W        1280
#define SCREEN_H        720
#define VRAM_PITCH      (SCREEN_W)
#define PHYS_RAM_SIZE   (64u * 1024u * 1024u)

#define KERNEL_BASE     0x00000000u        /* kernel structures, 1 MB          */
#define KERNEL_SIZE     (1u * 1024u * 1024u)
#define HEAP_BASE       0x00100000u        /* dynamic kernel+user heap, 31 MB  */
#define HEAP_SIZE       (31u * 1024u * 1024u)
#define DISK_BASE       0x02000000u        /* NVRAM ramdisk, 32 MB             */
#define DISK_SIZE       (32u * 1024u * 1024u)

/* VFS geometry (lives inside the ramdisk */
#define VFS_MAGIC       0xA37E4F53u        /* "A7~OS"                          */
#define VFS_INODES      512
#define VFS_BLOCKS      4096
#define VFS_BLOCK_SIZE  4096
#define VFS_MAX_NAME    28
#define VFS_MAX_OPEN    64
#define VFS_FS_BYTES    ((u32)VFS_BLOCKS * VFS_BLOCK_SIZE)

/* --------------------------------------------------------------------------
 *  Shared logging / panic
 * -------------------------------------------------------------------------- */
#define LOG_BUF_SIZE (64 * 1024)

/* A modern article page is routinely 300-800 KB of HTML before any script runs,
   and an older 512 KB ceiling truncated the biggest ones.  The reader keeps up
   to HTML_TEXT_MAX (2 MB) of text; this is the transfer budget above that.  It
   lives here because every chassis honours it, not just the wasm one. */
#define AETHER_HTTP_MAX_BODY (2 * 1024 * 1024)

/* Networking (20_hal.c).  net_fetch downloads a URL into dst, NUL-terminates it
   and returns the byte count, or a negative NET_ERR_* code. */
enum {
    NET_ERR_UNSUPPORTED = -1,
    NET_ERR_TIMEOUT     = -2,
    NET_ERR_IO          = -3,
    NET_ERR_TOO_BIG     = -4
};
int net_available(void);
/* Blocking form: fine during boot, unsafe once the frame loop is running. */
int net_fetch(const char *url, char *dst, int cap);
/* Non-blocking form used by the browser: start, poll each frame, finish. */
int net_begin(const char *url);
/* POST form, needed for JSON-RPC (the MCP search backend).  ctype/body are
   borrowed until the request finishes. */
int net_begin_post(const char *url, const char *ctype, const char *body);
/* Full form: method, content type, body and extra request headers.  All strings
   are borrowed until the request finishes. */
int net_begin_full(const char *url, const char *method, const char *ctype,
                   const char *body, const char *hdrs);
/* Response headers of a finished request (used for the MCP session id). */
int net_headers(int slot, char *dst, int cap);

/* MCP search back end (76_mcp.c).  A search is a JSON-RPC POST to a keyless
   hosted MCP server; the browser calls the three-stage form so the frame loop
   never blocks on the network. */
typedef struct {
    char title[96];
    char url[256];
    char snippet[224];
} mcp_hit_t;

int  mcp_available(void);
void mcp_init(void);
int  mcp_search(const char *query);
int  mcp_poll(void);
int  mcp_finish(void);
int  mcp_hits(void);
const mcp_hit_t *mcp_hit(int i);
int  mcp_busy(void);
const char *mcp_note(void);
const char *mcp_query(void);
int  mcp_daily_limit(void);
int  mcp_is_search(const char *s);
int net_poll(int slot);
int net_finish(int slot, char *dst, int cap);
void net_abort(int slot);

/* --------------------------------------------------------------------------
 *  Compositor handoff (40_gfx.c, 80_wm.c, 95_main.c)
 *
 *  The WM decides each frame which rectangle of the framebuffer it had to
 *  regenerate; the frame loop presents exactly that rect, and the chassis
 *  restores the pointer sprite's pixels before the rect is painted again.
 * -------------------------------------------------------------------------- */
typedef struct { int x, y, w, h; } wm_rect_t;   /* must match 80_wm.c exactly  */
void wm_damage_all(void);
void wm_cursor_presented(void);
wm_rect_t wm_last_damage(void);
void gfx_cursor_restore(void);
void hal_present(int x, int y, int w, int h);

/* --------------------------------------------------------------------------
 *  Colors (0xAARRGGBB, straight alpha)
 * -------------------------------------------------------------------------- */
#define RGBA(r, g, b, a) (((u32)(a) << 24) | ((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))
#ifdef RGB
#  undef RGB                  /* win32 GDI macro; AE packs ARGB */
#endif
#define RGB(r, g, b)     RGBA(r, g, b, 255)
#define ALPHA(c)         (((c) >> 24) & 0xFF)
#define RED(c)           (((c) >> 16) & 0xFF)
#define GREEN(c)         (((c) >> 8) & 0xFF)
#define BLUE(c)          ((c) & 0xFF)

#define CLR_BLACK       RGB(0, 0, 0)
#define CLR_WHITE       RGB(255, 255, 255)

/* --------------------------------------------------------------------------
 *  Global machine state, declared here and defined in the HAL module.
 * -------------------------------------------------------------------------- */
typedef struct {
    u32  magic;
    u32  width, height, pitch, bpp;
    u32  total_ram, kernel_bytes, heap_bytes, disk_bytes;
    u32  cpu_mhz;
    char cpu_name[64];
    char host_os[64];
    u32  boot_ticks;
    int  running;
} machine_t;

extern machine_t g_machine;

/* --------------------------------------------------------------------------
 *  Misc helpers (10_util.c)
 * -------------------------------------------------------------------------- */
void  *kmemset(void *d, int c, size_t n);
void  *kmemcpy(void *d, const void *s, size_t n);
void  *kmemmove(void *d, const void *s, size_t n);
int    kmemcmp(const void *a, const void *b, size_t n);
size_t kstrlen(const char *s);
int    kstrcmp(const char *a, const char *b);
int    kstrncmp(const char *a, const char *b, size_t n);
char  *kstrcpy(char *d, const char *s);
char  *kstrncpy(char *d, const char *s, size_t n);
char  *kstrcat(char *d, const char *s);
int    katoi(const char *s);
u32    kstrtoul(const char *s, int base, int *ok);
int    ksnprintf(char *buf, size_t n, const char *fmt, ...);
int    kvsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int    kstrnicmp(const char *a, const char *b, size_t n);
char  *kstrchr(const char *s, int c);
char  *kstrrchr(const char *s, int c);
const char *kstrstr(const char *haystack, const char *needle);
char  *kstrncat(char *d, const char *s, size_t n);
void   kstrlower(char *s);
u32    krandom(void);
void   krand_seed(u32 s);
u32    k_abs(i32 v);
int    k_min(int a, int b);
int    k_max(int a, int b);
int    k_clamp(int v, int lo, int hi);
u32    k_isqrt(u32 v);                     /* integer square root (floor)     */
u64    now_us(void);
u32    now_ms(void);
void   klog(const char *fmt, ...);         /* kernel ring buffer + boot log    */
const char *klog_data(u32 *len);
void   klog_clear(void);
