/* ==========================================================================
 *  APP: AetherShell -- the terminal
 *
 *  Scrollback ring buffer, command history (Up/Down), blinking cursor,
 *  colour-coded output and the full command set (file ops, process/memory
 *  ops, compiler driver, system control).
 * ========================================================================== */

#define TERM_COLS       150
#define TERM_ROWS       400
#define TERM_LINE_LEN   200
#define TERM_CMD_LEN    256
#define TERM_HIST       48

enum { TC_DEFAULT = 0, TC_GREEN, TC_CYAN, TC_YELLOW, TC_RED, TC_MAGENTA,
       TC_BLUE, TC_GREY, TC_WHITE, TC_BOLD, TC_COUNT };

typedef struct {
    char text[TERM_LINE_LEN];
    u8   color;
    u8   is_prompt;
} term_line_t;

typedef struct {
    term_line_t lines[TERM_ROWS];
    int  nlines;
    int  scroll;                /* lines scrolled up from the bottom          */
    char input[TERM_CMD_LEN];
    int  input_len;
    int  cursor;
    char hist[TERM_HIST][TERM_CMD_LEN];
    int  nhist;
    int  hist_pos;
    char cwd[256];
    u32  blink_ms;
    int  blink_on;
    int  scrollbar_drag;
    u32  last_exec_ms;
    int  total_cmds;
    char last_status[96];
} shell_t;

/* -------------------------------------------------------------------------- */
/*  output buffer                                                            */
/* -------------------------------------------------------------------------- */

static void sh_emit(shell_t *sh, u8 color, const char *text) {
    /* split on \n, wrapping at TERM_COLS; honour a tiny ANSI-ish escape
       vocabulary: \x01..\x09 select colours */
    int i = 0;
    u8 cur = color;
    while (text[i]) {
        term_line_t *ln;
        int used;
        char ch = text[i];
        if ((u8)ch == 1 || (u8)ch == 2 || (u8)ch == 3 || (u8)ch == 4 ||
            (u8)ch == 5 || (u8)ch == 6 || (u8)ch == 7 || (u8)ch == 8) {
            cur = (u8)(ch);
            i++;
            continue;
        }
        if (ch == '\r') { i++; continue; }
        if (sh->nlines >= TERM_ROWS) {
            kmemmove(&sh->lines[0], &sh->lines[1], sizeof(term_line_t) * (TERM_ROWS - 1));
            sh->nlines = TERM_ROWS - 1;
        }
        ln = &sh->lines[sh->nlines];
        kmemset(ln, 0, sizeof(*ln));
        ln->color = cur;
        used = 0;
        while (text[i] && text[i] != '\n') {
            if (used >= TERM_COLS - 1) {
                /* wrap */
                ln->text[used] = 0;
                sh->nlines++;
                if (sh->nlines >= TERM_ROWS) {
                    kmemmove(&sh->lines[0], &sh->lines[1], sizeof(term_line_t) * (TERM_ROWS - 1));
                    sh->nlines = TERM_ROWS - 1;
                }
                ln = &sh->lines[sh->nlines];
                kmemset(ln, 0, sizeof(*ln));
                ln->color = cur;
                used = 0;
            }
            ln->text[used++] = text[i++];
        }
        ln->text[used] = 0;
        sh->nlines++;
        if (text[i] == '\n') i++;
    }
    sh->scroll = 0;
}

static void sh_printf(shell_t *sh, u8 color, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sh_emit(sh, color, buf);
}

static void sh_prompt(shell_t *sh) {
    char buf[400];
    const char *user = "user";
    ksnprintf(buf, sizeof(buf), "\x02%s@aether\x07:\x02%s\x07$ ", user, sh->cwd);
    sh_emit(sh, TC_DEFAULT, buf);
    sh->nlines ? (sh->lines[sh->nlines - 1].is_prompt = 1) : 0;
    if (sh->nlines) sh->lines[sh->nlines - 1].is_prompt = 1;
}

/* -------------------------------------------------------------------------- */
/*  argument parsing                                                         */
/* -------------------------------------------------------------------------- */

static int sh_split(char *line, char **argv, int maxargs) {
    int n = 0;
    char *p = line;
    while (*p && n < maxargs) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            argv[n++] = p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = 0;
        } else {
            argv[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = 0;
        }
    }
    return n;
}

static const char *sh_base(const char *path) {
    const char *s = kstrrchr(path, '/');
    return s ? s + 1 : path;
}

static void sh_absolute(shell_t *sh, const char *in, char *out, size_t n) {
    vfs_normalise(sh->cwd, in, out, n);
}

/* -------------------------------------------------------------------------- */
/*  command implementations                                                 */
/* -------------------------------------------------------------------------- */

static void cmd_help(shell_t *sh) {
    sh_printf(sh, TC_CYAN, "AetherShell " AETHER_VERSION_STR " -- command reference\n");
    sh_emit(sh, TC_GREY,
        "  FILE            ls [-l] [path]   cd <dir>   pwd   cat <file>   touch <file>\n"
        "                  mkdir <dir>      rm <file>  cp <a> <b>   mv <a> <b>\n"
        "                  write <file> <text>          hexdump <file>   stat <file>\n"
        "                  df   find <name>   tree\n"
        "  PROCESS/MEMORY  ps   top   kill <pid>   free   memmap   spawn <app>\n"
        "                  uptime   uname -a   dmesg   syscalls   irq\n"
        "  COMPILER        cc <src.c> [-o <out>]   run <prog>   disasm <src.c>\n"
        "  CUSTOMISE       theme <0-3>  wallpaper <0-5>  accent <rrggbb|off>\n"
        "                  font <0-2>   crt <0|1>   customise   resetprefs\n"
        "  WEB             browser [url]   (also: web <url>)\n"
        "  SYSTEM          clear   theme <0-3>   wallpaper <0-3>   reboot   panic\n"
        "                  help    apps   about   exit\n");
}

static void cmd_ls(shell_t *sh, int argc, char **argv) {
    const char *path = ".";
    int lng = 0, i, n, total = 0;
    char out[TERM_LINE_LEN * 2];
    if (argc > 1 && !kstrcmp(argv[1], "-l")) { lng = 1; if (argc > 2) path = argv[2]; }
    else if (argc > 1) path = argv[1];
    n = vfs_dir_count(path);
    if (n < 0) { sh_printf(sh, TC_RED, "ls: %s: no such directory\n", path); return; }
    if (lng) sh_printf(sh, TC_GREY, "total %d entries in %s\n", n - 2, path);
    out[0] = 0;
    for (i = 0; i < n; i++) {
        vfs_dirent_t d;
        if (vfs_dir_get(path, i, &d) != 0) break;
        if (!kstrcmp(d.name, ".") || !kstrcmp(d.name, "..")) continue;
        total++;
        if (lng) {
            vfs_inode_t st;
            char mode[16];
            char full[512];
            kmemset(&st, 0, sizeof(st));
            ksnprintf(full, sizeof(full), "%s/%s", kstrcmp(path, "/") ? path : "", d.name);
            vfs_stat_path(full, &st);
            vfs_mode_string(d.type, st.mode, mode);
            sh_printf(sh, d.type == FT_DIR ? TC_BLUE : (d.type == FT_PROC ? TC_MAGENTA : TC_DEFAULT),
                      "%s %6u  %-22s %s\n", mode, (unsigned)st.size, d.name,
                      d.type == FT_DIR ? "<dir>" : (d.type == FT_PROC ? "<proc>" : ""));
        } else {
            int l = (int)kstrlen(out);
            if (l + (int)kstrlen(d.name) + 2 >= TERM_COLS - 2) {
                sh_emit(sh, TC_DEFAULT, out);
                out[0] = 0;
            }
            kstrcat(out, d.name);
            if (d.type == FT_DIR) kstrcat(out, "/");
            kstrcat(out, "   ");
        }
    }
    if (!lng && out[0]) sh_emit(sh, TC_DEFAULT, out);
    if (!total) sh_emit(sh, TC_GREY, "(empty)\n");
}

