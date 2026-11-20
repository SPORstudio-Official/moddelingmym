/* ==========================================================================
 *  AetherOS -- Windows launcher stub
 *
 *  A small native executable that carries the entire single-file AetherOS build
 *  inside itself, writes it to a temp file and opens it in a browser's app mode
 *  (Edge or Chrome with --app: no tabs, no address bar, no menu).
 *
 *  Why a stub rather than an embedded engine: AetherOS is a WebAssembly module
 *  and needs a JavaScript engine to host it.  Shipping one means embedding
 *  Chromium (~150 MB) or writing a JS interpreter.  Every Windows 10/11 machine
 *  already has Edge, so the honest single-file application is a small launcher
 *  carrying the OS, not a 150 MB bundler.  If no browser is found the stub
 *  falls back to ShellExecute, which opens the default one.
 *
 *  Linked freestanding (-nostdlib), like the kernel: no C runtime, no imports
 *  beyond kernel32 and shell32.
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
    char args[1024];
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

/* ---- error reporting --------------------------------------------------- *
 * The stub is a GUI binary: a failure that printed nothing would look like the
 * machine did nothing at all, which is the defect the old standalone build had.
 * Every failure path ends in a message box. */
static void fail(const char *what, DWORD code) {
    char msg[600];
    s_copy(msg, "AetherOS could not start.\n\n", sizeof(msg));
    s_cat(msg, what, sizeof(msg));
    if (code) {
        char num[24];
        s_uitoa(code, num);
        s_cat(msg, "\n\nWindows error code: ", sizeof(msg));
        s_cat(msg, num, sizeof(msg));
    }
    s_cat(msg, "\n\nIf this keeps happening, report it with the code above.", sizeof(msg));
    MessageBoxA(NULL, msg, "AetherOS", MB_ICONERROR | MB_OK);
}

/* ---- main -------------------------------------------------------------- */
int WINAPI aether_launcher_entry(void) {
    char self[MAX_PATH];
    char app[MAX_PATH];
    payload_t p;
    launch_t lt;
    int ok;

    if (!GetModuleFileNameA(NULL, self, MAX_PATH)) {
        fail("Cannot determine the location of this program.", GetLastError());
        return 1;
    }

    /* Already extracted and unchanged?  Reuse it, so launching twice does not
     * rewrite the file (and so a slow disk is only paid once). */
    {
        DWORD n = GetTempPathA(MAX_PATH, app);
        if (n == 0 || n >= MAX_PATH) {
            fail("Cannot find the temporary folder.", GetLastError());
            return 1;
        }
        s_cat(app, "AetherOS.html", MAX_PATH);
    }

    p = payload_read(self);
    if (!p.data || p.len < 1024) {
        fail("This file seems to be incomplete -- the AetherOS payload is missing.\n"
             "Download AetherOS.exe again.", 0);
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
    ZeroMemory(&lt, sizeof(lt));
    if (find_browser(&lt)) {
        char url[MAX_PATH + 96];
        char prof[MAX_PATH];
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;

        s_copy(url, "--app=\"file:///", sizeof(url));
        {
            unsigned long long i;
            char path_forward[MAX_PATH];
            for (i = 0; i + 1 < MAX_PATH && app[i]; i++)
                path_forward[i] = (app[i] == '\\') ? '/' : app[i];
            path_forward[i] = 0;
            s_cat(url, path_forward, sizeof(url));
        }
        s_cat(url, "\" --window-size=1320,820 --user-data-dir=\"", sizeof(url));
        {
            unsigned long long i, slash = 0;
            for (i = 0; app[i]; i++)
                if (app[i] == '\\' || app[i] == '/') slash = i + 1;
            for (i = 0; i < slash && i + 1 < MAX_PATH; i++) prof[i] = app[i];
            prof[i] = 0;
        }
        s_cat(url, prof, sizeof(url));
        s_cat(url, "AetherOS-profile\"", sizeof(url));

        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        /* CreateProcess hands the child one raw command line and the child
         * splits it itself.  Chrome reads argv[0] as its own path, so the line
         * has to start with the executable, quoted, or the --app flag would be
         * consumed as the program name and no window would open. */
        s_copy(lt.args, "\"", sizeof(lt.args));
        s_cat(lt.args, lt.path, sizeof(lt.args));
        s_cat(lt.args, "\" ", sizeof(lt.args));
        s_cat(lt.args, url, sizeof(lt.args));

        if (CreateProcessA(lt.path, lt.args, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 0;
        }
    }

    /* No Chromium-family browser: let Windows pick. */
    {
        HINSTANCE r = ShellExecuteA(NULL, "open", app, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) {
            fail("Cannot open AetherOS.html -- no web browser was found.", (DWORD)(INT_PTR)r);
            return 4;
        }
    }
    return 0;
}
