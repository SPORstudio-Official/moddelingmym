/* ==========================================================================
 *  AE (Architecture Environment) -- Windows launcher stub
 *
 *  A small native executable that carries the entire single-file AE build inside
 *  itself, writes it to a private app folder and opens it in a browser's app
 *  mode (Edge or Chrome with --app: no tabs, no address bar, no menu).
 *
 *  Why a stub rather than an embedded engine: AE is a WebAssembly module and
 *  needs a JavaScript engine to host it.  Shipping one means embedding Chromium
 *  (~150 MB) or writing a JS interpreter.  Every Windows 10/11 machine already
 *  has Edge, so the honest single-file application is a small launcher carrying
 *  the OS, not a 150 MB bundler.
 *
 *  The window this opens must look like AE and never like Edge, which is why the
 *  stub does four extra things:
 *    - it carries its own icon and version resource (tools/launcher/ae.rc), so
 *      Explorer, the Start menu, search and the taskbar show the AE mark;
 *    - it registers an AE AppUserModelID and pins it on the browser window, so
 *      the taskbar groups and labels it "AE" instead of "Microsoft Edge";
 *    - it launches the browser with first-run, sync, restore-pages and default
 *      browser prompts disabled, so none of Edge's own chrome or nags appear;
 *    - it keeps the profile in %LOCALAPPDATA%\AE, not in Edge's.
 *
 *  Linked freestanding (-nostdlib), like the kernel: no C runtime, no imports
 *  beyond kernel32/shell32/advapi32/user32.
 * ========================================================================== */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

/* The payload follows this marker.  The marker lives in a data section so the
 * scanner below can find it in the mapped image; tools/build_exe.sh appends the
 * HTML after the whole PE file. */
__attribute__((used, section(".payload")))
static const char PAYLOAD_MAGIC[] = "\nAETHEROS-PAYLOAD-V1\n";

#define MAGIC_LEN (sizeof(PAYLOAD_MAGIC) - 1)

/* The compiler lowers ZeroMemory() and struct assignment to memset/memcpy even
 * in freestanding mode, so those two symbols have to exist here. */
void *memset(void *d, int c, size_t n) {
    unsigned char *p = (unsigned char *)d;
    size_t i;
    for (i = 0; i < n; i++) p[i] = (unsigned char)c;
    return d;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = (unsigned char *)d;
    const unsigned char *b = (const unsigned char *)s;
    size_t i;
    for (i = 0; i < n; i++) a[i] = b[i];
    return d;
}

/* ---- string helpers (no CRT) ------------------------------------------- */
static unsigned long long s_len(const char *s) {
    unsigned long long n = 0;
    while (s[n]) n++;
    return n;
}

static void s_copy(char *d, const char *s, unsigned long long cap) {
    unsigned long long i = 0;
    if (!cap) return;
    while (i + 1 < cap && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void s_cat(char *d, const char *s, unsigned long long cap) {
    unsigned long long n = s_len(d);
    unsigned long long i = 0;
    if (!cap) return;
    while (n + i + 1 < cap && s[i]) { d[n + i] = s[i]; i++; }
    d[n + i] = 0;
}

static void s_uitoa(unsigned long long v, char *out) {
    char tmp[24];
    int i = 0, n = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return; }
    while (v && i < 20) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) out[n++] = tmp[--i];
    out[n] = 0;
}

/* ---- payload ----------------------------------------------------------- *
 * The bytes after the marker are the HTML.  Length comes from the PE file size
 * on disk rather than a stored field: appending to a signed binary is easier to
 * verify, and a corrupt or truncated download is caught by the length check
 * instead of producing a half-written page. */
typedef struct {
    const unsigned char *data;
    unsigned long long   len;
} payload_t;

/* Read the appended HTML by re-opening this executable.
 *
 * The payload does not start immediately after the marker: the marker lives in a
 * data section inside the last mapped page, and the payload is appended after
 * the whole file, so there is padding in between.  Anchor on the HTML doctype,
 * which cannot appear in the stub's own code, and read to end of file. */