static void cmd_cat(shell_t *sh, int argc, char **argv) {
    int i;
    if (argc < 2) { sh_emit(sh, TC_RED, "usage: cat <file> [file...]\n"); return; }
    for (i = 1; i < argc; i++) {
        u32 len = 0;
        const char *err = NULL;
        char *p = vfs_read_alloc(argv[i], &len, &err);
        if (!p) { sh_printf(sh, TC_RED, "cat: %s: %s\n", argv[i], err ? err : "error"); continue; }
        {
            /* print with line numbers off; keep raw */
            sh_emit(sh, TC_DEFAULT, p);
            if (len && p[len - 1] != '\n') sh_emit(sh, TC_DEFAULT, "\n");
        }
        mm_kfree(p);
    }
}

static void cmd_hexdump(shell_t *sh, int argc, char **argv) {
    u32 len = 0, off;
    const char *err = NULL;
    char *p;
    unsigned char *b;
    if (argc < 2) { sh_emit(sh, TC_RED, "usage: hexdump <file>\n"); return; }
    p = vfs_read_alloc(argv[1], &len, &err);
    if (!p) { sh_printf(sh, TC_RED, "hexdump: %s\n", err ? err : "error"); return; }
    b = (unsigned char *)p;
    for (off = 0; off < len; off += 16) {
        char hex[64], asc[24];
        u32 i;
        hex[0] = 0;
        asc[0] = 0;
        for (i = 0; i < 16; i++) {
            char tmp[8];
            if (off + i < len) ksnprintf(tmp, sizeof(tmp), "%02X ", b[off + i]);
            else ksnprintf(tmp, sizeof(tmp), "   ");
            kstrcat(hex, tmp);
            if (off + i < len) {
                unsigned char c = b[off + i];
                char a[2] = { (char)((c >= 32 && c < 127) ? c : '.'), 0 };
                kstrcat(asc, a);
            }
        }
        sh_printf(sh, TC_GREY, "%08X  ", (unsigned)off);
        sh_printf(sh, TC_CYAN, "%s", hex);
        sh_printf(sh, TC_GREEN, " |%s|\n", asc);
    }
    sh_printf(sh, TC_GREY, "%u bytes\n", (unsigned)len);
    mm_kfree(p);
}

static void cmd_stat(shell_t *sh, int argc, char **argv) {
    vfs_inode_t st;
    char mode[16];
    if (argc < 2) { sh_emit(sh, TC_RED, "usage: stat <file>\n"); return; }
    if (vfs_stat_path(argv[1], &st) != 0) {
        sh_printf(sh, TC_RED, "stat: %s: no such file\n", argv[1]);
        return;
    }
    vfs_mode_string(st.type, st.mode, mode);
    sh_printf(sh, TC_CYAN, "  File: %s\n", argv[1]);
    sh_printf(sh, TC_DEFAULT, "  Size: %-10u  Blocks: %u  IO Block: %u\n",
              (unsigned)st.size, (unsigned)((st.size + 4095) / 4096), (unsigned)VFS_BLOCK_SIZE);
    sh_printf(sh, TC_DEFAULT, "  Mode: %s  Type: %s  Links: %u\n",
              mode, vfs_type_name(st.type), (unsigned)st.link_count);
    sh_printf(sh, TC_DEFAULT, "  Inode: %-9u  Uid: %u  Gid: %u\n",
              (unsigned)st.ino, (unsigned)st.uid, (unsigned)st.gid);
    {
        u32 hz = irq_hz() ? irq_hz() : 1000;
        u32 up = irq_ticks() / hz;
        u32 secs = st.mtime / hz;         /* inode times are IRQ0 ticks */
        u32 age = (st.mtime && irq_ticks() >= st.mtime) ? (irq_ticks() - st.mtime) / hz : up;
        u32 t = (secs <= up) ? (up - secs) : 0;   /* seconds since boot */
        sh_printf(sh, TC_DEFAULT, "  Access: %us ago  Modify: %us ago  (clock %02u:%02u:%02u, up %us)\n",
                  (unsigned)t, (unsigned)age,
                  (t / 3600) % 24, (t / 60) % 60, t % 60, (unsigned)up);
    }
    sh_printf(sh, TC_GREY, "  Direct: %u %u %u %u ...  Indirect: %u\n",
              (unsigned)st.direct[0], (unsigned)st.direct[1],
              (unsigned)st.direct[2], (unsigned)st.direct[3], (unsigned)st.indirect);
}

static void cmd_df(shell_t *sh, int argc, char **argv) {
    u32 iu = 0, bu = 0, tot = 0;
    (void)argc; (void)argv;
    vfs_usage(&iu, &bu, &tot);
    sh_emit(sh, TC_GREY, "Filesystem      Inodes   IUsed   IFree  Blocks   BUsed   BFree   Mounted on\n");
    sh_printf(sh, TC_CYAN, "nvram0          %6u  %6u  %6u  %6u  %6u  %6u   /\n",
              (unsigned)VFS_INODES, (unsigned)iu, (unsigned)(VFS_INODES - iu),
              (unsigned)VFS_BLOCKS, (unsigned)bu, (unsigned)(VFS_BLOCKS - bu));
    sh_printf(sh, TC_DEFAULT, "ramdisk size    %u KiB total, %u KiB used, %u KiB free\n",
              (unsigned)(tot / 1024), (unsigned)(bu * 4), (unsigned)((VFS_BLOCKS - bu) * 4));
}

static void cmd_ps(shell_t *sh) {
    u32 i;
    sh_emit(sh, TC_GREY, "  PID  PPID STAT     CPU%    MEM      TICKS  NAME\n");
    for (i = 1; i < MAX_PROCS; i++) {
        pcb_t *p = proc_pcb(i);
        if (!p) continue;
        sh_printf(sh, p->state == PROC_ZOMBIE ? TC_RED :
                      (p->state == PROC_RUNNING ? TC_GREEN : TC_DEFAULT),
                  "%5u %5u %-8s %4u %7u %10u  %s\n",
                  (unsigned)p->pid, (unsigned)p->ppid, proc_state_name(p->state),
                  (unsigned)p->cpu_percent, (unsigned)p->mem_used,
                  (unsigned)p->cpu_ticks, p->name);
    }
    sh_printf(sh, TC_GREY, "%u processes, %u running, %u sleeping\n",
              (unsigned)proc_alive(), (unsigned)proc_count_state(PROC_RUNNING),
              (unsigned)proc_count_state(PROC_SLEEPING));
}

