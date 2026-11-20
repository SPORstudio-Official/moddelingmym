/* ==========================================================================
 *  MAIN -- kernel entry, boot sequence, panic screen and the frame loop
 * ========================================================================== */

static int g_boot_stage;
static u32 g_boot_start_ms;
static int g_panic_active;
static u32 g_panic_ms;
static char g_panic_reason[128];
static u32 g_panic_eip, g_panic_pid;
/* Timestamp of the previous frame, used to derive the frame delta.  Hoisted
   out of the loop body when the frame was split into aether_frame(). */
static u64 g_last_frame;
/* Negative hal_init() result if the machine failed to start (0 on success). */
static int g_aether_boot_error;


/* -------------------------------------------------------------------------- */
/*  BIOS POST / boot animation                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *tag;
    const char *text;
    u32  visible_at_ms;
} boot_line_t;

static const boot_line_t g_boot_lines[] = {
    { "BIOS",  "AetherBIOS v2.14 -- POST (press SPACE to skip)",        0   },
    { "BIOS",  "Detecting CPU ......................... AetherVirtual @ 2.4 GHz", 180 },
    { "BIOS",  "Memory test .......................... 65536 KB OK",    330 },
    { "BIOS",  "Initialising display .................. 1280x720x32",   470 },
    { "KERNEL","Loading /boot/kernel.sys ............... 412 KiB",        620 },
    { "KERNEL","[  OK  ] Initializing Physical Memory Manager (64 MB)",  780 },
    { "KERNEL","[  OK  ] Installing IDT & remapping PIC to 0x20",        900 },
    { "KERNEL","[  OK  ] Starting IRQ0 timer at 1000 Hz",                1010 },
    { "KERNEL","[  OK  ] Probing i8042 keyboard controller (IRQ1)",      1110 },
    { "KERNEL","[  OK  ] Probing PS/2 auxiliary port (IRQ12)",           1210 },
    { "KERNEL","[  OK  ] Mounting Inode VFS (16384 KB)",                 1330 },
    { "KERNEL","[  OK  ] Registering /proc virtual filesystem",          1440 },
    { "KERNEL","[  OK  ] Spawning init (PID 1) & Desktop Compositor",    1560 },
    { "KERNEL","[  OK  ] Calibrating AetherC compiler (45 opcodes)",     1670 },
    { "KERNEL","[  OK  ] Reached target Graphical Interface",            1780 },
    { "KERNEL","Starting desktop session ...",                           1900 }
};
#define BOOT_LINE_COUNT ((int)(sizeof(g_boot_lines) / sizeof(g_boot_lines[0])))

static void boot_render(u32 elapsed_ms) {
    int i;

    int blink = (int)((elapsed_ms / 380) % 2);
    theme_apply(theme_index());


    gfx_reset_clip();
    gfx_gradient_v(0, 0, SCREEN_W, SCREEN_H, RGB(2, 4, 8), RGB(8, 14, 24));
    gfx_radial_glow(SCREEN_W / 2, 260, 420, RGBA(30, 90, 160, 40));

    /* logo */
    {
        int cx = SCREEN_W / 2, cy = 150;
        int r = 54 + (int)(6 * k_sin((double)elapsed_ms / 260.0));
        gfx_circle(cx, cy, r, RGBA(60, 150, 220, 180));
        gfx_circle(cx, cy, r - 6, RGBA(30, 90, 150, 120));
        gfx_fill_circle(cx, cy, 10, RGBA(120, 220, 255, 220));
        {
            int k;
            for (k = 0; k < 8; k++) {
                double a = (double)k * 3.14159265 / 4.0 + (double)elapsed_ms / 900.0;
                gfx_line(cx + (int)(k_cos(a) * 16), cy + (int)(k_sin(a) * 16),
                         cx + (int)(k_cos(a) * (r - 10)), cy + (int)(k_sin(a) * (r - 10)),
                         RGBA(90, 190, 255, 120));
            }
        }
        gfx_text_center(cx, cy + r + 18, "AetherOS", RGB(226, 240, 255), 2);
        gfx_text_center(cx, cy + r + 44, AETHER_VERSION_STR "  \"" AETHER_CODENAME "\"  --  single-executable operating system",
                        RGB(120, 150, 180), 0);
    }

    /* log lines */
    for (i = 0; i < BOOT_LINE_COUNT; i++) {
        int y = 300 + i * 20;
        u32 col = RGB(150, 170, 190);
        if (elapsed_ms < g_boot_lines[i].visible_at_ms) break;
        if (!kstrcmp(g_boot_lines[i].tag, "BIOS")) col = RGB(120, 140, 160);
        if (!kstrncmp(g_boot_lines[i].text, "[  OK  ]", 8)) col = RGB(120, 210, 160);
        gfx_text(260, y, g_boot_lines[i].tag, RGB(80, 130, 190), 0);
        gfx_text(320, y, g_boot_lines[i].text, col, 0);
        /* little status dot */
        gfx_fill_circle_aa(244, y + 7, 3, RGB(70, 110, 160));
    }
    if (blink) gfx_fill_rect(260, 300 + BOOT_LINE_COUNT * 20, 8, 14, RGB(140, 200, 240));

    /* progress bar */
    {
        int w = 760, x = (SCREEN_W - w) / 2, y = SCREEN_H - 120;
        u32 pct = k_min(100, (elapsed_ms * 100) / 2600u);
        char buf[64];
        gfx_round_rect(x, y, w, 8, 4, RGBA(255, 255, 255, 20));
        gfx_round_rect_grad(x, y, (int)(w * pct / 100) + 1, 8, 4,
                            RGB(90, 190, 255), RGB(60, 120, 220));
        ksnprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
        gfx_text_right(x + w, y + 16, buf, RGB(140, 170, 200), 0);
        gfx_text_center(SCREEN_W / 2, y + 16, "booting the kernel ...", RGB(120, 150, 180), 0);
    }
    gfx_draw_cursor(g_wm.mouse_px, g_wm.mouse_py, CURSOR_ARROW);
}

