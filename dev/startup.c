/* ==========================================================================
 *  STARTUP -- process entry sequence for a runtime-less Windows executable
 *
 *  The Windows loader does not call main() for a PE image; it calls whatever
 *  address the header names as the entry point, and normally that address is
 *  the C runtime's mainCRTStartup, which initialises the CRT, parses the
 *  command line and then calls main()/WinMain().  AetherOS does not use the C
 *  runtime at all, so this file provides the equivalent of that prologue in a
 *  few dozen bytes: capture the process instance handle, run the kernel entry
 *  point declared in 95_main.c, and terminate with its exit code.
 *
 *  Nothing here touches the CRT, so the resulting import table contains only
 *  kernel32, user32, gdi32 and winmm.
 * ========================================================================== */

int aether_entry(void);

/* Resolved from kernel32 by name: the OS loader guarantees these exports
   exist, and importing them directly keeps the CRT out of the picture. */
__declspec(dllimport) void __stdcall ExitProcess(unsigned int code);
__declspec(dllimport) int __stdcall SetProcessDPIAware(void);
__declspec(dllimport) int __stdcall SetProcessDpiAwarenessContext(void *ctx);

void aether_crt_entry(void) {
    int rc;
    /* Report a DPI-aware process so the 1280x720 window is not bitmap-stretched
       by virtualisation on high-DPI displays.  The newer context call is only
       present on Windows 10 1703+, so it is used first and the legacy call is
       the fallback; the import is by name and resolved at load time from
       kernel32, which ships both. */
    SetProcessDPIAware();
    rc = aether_entry();
    ExitProcess((unsigned int)rc);
}