static void cmd_free(shell_t *sh) {
    mm_report_t rep;
    mm_verify(&rep);
    sh_emit(sh, TC_GREY, "              total        used        free\n");
    sh_printf(sh, TC_DEFAULT, "Mem:    %10u KiB %10u KiB %10u KiB\n",
              (unsigned)(PHYS_RAM_SIZE / 1024), (unsigned)(rep.bytes_used / 1024),
              (unsigned)((PHYS_RAM_SIZE - rep.bytes_used) / 1024));
    sh_printf(sh, TC_DEFAULT, "Heap:   %10u KiB %10u KiB %10u KiB\n",
              (unsigned)(mm_total() / 1024), (unsigned)(rep.bytes_used / 1024),
              (unsigned)(rep.bytes_free / 1024));
    sh_printf(sh, TC_DEFAULT, "Swap:   %10u KiB\n", 0u);
    sh_printf(sh, TC_DEFAULT, "blocks: %u used, %u free, largest free %u B, frag %u%%\n",
              (unsigned)rep.blocks_used, (unsigned)rep.blocks_free,
              (unsigned)rep.largest_free, (unsigned)rep.frag_pct);
    sh_printf(sh, rep.errors ? TC_RED : TC_GREEN,
              "allocator integrity: %s (%u errors)\n",
              rep.errors ? "CORRUPT" : "verified", (unsigned)rep.errors);
}

static void cmd_memmap(shell_t *sh) {
    sh_emit(sh, TC_GREY, "Physical address map (emulated machine)\n");
    sh_printf(sh, TC_CYAN, "  0x%08X - 0x%08X  %6u KiB  kernel: IDT, PCB table, syscall vector\n",
              (unsigned)KERNEL_BASE, (unsigned)(KERNEL_BASE + KERNEL_SIZE), (unsigned)(KERNEL_SIZE / 1024));
    sh_printf(sh, TC_CYAN, "  0x%08X - 0x%08X  %6u KiB  kernel/user heap (boundary tags)\n",
              (unsigned)HEAP_BASE, (unsigned)(HEAP_BASE + HEAP_SIZE), (unsigned)(HEAP_SIZE / 1024));
    sh_printf(sh, TC_CYAN, "  0x%08X - 0x%08X  %6u KiB  NVRAM ramdisk (inode VFS)\n",
              (unsigned)DISK_BASE, (unsigned)(DISK_BASE + DISK_SIZE), (unsigned)(DISK_SIZE / 1024));
    sh_printf(sh, TC_DEFAULT, "  mm state: total %u B used %u B blocks %u/%u\n",
              (unsigned)mm_total(), (unsigned)mm_used(),
              (unsigned)mm_state()->blocks_alloc, (unsigned)mm_state()->blocks_free);
}

static void cmd_top(shell_t *sh) {
    u32 i;
    {
        u32 up = irq_ticks() / (irq_hz() ? irq_hz() : 1000);
        u32 busy = proc_cpu_total_percent();
        sh_printf(sh, TC_CYAN, "top - up %u:%02u:%02u, %u tasks, load %u%%, cpu %u%%\n",
                  up / 3600, (up / 60) % 60, up % 60,
                  (unsigned)proc_count_state(PROC_READY) + (unsigned)proc_count_state(PROC_RUNNING) +
                  (unsigned)proc_count_state(PROC_SLEEPING),
                  busy, 100u - (busy > 100 ? 100 : busy));
    }
    sh_emit(sh, TC_GREY, "  PID USER      PRI  STAT    CPU%   RES   NAME\n");
    for (i = 1; i < MAX_PROCS; i++) {
        pcb_t *p = proc_pcb(i);
        if (!p) continue;
        sh_printf(sh, p->state == PROC_RUNNING ? TC_GREEN : TC_DEFAULT,
                  "%5u user      %3u  %-7s %4u %6uK  %s\n",
                  (unsigned)p->pid, (unsigned)p->priority, proc_state_name(p->state),
                  (unsigned)p->cpu_percent, (unsigned)(p->mem_used / 1024), p->name);
    }
}

static void cmd_uname(shell_t *sh, int argc, char **argv) {
    if (argc > 1 && !kstrcmp(argv[1], "-a")) {
        sh_printf(sh, TC_GREEN, "AetherOS aether 1.0.0-aether #1 SMP PREEMPT x86_64 Virtual\n");
        sh_printf(sh, TC_DEFAULT, "kernel: monolithic hybrid, 32-bit protected mode (emulated)\n");
        sh_printf(sh, TC_DEFAULT, "cpu: %s\n", g_machine.cpu_name);
        sh_printf(sh, TC_DEFAULT, "timer: %u Hz, ram: %u MiB, heap: %u MiB, ramdisk: %u MiB\n",
                  (unsigned)irq_hz(), (unsigned)(PHYS_RAM_SIZE / (1024 * 1024)),
                  (unsigned)(HEAP_SIZE / (1024 * 1024)), (unsigned)(DISK_SIZE / (1024 * 1024)));
    } else {
        sh_emit(sh, TC_DEFAULT, "AetherOS\n");
    }
}

static void cmd_dmesg(shell_t *sh) {
    u32 len = 0;
    const char *log = klog_data(&len);
    sh_emit(sh, TC_GREY, log);
    sh_printf(sh, TC_GREY, "[%u bytes in the kernel ring buffer]\n", (unsigned)len);
}

static void cmd_irq(shell_t *sh) {
    irq_ctrl_t *irq = irq_state();
    int i;
    sh_emit(sh, TC_GREY, " IRQ  VECTOR  COUNT      DEVICE\n");
    for (i = 0; i < 3; i++)
        sh_printf(sh, TC_CYAN, "%4s   0x%02X %8u   %s\n", irq->desc[i].name,
                  (unsigned)irq->desc[i].vector, (unsigned)irq->desc[i].count,
                  irq->desc[i].device);
    sh_printf(sh, TC_DEFAULT, "ticks=%u hz=%u kbd_events=%u mouse_events=%u masked=0x%X\n",
              (unsigned)irq->ticks, (unsigned)irq->hz,
              (unsigned)irq->kbd_events, (unsigned)irq->mouse_events,
              (unsigned)irq->irq_mask);
}

static void cmd_syscalls(shell_t *sh) {
    sched_t *s = proc_sched();
    int i;
    sh_printf(sh, TC_GREY, "syscall dispatcher: %u calls total, last %s -> %lld (%llu us)\n",
              (unsigned)s->syscall_total, syscall_name((int)syscall_last()->num),
              (long long)syscall_last()->ret, (unsigned long long)syscall_last()->tick_us);
    sh_emit(sh, TC_GREY, " vector  name         count\n");
    for (i = 0; i < SYS_COUNT; i++)
        if (s->syscall_counts[i])
            sh_printf(sh, TC_CYAN, "%7d  %-11s %7u\n", i, syscall_name(i),
                      (unsigned)s->syscall_counts[i]);
}

static void cmd_spawn(shell_t *sh, int argc, char **argv) {
    const app_desc_t *d;
    int id;
    if (argc < 2) {
        sh_emit(sh, TC_RED, "usage: spawn <app>   (try 'apps')\n");
        return;
    }
    d = app_by_id(argv[1]);
    if (!d) {
        sh_printf(sh, TC_RED, "spawn: no such application '%s'\n", argv[1]);
        return;
    }
    id = wm_open_app(d, argc > 2 ? argv[2] : NULL);
    if (id > 0) sh_printf(sh, TC_GREEN, "spawned %s as window %d\n", d->name, id);
}

static void cmd_apps(shell_t *sh) {
    int i;
    sh_emit(sh, TC_GREY, "index  id          name               description\n");
    for (i = 0; i < APP_COUNT; i++) {
        const app_desc_t *d = app_by_index(i);
        sh_printf(sh, TC_CYAN, "%5d  %-11s %-18s %s\n", i, d->id, d->name,
                  d->subtitle ? d->subtitle : "");
    }
}