/* -------------------------------------------------------------------------- */
/*  kernel panic screen                                                     */
/* -------------------------------------------------------------------------- */

void kernel_panic(const char *reason, u32 eip, u32 pid) {
    g_panic_active = 1;
    g_panic_ms = now_ms();
    kstrncpy(g_panic_reason, reason ? reason : "unspecified fault", sizeof(g_panic_reason) - 1);
    g_panic_eip = eip;
    g_panic_pid = pid;
    gfx_set_scanlines(1);
    klog("PANIC: %s at eip=0x%08X pid=%u", g_panic_reason, (unsigned)eip, (unsigned)pid);
}

static void panic_render(u32 elapsed) {
    int y;
    char buf[220];
    u32 blink = (elapsed / 450) % 2;
    /* authentic text-mode blue screen, drawn with the 8x16 console font */
    gfx_reset_clip();
    gfx_fill_rect(0, 0, SCREEN_W, SCREEN_H, RGB(0, 0, 168));
    gfx_fill_rect(0, 0, SCREEN_W, 22, RGB(0, 0, 168));
    gfx_text_term(40, 40, "*** AetherOS KERNEL PANIC ***", RGB(255, 255, 255), 0, 0);
    y = 72;
    ksnprintf(buf, sizeof(buf), "reason: %s", g_panic_reason);
    gfx_text_term(40, y, buf, RGB(255, 255, 255), 0, 0); y += 24;
    ksnprintf(buf, sizeof(buf), "EIP=0x%08X  PID=%u (%s)  kernel %s", (unsigned)g_panic_eip,
              (unsigned)g_panic_pid,
              proc_pcb(g_panic_pid) ? proc_pcb(g_panic_pid)->name : "kernel",
              AETHER_VERSION_STR);
    gfx_text_term(40, y, buf, RGB(255, 255, 255), 0, 0); y += 32;

    gfx_text_term(40, y, "CPU registers:", RGB(200, 200, 255), 0, 0); y += 20;
    ksnprintf(buf, sizeof(buf), "  eax=%08X ebx=%08X ecx=%08X edx=%08X",
              (unsigned)(0x41455448u), (unsigned)(mm_used()), (unsigned)(irq_ticks()),
              (unsigned)(proc_running_pid()));
    gfx_text_term(40, y, buf, RGB(255, 255, 255), 0, 0); y += 18;
    ksnprintf(buf, sizeof(buf), "  esi=%08X edi=%08X ebp=%08X esp=%08X",
              (unsigned)vfs_resolve("/"), (unsigned)wm_focused_id(),
              (unsigned)(uintptr_t)&g_machine, (unsigned)(mm_heap_base() + mm_used()));
    gfx_text_term(40, y, buf, RGB(255, 255, 255), 0, 0); y += 18;
    ksnprintf(buf, sizeof(buf), "  eip=%08X eflags=%08X cr0=%08X cr3=%08X",
              (unsigned)g_panic_eip, 0x00010246u, 0x80000011u, 0x00040000u);
    gfx_text_term(40, y, buf, RGB(255, 255, 255), 0, 0); y += 30;

    gfx_text_term(40, y, "Stack trace (most recent call first):", RGB(200, 200, 255), 0, 0); y += 20;
    {
        static const char *frames[] = {
            "  [<00000000>] kernel_panic+0x2A/0x40",
            "  [<0000A1C4>] trap_handler+0x88/0xC0",
            "  [<00012F88>] irq_dispatch+0x1C/0x40",
            "  [<0003C210>] sched_tick+0x44/0x90",
            "  [<0005AA08>] _start+0x0/0x10"
        };
        int i;
        for (i = 0; i < 5; i++) {
            gfx_text_term(40, y, frames[i], RGB(230, 230, 255), 0, 0);
            y += 18;
        }
    }
    y += 16;
    gfx_text_term(40, y, "Process table snapshot:", RGB(200, 200, 255), 0, 0); y += 20;
    {
        int shown = 0;
        u32 i;
        for (i = 1; i < MAX_PROCS && shown < 6; i++) {
            pcb_t *p = proc_pcb(i);
            if (!p) continue;
            ksnprintf(buf, sizeof(buf), "  pid %-3u %-12s %-8s cpu %3u%%  mem %u B",
                      (unsigned)p->pid, p->name, proc_state_name(p->state),
                      (unsigned)p->cpu_percent, (unsigned)p->mem_used);
            gfx_text_term(40, y, buf, i == g_panic_pid ? RGB(255, 220, 120) : RGB(230, 230, 255), 0, 0);
            y += 18;
            shown++;
        }
    }
    y += 20;
    gfx_text_term(40, y, "Kernel log tail:", RGB(200, 200, 255), 0, 0); y += 20;
    {
        u32 len = 0;
        const char *log = klog_data(&len);
        int lines = 0;
        u32 start = 0, i;
        /* find the last 5 lines */
        for (i = len; i > 0 && lines < 5; i--) if (log[i - 1] == '\n') lines++;
        start = (lines >= 5) ? (i + 1) : 0;
        {
            const char *p = log + start;
            while (*p && y < SCREEN_H - 70) {
                char line[200];
                int n = 0;
                while (*p && *p != '\n' && n < 190) line[n++] = *p++;
                line[n] = 0;
                if (*p == '\n') p++;
                gfx_text_term(40, y, line, RGB(210, 220, 255), 0, 0);
                y += 16;
            }
        }
    }
    if (blink)
        gfx_text_term(40, SCREEN_H - 48, "Press [R] to reboot hardware, [D] to dump state, [ESC] to keep going",
                      RGB(255, 255, 255), 0, 0);
}

