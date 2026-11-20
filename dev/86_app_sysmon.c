/* ==========================================================================
 *  APP: sysmon -- task & kernel monitor
 *
 *  Rolling 60-second CPU-load and RAM-usage graphs, a live interactive
 *  process table with a real "Kill Process" action (pid 1 is protected by
 *  the kernel), and heap / inode / block usage gauges.
 * ========================================================================== */

#define SYS_HIST 60
#define SYS_ROWS_VISIBLE 9

/* Client-relative geometry shared by paint and mouse hit-testing. */
#define SYS_TABS_Y      8
#define SYS_TABS_H      24
#define SYS_CHART_Y     42
#define SYS_CHART_H     96
#define SYS_TABLE_Y     142
#define SYS_ROW_H       20
#define SYS_KILL_W      54

typedef struct {
    u8  cpu[SYS_HIST];
    u8  ram[SYS_HIST];
    int head;
    int count;
    u32 sample_ms;
    int row_hover;
    int row_selected;
    int kill_flash_ms;
    int kill_target;
    char kill_msg[96];
    int kill_msg_kind;
    int show_graph_tab;         /* 0 cpu+ram, 1 heap chain, 2 vfs         */
    int scroll;
    u32 last_sample_ms;
    u32 total_samples;
    int heap_scroll;
    int auto_refresh;
} sysmon_t;

static void sysmon_sample(sysmon_t *sm) {
    u32 cpu = proc_cpu_total_percent();
    u32 ram = (u32)((mm_used() * 100ull) / (mm_total() ? mm_total() : 1));
    sm->cpu[sm->head] = (u8)k_clamp((int)cpu, 0, 100);
    sm->ram[sm->head] = (u8)k_clamp((int)ram, 0, 100);
    sm->head = (sm->head + 1) % SYS_HIST;
    if (sm->count < SYS_HIST) sm->count++;
    sm->total_samples++;
}

static void *sysmon_create(const char *arg) {
    sysmon_t *sm = (sysmon_t *)mm_kmalloc(sizeof(sysmon_t));
    int i;
    (void)arg;
    if (!sm) return NULL;
    kmemset(sm, 0, sizeof(*sm));
    for (i = 0; i < SYS_HIST; i++) {
        sm->cpu[i] = (u8)(20 + (krandom() % 40));
        sm->ram[i] = (u8)(10 + (krandom() % 25));
    }
    sm->count = SYS_HIST;
    sm->head = 0;
    sm->auto_refresh = 1;
    return sm;
}

static void sysmon_destroy(void *state) { if (state) mm_kfree(state); }

/* ---- drawing helpers ---------------------------------------------------- */

static void sysmon_graph(const char *label, u8 *data, int head, int count,
                         int x, int y, int w, int h, u32 line_col, int active_rows) {
    theme_t *t = theme_get();
    int i;
    gfx_round_rect(x, y, w, h, 6, RGBA(8, 12, 18, 220));
    gfx_round_rect_border(x, y, w, h, 6, RGBA(70, 90, 120, 180), 1);
    gfx_text(x + 8, y + 4, label, t->text_mute, 0);
    /* grid */
    for (i = 1; i < 4; i++) {
        int gy = y + 18 + (h - 26) * i / 4;
        for (int gx = x + 8; gx < x + w - 8; gx += 6)
            gfx_pixel(gx, gy, RGBA(120, 150, 190, 40));
    }
    /* line graph with fill */
    {
        int plot_x = x + 8, plot_y = y + 18, plot_w = w - 16, plot_h = h - 26;
        int prev_x = plot_x, prev_y = plot_y + plot_h;
        int n = k_min(active_rows, SYS_HIST);
        for (i = 0; i < n; i++) {
            int idx = (head - n + i + SYS_HIST * 2) % SYS_HIST;
            int vx = plot_x + (n > 1 ? i * plot_w / (n - 1) : 0);
            int vy = plot_y + plot_h - (data[idx] * plot_h) / 100;
            gfx_line(prev_x, prev_y, vx, vy, line_col);
            {
                int yy;
                for (yy = vy; yy < plot_y + plot_h; yy++)
                    gfx_pixel(vx, yy, RGBA(RED(line_col), GREEN(line_col), BLUE(line_col), 28));
            }
            prev_x = vx;
            prev_y = vy;
        }
        if (n > 0) {
            int idx = (head - 1 + SYS_HIST) % SYS_HIST;
            char buf[32];
            ksnprintf(buf, sizeof(buf), "%u%%", (unsigned)data[idx]);
            gfx_text_right(x + w - 8, y + 4, buf, line_col, 0);
        }
    }
}

