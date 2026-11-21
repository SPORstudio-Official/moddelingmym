/* ==========================================================================
 *  AE (Architecture Environment) -- Windows launcher stub
 *
 *  A small native executable that carries the whole AE operating system inside
 *  itself as an appended second PE image, writes it to a private app folder and
 *  starts it.  The extracted image is a real Win32 program -- its own window,
 *  its own message loop, its own compositor and its own HTTP client -- so what
 *  runs is AE, not a web page in someone else's browser.
 *
 *  Why a stub rather than a single unsigned blob: Windows caches an executable's
 *  icon and version metadata in the shell cache, and a file whose bytes change
 *  on every build keeps the same cache key.  Keeping the outer file stable and
 *  version-tagged (AE-<tag>.exe) means an upgrade never collides with a copy
 *  that is currently running, which on Windows would be a sharing violation.
 *
 *  The stub also does the shell work the OS image should not have to do every
 *  launch:
 *    - it carries its own icon and version resource (tools/launcher/ae.rc), so
 *      Explorer, the Start menu, search and the taskbar show the AE mark;
 *    - it registers an AE AppUserModelID, so the taskbar groups and labels the
 *      window "AE";
 *    - it teaches the shell about the executable (open verb, registered
 *      application, capabilities) so AE appears in Start-menu search and
 *      "Default apps" under its own name.
 *
 *  Linked freestanding (-nostdlib), like the kernel: no C runtime, no imports
 *  beyond kernel32/shell32/advapi32/user32.
 * ========================================================================== */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

/* The payload follows this marker.  The marker lives in a data section so the
 * scanner below can find it; tools/build_exe.sh appends a second PE image after
 * the whole outer file, so the reader walks from the marker to the next MZ. */
__attribute__((used, section(".payload")))
static const char PAYLOAD_MAGIC[] = "\nAETHEROS-PAYLOAD-V1\n";

#define MAGIC_LEN (sizeof(PAYLOAD_MAGIC) - 1)

/* Bumped together with the OS version.  The extracted image name includes it,
 * so upgrading AE never tries to overwrite an image the previous version still
 * has open. */
#define AE_IMAGE_TAG "0_2_0"

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

/* ---- application identity ---------------------------------------------- *
 * These are what make Windows call the program AE everywhere: the taskbar
 * button (AppUserModelID), Start-menu search and "Default apps" (registered
 * application + capabilities) and Explorer's open verb.  They are static
 * because the freestanding link has no CRT to run constructors. */
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
 * Every failure path ends in a message box: a silent exit is what made the
 * first browser-based build look like it did nothing at all. */
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

    /* The appended bytes are a second PE image: the native OS executable.
     * Find the marker the build script wrote into the stub, then take the next
     * MZ/PE header after it -- the marker itself lives inside a mapped data
     * section, so there is padding between the two. */
    {
        const unsigned long long limit = (unsigned long long)read;
        unsigned long long i, j;
        for (i = 0; i + sizeof(PAYLOAD_MAGIC) <= limit; i++) {
            unsigned long long k;
            if (buf[i] != (unsigned char)PAYLOAD_MAGIC[0]) continue;
            for (k = 1; k < sizeof(PAYLOAD_MAGIC) - 1; k++)
                if (buf[i + k] != (unsigned char)PAYLOAD_MAGIC[k]) break;
            if (k != sizeof(PAYLOAD_MAGIC) - 1) continue;
            j = i + k;
            for (; j + 0x40 < limit; j++) {
                if (buf[j] == 'M' && buf[j + 1] == 'Z'
                    && buf[j + 0x3c] < 0x40
                    && buf[j + 0x3c + 4] >= 0x40
                    && buf[j + 0x3c + 4] <= 0xf0) {
                    p.data = buf + j;
                    p.len = limit - j;
                    return p;
                }
            }
        }
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    return p;
}


/* ---- main -------------------------------------------------------------- *
 * The command-line buffers are file-scope rather than automatic.  A function
 * whose frame exceeds 4 KB needs a stack probe (___chkstk_ms) on Win64, and a
 * freestanding link has no provider for it -- which is how adding the browser
 * flags turned a working build into an undefined-symbol error. */
static char g_url[2048];   /* quoted command line for the OS image */
static char g_self[MAX_PATH];
static char g_app[MAX_PATH];