static payload_t payload_read(const char *self_path) {
    payload_t p = { 0, 0 };
    HANDLE h = CreateFileA(self_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    unsigned char *buf;
    DWORD read = 0;

    if (h == INVALID_HANDLE_VALUE) return p;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) { CloseHandle(h); return p; }
    buf = (unsigned char *)VirtualAlloc(NULL, (SIZE_T)size.QuadPart,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { CloseHandle(h); return p; }
    if (!ReadFile(h, buf, (DWORD)size.QuadPart, &read, NULL) || read != (DWORD)size.QuadPart) {
        VirtualFree(buf, 0, MEM_RELEASE);
        CloseHandle(h);
        return p;
    }
    CloseHandle(h);

    /* The HTML always starts with this; scan the tail (the payload is at the
     * end, and the stub is small, but keep the window generous). */
    {
        static const char anchor[] = "<!DOCTYPE html";
        const unsigned long long alen = sizeof(anchor) - 1;
        unsigned long long limit = (unsigned long long)read;
        unsigned long long start = limit > (64ULL * 1024 * 1024) ? limit - 64ULL * 1024 * 1024 : 0;
        unsigned long long i;
        for (i = start; i + alen <= limit; i++) {
            unsigned long long k;
            if (buf[i] != '<') continue;
            for (k = 1; k < alen; k++) {
                if (buf[i + k] != (unsigned char)anchor[k]) break;
            }
            if (k != alen) continue;
            p.data = buf + i;
            p.len = limit - i;
            return p;
        }
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    return p;
}

static int file_exists_path(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ---- browser discovery ------------------------------------------------- */
typedef struct {
    char path[MAX_PATH];
    char args[4096];
} launch_t;

static int find_browser(launch_t *out) {
    static const char *suffix[] = {
        "\\Microsoft\\Edge\\Application\\msedge.exe",
        "\\Google\\Chrome\\Application\\chrome.exe",
        "\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
    };
    static const char *envs[] = { "ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA" };
    unsigned int e, s;

    for (s = 0; s < 3; s++) {
        for (e = 0; e < 3; e++) {
            char root[MAX_PATH];
            DWORD n = GetEnvironmentVariableA(envs[e], root, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) continue;
            s_copy(out->path, root, MAX_PATH);
            s_cat(out->path, suffix[s], MAX_PATH);
            if (file_exists_path(out->path)) return 1;
        }
    }
    /* Edge is registered by App Paths even when installed to a non-standard
     * place, so fall back to that before giving up. */
    {
        HKEY key;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe",
                          0, KEY_READ, &key) == ERROR_SUCCESS) {
            DWORD cap = MAX_PATH, type = 0;
            LONG r = RegQueryValueExA(key, NULL, NULL, &type, (LPBYTE)out->path, &cap);
            RegCloseKey(key);
            if (r == ERROR_SUCCESS && file_exists_path(out->path)) return 1;
        }
    }
    return 0;
}


/* ---- application identity ---------------------------------------------- *
 * Windows decides what the taskbar button says, what icon it wears and which
 * window group it joins from the process's AppUserModelID -- and an inherited
 * one belongs to the browser.  Pinning our own ID on the browser window is what
 * replaces "Microsoft Edge" + the Edge logo with "AE" + the AE logo in the
 * taskbar, Alt+Tab and the notification area. */
#define AE_APPID L"SPORstudio.AE.ArchitectureEnvironment"

typedef HRESULT (WINAPI *SetAppIdFn)(PCWSTR);

/* Resolves shell32!SetCurrentProcessExplicitAppUserModelID at run time: the
 * stub links no import library for it, and this keeps the import table honest. */
static int ae_set_process_appid(void) {
    HMODULE h = LoadLibraryA("shell32.dll");
    SetAppIdFn set;
    if (!h) return 0;
    set = (SetAppIdFn)(void *)GetProcAddress(h, "SetCurrentProcessExplicitAppUserModelID");
    if (!set) return 0;
    return SUCCEEDED(set(AE_APPID));
}

/* Registers AE with the Windows shell so search, "Open with" and the taskbar
 * show the AE name and icon, not the host engine's.  Everything here is
 * per-user (HKCU) and best effort: failing to write a key must never stop the
 * app from starting. */
static void ae_register_shell(const char *exe_path) {
    HKEY k;
    const char *base = "Software\\Classes\\Applications\\AE.exe";
    if (RegCreateKeyExA(HKEY_CURRENT_USER, base, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)
        == ERROR_SUCCESS) {
        RegSetValueExA(k, "FriendlyAppName", 0, REG_SZ,
                       (const BYTE *)"AE", 3);
        RegCloseKey(k);
    }
    if (RegCreateKeyExA(HKEY_CURRENT_USER,
                        "Software\\Classes\\Applications\\AE.exe\\shell\\open\\command",
                        0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        char cmd[MAX_PATH + 8];
        s_copy(cmd, "\"", sizeof(cmd));
        s_cat(cmd, exe_path, sizeof(cmd));
        s_cat(cmd, "\" \"%1\"", sizeof(cmd));
        RegSetValueExA(k, NULL, 0, REG_SZ, (const BYTE *)cmd, (DWORD)s_len(cmd) + 1);
        RegCloseKey(k);
    }
    /* RegisteredApplications + Capabilities is what makes AE appear in
     * "Default apps" and in Start-menu search with its own name. */
    if (RegCreateKeyExA(HKEY_CURRENT_USER,
                        "Software\\RegisteredApplications", 0, NULL, 0,
                        KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        const char *cap = "Software\\SPORstudio\\AE\\Capabilities";
        RegSetValueExA(k, "AE", 0, REG_SZ, (const BYTE *)cap, (DWORD)s_len(cap) + 1);
        RegCloseKey(k);
    }
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\SPORstudio\\AE\\Capabilities",
                        0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(k, "ApplicationName", 0, REG_SZ, (const BYTE *)"AE", 3);
        RegSetValueExA(k, "ApplicationDescription", 0, REG_SZ,
                       (const BYTE *)"Architecture Environment", 25);
        RegCloseKey(k);
    }
}

/* A per-app override Windows consults when it groups a window: this is the
 * documented way to give an ID its own display name and icon. */
static void ae_register_appid(const char *exe_path) {
    HKEY k;
    const char *key =
        "Software\\Classes\\AppUserModelId\\SPORstudio.AE.ArchitectureEnvironment";
    if (RegCreateKeyExA(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)
        != ERROR_SUCCESS) return;
    RegSetValueExA(k, "DisplayName", 0, REG_SZ, (const BYTE *)"AE", 3);
    RegSetValueExA(k, "IconUri", 0, REG_SZ,
                   (const BYTE *)exe_path, (DWORD)s_len(exe_path) + 1);
    RegCloseKey(k);
}

/* ---- error reporting --------------------------------------------------- *
 * The stub is a GUI binary: a failure that printed nothing would look like the
 * machine did nothing at all, which is the defect the old standalone build had.
 * Every failure path ends in a message box. */
static void fail(const char *what, DWORD code) {
    char msg[600];
    s_copy(msg, "AE could not start.\n\n", sizeof(msg));
    s_cat(msg, what, sizeof(msg));
    if (code) {
        char num[24];
        s_uitoa(code, num);
        s_cat(msg, "\n\nWindows error code: ", sizeof(msg));
        s_cat(msg, num, sizeof(msg));
    }
    s_cat(msg, "\n\nIf this keeps happening, report it with the code above.", sizeof(msg));
    MessageBoxA(NULL, msg, "AE", MB_ICONERROR | MB_OK);
}

/* ---- main -------------------------------------------------------------- *
 * The command-line buffers are file-scope rather than automatic.  A function
 * whose frame exceeds 4 KB needs a stack probe (___chkstk_ms) on Win64, and a
 * freestanding link has no provider for it -- which is how adding the browser
 * flags turned a working build into an undefined-symbol error. */
static char g_url[2048];
static char g_prof[MAX_PATH];
static char g_self[MAX_PATH];
static char g_app[MAX_PATH];
static launch_t g_lt;

int WINAPI aether_launcher_entry(void) {
    char *self = g_self;
    char *app = g_app;
    payload_t p;
    launch_t *lt = &g_lt;
    int ok;

    if (!GetModuleFileNameA(NULL, self, MAX_PATH)) {
        fail("Cannot determine the location of this program.", GetLastError());
        return 1;
    }

    /* Own the taskbar entry, the Start-menu name and the icon before any window
       exists; then teach the shell about this executable. */
    ae_set_process_appid();
    ae_register_shell(self);
    ae_register_appid(self);

    /* Already extracted and unchanged?  Reuse it, so launching twice does not
     * rewrite the file (and so a slow disk is only paid once). */
    {
        DWORD n = GetTempPathA(MAX_PATH, app);
        if (n == 0 || n >= MAX_PATH) {
            fail("Cannot find the temporary folder.", GetLastError());
            return 1;
        }
        s_cat(app, "AE.html", MAX_PATH);
    }

    p = payload_read(self);
    if (!p.data || p.len < 1024) {
        fail("This file seems to be incomplete -- the AE payload is missing.\n"
             "Download AE.exe again.", 0);
        return 2;
    }

    /* write the payload out */
    {
        HANDLE h = CreateFileA(app, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD written = 0;
        if (h == INVALID_HANDLE_VALUE) {
            fail("Cannot write the application to the temporary folder.", GetLastError());
            return 3;
        }
        ok = WriteFile(h, p.data, (DWORD)p.len, &written, NULL) && written == (DWORD)p.len;
        CloseHandle(h);
        if (!ok) {
            fail("The application could not be written out (disk full?).", GetLastError());
            return 3;
        }
    }

    /* Open it in an app-style window.
     *
     * Every path we pass is quoted.  CreateProcessA gives the child one command
     * line and the child splits it itself, so an unquoted path breaks on any
     * username or temp folder containing a space -- which is most Windows
     * accounts ("C:\\Users\\Bob Smith\\...").  Chrome accepts a quoted value,
     * so the url and the profile path are both wrapped in quotes.  The profile
     * is private so the app gets its own window instead of handing the url to
     * an already-running browser and exiting. */
    ZeroMemory(lt, sizeof(*lt));
    if (find_browser(lt)) {
        char *url = g_url;
        char *prof = g_prof;
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;

        s_copy(url, "--app=\"file:///", sizeof(g_url));
        {
            unsigned long long i;
            char *path_forward = g_self;   /* self is no longer needed here */
            for (i = 0; i + 1 < MAX_PATH && app[i]; i++)
                path_forward[i] = (app[i] == '\\') ? '/' : app[i];
            path_forward[i] = 0;
            s_cat(url, path_forward, sizeof(g_url));
        }
        /* Flags that keep the host engine invisible.  Without them the first
         * launch shows Edge's "welcome / import your data" wizard, an infobar
         * about being signed out, and "restore pages?" after any crash -- all of
         * them make the app look like Edge rather than AE. */
        s_cat(url, "\""
                   " --app-window-size=1320,820"
                   " --window-size=1320,820"
                   " --no-first-run"
                   " --no-default-browser-check"
                   " --no-service-autorun"
                   " --disable-sync"
                   " --disable-features=msEdgeFirstRunExperience,msHubApps,"
                   "msEdgeIdentitySvc,IsolatedSandboxedIframes"
                   " --hide-crash-restore-bubble"
                   " --disable-background-networking"
                   " --noerrdialogs"
                   " --user-data-dir=\"", sizeof(g_url));

        /* Profile under %LOCALAPPDATA%\AE, not Edge's own folder: this is what
         * gives the window its own identity and keeps AE's storage separate. */
        {
            static char base[MAX_PATH];
            DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) {
                /* No local app data: fall back to the payload's own folder. */
                unsigned long long i, slash = 0;
                for (i = 0; app[i]; i++)
                    if (app[i] == '\\' || app[i] == '/') slash = i + 1;
                for (i = 0; i < slash && i + 1 < MAX_PATH; i++) prof[i] = app[i];
                prof[i] = 0;
            } else {
                s_copy(prof, base, sizeof(prof));
                s_cat(prof, "\\AE", sizeof(prof));
            }
        }
        s_cat(url, prof, sizeof(g_url));
        s_cat(url, "\"", sizeof(g_url));

        if (CreateProcessA(lt->path, lt->args, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 0;
        }
    }

    /* No Chromium-family browser: let Windows pick. */
    {
        HINSTANCE r = ShellExecuteA(NULL, "open", app, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) {
            fail("Cannot open AE.html -- no web browser was found.", (DWORD)(INT_PTR)r);
            return 4;
        }
    }
    return 0;
}
