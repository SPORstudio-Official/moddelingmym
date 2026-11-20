/* ==========================================================================
 *  APP: edit -- Code Studio
 *
 *  Multi-line editor with line numbers, C syntax highlighting, full keyboard
 *  navigation, mouse caret placement, and a toolbar with New / Open / Save /
 *  Compile & Run (which compiles the buffer with the built-in AetherC
 *  compiler and shows the output in an attached console).
 * ========================================================================== */

#define ED_MAX_LINES 512
#define ED_MAX_COLS  256
#define ED_MAX_TEXT  (64 * 1024)
#define ED_VISIBLE   200

typedef struct {
    char path[VFS_MAX_PATH];
    char *lines[ED_MAX_LINES];
    int  nlines;
    int  cx, cy;                /* caret column / line                        */
    int  scroll;                /* first visible line                         */
    int  hscroll;
    int  dirty;
    int  modified_tick;
    int  status_kind;
    char status[160];
    char out[4096];             /* compile/run console output                 */
    int  show_console;
    u32  out_lines;
    int  insn_count;
    int  caret_visible_ms;
    int  select_anchor;         /* unused selection anchor (mouse drag)       */
    int  drag_select;
    int  font_small;
    int  highlight;
    int  console_scroll;
} editor_t;

/* -------------------------------------------------------------------------- */
/*  buffer management                                                       */
/* -------------------------------------------------------------------------- */

static const char *ed_base(const char *path) {
    const char *s = kstrrchr(path, '/');
    return s ? s + 1 : path;
}

/* bounded string append used all over the editor */
static void kstrncat_ed(char *dst, const char *src, size_t cap) {
    size_t d = kstrlen(dst);
    while (*src && d + 1 < cap) dst[d++] = *src++;
    dst[d] = 0;
}

static char *ed_strdup(const char *s) {
    size_t n = kstrlen(s) + 1;
    char *p = (char *)mm_kmalloc((u32)n);
    if (p) kmemcpy(p, s, n);
    return p;
}

static void ed_insert_line(editor_t *ed, int at, const char *text) {
    int i;
    if (ed->nlines >= ED_MAX_LINES) return;
    for (i = ed->nlines; i > at; i--) ed->lines[i] = ed->lines[i - 1];
    ed->lines[at] = ed_strdup(text);
    ed->nlines++;
}

static void ed_delete_line(editor_t *ed, int at) {
    int i;
    if (at < 0 || at >= ed->nlines) return;
    if (ed->lines[at]) mm_kfree(ed->lines[at]);
    for (i = at; i < ed->nlines - 1; i++) ed->lines[i] = ed->lines[i + 1];
    ed->nlines--;
    ed->lines[ed->nlines] = NULL;
}

static int ed_line_len(editor_t *ed, int y) {
    if (y < 0 || y >= ed->nlines || !ed->lines[y]) return 0;
    return (int)kstrlen(ed->lines[y]);
}

static void ed_set_line(editor_t *ed, int y, const char *text) {
    char *old = ed->lines[y];
    ed->lines[y] = ed_strdup(text);
    if (old) mm_kfree(old);
}

static void ed_clear(editor_t *ed) {
    int i;
    for (i = 0; i < ed->nlines; i++) if (ed->lines[i]) mm_kfree(ed->lines[i]);
    ed->nlines = 0;
    ed->cx = ed->cy = ed->scroll = ed->hscroll = 0;
}

static void ed_load_text(editor_t *ed, const char *text, const char *path) {
    ed_clear(ed);
    while (*text && ed->nlines < ED_MAX_LINES) {
        char buf[ED_MAX_COLS];
        int n = 0;
        while (*text && *text != '\n' && n < ED_MAX_COLS - 1) buf[n++] = *text++;
        buf[n] = 0;
        if (n > 0 && buf[n - 1] == '\r') buf[n - 1] = 0;
        ed->lines[ed->nlines] = ed_strdup(buf);
        ed->nlines++;
        if (*text == '\n') text++;
    }
    if (ed->nlines == 0) ed_insert_line(ed, 0, "");
    if (path) kstrncpy(ed->path, path, sizeof(ed->path) - 1);
    ed->dirty = 0;
}