/* -------------------------------------------------------------------------- */
/*  main frame                                                              */
/* -------------------------------------------------------------------------- */

static u32 g_frame_no;

static void present_dirty_region(void) {
    static int last_x0, last_y0, last_x1, last_y1, have_last;
    hal_present(0, 0, SCREEN_W, SCREEN_H);
    hal_frame_boundary(++g_frame_no);
    (void)last_x0; (void)last_y0; (void)last_x1; (void)last_y1; (void)have_last;
}

static void handle_global_hotkeys(int key, int ch, int mods) {
    (void)mods;
    if (key == 0x7B) {          /* F12: dump a screenshot (headless builds) */
#ifdef AETHER_HEADLESS
        hal_dump_ppm("/tmp/aether_frame.ppm");
#endif
    }
    if (key == 0x73) {          /* F4: crash on purpose */
        kernel_panic("deliberate panic requested by F4", (unsigned)0xDEAD0F4, 1);
    }
    if (ch == '`' && (mods & MOD_CTRL)) {
        gfx_set_scanlines(!gfx_scanlines_enabled());
    }
}

/* Boot the kernel once: HAL, physical memory, VFS, scheduler, compositor.
   Returns 0 on success.  This is split out of aether_main so a host that cannot
   block -- the wasm shell drives the machine from requestAnimationFrame -- can
   boot first and then step the machine one frame at a time. */