static void cmd_cc(shell_t *sh, int argc, char **argv) {
    const char *src = NULL;
    char outpath[VFS_MAX_PATH];
    char *source;
    u32 len = 0;
    const char *err = NULL;
    int i;
    outpath[0] = 0;
    for (i = 1; i < argc; i++) {
        if (!kstrcmp(argv[i], "-o") && i + 1 < argc) { sh_absolute(sh, argv[++i], outpath, sizeof(outpath)); }
        else if (!src) {
            char abs[VFS_MAX_PATH];
            sh_absolute(sh, argv[i], abs, sizeof(abs));
            src = argv[i];
            kstrncpy(sh->last_status, abs, sizeof(sh->last_status) - 1);
        }
    }
    if (!src) { sh_emit(sh, TC_RED, "usage: cc <source.c> [-o /bin/out]\n"); return; }
    source = vfs_read_alloc(src, &len, &err);
    if (!source) { sh_printf(sh, TC_RED, "cc: %s: %s\n", src, err ? err : "cannot read"); return; }
    {
        char base[64];
        cc_program_t *prog = (cc_program_t *)mm_kmalloc(sizeof(cc_program_t));
        u8 *image;
        u32 imgsize;
        int rc;
        kstrncpy(base, sh_base(src), 63);
        {
            char *dot = kstrrchr(base, '.');
            if (dot) *dot = 0;
        }
        if (!prog) { mm_kfree(source); return; }
        sh_printf(sh, TC_GREY, "aethercc: compiling %s (%u bytes)\n", src, (unsigned)len);
        rc = cc_compile(source, base, prog);
        if (rc != 0) {
            sh_printf(sh, TC_RED, "%s\n", prog->error);
            sh_printf(sh, TC_RED, "aethercc: 1 error, no output written\n");
            mm_kfree(prog);
            mm_kfree(source);
            return;
        }
        imgsize = cc_image_size(prog);
        image = (u8 *)mm_kmalloc(imgsize + 16);
        if (!image) { mm_kfree(prog); mm_kfree(source); return; }
        imgsize = cc_serialise(prog, image, imgsize + 16);
        if (!outpath[0]) ksnprintf(outpath, sizeof(outpath), "/bin/%s.aeb", base);
        if (vfs_write_file(outpath, image, imgsize) == 0) {
            sh_printf(sh, TC_GREEN, "aethercc: %u instructions, %d bytes written to %s\n",
                      (unsigned)prog->code_len, (unsigned)imgsize, outpath);
            sh_printf(sh, TC_GREY, "aethercc: run it with: run %s\n", outpath);
        } else {
            sh_printf(sh, TC_RED, "aethercc: cannot write %s\n", outpath);
        }
        mm_kfree(image);
        mm_kfree(prog);
    }
    mm_kfree(source);
}

static void cmd_disasm(shell_t *sh, int argc, char **argv) {
    static const char *names[] = {
        "HALT","PUSH","POP","DUP","SWAP","ADD","SUB","MUL","DIV","MOD","NEG",
        "BAND","BOR","BXOR","SHL","SHR","LNOT","EQ","NE","LT","LE","GT","GE",
        "AND","OR","JMP","JZ","JNZ","CALL","RET","ENTER","LOADI","STOREI",
        "LOADG","STOREG","PUTS","PRINTI","NL","DRAW","CLEAR","SLEEP","TIME",
        "RAND","ALLOC","FREE"
    };
    u32 len = 0;
    const char *err = NULL;
    char *source;
    cc_program_t *prog;
    u32 i;
    if (argc < 2) { sh_emit(sh, TC_RED, "usage: disasm <source.c>\n"); return; }
    source = vfs_read_alloc(argv[1], &len, &err);
    if (!source) { sh_printf(sh, TC_RED, "disasm: %s\n", err ? err : "?"); return; }
    prog = (cc_program_t *)mm_kmalloc(sizeof(cc_program_t));
    if (!prog) { mm_kfree(source); return; }
    if (cc_compile(source, argv[1], prog) != 0) {
        sh_printf(sh, TC_RED, "%s\n", prog->error);
    } else {
        sh_printf(sh, TC_CYAN, "; %s: %u instructions\n", argv[1], (unsigned)prog->code_len);
        for (i = 0; i < prog->code_len; i++) {
            int op = prog->code[i].op;
            sh_printf(sh, TC_GREY, "%04X  ", (unsigned)i);
            sh_printf(sh, TC_GREEN, "%-7s", (op >= 0 && op < OP_COUNT) ? names[op] : "?");
            if (prog->code[i].arg || op == OP_CALL || op == OP_JMP || op == OP_JZ)
                sh_printf(sh, TC_YELLOW, " %d", (int)prog->code[i].arg);
            sh_emit(sh, TC_DEFAULT, "\n");
        }
    }
    mm_kfree(prog);
    mm_kfree(source);
}

static void cmd_run(shell_t *sh, int argc, char **argv) {
    char path[VFS_MAX_PATH];
    u32 len = 0;
    const char *err = NULL;
    char *data;
    if (argc < 2) { sh_emit(sh, TC_RED, "usage: run <program>\n"); return; }
    if (kstrchr(argv[1], '/')) kstrncpy(path, argv[1], sizeof(path) - 1);
    else ksnprintf(path, sizeof(path), "/bin/%s.aeb", argv[1]);
    data = vfs_read_alloc(path, &len, &err);
    if (!data) {
        /* try a .c source with the same name */
        ksnprintf(path, sizeof(path), "/home/user/%s.c", argv[1]);
        data = vfs_read_alloc(path, &len, &err);
        if (!data) { sh_printf(sh, TC_RED, "run: %s: %s\n", argv[1], err ? err : "not found"); return; }
    }
    if (len > 4 && !kstrncmp(data, "AEBX", 4)) {
        cc_program_t *prog = (cc_program_t *)mm_kmalloc(sizeof(cc_program_t));
        u32 pid = 0;
        if (prog && cc_deserialise((const u8 *)data, len, prog) == 0) {
            int slot = cc_spawn_program(prog, sh_base(path), &pid);
            if (slot >= 0)
                sh_printf(sh, TC_GREEN, "exec %s -> pid %u (bytecode VM, %u insns)\n",
                          path, (unsigned)pid, (unsigned)prog->code_len);
            else
                sh_emit(sh, TC_RED, "run: VM process slots exhausted\n");
        } else {
            sh_printf(sh, TC_RED, "run: %s: corrupt AEBX image\n", path);
        }
    } else {
        /* source: compile and run synchronously, showing output */
        static char out[4096];
        static char errbuf[256];
        int insns = 0;
        int rc = cc_run_source(data, sh_base(path), out, sizeof(out), &insns, errbuf, sizeof(errbuf));
        if (rc != 0) sh_printf(sh, TC_RED, "runtime: %s\n", errbuf[0] ? errbuf : "compile failed");
        if (out[0]) sh_emit(sh, TC_DEFAULT, out);
        sh_printf(sh, TC_GREY, "[%d instructions executed]\n", insns);
    }
    mm_kfree(data);
}