static void ed_to_text(editor_t *ed, char **out, u32 *len) {
    u32 total = 0;
    int i;
    char *buf;
    for (i = 0; i < ed->nlines; i++) total += (u32)ed_line_len(ed, i) + 1;
    if (total > ED_MAX_TEXT) total = ED_MAX_TEXT;
    buf = (char *)mm_kmalloc(total + 2);
    if (!buf) { *out = NULL; *len = 0; return; }
    buf[0] = 0;
    for (i = 0; i < ed->nlines; i++) {
        kstrcat(buf, ed->lines[i] ? ed->lines[i] : "");
        kstrcat(buf, "\n");
    }
    *out = buf;
    *len = (u32)kstrlen(buf);
}

/* -------------------------------------------------------------------------- */
/*  editing operations                                                      */
/* -------------------------------------------------------------------------- */

static void ed_insert_char(editor_t *ed, int ch) {
    char *ln = ed->lines[ed->cy];
    int len = ed_line_len(ed, ed->cy);
    if (len >= ED_MAX_COLS - 2) return;
    ln = (char *)mm_krealloc(ln, (u32)len + 3);
    if (!ln) return;
    ed->lines[ed->cy] = ln;
    kmemmove(ln + ed->cx + 1, ln + ed->cx, (size_t)(len - ed->cx + 1));
    ln[ed->cx] = (char)ch;
    ed->cx++;
    ed->dirty = 1;
}

static void ed_newline(editor_t *ed) {
    char *ln = ed->lines[ed->cy];
    int len = ed_line_len(ed, ed->cy);
    char tail[ED_MAX_COLS];
    char head[ED_MAX_COLS];
    int indent = 0;
    int i;
    if (!ln) return;
    /* Clamp the caret: mouse clicks and stale column state can leave cx past
       the end of the line, and every slice below assumes cx <= len. */
    if (ed->cx > len) ed->cx = len;
    if (ed->cx < 0) ed->cx = 0;
    kstrncpy(tail, ln + ed->cx, sizeof(tail) - 1);
    tail[sizeof(tail) - 1] = 0;
    /* Snapshot the head before the line is reallocated, so the indent scan
       cannot read from a moved or freed buffer. */
    kstrncpy(head, ln, sizeof(head) - 1);
    head[sizeof(head) - 1] = 0;
    ln[ed->cx] = 0;
    ln = (char *)mm_krealloc(ln, (u32)ed->cx + 1);
    if (!ln) return;                    /* keep the truncated line, no split */
    ed->lines[ed->cy] = ln;
    /* auto-indent */
    for (i = 0; i < ed->cx && i < 16; i++) {
        if (head[i] == ' ') indent++;
        else break;
    }
    if (ed->cx > 0 && head[ed->cx - 1] == '{') indent += 4;
    if (indent > 16) indent = 16;
    {
        char indent_buf[24];
        char combined[ED_MAX_COLS * 2];
        int k;
        for (k = 0; k < indent; k++) indent_buf[k] = ' ';
        indent_buf[indent] = 0;
        ksnprintf(combined, sizeof(combined), "%s%s", indent_buf, tail);
        ed_insert_line(ed, ed->cy + 1, combined);
    }
    ed->cy++;
    ed->cx = k_min(indent, ed_line_len(ed, ed->cy));
    ed->dirty = 1;
    (void)len;
}