int aether_boot(int argc, char **argv) {
    int rc;
    (void)argc; (void)argv;
    kmemset(&g_machine, 0, sizeof(g_machine));
    rc = hal_init();
    if (rc != 0) {
        /* Keep the code so the shell can report *which* step failed instead of
           exiting silently -- the exact gap that made the Win32 build look
           like it did nothing at all. */
        g_aether_boot_error = rc;
#ifdef AETHER_WASM
        js_log("hal_init failed");
#endif
        return 1;
    }
    klog_clear();
    klog("AetherOS %s (%s) booting on the %s chassis", AETHER_VERSION_STR, AETHER_CODENAME,
         g_machine.host_os);
    klog("hal: physical RAM 0x00000000-0x%08X reserved as one contiguous block",
         (unsigned)PHYS_RAM_SIZE);
    klog("hal: framebuffer %dx%d, pitch %d, 32 bpp linear", SCREEN_W, SCREEN_H, VRAM_PITCH);

    irq_init();
    mm_init();
    vfs_mount();
    proc_init();
    cc_init();
    irq_mouse_set_pos(SCREEN_W / 2, SCREEN_H / 2);
    wm_init();
    proc_boot_daemons();
    proc_set_duty(proc_spawn("sysmon", 1, 6, PROC_FLAG_KERNEL, 0), 2, 40);
    proc_spawn("shell", 1, 14, PROC_FLAG_KERNEL | PROC_FLAG_GUI, 0);
    klog("kernel: init complete, entering the compositor loop");

    g_boot_start_ms = now_ms();
    g_boot_stage = 1;
    g_last_frame = now_us();
    return 0;
}

/* --------------------------------------------------------------------------
 *  One frame of the machine
 *
 *  Everything the old `while (hal_running())` loop did for a single iteration
 *  lives here, so that either entry style can drive it:
 *
 *    - aether_main() calls it in a loop and then hal_sleep_ms(), which is what
 *      the Win32 and headless chassis want (a blocking sleep is fine there);
 *    - the wasm shell calls aether_frame() straight from requestAnimationFrame
 *      and never sleeps, because a wasm module cannot block its host.
 *
 *  Returns 1 while the machine is still running, 0 once it has shut down.
 * ------------------------------------------------------------------------ */