int WINAPI aether_launcher_entry(void) {
    char *self = g_self;
    char *app = g_app;
    payload_t p;
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

    /* Where the OS image is extracted.  A private folder under %LOCALAPPDATA%
     * rather than %TEMP%: the image is an executable, and virus scanners and
     * cleanup tools treat %TEMP% executables far more aggressively (some Windows
     * configurations refuse to run them at all).  %LOCALAPPDATA%\AE is the same
     * folder the installer uses, so a per-user install and a portable copy share
     * one image instead of extracting twice. */
    {
        char base[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            DWORD t = GetTempPathA(MAX_PATH, app);
            if (t == 0 || t >= MAX_PATH) {
                fail("Cannot find a folder to store the application in.", GetLastError());
                return 1;
            }
        } else {
            s_copy(app, base, MAX_PATH);
            s_cat(app, "\\AE", MAX_PATH);
            CreateDirectoryA(app, NULL);
            s_cat(app, "\\", MAX_PATH);
        }
        s_cat(app, "AE-", MAX_PATH);
        /* One image per AE version: an upgrade extracts alongside the old one
           instead of overwriting a file that a running copy has open (which
           fails with a sharing violation on Windows). */
        s_cat(app, AE_IMAGE_TAG, MAX_PATH);
        s_cat(app, ".exe", MAX_PATH);
    }

    p = payload_read(self);
    if (!p.data || p.len < 1024) {
        fail("This file seems to be incomplete -- the AE payload is missing.\n"
             "Download AE.exe again.", 0);
        return 2;
    }

    /* Extract the payload unless the right image is already there.  Comparing
     * the size is enough: the name carries the version tag, so a match means it
     * came from this build. */
    ok = 0;
    {
        HANDLE hin = CreateFileA(app, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
        if (hin != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            if (GetFileSizeEx(hin, &sz) && (unsigned long long)sz.QuadPart == (unsigned long long)p.len)
                ok = 1;
            CloseHandle(hin);
        }
    }
    if (!ok) {
        HANDLE h = CreateFileA(app, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD written = 0;
        if (h == INVALID_HANDLE_VALUE) {
            fail("Cannot write the application into \n%LOCALAPPDATA%\\AE.", GetLastError());
            return 3;
        }
        ok = WriteFile(h, p.data, (DWORD)p.len, &written, NULL) && written == (DWORD)p.len;
        CloseHandle(h);
        if (!ok) {
            fail("The application could not be written out (disk full?).", GetLastError());
            return 3;
        }
    }

    /* Run the payload.
     *
     * The payload is not a web page any more: tools/build_exe.sh appends a real
     * native executable (built from the same dev/ sources, with Win32 as its
     * chassis) and this launcher simply starts it.  That is what makes AE a
     * standalone application rather than a browser shortcut -- it has its own
     * window, its own event loop, its own compositor, and it fetches pages with
     * its own HTTP client.
     *
     * The launcher still does the things a shell integration needs: it owns the
     * icon, the version metadata, the AppUserModelID and the Start-menu entry,
     * and it keeps the extracted image in a private folder so a read-only or
     * roaming profile cannot break it.
     */
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char *cmd = g_url;
        DWORD flags = 0;

        /* Quote the image path: CreateProcess splits the command line itself, so
           an unquoted path breaks on any username or folder with a space. */
        s_copy(cmd, "\"", sizeof(g_url));
        s_cat(cmd, app, sizeof(g_url));
        s_cat(cmd, "\"", sizeof(g_url));

        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);

        if (CreateProcessA(app, cmd, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 0;
        }
        {
            /* One retry with a fresh copy: a stale extracted image (half written
               by an interrupted previous run) fails to load, and rewriting it is
               cheaper than asking the user to do anything. */
            HANDLE h = CreateFileA(app, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(h, p.data, (DWORD)p.len, &written, NULL);
                CloseHandle(h);
                if (CreateProcessA(app, cmd, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    return 0;
                }
            }
        }
        fail("AE could not start its own OS image.\n\n"
             "The file below could not be run:", GetLastError());
        MessageBoxA(NULL, app, "AE", MB_ICONINFORMATION | MB_OK);
        return 4;
    }

    return 1;
}