static void sysmon_gauge(const char *label, u32 used, u32 total, int x, int y, int w,
                         u32 col) {
    theme_t *t = theme_get();
    char buf[64];
    int frac = total ? (int)((u64)used * 100 / total) : 0;
    gfx_text(x, y, label, t->text_dim, 0);
    ksnprintf(buf, sizeof(buf), "%u / %u  (%d%%)", (unsigned)used, (unsigned)total, frac);
    gfx_text_right(x + w, y, buf, t->text_mute, 0);
    gfx_round_rect(x, y + 14, w, 10, 4, RGBA(0, 0, 0, 140));
    gfx_round_rect_grad(x, y + 14, k_max(3, w * frac / 100), 10, 4, col, RGBA(RED(col) / 2, GREEN(col) / 2, BLUE(col) / 2, 255));
}

/* ---- process table ------------------------------------------------------ */

typedef struct { int pid; int state; } pmap_t;

static int sysmon_build_pmap(pmap_t *map, int maxrows) {
    int n = 0;
    u32 i;
    for (i = 1; i < MAX_PROCS && n < maxrows; i++) {
        pcb_t *p = proc_pcb(i);
        if (!p) continue;
        map[n].pid = (int)p->pid;
        map[n].state = (int)p->state;
        n++;
    }
    return n;
}

