/* ==========================================================================
 *  APP: console -- VM program output console
 *
 *  Watches the bytecode VM processes, shows what each one printed, renders
 *  the draw_rect() commands a program issued onto a little framebuffer, and
 *  lets the user compile+spawn programs from the console prompt.
 * ========================================================================== */

typedef struct {
    int  scroll;
    int  follow;
    int  select;
    char last_cmd[128];
    char log[4096];
    u32  log_len;
    u32  fb[240 * 140];
    int  fb_ready;
    u32  out_seen[VM_MAX_PROGS];
} console_t;

static void console_render_vm_fb(console_t *ct) {
    vm_draw_t *list = NULL;
    int count = 0;
    u32 clear_color = 0;
    int clear_pending = 0;
    int i;
    cc_vm_take_draws(&list, &count, &clear_color, &clear_pending);
    if (clear_pending) {
        for (i = 0; i < 240 * 140; i++) ct->fb[i] = clear_color;
        ct->fb_ready = 1;
    }
    for (i = 0; i < count && i < 64; i++) {
        int x, y;
        int y0 = k_clamp(list[i].y, 0, 140), y1 = k_clamp(list[i].y + list[i].h, 0, 140);
        int x0 = k_clamp(list[i].x, 0, 240), x1 = k_clamp(list[i].x + list[i].w, 0, 240);
        for (y = y0; y < y1; y++)
            for (x = x0; x < x1; x++)
                ct->fb[y * 240 + x] = list[i].color;
        if (list[i].color) ct->fb_ready = 1;
        {
            char buf[96];
            ksnprintf(buf, sizeof(buf), "program: draw_rect %d,%d %dx%d #%06X\n",
                      (int)list[i].x, (int)list[i].y,
                      (int)list[i].w, (int)list[i].h, (unsigned)list[i].color & 0xFFFFFF);
            if (ct->log_len + kstrlen(buf) < sizeof(ct->log) - 2) {
                kstrcpy(ct->log + ct->log_len, buf);
                ct->log_len += (u32)kstrlen(buf);
            }
        }
    }
}


static void *console_create(const char *arg) {
    console_t *ct = (console_t *)mm_kmalloc(sizeof(console_t));
    int i;
    (void)arg;
    if (!ct) return NULL;
    kmemset(ct, 0, sizeof(*ct));
    for (i = 0; i < 240 * 140; i++) ct->fb[i] = 0xFF10141C;
    kstrcpy(ct->log, "AE program console\n"
                     "Compile a program with 'cc file.c -o /bin/out' in the shell,\n"
                     "then 'run out' -- its output and drawings appear here.\n\n");
    ct->log_len = (u32)kstrlen(ct->log);
    ct->fb_ready = 1;
    return ct;
}

static void console_destroy(void *state) { if (state) mm_kfree(state); }