static void ed_backspace(editor_t *ed) {
    char *ln;
    int len;
    if (ed->cx > 0) {
        ln = ed->lines[ed->cy];
        len = ed_line_len(ed, ed->cy);
        kmemmove(ln + ed->cx - 1, ln + ed->cx, (size_t)(len - ed->cx + 1));
        ed->cx--;
        ed->dirty = 1;
    } else if (ed->cy > 0) {
        char prev[ED_MAX_COLS];
        int plen = ed_line_len(ed, ed->cy - 1);
        kstrncpy(prev, ed->lines[ed->cy - 1], sizeof(prev) - 1);
        kstrncat_ed(prev, ed->lines[ed->cy], sizeof(prev));
        ed_delete_line(ed, ed->cy);
        ed->cy--;
        ed_set_line(ed, ed->cy, prev);
        ed->cx = plen;
        ed->dirty = 1;
    }
    (void)ln;
}

static void ed_delete_forward(editor_t *ed) {
    int len = ed_line_len(ed, ed->cy);
    if (ed->cx < len) {
        char *ln = ed->lines[ed->cy];
        kmemmove(ln + ed->cx, ln + ed->cx + 1, (size_t)(len - ed->cx));
        ed->dirty = 1;
    } else if (ed->cy < ed->nlines - 1) {
        char buf[ED_MAX_COLS];
        kstrncpy(buf, ed->lines[ed->cy], sizeof(buf) - 1);
        kstrncat_ed(buf, ed->lines[ed->cy + 1], sizeof(buf));
        ed_delete_line(ed, ed->cy + 1);
        ed_set_line(ed, ed->cy, buf);
        ed->dirty = 1;
    }
}

static void ed_save(editor_t *ed) {
    char *text;
    u32 len = 0;
    if (!ed->path[0]) {
        kstrcpy(ed->path, "/home/user/untitled.c");
    }
    ed_to_text(ed, &text, &len);
    if (!text) return;
    if (vfs_write_file(ed->path, text, len) == 0) {
        ed->dirty = 0;
        ksnprintf(ed->status, sizeof(ed->status), "saved %u bytes to %s", len, ed->path);
        ed->status_kind = 1;
        notify_ex(ed->status, 1);
    } else {
        ksnprintf(ed->status, sizeof(ed->status), "cannot write %s", ed->path);
        ed->status_kind = 2;
    }
    mm_kfree(text);
}

static void ed_open(editor_t *ed, const char *path) {
    u32 len = 0;
    const char *err = NULL;
    char *text;
    if (!path || !path[0]) return;
    text = vfs_read_alloc(path, &len, &err);
    if (!text) {
        ksnprintf(ed->status, sizeof(ed->status), "open: %s: %s", path, err ? err : "failed");
        ed->status_kind = 2;
        return;
    }
    ed_load_text(ed, text, path);
    mm_kfree(text);
    ksnprintf(ed->status, sizeof(ed->status), "opened %s (%u bytes)", path, len);
    ed->status_kind = 0;
    ed->show_console = 0;
}

static void ed_compile_run(editor_t *ed) {
    char *text;
    u32 len = 0;
    char err[256];
    int insns = 0;
    int rc;
    ed_to_text(ed, &text, &len);
    if (!text) return;
    kmemset(err, 0, sizeof(err));
    rc = cc_run_source(text, ed->path[0] ? ed_base(ed->path) : "buffer", ed->out,
                       sizeof(ed->out) - 1, &insns, err, sizeof(err));
    ed->show_console = 1;
    ed->insn_count = insns;
    ed->out_lines = 0;
    {
        const char *p = ed->out;
        while (*p) { if (*p == '\n') ed->out_lines++; p++; }
    }
    if (rc != 0) {
        char buf[512];
        ksnprintf(buf, sizeof(buf), "compile failed: %s", err[0] ? err : "unknown error");
        kstrncat_ed(ed->out, "\n", sizeof(ed->out));
        kstrncat_ed(ed->out, buf, sizeof(ed->out));
        ed->status_kind = 2;
        ksnprintf(ed->status, sizeof(ed->status), "%s", buf);
    } else {
        char buf[256];
        ksnprintf(buf, sizeof(buf), "compiled and ran in %d VM instructions", insns);
        kstrncat_ed(ed->out, "\n--- ", sizeof(ed->out));
        kstrncat_ed(ed->out, buf, sizeof(ed->out));
        kstrncat_ed(ed->out, " ---\n", sizeof(ed->out));
        ed->status_kind = 1;
        ksnprintf(ed->status, sizeof(ed->status), "%s", buf);
    }
    mm_kfree(text);
}