static void sysmon_paint(void *state, window_t *w, wm_rect_t c) {
    sysmon_t *sm = (sysmon_t *)state;
    theme_t *t = theme_get();
    int x = c.x + 10, y = c.y + 8;
    int colw = c.w - 20;
    pmap_t map[MAX_PROCS];
    int nprocs = sysmon_build_pmap(map, MAX_PROCS);
    int i;

    gfx_fill_rect(c.x, c.y, c.w, c.h, t->win_bg);

    /* toolbar */
    {
        const char *tabs[3] = { "Live", "Heap", "VFS" };
        int tx = x;
        for (i = 0; i < 3; i++) {
            int tw = gfx_text_w(tabs[i], 1) + 22;
            int active = sm->show_graph_tab == i;
            int hover = (g_wm.mouse_px >= tx && g_wm.mouse_px < tx + tw &&
                         g_wm.mouse_py >= y && g_wm.mouse_py < y + 24);
            gfx_round_rect(tx, y, tw, 24, 6,
                           active ? t->accent : (hover ? RGBA(80, 100, 140, 120) : RGBA(40, 50, 70, 140)));
            gfx_text_center(tx + tw / 2, y + 5, tabs[i], active ? RGB(10, 16, 24) : t->text, 1);
            tx += tw + 6;
        }
        {
            char buf[64];
            u32 up = irq_ticks() / (irq_hz() ? irq_hz() : 1000);
            ksnprintf(buf, sizeof(buf), "uptime %02u:%02u:%02u   frames %d   %d.%02d fps",
                      up / 3600, (up / 60) % 60, up % 60, wm_total_frames(),
                      wm_fps_x100() / 100, wm_fps_x100() % 100);
            gfx_text_right(x + colw, y + 6, buf, t->text_mute, 0);
        }
    }
    y += 34;

    if (sm->show_graph_tab == 0) {
        int half = (colw - 12) / 2;
        sysmon_graph("CPU load % (60 s)", sm->cpu, sm->head, sm->count, x, y, half, 92,
                     t->ok, k_min(sm->count, 60));
        sysmon_graph("Physical RAM used %", sm->ram, sm->head, sm->count, x + half + 12, y, half, 92,
                     t->accent, k_min(sm->count, 60));
        y += 100;
    } else if (sm->show_graph_tab == 1) {
        /* heap block chain visualisation */
        gfx_round_rect(x, y, colw, 96, 6, RGBA(8, 12, 18, 220));
        gfx_round_rect_border(x, y, colw, 96, 6, RGBA(70, 90, 120, 180), 1);
        gfx_text(x + 8, y + 4, "allocator block chain (green = used, grey = free)", t->text_mute, 0);
        {
            /* The bar spans the whole heap, so each block's width has to be
               proportional to its size: dividing by a fixed constant made the
               large free block wider than the panel and it was skipped, leaving
               the picture showing only the first block. */
            u32 idx = 0;
            int bx = x + 8;
            int span = colw - 16;
            u32 heap = (u32)HEAP_SIZE;
            mm_block_t *b;
            int cy = y + 22;
            int drawn = 0;
            while (mm_block_at(idx, &b)) {
                int bw;
                if (bx >= x + colw - 8) break;
                bw = (int)((u64)b->size * (u32)span / heap);
                if (bw < 1) bw = 1;               /* keep tiny blocks visible */
                if (bx + bw > x + colw - 8) bw = x + colw - 8 - bx;
                if (bw <= 0) break;
                gfx_fill_rect(bx, cy, bw, 40,
                              (b->flags & MM_FLAG_FREE) ? RGBA(70, 80, 100, 220)
                                                       : RGBA(RED(t->ok), GREEN(t->ok), BLUE(t->ok), 200));
                bx += bw;
                idx++;
                drawn++;
            }
            (void)drawn;
            ksnprintf(sm->kill_msg, sizeof(sm->kill_msg),
                      "%u blocks, %u used, %u free, largest free %u B",
                      (unsigned)mm_state()->blocks_alloc + (unsigned)mm_state()->blocks_free,
                      (unsigned)mm_state()->blocks_alloc, (unsigned)mm_state()->blocks_free,
                      (unsigned)mm_state()->total - (unsigned)mm_state()->used);
            gfx_text(x + 8, y + 70, sm->kill_msg, t->text_dim, 0);
        }
        sysmon_gauge("kernel heap in use", (u32)mm_used(), (u32)mm_total(),
                     x, y + 104, colw / 2 - 8, t->ok);
        sysmon_gauge("fragmentation", (u32)mm_state()->free_fragmentation, 100u,
                     x + colw / 2 + 8, y + 104, colw / 2 - 8, t->warn);
        y += 140;
    } else {
        u32 iu = 0, bu = 0;
        vfs_usage(&iu, &bu, NULL);
        gfx_round_rect(x, y, colw, 96, 6, RGBA(8, 12, 18, 220));
        gfx_round_rect_border(x, y, colw, 96, 6, RGBA(70, 90, 120, 180), 1);
        gfx_text(x + 8, y + 6, "VFS inode table", t->text_mute, 0);
        {
            int k;
            for (k = 0; k < 64; k++) {
                int gx = x + 8 + (k % 32) * ((colw - 16) / 32);
                int gy = y + 24 + (k / 32) * 16;
                int used = (int)((u64)iu * 64 / VFS_INODES) > k;
                gfx_fill_rect(gx, gy, ((colw - 16) / 32) - 2, 12,
                              used ? RGBA(RED(t->accent), GREEN(t->accent), BLUE(t->accent), 200)
                                   : RGBA(50, 60, 80, 200));
            }
        }
        {
            char buf[160];
            ksnprintf(buf, sizeof(buf), "inodes %u/%u   blocks %u/%u   fd open %d",
                      (unsigned)iu, (unsigned)VFS_INODES, (unsigned)bu,
                      (unsigned)VFS_BLOCKS, vfs_fd_count());
            gfx_text(x + 8, y + 62, buf, t->text_dim, 0);
        }
        sysmon_gauge("inodes allocated", iu, (u32)VFS_INODES,
                     x, y + 104, colw / 2 - 8, t->accent);
        sysmon_gauge("disk blocks used", bu, (u32)VFS_BLOCKS,
                     x + colw / 2 + 8, y + 104, colw / 2 - 8, t->info);
        y += 140;
    }

    /* process table */
    {
        int th = c.y + c.h - y - 52;
        int rows = k_max(3, th / 20);
        int header_y = c.y + SYS_TABLE_Y;
        int r;
        gfx_round_rect(x, header_y, colw, c.y + c.h - header_y - 8, 6, RGBA(10, 14, 22, 235));
        gfx_text(x + 8, header_y + 4, "PID", t->text_mute, 0);
        gfx_text(x + 58, header_y + 4, "NAME", t->text_mute, 0);
        gfx_text(x + 190, header_y + 4, "STATE", t->text_mute, 0);
        gfx_text(x + 268, header_y + 4, "CPU%", t->text_mute, 0);
        gfx_text(x + 318, header_y + 4, "CPU", t->text_mute, 0);
        gfx_text(x + 380, header_y + 4, "MEM", t->text_mute, 0);
        gfx_text(x + 450, header_y + 4, "PRI", t->text_mute, 0);
        gfx_text(x + colw - 68, header_y + 4, "ACTION", t->text_mute, 0);
        gfx_hline(x + 6, header_y + 18, colw - 12, RGBA(255, 255, 255, 18));

        for (r = 0; r < rows && r + sm->scroll < nprocs; r++) {
            int pid = map[r + sm->scroll].pid;
            pcb_t *p = proc_pcb((u32)pid);
            int ry = header_y + 22 + r * 20;
            int hover = (g_wm.mouse_px >= x + 4 && g_wm.mouse_px < x + colw - 4 &&
                         g_wm.mouse_py >= ry - 2 && g_wm.mouse_py < ry + 17);
            char buf[48];
            u32 stcol = t->text;
            sm->row_hover = hover ? pid : sm->row_hover;
            if (!p) continue;
            if (hover) gfx_round_rect(x + 4, ry - 2, colw - 8, 19, 4, t->sel_dim);
            if (sm->row_selected == pid) gfx_round_rect(x + 4, ry - 2, colw - 8, 19, 4, t->sel);
            switch (p->state) {
            case PROC_RUNNING: stcol = t->ok; break;
            case PROC_SLEEPING: stcol = t->info; break;
            case PROC_ZOMBIE: stcol = t->err; break;
            case PROC_BLOCKED: stcol = t->warn; break;
            default: stcol = t->text_dim; break;
            }
            ksnprintf(buf, sizeof(buf), "%u", (unsigned)pid);
            gfx_text(x + 8, ry, buf, t->text, 0);
            gfx_text_clip(x + 58, ry, 126, p->name, t->text, 0);
            gfx_text(x + 190, ry, proc_state_name(p->state), stcol, 0);
            ksnprintf(buf, sizeof(buf), "%u%%", (unsigned)p->cpu_percent);
            gfx_text(x + 268, ry, buf, t->text_dim, 0);
            /* tiny cpu bar */
            gfx_round_rect(x + 318, ry + 3, 50, 6, 2, RGBA(0, 0, 0, 150));
            gfx_round_rect(x + 318, ry + 3, k_max(1, (int)(p->cpu_percent / 2)), 6, 2, t->ok);
            ksnprintf(buf, sizeof(buf), "%u B", (unsigned)p->mem_used);
            gfx_text(x + 380, ry, buf, t->text_dim, 0);
            ksnprintf(buf, sizeof(buf), "%u", (unsigned)p->priority);
            gfx_text(x + 450, ry, buf, t->text_dim, 0);
            /* kill button */
            {
                int bx = x + colw - 62, by = ry - 2;
                int protected = (pid == 1);
                int bhov = (g_wm.mouse_px >= bx && g_wm.mouse_px < bx + 54 &&
                            g_wm.mouse_py >= by && g_wm.mouse_py < by + 18);
                gfx_round_rect(bx, by, 54, 17, 4,
                               protected ? RGBA(90, 90, 90, 160)
                                         : (bhov ? RGBA(232, 80, 90, 230) : RGBA(120, 40, 48, 200)));
                gfx_text_center(bx + 27, by + 2, protected ? "init" : "kill",
                                RGB(240, 240, 250), 0);
            }
        }
        if (nprocs > rows + sm->scroll)
            gfx_text(x + 8, header_y + 22 + rows * SYS_ROW_H + 2,
                     "scroll for more processes", t->text_mute, 0);
    }

    /* status line */
    if (sm->kill_msg[0]) {
        u32 col = sm->kill_msg_kind == 1 ? t->ok : (sm->kill_msg_kind == 2 ? t->err : t->info);
        gfx_text(x + 8, c.y + c.h - 34, sm->kill_msg, col, 0);
    }
    {
        char buf[200];
        u32 hz = irq_hz() ? irq_hz() : 1000;
        ksnprintf(buf, sizeof(buf),
                  "sched: %u switches  syscalls: %u  irq0=%u  heap %u B  frag %u%%",
                  (unsigned)proc_sched()->switch_count, (unsigned)proc_sched()->syscall_total,
                  (unsigned)irq_ticks(), (unsigned)mm_used(), (unsigned)mm_state()->free_fragmentation);
        gfx_text(x + 8, c.y + c.h - 18, buf, t->text_mute, 0);
        (void)hz;
    }
}