static void cmd_tree(shell_t *sh, const char *path, int depth) {
    int i, n;
    if (depth > 3) return;
    n = vfs_dir_count(path);
    for (i = 0; i < n; i++) {
        vfs_dirent_t d;
        char child[VFS_MAX_PATH];
        int j;
        if (vfs_dir_get(path, i, &d) != 0) break;
        if (!kstrcmp(d.name, ".") || !kstrcmp(d.name, "..")) continue;
        for (j = 0; j < depth; j++) sh_emit(sh, TC_GREY, "|   ");
        sh_printf(sh, d.type == FT_DIR ? TC_BLUE : TC_DEFAULT, "%s%s\n", d.name,
                  d.type == FT_DIR ? "/" : "");
        ksnprintf(child, sizeof(child), "%s%s%s", kstrcmp(path, "/") ? path : "",
                  kstrcmp(path, "/") ? "/" : "", d.name);
        if (d.type == FT_DIR) cmd_tree(sh, child, depth + 1);
    }
}

static int kstrstr_ci(const char *hay, const char *needle);

static void cmd_find(shell_t *sh, const char *path, const char *needle, int depth) {
    int i, n;
    if (depth > 6) return;
    n = vfs_dir_count(path);
    for (i = 0; i < n; i++) {
        vfs_dirent_t d;
        char child[VFS_MAX_PATH];
        if (vfs_dir_get(path, i, &d) != 0) break;
        if (!kstrcmp(d.name, ".") || !kstrcmp(d.name, "..")) continue;
        if (kstrstr_ci(d.name, needle))
            sh_printf(sh, TC_GREEN, "%s%s%s\n", kstrcmp(path, "/") ? path : "",
                      kstrcmp(path, "/") ? "/" : "", d.name);
        ksnprintf(child, sizeof(child), "%s%s%s", kstrcmp(path, "/") ? path : "",
                  kstrcmp(path, "/") ? "/" : "", d.name);
        if (d.type == FT_DIR) cmd_find(sh, child, needle, depth + 1);
    }
}

static int cmd_execute(shell_t *sh, char *line);

static void sh_exec_line(shell_t *sh, const char *line) {
    char buf[TERM_CMD_LEN];
    kstrncpy(buf, line, sizeof(buf) - 1);
    if (sh->nhist == 0 || kstrcmp(sh->hist[(sh->nhist - 1) % TERM_HIST], buf)) {
        kstrncpy(sh->hist[sh->nhist % TERM_HIST], buf, TERM_CMD_LEN - 1);
        sh->hist[sh->nhist % TERM_HIST][TERM_CMD_LEN - 1] = 0;
        sh->nhist++;
        sh->hist_pos = sh->nhist;
    }
    cmd_execute(sh, buf);
}

/* -------------------------------------------------------------------------- */
/*  dispatcher                                                              */
/* -------------------------------------------------------------------------- */