int aether_frame(void) {
    hal_event_t ev;
    u64 now;
    u32 dt;
    u32 frame_dt = 16;

    while (hal_poll_event(&ev)) {
        if (ev.type == HAL_EV_QUIT) { hal_request_quit(); break; }
        irq_host_event(&ev);
    }
    irq_timer_poll();
    now = now_us();
    /* g_last_frame persists between calls: a local would be reset on entry and
       every frame would see the whole uptime as its delta (clamped to 100 ms),
       which makes animations and the scheduler run at a fixed wrong speed. */
    dt = (u32)((now - g_last_frame) / 1000);
    if (dt > 100) dt = 100;
    g_last_frame = now;
    frame_dt = dt;

    /* drain keyboard/mouse hardware queues into the desktop */
    {
        kbd_event_t ke;
        while (irq_keyboard_pop(&ke)) {
            if (!ke.pressed) continue;
            if (g_boot_stage == 1) {
                if (ke.scancode == 0x39 || ke.scancode == 0x1C) {
                    g_boot_start_ms = now_ms() - 3000;
                }
                continue;
            }
            if (g_panic_active) {
                if (ke.ascii == 'r' || ke.ascii == 'R') {
                    g_panic_active = 0;
                    wm_state()->reboot_requested = 1;
                } else if (ke.scancode == 0x01) {
                    g_panic_active = 0;
                } else if (ke.ascii == 'd' || ke.ascii == 'D') {
                    klog("PANIC: state dumped to /var/log/panic.log");
                    vfs_write_string("/var/log/panic.log", klog_data(NULL));
                }
                continue;
            }
            {
                /* map scancodes back to host virtual keys for the apps */
                static const struct { u8 sc; int vk; } map[] = {
                    {0x1C, 0x0D}, {0x0E, 0x08}, {0x0F, 0x09}, {0x01, 0x1B},
                    {0x48, 0x26}, {0x50, 0x28}, {0x4B, 0x25}, {0x4D, 0x27},
                    {0x47, 0x24}, {0x4F, 0x23}, {0x49, 0x21}, {0x51, 0x22},
                    {0x53, 0x2E}, {0x52, 0x2D}, {0x3B, 0x70}, {0x3C, 0x71},
                    {0x3D, 0x72}, {0x3E, 0x73}, {0x3F, 0x74}, {0x40, 0x75}
                };
                int vk = 0;
                int mi;
                int special = 0;
                /* Scancode 0 means the host handed us a translated glyph:
                   the character itself is the key code.  Otherwise the
                   scancode decides, and only known special keys map to a
                   virtual-key code.  Getting this backwards makes plain
                   letters look like function keys ('s' == 0x73 == F4). */
                /* Glyph-only events carry NO virtual-key code: ASCII
                   collides with VK codes ('.' is 0x2E, i.e. VK_DELETE, and
                   '-' is 0x2D, i.e. VK_INSERT), which used to make typing
                   those characters act as cursor keys.  Apps read the
                   character from `ch` and only special keys get a code. */
                if (ke.scancode == 0) vk = 0;
                for (mi = 0; mi < (int)(sizeof(map) / sizeof(map[0])); mi++)
                    if (ke.scancode == map[mi].sc) { vk = map[mi].vk; special = 1; break; }
                if (special) handle_global_hotkeys(vk, 0, ke.mods);
                wm_on_key(vk, ke.ascii, ke.mods);
            }
        }
        {
            mouse_event_t me;
            while (irq_mouse_pop(&me)) {
                int buttons = me.buttons;
                if (g_boot_stage != 1 && !g_panic_active) {
                    /* Order matters: the button transition is detected from
                       the previous state, and the move handler must not be
                       allowed to clear it before wm_on_mouse_down/up run. */
                    int was_down = g_wm.mouse_down;
                    g_wm.mouse_px = me.x;
                    g_wm.mouse_py = me.y;
                    if (me.dz) wm_on_wheel(me.x, me.y, me.dz);
                    if ((buttons & 1) && !was_down) {
                        wm_on_mouse_down(me.x, me.y, 1);
                        wm_on_mouse_move(me.x, me.y, buttons);
                    } else if (!(buttons & 1) && was_down) {
                        wm_on_mouse_up(me.x, me.y, 1);
                        wm_on_mouse_move(me.x, me.y, buttons);
                    } else {
                        wm_on_mouse_move(me.x, me.y, buttons);
                    }
                }
            }
        }
    }

    /* boot phase */
    if (g_boot_stage == 1) {
        u32 elapsed = now_ms() - g_boot_start_ms;
        boot_render(elapsed);
        present_dirty_region();
        if (elapsed > 2650) {
            g_boot_stage = 2;
            {
                const app_desc_t *d = app_by_id("sh");
                if (d) wm_open_app(d, NULL);
            }
            notify_ex("Welcome to AetherOS -- right-click a desktop icon", 1);
        }
        return hal_running();
    }

    /* panic phase */
    if (g_panic_active) {
        panic_render(now_ms() - g_panic_ms);
        present_dirty_region();
        return hal_running();
    }

    /* main desktop */
    proc_tick();
    cc_step_all(600);
    wm_update(frame_dt);
    wm_render();
    present_dirty_region();

    if (wm_take_quit()) { hal_request_quit(); }
    if (wm_take_reboot()) {
        klog("kernel: reboot requested, restarting the kernel subsystems");
        gfx_clear(RGB(0, 0, 0));
        present_dirty_region();
        proc_init();
        cc_init();
        irq_init();
        mm_release_owner(0);
        wm_init();
        proc_boot_daemons();
        g_boot_start_ms = now_ms() + 400;
        g_boot_stage = 1;
    }
    if (wm_take_crash()) {
        kernel_panic("kernel panic triggered from the Start menu", 0x0001F4A0, proc_running_pid());
    }
    return hal_running();
}