static void sysmon_tick(void *state, u32 dt) {
    sysmon_t *sm = (sysmon_t *)state;
    sm->sample_ms += dt;
    if (!sm->auto_refresh) return;
    if (sm->sample_ms >= 250) {
        sm->sample_ms = 0;
        sysmon_sample(sm);
    }
}

static void sysmon_key(void *state, int key, int ch, int mods) {
    sysmon_t *sm = (sysmon_t *)state;
    (void)mods;
    if (key == 0x09 || ch == '\t') sm->show_graph_tab = (sm->show_graph_tab + 1) % 3;
    if (key == 0x26) sm->scroll = k_max(0, sm->scroll - 1);
    if (key == 0x28) sm->scroll++;
    if (ch == 'k' && sm->row_selected) {
        int rc = proc_kill((u32)sm->row_selected, 9);
        if (rc == 0) {
            ksnprintf(sm->kill_msg, sizeof(sm->kill_msg), "SIGKILL delivered to pid %d", sm->row_selected);
            sm->kill_msg_kind = 1;
            wm_close_by_pid((u32)sm->row_selected);
        } else if (rc == -2) {
            ksnprintf(sm->kill_msg, sizeof(sm->kill_msg),
                      "pid 1 (init) is protected by the kernel -- kill denied");
            sm->kill_msg_kind = 2;
        } else {
            ksnprintf(sm->kill_msg, sizeof(sm->kill_msg), "kill: no such process");
            sm->kill_msg_kind = 2;
        }
    }
}