static void console_paint(void *state, window_t *w, wm_rect_t c) {
    console_t *ct = (console_t *)state;
    theme_t *t = theme_get();
    int i;

    gfx_fill_rect(c.x, c.y, c.w, c.h, RGB(8, 10, 16));

    /* vm process list */
    {
        int py = c.y + 8;
        int px = c.x + 8;
        gfx_text(px, py, "bytecode VM processes", t->text_mute, 0);
        py += 18;
        for (i = 0; i < VM_MAX_PROGS; i++) {
            vm_process_t *vp = cc_vm_process(i);
            char buf[160];
            if (!vp) continue;
            ksnprintf(buf, sizeof(buf), "pid %-4u %-18s %5d insns %s",
                      (unsigned)vp->pid, vp->name, vp->vm->steps,
                      vp->vm->halted ? "[halted]" : "[running]");
            gfx_text(px, py, buf, vp->vm->halted ? t->text_mute : t->ok, 0);
            py += 16;
        }
        if (py == c.y + 26) gfx_text(px, py, "no VM processes yet", t->text_mute, 0);
    }

    /* framebuffer from draw_rect calls */
    {
        int fx = c.x + c.w - 248, fy = c.y + 26;
        gfx_text(fx, c.y + 8, "program framebuffer", t->text_mute, 0);
        gfx_round_rect_border(fx - 2, fy - 2, 244, 144, 4, t->border, 1);
        {
            int x, y;
            for (y = 0; y < 140; y++)
                for (x = 0; x < 240; x++)
                    gfx_pixel(fx + x, fy + y, ct->fb[y * 240 + x]);
        }
    }

    /* console log */
    {
        int ly = c.y + 176;
        int vis = (c.y + c.h - ly - 30) / 16;
        int lines = 0;
        const char *p = ct->log;
        u32 starts[256];
        int li;
        if (vis < 1) vis = 1;
        starts[0] = 0;
        while (*p && lines < 255) {
            if (*p == '\n') starts[++lines] = (u32)(p - ct->log) + 1;
            p++;
        }
        {
            int first = k_max(0, lines - vis + 1 + ct->scroll);
            for (li = 0; li < vis && first + li <= lines; li++) {
                u32 st = starts[first + li];
                char line[220];
                int n = 0;
                u32 k = st;
                while (k < ct->log_len && ct->log[k] != '\n' && n < 210) line[n++] = ct->log[k++];
                line[n] = 0;
                gfx_text_term(c.x + 8, ly + li * 16, line, t->text, 0, 0);
            }
        }
    }
    {
        int sy = c.y + c.h - 22;
        gfx_fill_rect(c.x, sy, c.w, 22, RGBA(10, 14, 22, 240));
        gfx_text(c.x + 8, sy + 4, "scheduler drives the VM; programs run as real PIDs", t->text_mute, 0);
    }
}

static void console_tick(void *state, u32 dt) {
    console_t *ct = (console_t *)state;
    (void)dt;
    console_render_vm_fb(ct);
    /* fold VM output into the log */
    {
        int i;
        for (i = 0; i < VM_MAX_PROGS; i++) {
            vm_process_t *vp = cc_vm_process(i);
            u32 len = 0;
            const char *out;

            if (!vp) continue;
            out = cc_vm_output(i, &len);
            if (!out || !len || len <= ct->out_seen[i]) continue;
            if (len > 6000) len = 6000;
            if (ct->out_seen[i] > len) ct->out_seen[i] = 0;
            if (ct->log_len + 1 < sizeof(ct->log)) {
                u32 copy = len - ct->out_seen[i];
                const char *srcp = out + ct->out_seen[i];
                if (copy > sizeof(ct->log) - ct->log_len - 1) copy = sizeof(ct->log) - ct->log_len - 1;
                ct->log_len = (u32)kstrlen(ct->log);
                ct->out_seen[i] = len;
                if (copy > sizeof(ct->log) - ct->log_len - 1) copy = sizeof(ct->log) - ct->log_len - 1;
                kmemcpy(ct->log + ct->log_len, srcp, copy);
                ct->log_len += copy;
                ct->log[ct->log_len] = 0;
            }
        }
    }
}

static void console_key(void *state, int key, int ch, int mods) {
    console_t *ct = (console_t *)state;
    (void)ch; (void)mods;
    if (key == 0x26) ct->scroll = k_max(0, ct->scroll - 1);
    if (key == 0x28) ct->scroll++;
}

static void console_mouse(void *state, int event, int x, int y, int button, int wheel) {
    console_t *ct = (console_t *)state;
    (void)x; (void)y; (void)button;
    if (event == APP_EV_WHEEL) ct->scroll = k_max(0, ct->scroll - wheel);
}

const app_desc_t app_console = {
    "console", "Program Console", "VM output", ICON_TERMINAL,
    700, 480, 420, 300, APP_CAT_DEV, 0, 1,
    console_create, console_destroy, console_paint, console_key, console_mouse,
    console_tick, NULL
};
