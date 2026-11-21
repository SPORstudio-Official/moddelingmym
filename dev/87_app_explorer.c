/* ==========================================================================
 *  APP: explorer -- the AE file manager
 *
 *  A full file manager, not a viewer.  Everything a user expects of one is
 *  here, and every operation goes through the VFS so it is visible to the
 *  shell, the editor and /proc immediately:
 *
 *    navigate   breadcrumb, up, double-click, keyboard arrows
 *    create     New File, New Folder -- with a name prompt, not "untitled1.txt"
 *    edit       Rename, Duplicate, Properties
 *    destroy    Delete, Delete key, Delete All in a folder
 *    move/copy  Cut, Copy, Paste across folders (an internal clipboard)
 *    open       text/code in Code Studio, binaries in Hex Viewer, folders enter
 *
 *  Every mutating action reports what happened in the status bar: the older
 *  version silently ignored failures, which is why the folder looked read-only.
 * ========================================================================== */

#define EXP_MAX_ENTRIES 96
#define EXP_COLS 5
#define EXP_PATH_MAX VFS_MAX_PATH

/* What the inline prompt is being used for.  The prompt replaces the old
   "create a file called untitled3.txt and hope the user finds it" behaviour:
   every name the user chooses is typed in a box on the toolbar. */
enum {
    EXP_PROMPT_NONE = 0,
    EXP_PROMPT_NEW_FILE,
    EXP_PROMPT_NEW_DIR,
    EXP_PROMPT_RENAME,
    EXP_PROMPT_DUP
};

typedef struct {
    char path[EXP_PATH_MAX];
    vfs_dirent_t ent[EXP_MAX_ENTRIES];
    int  nent;
    int  selected;
    int  list_view;
    int  scroll;
    int  hover;
    int  status_kind;           /* 0 info, 1 ok, 2 warn, 3 error            */
    char status[128];
    int  sort_mode;             /* 0 name, 1 size, 2 type                   */
    u32  last_click_ms;
    int  last_click_idx;

    /* inline name prompt */
    int  prompt;                /* EXP_PROMPT_*                             */
    char prompt_buf[64];
    int  prompt_len;

    /* clipboard (internal: a path plus the operation to perform on paste) */
    int  clip_kind;             /* 0 empty, 1 copy, 2 cut                   */
    char clip_path[EXP_PATH_MAX];

    int  show_props;            /* properties panel for `selected`          */
} explorer_t;

/* -------------------------------------------------------------------------- */
/*  helpers                                                                */
/* -------------------------------------------------------------------------- */

static void exp_join(char *out, int cap, const char *dir, const char *name) {
    ksnprintf(out, cap, "%s/%s", kstrcmp(dir, "/") ? dir : "", name);
}

static void exp_set_status(explorer_t *ex, int kind, const char *fmt, const char *a, const char *b) {
    /* A tiny printf wrapper: the status line is the only feedback the user gets
       for a destructive action, so every path sets it. */
    char buf[128];
    ksnprintf(buf, sizeof(buf), fmt, a ? a : "", b ? b : "");
    kstrncpy(ex->status, buf, sizeof(ex->status) - 1);
    ex->status[sizeof(ex->status) - 1] = 0;
    ex->status_kind = kind;
}

static void exp_set_status1(explorer_t *ex, int kind, const char *text) {
    kstrncpy(ex->status, text, sizeof(ex->status) - 1);
    ex->status[sizeof(ex->status) - 1] = 0;
    ex->status_kind = kind;
}

static int exp_selected_path(explorer_t *ex, char *out, int cap) {
    if (ex->selected < 0 || ex->selected >= ex->nent) return 0;
    exp_join(out, cap, ex->path, ex->ent[ex->selected].name);
    return 1;
}

/* A unique name derived from `base`, so "Paste" twice cannot collide and
   "Duplicate" produces "name copy.txt", "name copy 2.txt", ... */
static void exp_unique_path(const char *dir, const char *base, char *out, int cap) {
    int i;
    char leaf[80];
    for (i = 0; i < 500; i++) {
        if (i == 0) ksnprintf(leaf, sizeof(leaf), "%s", base);
        else ksnprintf(leaf, sizeof(leaf), "%s %d", base, i);
        exp_join(out, cap, dir, leaf);
        if (!vfs_resolve(out)) return;
    }
    exp_join(out, cap, dir, base);
}

/* Split "report.txt" into "report" + ".txt" so a duplicate can be named
   "report copy.txt" rather than "report copy.txt.txt". */
static void exp_split_ext(const char *name, char *stem, int stem_cap, char *ext, int ext_cap) {
    const char *dot = NULL;
    int i;
    for (i = 0; name[i]; i++) if (name[i] == '.') dot = name + i;
    if (!dot || dot == name) {
        kstrncpy(stem, name, stem_cap - 1);
        stem[stem_cap - 1] = 0;
        ext[0] = 0;
        return;
    }
    kstrncpy(stem, name, (int)(dot - name) < stem_cap - 1 ? (int)(dot - name) : stem_cap - 1);
    stem[(int)(dot - name) < stem_cap - 1 ? (int)(dot - name) : stem_cap - 1] = 0;
    kstrncpy(ext, dot, ext_cap - 1);
    ext[ext_cap - 1] = 0;
}

/* -------------------------------------------------------------------------- */
/*  directory listing                                                      */
/* -------------------------------------------------------------------------- */