static int cmd_execute(shell_t *sh, char *line) {
    char *argv[16];
    int argc;
    const char *cmd;
    int i;

    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return 0;
    argc = sh_split(line, argv, 16);
    if (argc == 0) return 0;
    cmd = argv[0];
    sh->total_cmds++;

    /* pipe/redirect are intentionally rejected with a helpful message so
       nothing silently does the wrong thing */
    for (i = 1; i < argc; i++) {
        if (!kstrcmp(argv[i], "|") || !kstrcmp(argv[i], ">")) {
            sh_printf(sh, TC_YELLOW, "sh: '%s' is not supported by AetherShell (no pipes)\n", argv[i]);
            return 0;
        }
    }

    if (!kstrcmp(cmd, "help") || !kstrcmp(cmd, "?")) { cmd_help(sh); return 0; }
    if (!kstrcmp(cmd, "ls") || !kstrcmp(cmd, "dir")) { cmd_ls(sh, argc, argv); return 0; }
    if (!kstrcmp(cmd, "cd")) {
        const char *target = argc > 1 ? argv[1] : "/home/user";
        char abs[VFS_MAX_PATH];
        if (vfs_chdir(target) != 0) {
            sh_printf(sh, TC_RED, "cd: %s: no such directory\n", target);
            return 0;
        }
        vfs_normalise("/", vfs_cwd(), abs, sizeof(abs));
        kstrncpy(sh->cwd, abs, sizeof(sh->cwd) - 1);
        return 0;
    }
    if (!kstrcmp(cmd, "pwd")) { sh_printf(sh, TC_DEFAULT, "%s\n", sh->cwd); return 0; }
    if (!kstrcmp(cmd, "cat")) { cmd_cat(sh, argc, argv); return 0; }
    if (!kstrcmp(cmd, "touch")) {
        if (argc < 2) { sh_emit(sh, TC_RED, "usage: touch <file>\n"); return 0; }
        if (vfs_resolve(argv[1])) sh_printf(sh, TC_GREY, "touch: %s already exists\n", argv[1]);
        else if (vfs_create_node(argv[1], FT_FILE)) sh_printf(sh, TC_GREEN, "created %s\n", argv[1]);
        else sh_printf(sh, TC_RED, "touch: cannot create %s\n", argv[1]);
        return 0;
    }
    if (!kstrcmp(cmd, "mkdir")) {
        if (argc < 2) { sh_emit(sh, TC_RED, "usage: mkdir <dir>\n"); return 0; }
        if (vfs_mkdir_path(argv[1]) == 0) sh_printf(sh, TC_GREEN, "created directory %s\n", argv[1]);
        else sh_printf(sh, TC_RED, "mkdir: cannot create %s\n", argv[1]);
        return 0;
    }
    if (!kstrcmp(cmd, "rm") || !kstrcmp(cmd, "unlink")) {
        if (argc < 2) { sh_emit(sh, TC_RED, "usage: rm <file>\n"); return 0; }
        {
            int rc = vfs_unlink(argv[1]);
            if (rc == 0) sh_printf(sh, TC_GREEN, "removed %s\n", argv[1]);
            else if (rc == -2) sh_printf(sh, TC_RED, "rm: %s: directory not empty\n", argv[1]);
            else sh_printf(sh, TC_RED, "rm: %s: cannot remove\n", argv[1]);
        }
        return 0;
    }
    if (!kstrcmp(cmd, "cp")) {
        if (argc < 3) { sh_emit(sh, TC_RED, "usage: cp <src> <dst>\n"); return 0; }
        if (vfs_copy(argv[1], argv[2]) == 0) sh_printf(sh, TC_GREEN, "%s -> %s\n", argv[1], argv[2]);
        else sh_emit(sh, TC_RED, "cp: failed\n");
        return 0;
    }
    if (!kstrcmp(cmd, "mv")) {
        if (argc < 3) { sh_emit(sh, TC_RED, "usage: mv <src> <dst>\n"); return 0; }
        if (vfs_rename(argv[1], argv[2]) == 0) sh_printf(sh, TC_GREEN, "%s => %s\n", argv[1], argv[2]);
        else sh_emit(sh, TC_RED, "mv: failed\n");
        return 0;
    }
    if (!kstrcmp(cmd, "write") || !kstrcmp(cmd, "echo")) {
        const char *file = NULL, *text = NULL;
        int j;
        if (argc < 3) { sh_emit(sh, TC_RED, "usage: write <file> <text...>\n"); return 0; }
        file = argv[1];
        for (j = 2; j < argc; j++) {
            if (!text) text = argv[j];
            else {
                static char jbuf[512];
                ksnprintf(jbuf, sizeof(jbuf), "%s %s", text, argv[j]);
                text = jbuf;
            }
        }
        if (vfs_write_string(file, text) == 0) {
            sh_printf(sh, TC_GREEN, "wrote %u bytes to %s\n", (unsigned)kstrlen(text), file);
        } else sh_emit(sh, TC_RED, "write: failed\n");
        return 0;
    }
    if (!kstrcmp(cmd, "hexdump") || !kstrcmp(cmd, "hexview")) { cmd_hexdump(sh, argc, argv); return 0; }
    if (!kstrcmp(cmd, "stat")) { cmd_stat(sh, argc, argv); return 0; }
    if (!kstrcmp(cmd, "df")) { cmd_df(sh, argc, argv); return 0; }
    if (!kstrcmp(cmd, "tree")) { cmd_tree(sh, argc > 1 ? argv[1] : ".", 0); return 0; }
    if (!kstrcmp(cmd, "find")) {
        if (argc < 2) { sh_emit(sh, TC_RED, "usage: find <name>\n"); return 0; }
        cmd_find(sh, argc > 2 ? argv[2] : "/", argv[1], 0);
        return 0;
    }
    if (!kstrcmp(cmd, "ps")) { cmd_ps(sh); return 0; }
    if (!kstrcmp(cmd, "top")) { cmd_top(sh); return 0; }
    if (!kstrcmp(cmd, "kill")) {
        int pid;
        if (argc < 2) { sh_emit(sh, TC_RED, "usage: kill <pid>\n"); return 0; }
        pid = katoi(argv[1]);
        {
            int rc = proc_kill((u32)pid, 9);
            if (rc == 0) { sh_printf(sh, TC_GREEN, "SIGKILL sent to pid %d\n", pid); wm_close_by_pid((u32)pid); }
            else if (rc == -2) sh_printf(sh, TC_RED, "kill: pid 1 (init) is protected by the kernel\n");
            else if (rc == -3) sh_printf(sh, TC_RED, "kill: pid %d is a kernel thread\n", pid);
            else sh_printf(sh, TC_RED, "kill: no such process %d\n", pid);
        }
        return 0;
    }
    if (!kstrcmp(cmd, "free")) { cmd_free(sh); return 0; }
    if (!kstrcmp(cmd, "memmap")) { cmd_memmap(sh); return 0; }
    if (!kstrcmp(cmd, "irq")) { cmd_irq(sh); return 0; }
    if (!kstrcmp(cmd, "syscalls")) { cmd_syscalls(sh); return 0; }
    if (!kstrcmp(cmd, "spawn")) { cmd_spawn(sh, argc, argv); return 0; }
    if (!kstrcmp(cmd, "apps")) { cmd_apps(sh); return 0; }
    if (!kstrcmp(cmd, "uptime")) {
        u32 s = irq_ticks() / (irq_hz() ? irq_hz() : 1000);
        sh_printf(sh, TC_DEFAULT, "up %u days, %02u:%02u:%02u, %u processes, load %u%%\n",
                  s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60,
                  (unsigned)proc_alive(), (unsigned)proc_cpu_total_percent());
        return 0;
    }
    if (!kstrcmp(cmd, "uname")) { cmd_uname(sh, argc, argv); return 0; }
    if (!kstrcmp(cmd, "dmesg")) { cmd_dmesg(sh); return 0; }
    if (!kstrcmp(cmd, "cc") || !kstrcmp(cmd, "aethercc")) { cmd_cc(sh, argc, argv); return 0; }
    if (!kstrcmp(cmd, "run") || !kstrcmp(cmd, "exec")) { cmd_run(sh, argc, argv); return 0; }
    if (!kstrcmp(cmd, "disasm")) { cmd_disasm(sh, argc, argv); return 0; }
    if (!kstrcmp(cmd, "clear") || !kstrcmp(cmd, "cls")) {
        sh->nlines = 0;
        sh->scroll = 0;
        return 0;
    }
    if (!kstrcmp(cmd, "theme")) {
        int idx = argc > 1 ? katoi(argv[1]) : (theme_index() + 1) % 4;
        desktop_set_theme(idx);
        sh_printf(sh, TC_GREEN, "theme %d: %s  (saved)\n", theme_index(), theme_get()->name);
        notify(theme_get()->name);
        return 0;
    }
    if (!kstrcmp(cmd, "accent")) {
        /* accent [rrggbb | off] -- picks the highlight colour every app uses */
        if (argc < 2) {
            sh_printf(sh, TC_GREY, "accent is %s\n",
                      desktop_cfg()->accent ? "custom" : "from the theme");
            return 0;
        }
        if (!kstrcmp(argv[1], "off")) {
            desktop_set_accent(0, 0);
            sh_emit(sh, TC_GREEN, "accent follows the theme again\n");
            return 0;
        }
        {
            int ok = 0;
            u32 v = kstrtoul(argv[1][0] == '#' ? argv[1] + 1 : argv[1], 16, &ok);
            if (!ok) { sh_emit(sh, TC_RED, "accent: expected a hex colour like 4fc3f7\n"); return 0; }
            desktop_set_accent(1, v);
            sh_printf(sh, TC_GREEN, "accent set to #%06x (saved)\n", (unsigned)v);
        }
        return 0;
    }
    if (!kstrcmp(cmd, "fontscale") || !kstrcmp(cmd, "font")) {
        int i = argc > 1 ? katoi(argv[1]) : (desktop_cfg()->font_scale + 1) % 3;
        static const char *names[3] = { "compact", "normal", "large" };
        desktop_set_font_scale(i);
        sh_printf(sh, TC_GREEN, "interface scale: %s (saved)\n", names[desktop_cfg()->font_scale]);
        return 0;
    }
    if (!kstrcmp(cmd, "crt")) {
        desktop_set_crt(argc > 1 ? katoi(argv[1]) : !gfx_scanlines_enabled());
        sh_printf(sh, TC_GREEN, "CRT shader %s (saved)\n",
                  gfx_scanlines_enabled() ? "on" : "off");
        return 0;
    }
    if (!kstrcmp(cmd, "customise") || !kstrcmp(cmd, "customize") || !kstrcmp(cmd, "prefs")) {
        desktop_cfg_t *c = desktop_cfg();
        sh_emit(sh, TC_CYAN, "current desktop settings (/etc/desktop.cfg)\n");
        sh_printf(sh, TC_GREY, "  theme         %d  %s\n", c->theme, theme_name(c->theme));
        sh_printf(sh, TC_GREY, "  wallpaper     %d  %s\n", c->wallpaper, wallpaper_name(c->wallpaper));
        sh_printf(sh, TC_GREY, "  accent        %s #%06x\n", c->accent ? "custom" : "theme ",
                  (unsigned)(c->accent ? c->accent_color : theme_get()->accent));
        sh_printf(sh, TC_GREY, "  font scale    %d  (0 compact, 1 normal, 2 large)\n", c->font_scale);
        sh_printf(sh, TC_GREY, "  CRT shader    %s\n", gfx_scanlines_enabled() ? "on" : "off");
        sh_printf(sh, TC_GREY, "  transparency  %d%%\n", c->transparency);
        sh_emit(sh, TC_GREY, "  change with: theme N  wallpaper N  accent RRGGBB  crt 0|1  font 0-2\n");
        sh_emit(sh, TC_GREY, "  reset      : resetprefs\n");
        return 0;
    }
    if (!kstrcmp(cmd, "resetprefs")) {
        desktop_reset();
        sh_emit(sh, TC_GREEN, "desktop settings restored to defaults\n");
        notify("Settings reset");
        return 0;
    }
    if (!kstrcmp(cmd, "browser") || !kstrcmp(cmd, "web")) {
        const app_desc_t *d = app_by_id("browser");
        if (!d) { sh_emit(sh, TC_RED, "browser: not registered\n"); return 0; }
        if (argc > 1) {
            wm_open_app(d, argv[1]);
            sh_printf(sh, TC_GREEN, "opening %s\n", argv[1]);
        } else {
            wm_open_app(d, NULL);
            sh_emit(sh, TC_GREEN, "opening the browser\n");
        }
        return 0;
    }
    if (!kstrcmp(cmd, "wallpaper")) {
        int idx = argc > 1 ? katoi(argv[1]) : (wallpaper_index() + 1) % 4;
        wallpaper_set(idx);
        sh_printf(sh, TC_GREEN, "wallpaper %d: %s\n", idx, wallpaper_name(idx));
        return 0;
    }
    if (!kstrcmp(cmd, "reboot")) {
        sh_emit(sh, TC_YELLOW, "rebooting the AetherOS kernel...\n");
        wm_state()->reboot_requested = 1;
        return 0;
    }
    if (!kstrcmp(cmd, "panic") || !kstrcmp(cmd, "crash")) {
        sh_emit(sh, TC_RED, "*** deliberately triggering a kernel panic ***\n");
        wm_state()->crash_requested = 1;
        return 0;
    }
    if (!kstrcmp(cmd, "exit") || !kstrcmp(cmd, "logout")) {
        sh_emit(sh, TC_GREY, "closing the shell...\n");
        wm_state()->quit_requested = 1;
        return 0;
    }
    if (!kstrcmp(cmd, "about")) {
        sh_printf(sh, TC_CYAN, "AetherOS %s \"%s\"\n", AETHER_VERSION_STR, AETHER_CODENAME);
        sh_emit(sh, TC_DEFAULT,
            "Single-executable operating system: HAL, memory manager, inode VFS,\n"
            "scheduler, syscalls, compiler, VM, compositor and seven apps all live\n"
            "in this one C file. No external assets, no DLLs beyond the Win32\n"
            "chassis libraries.\n");
        return 0;
    }
    if (!kstrcmp(cmd, "sysinfo") || !kstrcmp(cmd, "neofetch")) {
        sh_emit(sh, TC_CYAN, "        /\\        AetherOS " AETHER_VERSION_STR "\n");
        sh_emit(sh, TC_CYAN, "       /  \\       ------------------\n");
        sh_emit(sh, TC_CYAN, "      / /\\ \\      kernel   : monolithic hybrid (emulated 32-bit)\n");
        sh_emit(sh, TC_CYAN, "     / ____ \\     cpu      : ");
        sh_emit(sh, TC_DEFAULT, g_machine.cpu_name);
        sh_emit(sh, TC_DEFAULT, "\n");
        sh_printf(sh, TC_CYAN, "    /_/    \\_\\    memory   : %u MiB (%u MiB heap)\n",
                  (unsigned)(PHYS_RAM_SIZE / (1024 * 1024)), (unsigned)(HEAP_SIZE / (1024 * 1024)));
        sh_printf(sh, TC_CYAN, "                   vfs      : %u inodes, %u blocks\n",
                  (unsigned)VFS_INODES, (unsigned)VFS_BLOCKS);
        sh_printf(sh, TC_CYAN, "                   windows  : %d open\n", wm_window_count());
        sh_printf(sh, TC_CYAN, "                   uptime   : %us\n",
                  (unsigned)(irq_ticks() / (irq_hz() ? irq_hz() : 1000)));
        return 0;
    }

    /* unknown command: maybe a program in /bin or the user's directory */
    {
        char path[VFS_MAX_PATH];
        ksnprintf(path, sizeof(path), "/bin/%s.aeb", cmd);
        if (vfs_resolve(path)) { cmd_run(sh, argc, argv); return 0; }
        ksnprintf(path, sizeof(path), "/home/user/%s.c", cmd);
        if (vfs_resolve(path)) { cmd_run(sh, argc, argv); return 0; }
    }
    sh_printf(sh, TC_RED, "sh: %s: command not found (type 'help')\n", cmd);
    return -1;
}