/* -------------------------------------------------------------------------- */
/*  syntax highlighting                                                     */
/* -------------------------------------------------------------------------- */

static int is_ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_ident_char(int c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

static int is_c_keyword(const char *s, int n) {
    static const char *kw[] = { "func","var","if","else","while","for","return","break",
                                "continue","print","println","int","void","draw_rect",
                                "clear","sleep","time","rand","malloc", NULL };
    int i;
    for (i = 0; kw[i]; i++) {
        if ((int)kstrlen(kw[i]) == n && !kstrncmp(s, kw[i], (size_t)n)) return 1;
    }
    return 0;
}

typedef struct { int start, len; u32 color; } span_t;

static int hl_line(const char *line, span_t *spans, int maxspans, u32 *default_col,
                   u32 c_text, u32 c_kw, u32 c_num, u32 c_str, u32 c_com, u32 c_fn) {
    int i = 0, n = 0;
    int len = (int)kstrlen(line);
    *default_col = c_text;
    while (i < len && n < maxspans) {
        char c = line[i];
        if (c == '/' && line[i + 1] == '/') {
            spans[n].start = i; spans[n].len = len - i; spans[n].color = c_com; n++;
            break;
        }
        if (c == '"') {
            int j = i + 1;
            while (j < len && line[j] != '"') j++;
            if (j < len) j++;
            spans[n].start = i; spans[n].len = j - i; spans[n].color = c_str; n++;
            i = j;
            continue;
        }
        if (c >= '0' && c <= '9') {
            int j = i;
            while (j < len && ((line[j] >= '0' && line[j] <= '9') ||
                   (line[j] >= 'a' && line[j] <= 'f') || (line[j] >= 'A' && line[j] <= 'F') ||
                   line[j] == 'x' || line[j] == 'X')) j++;
            spans[n].start = i; spans[n].len = j - i; spans[n].color = c_num; n++;
            i = j;
            continue;
        }
        if (is_ident_start(c)) {
            int j = i;
            while (j < len && is_ident_char(line[j])) j++;
            if (is_c_keyword(line + i, j - i)) {
                spans[n].start = i; spans[n].len = j - i; spans[n].color = c_kw; n++;
            } else if (j < len && line[j] == '(') {
                spans[n].start = i; spans[n].len = j - i; spans[n].color = c_fn; n++;
            }
            i = j;
            continue;
        }
        i++;
    }
    return n;
}

/* -------------------------------------------------------------------------- */
/*  app interface                                                           */
/* -------------------------------------------------------------------------- */

static void *edit_create(const char *arg) {
    editor_t *ed = (editor_t *)mm_kmalloc(sizeof(editor_t));
    if (!ed) return NULL;
    kmemset(ed, 0, sizeof(*ed));
    ed->highlight = 1;
    ed_load_text(ed, "// welcome to Code Studio\n// Open a file from the File Explorer or press Ctrl+O.\n",
                 "/home/user/untitled.c");
    if (arg && arg[0]) ed_open(ed, arg);
    return ed;
}

static void edit_destroy(void *state) {
    editor_t *ed = (editor_t *)state;
    if (!ed) return;
    ed_clear(ed);
    mm_kfree(ed);
}

static void edit_paint(void *state, window_t *w, wm_rect_t c) {
    editor_t *ed = (editor_t *)state;
    theme_t *t = theme_get();
    int gutter = 46;
    int lh = 16;
    int rows = (c.h - 34 - (ed->show_console ? c.h / 3 : 0)) / lh;
    int i;

    gfx_fill_rect(c.x, c.y, c.w, c.h, RGB(14, 18, 26));
    if (rows < 1) rows = 1;

    /* toolbar */
    {
        const char *labels[4] = { "New", "Open", "Save", "Compile & Run" };
        int icons[4] = { ICON_TXT, ICON_FILES, ICON_CHECK, ICON_TERMINAL };
        int bx = c.x + 6, by = c.y + 4;
        for (i = 0; i < 4; i++) {
            int bw = gfx_text_w(labels[i], 0) + 30;
            int hover = (g_wm.mouse_px >= bx && g_wm.mouse_px < bx + bw &&
                         g_wm.mouse_py >= by && g_wm.mouse_py < by + 24);
            u32 bg = (i == 3) ? (hover ? RGB(70, 170, 110) : RGB(46, 130, 86))
                              : (hover ? t->sel : RGBA(40, 52, 72, 210));
            gfx_round_rect(bx, by, bw, 24, 5, bg);
            gfx_draw_icon(icons[i], bx + 4, by + 5, 14);
            gfx_text(bx + 22, by + 5, labels[i], t->text, 0);
            bx += bw + 4;
        }
        /* right side: line/col + modified marker */
        {
            char buf[96];
            ksnprintf(buf, sizeof(buf), "%s%s  Ln %d, Col %d",
                      ed->dirty ? "* " : "", ed_base(ed->path), ed->cy + 1, ed->cx + 1);
            gfx_text_right(c.x + c.w - 10, by + 6, buf, ed->dirty ? t->warn : t->text_dim, 0);
        }
    }

    /* editor body */
    {
        int top = c.y + 32;
        int editor_h = rows * lh;
        gfx_fill_rect(c.x, top, c.w, editor_h, RGB(12, 16, 22));
        gfx_fill_rect(c.x, top, gutter, editor_h, RGB(18, 24, 34));
        /* gutter separator */
        gfx_vline(c.x + gutter - 1, top, editor_h, RGBA(80, 100, 130, 90));

        for (i = 0; i < rows; i++) {
            int li = ed->scroll + i;
            int y = top + i * lh;
            char num[16];
            if (li >= ed->nlines) break;
            ksnprintf(num, sizeof(num), "%d", li + 1);
            gfx_text_right(c.x + gutter - 8, y + 1, num, li == ed->cy ? t->accent : t->text_mute, 0);
            if (li == ed->cy)
                gfx_fill_rect(c.x + 2, y, c.w - 4, lh, RGBA(255, 255, 255, 10));
            {
                const char *line = ed->lines[li] ? ed->lines[li] : "";
                span_t spans[64];
                u32 defcol = t->text;
                int ns = ed->highlight ? hl_line(line, spans, 64, &defcol, t->text,
                                                 RGB(255, 140, 190), RGB(250, 200, 120),
                                                 RGB(140, 220, 150), RGB(110, 130, 150),
                                                 RGB(140, 200, 255))
                                       : 0;
                int col = 0;
                char tmp[ED_MAX_COLS];
                int k;
                kstrncpy(tmp, line, sizeof(tmp) - 1);
                /* draw unstyled runs, overlaying coloured spans */
                gfx_text(c.x + gutter + 6 - ed->hscroll, y + 1, tmp, defcol, 0);
                for (k = 0; k < ns; k++) {
                    int n = spans[k].len;
                    if (n > ED_MAX_COLS - 1) n = ED_MAX_COLS - 1;
                    kmemcpy(tmp, line + spans[k].start, (size_t)n);
                    tmp[n] = 0;
                    /* erase the region then redraw in colour */
                    {
                        int px = c.x + gutter + 6 - ed->hscroll + gfx_text_w_n(line, spans[k].start, 0);
                        gfx_fill_rect(px, y, gfx_text_w(tmp, 0) + 1, lh, RGB(12, 16, 22));
                        gfx_text(px, y + 1, tmp, spans[k].color, 0);
                    }
                }
                col = gfx_text_w_n(line, ed->cx, 0);
                if (li == ed->cy) {
                    int cx = c.x + gutter + 6 - ed->hscroll + col;
                    if ((now_ms() / 500) % 2)
                        gfx_fill_rect(cx, y + 1, 1, lh - 2, t->accent);
                }
                (void)col;
            }
        }
        /* scrollbar */
        {
            int sbx = c.x + c.w - 8;
            int thumb = k_max(24, editor_h * rows / k_max(1, ed->nlines));
            int pos = (int)((long)(editor_h - thumb) * ed->scroll / k_max(1, ed->nlines - rows));
            gfx_fill_rect(sbx, top, 8, editor_h, RGBA(0, 0, 0, 80));
            if (ed->nlines > rows)
                gfx_round_rect(sbx + 2, top + pos, 4, thumb, 2, RGBA(120, 150, 190, 150));
        }
    }

    /* console */
    if (ed->show_console) {
        int cy = c.y + c.h - c.h / 3;
        int ch = c.h / 3;
        gfx_fill_rect(c.x, cy, c.w, ch, RGB(8, 10, 16));
        gfx_hline(c.x, cy, c.w, t->border);
        gfx_text(c.x + 8, cy + 4, "output console", t->text_mute, 0);
        gfx_text_right(c.x + c.w - 8, cy + 4, "[x] close", t->text_mute, 0);
        {
            const char *p = ed->out;
            int y = cy + 20;
            int shown = 0;
            while (*p && y < cy + ch - 12 && shown < 200) {
                char line[200];
                int n = 0;
                while (*p && *p != '\n' && n < 190) line[n++] = *p++;
                line[n] = 0;
                if (*p == '\n') p++;
                gfx_text_term(c.x + 8, y, line, t->text, 0, 0);
                y += FONT_TERM_H - 1;
                shown++;
            }
        }
    }

    /* status bar */
    {
        int sy = c.y + c.h - 20;
        gfx_fill_rect(c.x, sy, c.w, 20, RGBA(10, 14, 22, 240));
        if (ed->status[0])
            gfx_text(c.x + 8, sy + 3, ed->status,
                     ed->status_kind == 1 ? t->ok : ed->status_kind == 2 ? t->err : t->text_dim, 0);
        gfx_text_right(c.x + c.w - 8, sy + 3, "Ctrl+S save   Ctrl+O open   F5 run   Tab indent",
                       t->text_mute, 0);
    }
}

static void edit_tick(void *state, u32 dt) { (void)state; (void)dt; }

static void edit_key(void *state, int key, int ch, int mods) {
    editor_t *ed = (editor_t *)state;
    int rows = 24;
    if (mods & MOD_CTRL) {
        if (ch == 's' || ch == 'S') { ed_save(ed); return; }
        if (ch == 'o' || ch == 'O') { ed_open(ed, "/home/user/hello.c"); return; }
        if (ch == 'n' || ch == 'N') {
            ed_clear(ed);
            ed_insert_line(ed, 0, "");
            kstrcpy(ed->path, "/home/user/untitled.c");
            ed->dirty = 0;
            return;
        }
        if (ch == 'r' || ch == 'R') { ed_compile_run(ed); return; }
    }
    if (key == 0x74) { ed_compile_run(ed); return; }
    switch (key) {
    case 0x26:                                      /* up */
        if (ed->cy > 0) ed->cy--;
        ed->cx = k_min(ed->cx, ed_line_len(ed, ed->cy));
        break;
    case 0x28:                                      /* down */
        if (ed->cy < ed->nlines - 1) ed->cy++;
        ed->cx = k_min(ed->cx, ed_line_len(ed, ed->cy));
        break;
    case 0x25:                                      /* left */
        if (ed->cx > 0) ed->cx--;
        else if (ed->cy > 0) { ed->cy--; ed->cx = ed_line_len(ed, ed->cy); }
        break;
    case 0x27:                                      /* right */
        if (ed->cx < ed_line_len(ed, ed->cy)) ed->cx++;
        else if (ed->cy < ed->nlines - 1) { ed->cy++; ed->cx = 0; }
        break;
    case 0x24:                                      /* home */
        ed->cx = 0;
        break;
    case 0x23:                                      /* end */
        ed->cx = ed_line_len(ed, ed->cy);
        break;
    case 0x21:                                      /* page up */
        ed->cy = k_max(0, ed->cy - rows);
        ed->cx = k_min(ed->cx, ed_line_len(ed, ed->cy));
        break;
    case 0x22:                                      /* page down */
        ed->cy = k_min(ed->nlines - 1, ed->cy + rows);
        ed->cx = k_min(ed->cx, ed_line_len(ed, ed->cy));
        break;
    case 0x2E: ed_delete_forward(ed); break;
    case 0x08: ed_backspace(ed); break;
    case 0x09: {
        int n = 4 - (ed->cx % 4);
        while (n--) ed_insert_char(ed, ' ');
        break;
    }
    case 0x0D: ed_newline(ed); break;
    default:
        if (ch >= 32 && ch < 127) ed_insert_char(ed, ch);
        break;
    }
    /* keep the caret visible */
    if (ed->cy < ed->scroll) ed->scroll = ed->cy;
    if (ed->cy >= ed->scroll + rows) ed->scroll = ed->cy - rows + 1;
    if (ed->scroll < 0) ed->scroll = 0;
    if (kstrcmp(ed->status, "") != 0) ed->modified_tick = (int)now_ms();
}

static void edit_mouse(void *state, int event, int x, int y, int button, int wheel) {
    editor_t *ed = (editor_t *)state;
    wm_rect_t c = wm_input_client_rect();
    (void)button;
    if (event == APP_EV_WHEEL) {
        if (ed->show_console && y > c.y + c.h - c.h / 3) ed->console_scroll = k_max(0, ed->console_scroll - wheel);
        else ed->scroll = k_clamp(ed->scroll - wheel, 0, k_max(0, ed->nlines - 1));
        return;
    }
    if (event != APP_EV_MOUSE_DOWN) return;
    /* toolbar */
    if (y < c.y + 32) {
        const char *labels[4] = { "New", "Open", "Save", "Compile & Run" };
        int bx = c.x + 6;
        int i;
        for (i = 0; i < 4; i++) {
            int bw = gfx_text_w(labels[i], 0) + 30;
            if (x >= bx && x < bx + bw) {
                if (i == 0) {
                    ed_clear(ed);
                    ed_insert_line(ed, 0, "");
                    kstrcpy(ed->path, "/home/user/untitled.c");
                } else if (i == 1) {
                    ed_open(ed, "/home/user/hello.c");
                } else if (i == 2) {
                    ed_save(ed);
                } else {
                    ed_compile_run(ed);
                }
                return;
            }
            bx += bw + 4;
        }
        return;
    }
    if (ed->show_console && y > c.y + c.h - c.h / 3 - 18)
        return;
    /* caret placement */
    {
        int lh = 16;
        int top = c.y + 32;
        int li = ed->scroll + (y - top) / lh;
        int col = x - (c.x + 46 + 6) + ed->hscroll;
        if (li < 0) li = 0;
        if (li >= ed->nlines) li = ed->nlines - 1;
        ed->cy = li;
        {
            int len = ed_line_len(ed, li);
            int best = 0, bestd = 99999, i;
            for (i = 0; i <= len; i++) {
                int wpx = gfx_text_w_n(ed->lines[li], i, 0);
                int d = k_abs(wpx - col);
                if (d < bestd) { bestd = d; best = i; }
            }
            ed->cx = best;
        }
    }
}

const app_desc_t app_edit = {
    "edit", "Code Studio", "text editor", ICON_CODE,
    860, 600, 520, 340, APP_CAT_DEV, 1, 0,
    edit_create, edit_destroy, edit_paint, edit_key, edit_mouse, edit_tick, NULL
};