static void sysmon_mouse(void *state, int event, int x, int y, int button, int wheel) {
    sysmon_t *sm = (sysmon_t *)state;
    wm_rect_t c = wm_input_client_rect();
    int rx = x - c.x, ry = y - c.y;
    int colw = c.w - 20;
    int rows, r, nprocs;
    pmap_t map[MAX_PROCS];
    (void)button;

    if (event == APP_EV_WHEEL) {
        nprocs = sysmon_build_pmap(map, MAX_PROCS);
        rows = k_max(1, (c.h - SYS_TABLE_Y - 46) / SYS_ROW_H);
        sm->scroll = k_clamp(sm->scroll - wheel, 0, k_max(0, nprocs - rows));
        return;
    }
    if (event != APP_EV_MOUSE_DOWN && event != APP_EV_MOUSE_DBLCLICK) return;

    /* tab strip */
    if (ry >= SYS_TABS_Y && ry < SYS_TABS_Y + SYS_TABS_H) {
        int tx = 10, i;
        for (i = 0; i < 3; i++) {
            const char *tabs[3] = { "Live", "Heap", "VFS" };
            int tw = gfx_text_w(tabs[i], 1) + 22;
            if (rx >= tx && rx < tx + tw) { sm->show_graph_tab = i; return; }
            tx += tw + 6;
        }
        return;
    }
    if (ry < SYS_TABLE_Y) return;

    nprocs = sysmon_build_pmap(map, MAX_PROCS);
    rows = k_max(1, (c.h - SYS_TABLE_Y - 46) / SYS_ROW_H);
    r = (ry - SYS_TABLE_Y - 22) / SYS_ROW_H;
    if (r < 0 || r >= rows) return;
    r += sm->scroll;
    if (r >= nprocs) return;
    sm->row_selected = map[r].pid;

    /* kill column, right aligned inside the client rect */
    if (rx >= colw - SYS_KILL_W + 4) {
        int pid = map[r].pid;
        int rc = proc_kill((u32)pid, 9);
        if (rc == 0) {
            ksnprintf(sm->kill_msg, sizeof(sm->kill_msg),
                      "SIGKILL delivered to pid %d (%s)", pid,
                      proc_pcb((u32)pid) ? proc_pcb((u32)pid)->name : "?");
            sm->kill_msg_kind = 1;
            wm_close_by_pid((u32)pid);
        } else if (rc == -2) {
            ksnprintf(sm->kill_msg, sizeof(sm->kill_msg),
                      "pid 1 (init) is protected by the kernel -- kill denied");
            sm->kill_msg_kind = 2;
        } else if (rc == -3) {
            ksnprintf(sm->kill_msg, sizeof(sm->kill_msg),
                      "pid %d is a kernel thread -- kill denied", pid);
            sm->kill_msg_kind = 2;
        } else {
            ksnprintf(sm->kill_msg, sizeof(sm->kill_msg), "kill: no such process");
            sm->kill_msg_kind = 2;
        }
    }
}

const app_desc_t app_sysmon = {
    "sysmon", "System Monitor", "tasks + memory", ICON_SYSMON,
    820, 560, 560, 380, APP_CAT_SYSTEM, 1, 1,
    sysmon_create, sysmon_destroy, sysmon_paint, sysmon_key, sysmon_mouse, sysmon_tick, NULL
};