static int kstrstr_ci(const char *hay, const char *needle) {
    size_t n = kstrlen(needle);
    if (!n) return 1;
    while (*hay) {
        if (!kstrnicmp(hay, needle, n)) return 1;
        hay++;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  app interface                                                           */
/* -------------------------------------------------------------------------- */

static void *shell_create(const char *arg) {
    shell_t *sh = (shell_t *)mm_kmalloc(sizeof(shell_t));
    if (!sh) return NULL;
    kmemset(sh, 0, sizeof(*sh));
    kstrcpy(sh->cwd, vfs_cwd());
    sh->blink_on = 1;
    sh_emit(sh, TC_CYAN, "AetherShell " AETHER_VERSION_STR "  (AetherOS " AETHER_CODENAME ")\n");
    sh_emit(sh, TC_GREY, "Type 'help' for commands, 'apps' for programs, 'sysinfo' for a summary.\n\n");
    {
        u32 len = 0;
        const char *err = NULL;
        char *motd = vfs_read_alloc("/etc/motd", &len, &err);
        if (motd) { sh_emit(sh, TC_GREEN, motd); mm_kfree(motd); }
    }
    sh_prompt(sh);
    if (arg) {
        char cmd[TERM_CMD_LEN];
        kstrncpy(cmd, arg, sizeof(cmd) - 1);
        sh_exec_line(sh, cmd);
        sh_prompt(sh);
    }
    return sh;
}

static void shell_destroy(void *state) { if (state) mm_kfree(state); }

static void shell_paint(void *state, window_t *w, wm_rect_t client) {
    shell_t *sh = (shell_t *)state;
    theme_t *t = theme_get();
    int rows = (client.h - 18) / FONT_TERM_H;
    int first = sh->nlines - rows - sh->scroll;
    int i;
    if (rows < 1) rows = 1;
    if (first < 0) first = 0;

    gfx_fill_rect(client.x, client.y, client.w, client.h, RGBA(8, 12, 18, 252));
    /* title strip inside the client */
    gfx_gradient_h(client.x, client.y, client.w, 1, t->accent, RGBA(0, 0, 0, 0));

    for (i = 0; i < rows; i++) {
        int li = first + i;
        int y = client.y + 6 + i * FONT_TERM_H;
        u32 col = t->text;
        if (li >= sh->nlines) break;
        switch (sh->lines[li].color) {
        case TC_GREEN: col = t->ok; break;
        case TC_CYAN: col = t->info; break;
        case TC_YELLOW: col = t->warn; break;
        case TC_RED: col = t->err; break;
        case TC_MAGENTA: col = RGB(220, 130, 240); break;
        case TC_BLUE: col = RGB(120, 170, 255); break;
        case TC_GREY: col = t->text_dim; break;
        case TC_WHITE: col = CLR_WHITE; break;
        default: col = t->text; break;
        }
        gfx_text_term(client.x + 8, y, sh->lines[li].text, col, 0, 0);
    }

    /* input line */
    {
        int y = client.y + 6 + rows * FONT_TERM_H;
        char buf[TERM_CMD_LEN + 4];
        int cx;
        if (y + FONT_TERM_H > client.y + client.h) {
            /* shrink and put the input on the last row */
            y = client.y + client.h - FONT_TERM_H - 2;
            gfx_fill_rect(client.x + 2, y - 2, client.w - 4, FONT_TERM_H + 2, RGBA(10, 14, 22, 255));
        }
        /* coloured prompt: "user@aether:" + cwd + " $ " then the input line,
           laid out from one running x so the caret always lines up. */
        int px = client.x + 8;
        const char *pre = "user@aether:";
        ksnprintf(buf, sizeof(buf), "%s", "");
        gfx_text_term(px, y, pre, t->ok, 0, 0);
        px += (int)kstrlen(pre) * FONT_TERM_W;
        gfx_text_term(px, y, sh->cwd, t->info, 0, 0);
        px += (int)kstrlen(sh->cwd) * FONT_TERM_W;
        gfx_text_term(px, y, " $ ", t->ok, 0, 0);
        px += 3 * FONT_TERM_W;
        gfx_text_term(px, y, sh->input, t->text, 0, 0);
        cx = px + sh->cursor * FONT_TERM_W;
        if (sh->blink_on)
            gfx_fill_rect(cx, y, FONT_TERM_W, FONT_TERM_H,
                          RGBA(RED(t->accent), GREEN(t->accent), BLUE(t->accent), 200));
        (void)buf;
    }

    /* scrollbar */
    {
        int sbx = client.x + client.w - 8;
        int total = sh->nlines + 1;
        int thumb = k_max(20, client.h * rows / k_max(total, 1));
        int pos = (int)((long)(client.h - thumb) * sh->scroll / k_max(1, total - rows));
        gfx_fill_rect(sbx, client.y, 8, client.h, RGBA(0, 0, 0, 90));
        if (total > rows)
            gfx_round_rect(sbx + 2, client.y + pos, 4, thumb, 2, RGBA(120, 150, 190, 150));
    }
}

static void shell_tick(void *state, u32 dt_ms) {
    shell_t *sh = (shell_t *)state;
    sh->blink_ms += dt_ms;
    if (sh->blink_ms >= 520) { sh->blink_ms = 0; sh->blink_on = !sh->blink_on; }
}

static void shell_exec(shell_t *sh) {
    char cmd[TERM_CMD_LEN];
    kstrncpy(cmd, sh->input, sizeof(cmd) - 1);
    {
        /* echo the typed line into the scrollback as a completed prompt line */
        char echo[TERM_CMD_LEN + 320];
        ksnprintf(echo, sizeof(echo), "user@aether:%s$ %s", sh->cwd, sh->input);
        if (sh->nlines > 0 && sh->lines[sh->nlines - 1].is_prompt) sh->nlines--;
        sh_emit(sh, TC_DEFAULT, echo);
        sh_emit(sh, TC_DEFAULT, "\n");
    }
    if (cmd[0]) {
        if (sh->nhist == 0 || kstrcmp(sh->hist[(sh->nhist - 1) % TERM_HIST], cmd)) {
            kstrncpy(sh->hist[sh->nhist % TERM_HIST], cmd, TERM_CMD_LEN - 1);
            sh->nhist++;
        }
    }
    sh->hist_pos = sh->nhist;
    sh->input[0] = 0;
    sh->input_len = 0;
    sh->cursor = 0;
    sh->last_exec_ms = now_ms();
    cmd_execute(sh, cmd);
    kstrcpy(sh->cwd, vfs_cwd());
    sh_prompt(sh);
}

static void shell_exec(shell_t *sh);

static void shell_key(void *state, int key, int ch, int mods) {
    shell_t *sh = (shell_t *)state;
    (void)mods;
    if (key == 0x26) {                                  /* up */
        if (sh->hist_pos > 0) {
            int idx = --sh->hist_pos;
            if (idx >= 0 && idx < sh->nhist) {
                kstrncpy(sh->input, sh->hist[idx % TERM_HIST], TERM_CMD_LEN - 1);
                sh->input_len = (int)kstrlen(sh->input);
                sh->cursor = sh->input_len;
            }
        }
        return;
    }
    if (key == 0x28) {                                  /* down */
        if (sh->hist_pos < sh->nhist) {
            int idx = ++sh->hist_pos;
            if (idx < sh->nhist) {
                kstrncpy(sh->input, sh->hist[idx % TERM_HIST], TERM_CMD_LEN - 1);
                sh->input_len = (int)kstrlen(sh->input);
                sh->cursor = sh->input_len;
            } else {
                sh->input[0] = 0;
                sh->input_len = 0;
                sh->cursor = 0;
            }
        }
        return;
    }
    if (key == 0x21) { sh->scroll = k_min(sh->scroll + 4, k_max(0, sh->nlines)); return; }
    if (key == 0x22) { sh->scroll = k_max(0, sh->scroll - 4); return; }
    if (key == 0x09) {                                  /* tab completion */
        int i, n = vfs_dir_count(".");
        int matches = 0;
        char last[TERM_CMD_LEN];
        for (i = 0; i < n; i++) {
            vfs_dirent_t d;
            if (vfs_dir_get(".", i, &d) != 0) break;
            if (!kstrncmp(d.name, sh->input, kstrlen(sh->input)) && kstrlen(sh->input)) {
                kstrncpy(last, d.name, TERM_CMD_LEN - 1);
                matches++;
            }
        }
        if (matches == 1) {
            kstrcpy(sh->input, last);
            if (vfs_dir_count(last) >= 0) kstrcat(sh->input, "/");
            sh->input_len = (int)kstrlen(sh->input);
            sh->cursor = sh->input_len;
        }
        return;
    }
    if (key == 0x1B) { sh->input[0] = 0; sh->input_len = 0; sh->cursor = 0; return; }
    if (key == 0x08) {                                  /* backspace */
        if (sh->cursor > 0) {
            kmemmove(sh->input + sh->cursor - 1, sh->input + sh->cursor,
                     (size_t)(sh->input_len - sh->cursor + 1));
            sh->cursor--;
            sh->input_len--;
        }
        return;
    }
    if (key == 0x2E) {                                  /* delete */
        if (sh->cursor < sh->input_len) {
            kmemmove(sh->input + sh->cursor, sh->input + sh->cursor + 1,
                     (size_t)(sh->input_len - sh->cursor));
            sh->input_len--;
        }
        return;
    }
    if (key == 0x25) {                                  /* home */
        sh->cursor = 0;
        return;
    }
    if (key == 0x23) {                                  /* end */
        sh->cursor = sh->input_len;
        return;
    }
    if (key == 0x0D) { shell_exec(sh); return; }
    if (key == 0x1B) return;
    if (ch == '\n' || ch == '\r') { shell_exec(sh); return; }
    if (ch == 3) { sh->input[0] = 0; sh->input_len = 0; sh->cursor = 0; sh_prompt(sh); return; }
    if (ch == 12) { sh->nlines = 0; sh->scroll = 0; return; }
    if (ch >= 32 && ch < 127) {
        if (sh->input_len < TERM_CMD_LEN - 2) {
            kmemmove(sh->input + sh->cursor + 1, sh->input + sh->cursor,
                     (size_t)(sh->input_len - sh->cursor + 1));
            sh->input[sh->cursor] = (char)ch;
            sh->cursor++;
            sh->input_len++;
            sh->blink_on = 1;
            sh->blink_ms = 0;
        }
    }
}

static void shell_mouse(void *state, int event, int x, int y, int button, int wheel) {
    shell_t *sh = (shell_t *)state;
    (void)x; (void)y; (void)button;
    if (event == APP_EV_WHEEL) {
        sh->scroll = k_clamp(sh->scroll + wheel * 3, 0, k_max(0, sh->nlines));
    } else if (event == APP_EV_MOUSE_DOWN) {
        sh->scroll = 0;
    }
}

const app_desc_t app_shell = {
    "sh", "AetherShell", "terminal", ICON_TERMINAL,
    760, 460, 380, 220, APP_CAT_SYSTEM, 1, 0,
    shell_create, shell_destroy, shell_paint, shell_key, shell_mouse, shell_tick, NULL
};