/* --------------------------------------------------------------------------
 *  Blocking entry point (Win32 + headless)
 *
 *  Identical behaviour to the original loop: one frame, then a short host
 *  sleep to pace the machine.  Only the wasm shell takes the non-blocking
 *  route through aether_boot()/aether_frame().
 * ------------------------------------------------------------------------ */
#ifndef AETHER_WASM
int aether_main(int argc, char **argv) {
    int rc = aether_boot(argc, argv);
    if (rc != 0) return rc;
    while (aether_frame()) {
        /* Pace the frame: the boot and panic screens are cheaper than the full
           desktop, but a fixed 1 ms yield is what the original loop used for the
           steady state.  8 ms during boot keeps the animation smooth. */
        hal_sleep_ms(g_boot_stage == 1 ? 8 : 1);
    }
    hal_shutdown();
    return 0;
}
#endif

#ifdef AETHER_HEADLESS
int main(int argc, char **argv) {
    /* The headless chassis is driven by the AETHER_SCRIPT/AETHER_DUMP_AT
       variables and always exits through hal_request_quit(). */
    return aether_main(argc, argv);
}
#elif !defined(AETHER_WASM)
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;
    return aether_main(0, NULL);
}

/* --------------------------------------------------------------------------
 *  Real entry point
 *
 *  The executable is linked without a C runtime (no ucrt, no mingw crt0), so
 *  the loader's entry point is this function rather than mainCRTStartup.  It
 *  has to do only three things: ask Win32 for the instance handle, run the
 *  kernel, and hand the exit code back to the kernel.  Everything the kernel
 *  needs (string ops, formatting, heap, maths) is implemented in this file.
 * -------------------------------------------------------------------------- */
int WINAPI aether_entry(void) {
    int rc = aether_main(0, NULL);
    ExitProcess((UINT)rc);
    return rc;
}
#endif

/* --------------------------------------------------------------------------
 *  WebAssembly entry points
 *
 *  The Electron shell loads the module, calls aether_wasm_boot() once, then
 *  calls aether_wasm_frame() from requestAnimationFrame.  Neither function
 *  blocks, so the host's UI thread keeps painting and stays responsive.
 * ------------------------------------------------------------------------ */
#ifdef AETHER_WASM
__attribute__((export_name("aether_boot")))
int aether_wasm_boot(void) {
    return aether_boot(0, NULL);
}

__attribute__((export_name("aether_frame")))
int aether_wasm_frame(void) {
    return aether_frame();
}

__attribute__((export_name("aether_running")))
int aether_wasm_running(void) {
    return hal_running();
}

__attribute__((export_name("aether_shutdown")))
void aether_wasm_shutdown(void) {
    hal_shutdown();
}

/* The framebuffer lives inside the module's linear memory.  The shell reads
   these two so it never has to hard-code the geometry of the emulated VGA. */
__attribute__((export_name("aether_vram")))
u32 *aether_wasm_vram(void) { return hal_vram(); }

__attribute__((export_name("aether_screen_w")))
int aether_wasm_w(void) { return SCREEN_W; }

__attribute__((export_name("aether_screen_h")))
int aether_wasm_h(void) { return SCREEN_H; }

/* Exit code of the last failed hal_init(), so a shell can say *why* the machine
   refused to boot instead of failing silently. */
__attribute__((export_name("aether_last_error")))
int aether_wasm_error(void) { return g_aether_boot_error; }

#endif