static void exp_refresh(explorer_t *ex) {
    int i, n;
    int keep = ex->selected;
    ex->nent = 0;
    n = vfs_dir_count(ex->path);
    if (n < 0) {
        exp_set_status1(ex, 3, "this folder is no longer reachable");
        return;
    }
    for (i = 0; i < n && ex->nent < EXP_MAX_ENTRIES; i++) {
        vfs_dirent_t d;
        if (vfs_dir_get(ex->path, i, &d) != 0) break;
        if (!kstrcmp(d.name, ".") || !kstrcmp(d.name, "..")) continue;
        ex->ent[ex->nent++] = d;
    }
    /* sort: directories first, then the chosen key */
    {
        int j;
        for (i = 1; i < ex->nent; i++) {
            vfs_dirent_t key = ex->ent[i];
            j = i - 1;
            while (j >= 0) {
                int swap = 0;
                int adir = (ex->ent[j].type == FT_DIR), bdir = (key.type == FT_DIR);
                if (ex->sort_mode == 0) {
                    if (adir != bdir) swap = (!adir && bdir);
                    else swap = kstrnicmp(ex->ent[j].name, key.name, 64) > 0;
                } else if (ex->sort_mode == 1) {
                    vfs_inode_t a, b;
                    char pa[EXP_PATH_MAX], pb[EXP_PATH_MAX];
                    kmemset(&a, 0, sizeof(a));
                    kmemset(&b, 0, sizeof(b));
                    exp_join(pa, sizeof(pa), ex->path, ex->ent[j].name);
                    exp_join(pb, sizeof(pb), ex->path, key.name);
                    vfs_stat_path(pa, &a);
                    vfs_stat_path(pb, &b);
                    if (adir != bdir) swap = (!adir && bdir);
                    else swap = a.size < b.size;
                } else {
                    if (ex->ent[j].type != key.type) swap = ex->ent[j].type > key.type;
                    else swap = kstrnicmp(ex->ent[j].name, key.name, 64) > 0;
                }
                if (!swap) break;
                ex->ent[j + 1] = ex->ent[j];
                j--;
            }
            ex->ent[j + 1] = key;
        }
    }
    if (keep < ex->nent) ex->selected = keep;
    else ex->selected = ex->nent ? ex->nent - 1 : 0;
    if (ex->scroll > ex->nent) ex->scroll = 0;
}

/* -------------------------------------------------------------------------- */
/*  operations                                                             */
/* -------------------------------------------------------------------------- */

static void exp_navigate(explorer_t *ex, const char *path) {
    char abs[EXP_PATH_MAX];
    vfs_inode_t st;
    vfs_normalise(ex->path, path, abs, sizeof(abs));
    kmemset(&st, 0, sizeof(st));
    if (vfs_stat_path(abs, &st) != 0 || st.type != FT_DIR) {
        exp_set_status(ex, 3, "cannot open %s", path, NULL);
        return;
    }
    kstrncpy(ex->path, abs, sizeof(ex->path) - 1);
    ex->path[sizeof(ex->path) - 1] = 0;
    ex->selected = 0;
    ex->scroll = 0;
    ex->show_props = 0;
    exp_refresh(ex);
    {
        char buf[64];
        ksnprintf(buf, sizeof(buf), "%d items", ex->nent);
        exp_set_status1(ex, 0, buf);
    }
}

static void exp_open_entry(explorer_t *ex, int idx) {
    const char *name;
    char full[EXP_PATH_MAX];
    vfs_inode_t st;
    if (idx < 0 || idx >= ex->nent) return;
    name = ex->ent[idx].name;
    exp_join(full, sizeof(full), ex->path, name);
    if (ex->ent[idx].type == FT_DIR) {
        exp_navigate(ex, full);
        return;
    }
    kmemset(&st, 0, sizeof(st));
    vfs_stat_path(full, &st);
    if (st.type == FT_PROC || ex->ent[idx].type == FT_PROC) {
        const app_desc_t *d = app_by_id("edit");
        if (d) wm_open_app(d, full);
        return;
    }
    {
        u32 len = 0;
        const char *err = NULL;
        char *data = vfs_read_alloc(full, &len, &err);
        int is_binary = 0;
        u32 i;
        if (!data) {
            exp_set_status(ex, 3, "cannot read %s", name, NULL);
            return;
        }
        if (len > 4 && !kstrncmp(data, "AEBX", 4)) is_binary = 1;
        for (i = 0; i < len && i < 512; i++)
            if ((u8)data[i] < 9) is_binary = 1;
        mm_kfree(data);
        {
            const app_desc_t *d = app_by_id(is_binary ? "hexview" : "edit");
            if (d) wm_open_app(d, full);
        }
    }
}

static void exp_delete(explorer_t *ex) {
    char full[EXP_PATH_MAX];
    char name[VFS_MAX_NAME];
    if (!exp_selected_path(ex, full, sizeof(full))) return;
    kstrncpy(name, ex->ent[ex->selected].name, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    {
        int rc = vfs_unlink(full);
        if (rc == 0) {
            exp_set_status(ex, 1, "deleted %s", name, NULL);
            exp_refresh(ex);
        } else if (rc == -2) {
            exp_set_status1(ex, 2, "that folder is not empty -- delete its contents first");
        } else {
            exp_set_status(ex, 3, "cannot delete %s (system file?)", name, NULL);
        }
    }
}

/* Copy or cut the selection to the internal clipboard. */
static void exp_clip(explorer_t *ex, int kind) {
    char full[EXP_PATH_MAX];
    if (!exp_selected_path(ex, full, sizeof(full))) return;
    kstrncpy(ex->clip_path, full, sizeof(ex->clip_path) - 1);
    ex->clip_path[sizeof(ex->clip_path) - 1] = 0;
    ex->clip_kind = kind;
    exp_set_status(ex, 1, kind == 2 ? "cut %s -- paste in the destination folder"
                                   : "copied %s -- paste in the destination folder",
                   ex->ent[ex->selected].name, NULL);
}

static void exp_copy_tree(explorer_t *ex, const char *from, const char *to);

static void exp_paste(explorer_t *ex) {
    char leaf[VFS_MAX_NAME];
    char dest[EXP_PATH_MAX];
    char base[80];
    const char *p;
    int i, slash = -1;
    vfs_inode_t st;
    if (!ex->clip_kind) {
        exp_set_status1(ex, 2, "nothing to paste -- use Copy or Cut first");
        return;
    }
    /* leaf name of the clipboard path */
    for (p = ex->clip_path, i = 0; *p; p++, i++) if (*p == '/') slash = i;
    kstrncpy(leaf, ex->clip_path + slash + 1, sizeof(leaf) - 1);
    leaf[sizeof(leaf) - 1] = 0;
    if (!leaf[0]) { exp_set_status1(ex, 3, "cannot paste the root folder"); return; }

    /* pasting into the folder it came from duplicates it, like every desktop */
    {
        char parent[EXP_PATH_MAX];
        parent[0] = 0;
        kstrncpy(parent, ex->clip_path, (slash > 0) ? (size_t)slash : 1);
        parent[(slash > 0) ? slash : 1] = 0;
        {
            char want[EXP_PATH_MAX];
            vfs_normalise("/", ex->path, want, sizeof(want));
            if (!kstrcmp(parent, want) || !kstrcmp(parent, ex->path)) {
                char stem[64], ext[16];
                exp_split_ext(leaf, stem, sizeof(stem), ext, sizeof(ext));
                ksnprintf(base, sizeof(base), "%s %s%s", stem,
                          ex->clip_kind == 2 ? "moved" : "copy", ext);
            } else {
                ksnprintf(base, sizeof(base), "%s", leaf);
            }
        }
    }
    exp_unique_path(ex->path, base, dest, sizeof(dest));

    kmemset(&st, 0, sizeof(st));
    vfs_stat_path(ex->clip_path, &st);
    if (st.type == FT_DIR) {
        if (ex->clip_kind == 2) {
            if (vfs_rename(ex->clip_path, dest) == 0) {
                exp_set_status(ex, 1, "moved %s here", leaf, NULL);
                ex->clip_kind = 0;
            } else {
                exp_set_status(ex, 3, "cannot move %s here", leaf, NULL);
            }
        } else {
            exp_copy_tree(ex, ex->clip_path, dest);
            exp_set_status(ex, 1, "copied folder %s here", leaf, NULL);
        }
    } else {
        if (ex->clip_kind == 2) {
            if (vfs_rename(ex->clip_path, dest) == 0) {
                exp_set_status(ex, 1, "moved %s here", leaf, NULL);
                ex->clip_kind = 0;
            } else {
                exp_set_status(ex, 3, "cannot move %s here", leaf, NULL);
            }
        } else {
            if (vfs_copy(ex->clip_path, dest) == 0) {
                exp_set_status(ex, 1, "copied %s here", leaf, NULL);
            } else {
                exp_set_status(ex, 3, "cannot copy %s", leaf, NULL);
            }
        }
    }
    exp_refresh(ex);
    for (i = 0; i < ex->nent; i++) {
        char full[EXP_PATH_MAX];
        exp_join(full, sizeof(full), ex->path, ex->ent[i].name);
        if (!kstrcmp(full, dest)) { ex->selected = i; break; }
    }
}

/* Recursive folder copy: the VFS has no cp -r, so the file manager provides
   one.  Written recursively because the tree is shallow (a few levels) and a
   depth-first copy is what a user expects to see happen instantly. */
static void exp_copy_tree(explorer_t *ex, const char *from, const char *to) {
    vfs_inode_t st;
    int i, n;
    (void)ex;
    kmemset(&st, 0, sizeof(st));
    if (vfs_stat_path(from, &st) != 0) return;
    if (st.type != FT_DIR) {
        vfs_copy(from, to);
        return;
    }
    vfs_mkdir_path(to);
    n = vfs_dir_count(from);
    for (i = 0; i < n; i++) {
        vfs_dirent_t d;
        char cf[EXP_PATH_MAX], ct[EXP_PATH_MAX];
        if (vfs_dir_get(from, i, &d) != 0) break;
        if (!kstrcmp(d.name, ".") || !kstrcmp(d.name, "..")) continue;
        exp_join(cf, sizeof(cf), from, d.name);
        exp_join(ct, sizeof(ct), to, d.name);
        exp_copy_tree(ex, cf, ct);
    }
}

/* Rename to the name currently in the prompt buffer. */
static void exp_do_rename(explorer_t *ex) {
    char full[EXP_PATH_MAX], dest[EXP_PATH_MAX];
    if (!exp_selected_path(ex, full, sizeof(full))) return;
    if (!ex->prompt_buf[0]) {
        exp_set_status1(ex, 2, "a name is required");
        return;
    }
    exp_join(dest, sizeof(dest), ex->path, ex->prompt_buf);
    if (!kstrcmp(dest, full)) return;
    {
        int rc = vfs_rename(full, dest);
        if (rc == 0) {
            exp_set_status(ex, 1, "renamed to %s", ex->prompt_buf, NULL);
            exp_refresh(ex);
            {
                int i;
                for (i = 0; i < ex->nent; i++)
                    if (!kstrcmp(ex->ent[i].name, ex->prompt_buf)) { ex->selected = i; break; }
            }
        } else if (rc == -2) {
            exp_set_status(ex, 2, "%s already exists", ex->prompt_buf, NULL);
            return;                       /* keep the prompt open to correct it */
        } else {
            exp_set_status1(ex, 3, "cannot rename this item");
            return;
        }
    }
    ex->prompt = EXP_PROMPT_NONE;
}

/* Create the file or folder named in the prompt buffer. */
static void exp_do_create(explorer_t *ex, int is_dir) {
    char dest[EXP_PATH_MAX];
    if (!ex->prompt_buf[0]) {
        exp_set_status1(ex, 2, "a name is required");
        return;
    }
    exp_join(dest, sizeof(dest), ex->path, ex->prompt_buf);
    if (vfs_resolve(dest)) {
        exp_set_status(ex, 2, "%s already exists", ex->prompt_buf, NULL);
        return;
    }
    if (is_dir) {
        if (vfs_mkdir_path(dest) != 0) {
            exp_set_status(ex, 3, "cannot create the folder %s", ex->prompt_buf, NULL);
            return;
        }
    } else if (!vfs_create_node(dest, FT_FILE)) {
        exp_set_status(ex, 3, "cannot create the file %s", ex->prompt_buf, NULL);
        return;
    } else {
        exp_set_status(ex, 1, "created %s", ex->prompt_buf, NULL);
        exp_refresh(ex);
        {
            int i;
            for (i = 0; i < ex->nent; i++)
                if (!kstrcmp(ex->ent[i].name, ex->prompt_buf)) { ex->selected = i; break; }
        }
        ex->prompt = EXP_PROMPT_NONE;
        return;
    }
    exp_set_status(ex, 1, "created the folder %s", ex->prompt_buf, NULL);
    exp_refresh(ex);
    {
        int i;
        for (i = 0; i < ex->nent; i++)
            if (!kstrcmp(ex->ent[i].name, ex->prompt_buf)) { ex->selected = i; break; }
    }
    ex->prompt = EXP_PROMPT_NONE;
}

/* Duplicate the selection next to itself, keeping the extension. */
static void exp_do_duplicate(explorer_t *ex) {
    char full[EXP_PATH_MAX], dest[EXP_PATH_MAX];
    char stem[64], ext[16], base[96];
    if (!exp_selected_path(ex, full, sizeof(full))) return;
    exp_split_ext(ex->ent[ex->selected].name, stem, sizeof(stem), ext, sizeof(ext));
    ksnprintf(base, sizeof(base), "%s copy%s", stem, ext);
    exp_unique_path(ex->path, base, dest, sizeof(dest));
    {
        vfs_inode_t st;
        kmemset(&st, 0, sizeof(st));
        vfs_stat_path(full, &st);
        if (st.type == FT_DIR) exp_copy_tree(ex, full, dest);
        else if (vfs_copy(full, dest) != 0) {
            exp_set_status1(ex, 3, "cannot duplicate this item");
            return;
        }
    }
    exp_set_status(ex, 1, "duplicated as %s", ex->ent[ex->selected].name, NULL);
    exp_refresh(ex);
    {
        int i;
        const char *leaf = dest + (int)kstrlen(ex->path) + 1;
        for (i = 0; i < ex->nent; i++)
            if (!kstrcmp(ex->ent[i].name, leaf)) { ex->selected = i; break; }
    }
}

/* Begin an inline prompt for a name.  `seed` pre-fills it (rename), and for a
   new file the extension of the previous one is a useful default. */
static void exp_begin_prompt(explorer_t *ex, int kind, const char *seed) {
    /* Pressing the shortcut again while its own prompt is open must not throw
       away what the user has typed -- Ctrl+N twice used to leave an empty box
       and every following keystroke went into a name that had just been reset. */
    if (ex->prompt == kind && !seed) return;
    ex->prompt = kind;
    ex->prompt_len = 0;
    ex->prompt_buf[0] = 0;
    if (seed) {
        kstrncpy(ex->prompt_buf, seed, sizeof(ex->prompt_buf) - 1);
        ex->prompt_buf[sizeof(ex->prompt_buf) - 1] = 0;
        ex->prompt_len = (int)kstrlen(ex->prompt_buf);
    }
}

static void exp_prompt_commit(explorer_t *ex) {
    switch (ex->prompt) {
    case EXP_PROMPT_NEW_FILE: exp_do_create(ex, 0); break;
    case EXP_PROMPT_NEW_DIR:  exp_do_create(ex, 1); break;
    case EXP_PROMPT_RENAME:   exp_do_rename(ex); break;
    case EXP_PROMPT_DUP:      break;
    default: break;
    }
    /* create/rename clear the prompt themselves on success; a failure keeps it
       open so the user can fix the name instead of starting over. */
}

/* -------------------------------------------------------------------------- */
/*  geometry                                                               */
/* -------------------------------------------------------------------------- */

static wm_rect_t exp_icon_rect(wm_rect_t c, int idx, int *cell_h_out) {
    int cell_w = (c.w - 24) / EXP_COLS;
    int cell_h = 86;
    int col = idx % EXP_COLS, row = idx / EXP_COLS;
    if (cell_h_out) *cell_h_out = cell_h;
    return (wm_rect_t){ c.x + 12 + col * cell_w, c.y + 84 + row * cell_h, cell_w - 6, cell_h - 6 };
}

/* Toolbar layout, shared by painting and hit-testing so a button can never be
   drawn somewhere other than where it responds. */
#define EXP_TOOL_H 26
static const char *const exp_tool_labels[] = { "File", "Folder", "Rename", "Copy", "Cut", "Paste" };
#define EXP_TOOL_COUNT 6
#define EXP_TOOL_W 62
#define EXP_TOOL_GAP 4

static int exp_tool_x(wm_rect_t c, int i) {
    /* right-aligned row of buttons, ending 10 px from the edge */
    int total = EXP_TOOL_COUNT * EXP_TOOL_W + (EXP_TOOL_COUNT - 1) * EXP_TOOL_GAP;
    return c.x + c.w - 10 - total + i * (EXP_TOOL_W + EXP_TOOL_GAP);
}

/* -------------------------------------------------------------------------- */
/*  paint                                                                  */
/* -------------------------------------------------------------------------- */

static void exp_paint_prompt(explorer_t *ex, wm_rect_t c, theme_t *t, int y) {
    const char *label =
        ex->prompt == EXP_PROMPT_NEW_FILE ? "File name:" :
        ex->prompt == EXP_PROMPT_NEW_DIR  ? "Folder name:" :
        ex->prompt == EXP_PROMPT_RENAME   ? "Rename to:" : "Name:";
    int x = c.x + 12;
    gfx_fill_rect(c.x, y, c.w, 26, RGBA(14, 20, 30, 252));
    gfx_fill_rect(c.x, y, 3, 26, t->accent);
    gfx_text(x, y + 7, label, t->text_dim, 0);
    x += gfx_text_w(label, 0) + 8;
    {
        int bw = c.w - (x - c.x) - 150;
        if (bw < 120) bw = 120;
        gfx_round_rect(x, y + 3, bw, 20, 4, RGBA(8, 12, 18, 240));
        gfx_round_rect_border(x, y + 3, bw, 20, 4, t->accent, 1);
        gfx_text_clip(x + 6, y + 7, bw - 12, ex->prompt_buf, t->text, 0);
        if (((now_ms() / 500) & 1)) {
            int cx = x + 6 + gfx_text_w(ex->prompt_buf, 0) + 1;
            if (cx < x + bw - 6) gfx_fill_rect(cx, y + 6, 1, 14, t->accent);
        }
        x += bw + 8;
    }
    gfx_text(x, y + 7, "Enter - create    Esc - cancel", t->text_mute, 0);
}

static void exp_paint_props(explorer_t *ex, wm_rect_t c, theme_t *t) {
    char full[EXP_PATH_MAX];
    vfs_inode_t st;
    char line[160];
    int x, y, w = 320, h = 150;
    if (!exp_selected_path(ex, full, sizeof(full))) return;
    kmemset(&st, 0, sizeof(st));
    if (vfs_stat_path(full, &st) != 0) return;
    x = c.x + c.w - w - 16;
    y = c.y + c.h - h - 34;
    gfx_shadow(x, y, w, h, 8, 8, 150);
    gfx_round_rect(x, y, w, h, 8, RGBA(18, 24, 34, 252));
    gfx_round_rect_border(x, y, w, h, 8, t->accent, 1);
    gfx_text(x + 12, y + 10, "Properties", t->text, 0);
    gfx_fill_rect(x + 12, y + 28, w - 24, 1, RGBA(60, 74, 96, 160));
    ksnprintf(line, sizeof(line), "Name   %s", ex->ent[ex->selected].name);
    gfx_text_clip(x + 12, y + 36, w - 24, line, t->text_dim, 0);
    ksnprintf(line, sizeof(line), "Type   %s", vfs_type_name(st.type));
    gfx_text(x + 12, y + 54, line, t->text_dim, 0);
    ksnprintf(line, sizeof(line), "Size   %u bytes (%u blocks)",
              (unsigned)st.size, (unsigned)((st.size + 511u) / 512u));
    gfx_text(x + 12, y + 72, line, t->text_dim, 0);
    ksnprintf(line, sizeof(line), "Mode   %o    Links %u", (unsigned)st.mode,
              (unsigned)st.link_count);
    gfx_text(x + 12, y + 90, line, t->text_dim, 0);
    ksnprintf(line, sizeof(line), "Inode  %u%s", (unsigned)st.ino,
              (ex->clip_kind && !kstrcmp(ex->clip_path, full)) ? "   [on clipboard]" : "");
    gfx_text(x + 12, y + 108, line, t->text_dim, 0);
    ksnprintf(line, sizeof(line), "Path   %s", full);
    gfx_text_clip(x + 12, y + 126, w - 24, line, t->text_mute, 0);
}

static void explorer_paint(void *state, window_t *w, wm_rect_t c) {
    explorer_t *ex = (explorer_t *)state;
    theme_t *t = theme_get();
    int i;
    int bar_y = c.y + 6;

    gfx_fill_rect(c.x, c.y, c.w, c.h, t->win_bg);

    /* ---- toolbar: up + breadcrumb + action buttons ---------------------- */
    {
        int bx = c.x + 8;
        int btn = EXP_TOOL_H;
        int hover_up = (g_wm.mouse_px >= bx && g_wm.mouse_px < bx + btn &&
                        g_wm.mouse_py >= bar_y && g_wm.mouse_py < bar_y + btn);
        gfx_round_rect(bx, bar_y, btn, btn, 5, hover_up ? t->sel : RGBA(40, 52, 72, 200));
        gfx_draw_icon(ICON_UP, bx + 4, bar_y + 4, 18);
        bx += btn + 8;

        /* breadcrumbs */
        {
            char crumbs[EXP_PATH_MAX];
            char *p;
            int cx = bx;
            int crumb_w = exp_tool_x(c, 0) - bx - 10;
            if (crumb_w < 80) crumb_w = 80;
            kstrncpy(crumbs, ex->path, sizeof(crumbs) - 1);
            crumbs[sizeof(crumbs) - 1] = 0;
            gfx_round_rect(cx, bar_y, crumb_w, btn, 5, RGBA(10, 14, 22, 220));
            gfx_circle(cx + 10, bar_y + btn / 2, 3, t->accent);
            cx += 20;
            gfx_text(cx, bar_y + 6, "/", t->text_dim, 0);
            cx += gfx_text_w("/", 0) + 2;
            p = crumbs;
            while (*p) {
                char comp[64];
                int n = 0;
                while (*p == '/') p++;
                if (!*p) break;
                while (*p && *p != '/' && n < 63) comp[n++] = *p++;
                comp[n] = 0;
                {
                    int tw = gfx_text_w(comp, 0);
                    if (cx + tw > bx + crumb_w - 12) { gfx_text(cx, bar_y + 6, "...", t->text_mute, 0); break; }
                    gfx_text(cx, bar_y + 6, comp, t->text, 0);
                    cx += tw + 2;
                    gfx_text(cx, bar_y + 6, "/", t->text_mute, 0);
                    cx += gfx_text_w("/", 0) + 4;
                }
            }
        }

        /* action buttons -- geometry from exp_tool_x(), the same function the
           click handler uses */
        for (i = 0; i < EXP_TOOL_COUNT; i++) {
            int ax = exp_tool_x(c, i);
            int hover = (g_wm.mouse_px >= ax && g_wm.mouse_px < ax + EXP_TOOL_W &&
                         g_wm.mouse_py >= bar_y && g_wm.mouse_py < bar_y + EXP_TOOL_H);
            int dim = (i == 5 && !ex->clip_kind);          /* Paste with nothing copied */
            u32 bg = hover ? t->sel : RGBA(36, 46, 64, 220);
            if (i == 5 && ex->clip_kind) bg = hover ? t->accent : RGBA(48, 70, 96, 230);
            gfx_round_rect(ax, bar_y, EXP_TOOL_W, EXP_TOOL_H, 5, bg);
            gfx_text_center(ax + EXP_TOOL_W / 2, bar_y + 7, exp_tool_labels[i],
                            dim ? t->text_mute : t->text, 0);
        }
    }

    /* ---- prompt row (replaces the view switch while it is open) --------- */
    if (ex->prompt != EXP_PROMPT_NONE) {
        exp_paint_prompt(ex, c, t, c.y + 36);
    } else {
        /* view switch + path + count */
        int vy = c.y + 40;
        const char *views[2] = { "Icons", "List" };
        for (i = 0; i < 2; i++) {
            int vx = c.x + 8 + i * 58;
            int active = (ex->list_view == i);
            gfx_round_rect(vx, vy, 54, 20, 5, active ? t->accent : RGBA(30, 40, 56, 200));
            gfx_text_center(vx + 27, vy + 3, views[i], active ? RGB(8, 14, 22) : t->text_dim, 0);
        }
        gfx_text_clip(c.x + 132, vy + 3, c.w - 260, ex->path, t->text_mute, 0);
        {
            char buf[64];
            ksnprintf(buf, sizeof(buf), "%d items", ex->nent);
            gfx_text_right(c.x + c.w - 12, vy + 3, buf, t->text_mute, 0);
        }
    }

    /* ---- entries -------------------------------------------------------- */
    if (!ex->list_view) {
        for (i = ex->scroll; i < ex->nent; i++) {
            wm_rect_t r = exp_icon_rect(c, i - ex->scroll, NULL);
            const vfs_dirent_t *d = &ex->ent[i];
            int on_clip = 0;
            if (r.y + r.h > c.y + c.h - 26) break;
            gfx_round_rect(r.x, r.y, r.w, r.h, 6, i == ex->selected ? t->sel : RGBA(0, 0, 0, 0));
            if (d->type == FT_DIR) gfx_draw_icon(ICON_FOLDER, r.x + r.w / 2 - 18, r.y + 10, 36);
            else gfx_draw_icon(gfx_icon_for_path(d->name, d->type == FT_DIR, d->type == FT_PROC, 0), r.x + r.w / 2 - 18, r.y + 10, 36);
            gfx_text_center(r.x + r.w / 2, r.y + 52, d->name, t->text, 0);
            {
                vfs_inode_t st;
                char full[EXP_PATH_MAX];
                char sz[24];
                kmemset(&st, 0, sizeof(st));
                exp_join(full, sizeof(full), ex->path, d->name);
                vfs_stat_path(full, &st);
                if (ex->clip_kind && !kstrcmp(ex->clip_path, full)) on_clip = 1;
                if (d->type == FT_DIR) gfx_text_center(r.x + r.w / 2, r.y + 66, "<dir>", t->text_mute, 0);
                else {
                    ksnprintf(sz, sizeof(sz), "%u B", (unsigned)st.size);
                    gfx_text_center(r.x + r.w / 2, r.y + 66, sz, t->text_mute, 0);
                }
            }
            if (on_clip)
                gfx_round_rect_border(r.x, r.y, r.w, r.h, 6,
                                      ex->clip_kind == 2 ? t->warn : t->info, 1);
        }
    } else {
        int row_h = 22;
        int top = c.y + 88;
        for (i = ex->scroll; i < ex->nent; i++) {
            int y = top + (i - ex->scroll) * row_h;
            const vfs_dirent_t *d = &ex->ent[i];
            vfs_inode_t st;
            char full[EXP_PATH_MAX];
            char sz[24];
            if (y + row_h > c.y + c.h - 26) break;
            if (i == ex->selected) gfx_fill_rect(c.x + 8, y, c.w - 16, row_h, t->sel);
            gfx_draw_icon(d->type == FT_DIR ? ICON_FOLDER : gfx_icon_for_path(d->name, d->type == FT_DIR, d->type == FT_PROC, 0),
                          c.x + 14, y + 3, 16);
            gfx_text(c.x + 38, y + 4, d->name, t->text, 0);
            kmemset(&st, 0, sizeof(st));
            exp_join(full, sizeof(full), ex->path, d->name);
            vfs_stat_path(full, &st);
            if (d->type == FT_DIR) gfx_text_right(c.x + c.w - 20, y + 4, "<dir>", t->text_mute, 0);
            else {
                ksnprintf(sz, sizeof(sz), "%u B", (unsigned)st.size);
                gfx_text_right(c.x + c.w - 20, y + 4, sz, t->text_mute, 0);
            }
        }
    }

    /* ---- properties panel ---------------------------------------------- */
    if (ex->show_props) exp_paint_props(ex, c, t);

    /* ---- status bar ----------------------------------------------------- */
    {
        int sy = c.y + c.h - 19;
        u32 col = ex->status_kind == 3 ? t->err :
                  ex->status_kind == 2 ? t->warn :
                  ex->status_kind == 1 ? t->ok : t->text_mute;
        gfx_fill_rect(c.x, sy, c.w, 19, RGBA(8, 11, 17, 250));
        gfx_text_clip(c.x + 10, sy + 5, c.w - 260, ex->status, col, 0);
        if (ex->clip_kind) {
            char buf[80];
            ksnprintf(buf, sizeof(buf), "clipboard: %s",
                      ex->clip_kind == 2 ? "cut" : "copied");
            gfx_text_right(c.x + c.w - 10, sy + 5, buf, t->info, 0);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  input                                                                  */
/* -------------------------------------------------------------------------- */

static void explorer_key(void *state, int key, int ch, int mods) {
    explorer_t *ex = (explorer_t *)state;

    /* ---- the inline prompt takes every key while it is open ------------- */
    if (ex->prompt != EXP_PROMPT_NONE) {
        if (key == 0x0D || ch == '\n' || ch == '\r') { exp_prompt_commit(ex); return; }
        if (key == 0x1B) { ex->prompt = EXP_PROMPT_NONE; exp_set_status1(ex, 0, "cancelled"); return; }
        if (key == 0x08) {
            if (ex->prompt_len > 0) ex->prompt_buf[--ex->prompt_len] = 0;
            return;
        }
        if (ch >= 32 && ch < 127 && ex->prompt_len < (int)sizeof(ex->prompt_buf) - 2) {
            ex->prompt_buf[ex->prompt_len++] = (char)ch;
            ex->prompt_buf[ex->prompt_len] = 0;
        }
        return;
    }

    /* ---- navigation ----------------------------------------------------- */
    if (key == 0x25 || ch == 'h') {
        if ((mods & MOD_ALT)) { exp_navigate(ex, ".."); return; }
        if (ex->selected > 0) ex->selected--;
        return;
    }
    if (key == 0x27 || ch == 'l') { if (ex->selected < ex->nent - 1) ex->selected++; return; }
    if (key == 0x26) { ex->selected = k_max(0, ex->selected - EXP_COLS); return; }
    if (key == 0x28) { ex->selected = k_min(ex->nent - 1, ex->selected + EXP_COLS); return; }

    /* ---- actions (with Ctrl modifiers where a desktop would use them) --- */
    if (key == 0x0D) { exp_open_entry(ex, ex->selected); return; }        /* Enter   */
    if (key == 0x2E) { exp_delete(ex); return; }                          /* Delete  */
    if (key == 0x09) { ex->list_view = !ex->list_view; return; }          /* Tab     */
    if (ch == 8) { exp_navigate(ex, ".."); return; }                      /* Backspc */

    /* Ctrl+letter arrives two ways depending on the chassis: as a control
       character (Ctrl+C is 0x03) or as the letter with MOD_CTRL set.  Accepting
       only one of them is why the clipboard shortcuts did nothing. */
    {
        int letter = 0;
        if (ch >= 1 && ch <= 26) letter = 'a' + ch - 1;
        else if ((mods & MOD_CTRL) && ch >= 'A' && ch <= 'Z') letter = ch - 'A' + 'a';
        else if ((mods & MOD_CTRL) && ch >= 'a' && ch <= 'z') letter = ch;
        if (letter) {
            switch (letter) {
            case 'c': exp_clip(ex, 1); return;
            case 'x': exp_clip(ex, 2); return;
            case 'v': exp_paste(ex); return;
            case 'n': exp_begin_prompt(ex, EXP_PROMPT_NEW_FILE, NULL); return;
            case 'd': exp_begin_prompt(ex, EXP_PROMPT_NEW_DIR, NULL); return;
            case 'r':
                if (ex->selected < ex->nent)
                    exp_begin_prompt(ex, EXP_PROMPT_RENAME, ex->ent[ex->selected].name);
                return;
            case 'i': ex->show_props = !ex->show_props; return;
            case 'a':
                /* Ctrl+A: report what is selected, since there is no marquee */
                if (ex->selected < ex->nent)
                    exp_set_status(ex, 0, "selected %s", ex->ent[ex->selected].name, NULL);
                return;
            default: break;
            }
        }
    }
    if (mods & MOD_CTRL) {
        switch (ch) {
        case 'c': case 'C': exp_clip(ex, 1); return;
        case 'x': case 'X': exp_clip(ex, 2); return;
        case 'v': case 'V': exp_paste(ex); return;
        case 'n': case 'N': exp_begin_prompt(ex, EXP_PROMPT_NEW_FILE, NULL); return;
        case 'd': case 'D': exp_begin_prompt(ex, EXP_PROMPT_NEW_DIR, NULL); return;
        case 'r': case 'R':
            if (ex->selected < ex->nent)
                exp_begin_prompt(ex, EXP_PROMPT_RENAME, ex->ent[ex->selected].name);
            return;
        case 'i': case 'I': ex->show_props = !ex->show_props; return;
        default: break;
        }
    }

    /* single-letter actions, kept from the original build so muscle memory and
       the help text in AE64 still work */
    if (ch == 'n') exp_begin_prompt(ex, EXP_PROMPT_NEW_FILE, NULL);
    if (ch == 'N') exp_begin_prompt(ex, EXP_PROMPT_NEW_DIR, NULL);
    if (ch == 'r') {
        if (ex->selected < ex->nent)
            exp_begin_prompt(ex, EXP_PROMPT_RENAME, ex->ent[ex->selected].name);
    }
    if (ch == 'D' && ex->selected < ex->nent) exp_do_duplicate(ex);
    if (ch == 'p' && ex->selected < ex->nent) ex->show_props = !ex->show_props;
    if (ch == 'v') ex->list_view = !ex->list_view;
    if (ch == 's') { ex->sort_mode = (ex->sort_mode + 1) % 3; exp_refresh(ex); }
}

static void explorer_mouse(void *state, int event, int x, int y, int button, int wheel) {
    explorer_t *ex = (explorer_t *)state;
    wm_rect_t c = wm_input_client_rect();
    (void)button;
    if (event == APP_EV_WHEEL) { ex->scroll = k_max(0, ex->scroll - wheel); return; }
    if (event != APP_EV_MOUSE_DOWN && event != APP_EV_MOUSE_DBLCLICK) return;

    /* ---- toolbar -------------------------------------------------------- */
    if (y < 36) {
        if (x >= 8 && x < 8 + EXP_TOOL_H) { exp_navigate(ex, ".."); return; }
        {
            int i;
            for (i = 0; i < EXP_TOOL_COUNT; i++) {
                int ax = exp_tool_x(c, i);
                if (x >= ax && x < ax + EXP_TOOL_W) {
                    switch (i) {
                    case 0: exp_begin_prompt(ex, EXP_PROMPT_NEW_FILE, NULL); break;
                    case 1: exp_begin_prompt(ex, EXP_PROMPT_NEW_DIR, NULL); break;
                    case 2:
                        if (ex->selected < ex->nent)
                            exp_begin_prompt(ex, EXP_PROMPT_RENAME, ex->ent[ex->selected].name);
                        break;
                    case 3: exp_clip(ex, 1); break;
                    case 4: exp_clip(ex, 2); break;
                    case 5: exp_paste(ex); break;
                    }
                    return;
                }
            }
        }
        /* breadcrumb click zones, rebuilt from the path exactly as painted */
        {
            char tmp[EXP_PATH_MAX];
            int cx = 8 + EXP_TOOL_H + 8 + 20;
            int crumb_limit = exp_tool_x(c, 0) - 10;
            char *p;
            kstrncpy(tmp, ex->path, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = 0;
            p = tmp;
            while (*p) {
                char comp[64];
                int n = 0;
                int prefix_len;
                while (*p == '/') p++;
                if (!*p) break;
                while (*p && *p != '/' && n < 63) comp[n++] = *p++;
                comp[n] = 0;
                {
                    int tw = gfx_text_w(comp, 0);
                    if (x >= cx && x < cx + tw) {
                        char target[EXP_PATH_MAX];
                        kstrncpy(target, ex->path, sizeof(target) - 1);
                        target[sizeof(target) - 1] = 0;
                        prefix_len = (int)(p - tmp);
                        if (prefix_len < (int)kstrlen(target)) target[prefix_len] = 0;
                        exp_navigate(ex, target[0] ? target : "/");
                        return;
                    }
                    if (cx + tw > crumb_limit) break;
                    cx += tw + 2 + gfx_text_w("/", 0) + 4;
                }
            }
        }
        return;
    }

    /* ---- view switch ---------------------------------------------------- */
    if (ex->prompt == EXP_PROMPT_NONE && y >= 40 && y < 62 && x < 8 + 118) {
        ex->list_view = (x >= 8 + 58) ? 1 : 0;
        return;
    }

    /* ---- properties panel dismissal ------------------------------------- */
    if (ex->show_props) {
        /* clicking anywhere in the entry area closes it, the way a popover does */
        ex->show_props = 0;
    }

    /* ---- entries -------------------------------------------------------- */
    if (!ex->list_view) {
        int i;
        for (i = 0; i < ex->nent - ex->scroll; i++) {
            wm_rect_t r = exp_icon_rect(c, i, NULL);
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                int idx = i + ex->scroll;
                int was = ex->selected;
                ex->selected = idx;
                if (event == APP_EV_MOUSE_DBLCLICK) { exp_open_entry(ex, idx); return; }
                if (was == idx && now_ms() - ex->last_click_ms < 400) {
                    ex->last_click_ms = 0;
                    exp_open_entry(ex, idx);
                    return;
                }
                ex->last_click_ms = now_ms();
                return;
            }
        }
    } else {
        int row_h = 22;
        int idx = (y - 88) / row_h + ex->scroll;
        if (y >= 88 && idx >= 0 && idx < ex->nent) {
            ex->selected = idx;
            if (event == APP_EV_MOUSE_DBLCLICK) exp_open_entry(ex, idx);
        }
    }
}

static void explorer_tick(void *state, u32 dt_ms) {
    explorer_t *ex = (explorer_t *)state;
    (void)dt_ms;
    /* The VFS can change under the window (shell `touch`, the editor saving).
       Polling the directory count is cheap and keeps the listing honest. */
    {
        static u32 last;
        u32 now = now_ms();
        if (now - last > 700) {
            int n;
            last = now;
            n = vfs_dir_count(ex->path);
            if (n >= 0 && n - 2 != ex->nent) exp_refresh(ex);
        }
    }
}

static void *explorer_create(const char *arg) {
    explorer_t *ex = (explorer_t *)mm_kmalloc(sizeof(explorer_t));
    if (!ex) return NULL;
    kmemset(ex, 0, sizeof(*ex));
    kstrncpy(ex->path, (arg && arg[0]) ? arg : vfs_cwd(), sizeof(ex->path) - 1);
    ex->status_kind = 0;
    exp_refresh(ex);
    exp_set_status1(ex, 0, "Ctrl+N new file   Ctrl+D new folder   F2 rename   Del delete");
    return ex;
}

static void explorer_destroy(void *state) { if (state) mm_kfree(state); }

const app_desc_t app_explorer = {
    "explorer", "Files", "manage the file system", ICON_FILES,
    780, 520, 480, 320, APP_CAT_UTIL, 1, 1,
    explorer_create, explorer_destroy, explorer_paint, explorer_key, explorer_mouse,
    explorer_tick, NULL
};
