/* ==========================================================================
 *  APP: explorer -- VFS file manager
 *
 *  Breadcrumb path bar, up button, new file / new folder / delete actions,
 *  grid and list views with procedural per-type icons, double-click to enter
 *  folders, open text/code in Code Studio and launch executables.
 * ========================================================================== */

#define EXP_MAX_ENTRIES 96
#define EXP_COLS 5

typedef struct {
    char path[VFS_MAX_PATH];
    vfs_dirent_t ent[EXP_MAX_ENTRIES];
    int  nent;
    int  selected;
    int  list_view;
    int  scroll;
    int  hover;
    int  status_kind;
    char status[128];
    char rename_buf[64];
    int  renaming;
    u32  last_click_ms;
    int  last_click_idx;
    int  sort_mode;             /* 0 name, 1 size, 2 type               */
} explorer_t;

static void exp_refresh(explorer_t *ex) {
    int i, n;
    ex->nent = 0;
    n = vfs_dir_count(ex->path);
    if (n < 0) {
        ksnprintf(ex->status, sizeof(ex->status), "cannot open %s", ex->path);
        ex->status_kind = 2;
        return;
    }
    for (i = 0; i < n && ex->nent < EXP_MAX_ENTRIES; i++) {
        vfs_dirent_t d;
        if (vfs_dir_get(ex->path, i, &d) != 0) break;
        if (!kstrcmp(d.name, ".") || !kstrcmp(d.name, "..")) continue;
        ex->ent[ex->nent++] = d;
    }
    /* sort: directories first, then name */
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
                    char pa[VFS_MAX_PATH], pb[VFS_MAX_PATH];
                    ksnprintf(pa, sizeof(pa), "%s/%s", kstrcmp(ex->path, "/") ? ex->path : "", ex->ent[j].name);
                    ksnprintf(pb, sizeof(pb), "%s/%s", kstrcmp(ex->path, "/") ? ex->path : "", key.name);
                    kmemset(&a, 0, sizeof(a));
                    kmemset(&b, 0, sizeof(b));
                    vfs_stat_path(pa, &a);
                    vfs_stat_path(pb, &b);
                    if (adir != bdir) swap = (!adir && bdir);
                    else swap = a.size > b.size;
                } else {
                    if (adir != bdir) swap = (!adir && bdir);
                    else swap = ex->ent[j].type > key.type ||
                                (ex->ent[j].type == key.type && kstrnicmp(ex->ent[j].name, key.name, 64) > 0);
                }
                if (!swap) break;
                ex->ent[j + 1] = ex->ent[j];
                j--;
            }
            ex->ent[j + 1] = key;
        }
    }
    if (ex->selected >= ex->nent) ex->selected = ex->nent - 1;
    if (ex->selected < 0) ex->selected = 0;
}

static void *explorer_create(const char *arg) {
    explorer_t *ex = (explorer_t *)mm_kmalloc(sizeof(explorer_t));
    if (!ex) return NULL;
    kmemset(ex, 0, sizeof(*ex));
    if (arg && arg[0] == '/') kstrncpy(ex->path, arg, sizeof(ex->path) - 1);
    else kstrcpy(ex->path, vfs_cwd());
    if (vfs_dir_count(ex->path) < 0) kstrcpy(ex->path, "/home/user");
    exp_refresh(ex);
    return ex;
}

static void explorer_destroy(void *state) { if (state) mm_kfree(state); }

static void exp_navigate(explorer_t *ex, const char *path) {
    char abs[VFS_MAX_PATH];
    vfs_normalise(ex->path, path, abs, sizeof(abs));
    if (vfs_dir_count(abs) < 0) {
        ksnprintf(ex->status, sizeof(ex->status), "no such directory: %s", path);
        ex->status_kind = 2;
        return;
    }
    kstrncpy(ex->path, abs, sizeof(ex->path) - 1);
    ex->selected = 0;
    ex->scroll = 0;
    exp_refresh(ex);
    ksnprintf(ex->status, sizeof(ex->status), "%d entries", ex->nent);
    ex->status_kind = 0;
}

static void exp_open_entry(explorer_t *ex, int idx) {
    const char *name;
    char full[VFS_MAX_PATH];
    vfs_inode_t st;
    if (idx < 0 || idx >= ex->nent) return;
    name = ex->ent[idx].name;
    ksnprintf(full, sizeof(full), "%s/%s", kstrcmp(ex->path, "/") ? ex->path : "", name);
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
        if (!data) return;
        if (len > 4 && !kstrncmp(data, "AEBX", 4)) is_binary = 1;
        for (i = 0; i < len && i < 512; i++)
            if ((u8)data[i] < 9) is_binary = 1;
        mm_kfree(data);
        if (is_binary) {
            const app_desc_t *d = app_by_id("hexview");
            if (d) wm_open_app(d, full);
        } else {
            const app_desc_t *d = app_by_id("edit");
            if (d) wm_open_app(d, full);
        }
    }
}

static void exp_delete(explorer_t *ex) {
    char full[VFS_MAX_PATH];
    if (ex->selected < 0 || ex->selected >= ex->nent) return;
    ksnprintf(full, sizeof(full), "%s/%s", kstrcmp(ex->path, "/") ? ex->path : "",
              ex->ent[ex->selected].name);
    {
        int rc = vfs_unlink(full);
        if (rc == 0) {
            ksnprintf(ex->status, sizeof(ex->status), "deleted %s", ex->ent[ex->selected].name);
            ex->status_kind = 1;
            exp_refresh(ex);
        } else if (rc == -2) {
            ksnprintf(ex->status, sizeof(ex->status), "directory not empty");
            ex->status_kind = 2;
        } else {
            ksnprintf(ex->status, sizeof(ex->status), "cannot delete %s", ex->ent[ex->selected].name);
            ex->status_kind = 2;
        }
    }
}

static void exp_new_file(explorer_t *ex, int is_dir) {
    char name[64];
    char full[VFS_MAX_PATH];
    int i = 1;
    for (;;) {
        ksnprintf(name, sizeof(name), is_dir ? "new_folder%d" : "untitled%d.txt", i);
        ksnprintf(full, sizeof(full), "%s/%s", kstrcmp(ex->path, "/") ? ex->path : "", name);
        if (!vfs_resolve(full)) break;
        if (++i > 500) return;
    }
    if (is_dir) {
        if (vfs_create_node(full, FT_DIR)) {
            ksnprintf(ex->status, sizeof(ex->status), "created folder %s", name);
            ex->status_kind = 1;
        }
    } else {
        if (vfs_create_node(full, FT_FILE)) {
            ksnprintf(ex->status, sizeof(ex->status), "created file %s", name);
            ex->status_kind = 1;
        }
    }
    exp_refresh(ex);
    for (i = 0; i < ex->nent; i++)
        if (!kstrcmp(ex->ent[i].name, name)) ex->selected = i;
}

static wm_rect_t exp_icon_rect(wm_rect_t c, int idx, int *rows_visible) {
    int cell_w = (c.w - 24) / EXP_COLS;
    int cell_h = 86;
    int col = idx % EXP_COLS, row = idx / EXP_COLS;
    if (rows_visible) *rows_visible = cell_h;
    return (wm_rect_t){ c.x + 12 + col * cell_w, c.y + 84 + row * cell_h, cell_w - 6, cell_h - 6 };
}

static void explorer_paint(void *state, window_t *w, wm_rect_t c) {
    explorer_t *ex = (explorer_t *)state;
    theme_t *t = theme_get();
    int i;
    int bar_y = c.y + 6;

    gfx_fill_rect(c.x, c.y, c.w, c.h, t->win_bg);

    /* toolbar: up + breadcrumb + action buttons */
    {
        int bx = c.x + 8;
        int btn = 26;
        int hover_up = (g_wm.mouse_px >= bx && g_wm.mouse_px < bx + btn &&
                        g_wm.mouse_py >= bar_y && g_wm.mouse_py < bar_y + btn);
        gfx_round_rect(bx, bar_y, btn, btn, 5, hover_up ? t->sel : RGBA(40, 52, 72, 200));
        gfx_draw_icon(ICON_UP, bx + 4, bar_y + 4, 18);
        bx += btn + 8;

        /* breadcrumbs */
        {
            char crumbs[VFS_MAX_PATH];
            char *p;
            int cx = bx;
            kstrncpy(crumbs, ex->path, sizeof(crumbs) - 1);
            gfx_round_rect(cx, bar_y, c.w - (bx - c.x) - 190, btn, 5, RGBA(10, 14, 22, 220));
            gfx_circle(cx + 10, bar_y + btn / 2, 3, t->accent);
            cx += 20;
            gfx_text(cx, bar_y + 6, "/", t->text_dim, 0);
            cx += gfx_text_w("/", 0) + 2;
            p = crumbs;
            while (*p) {
                char comp[64];
                int n = 0;
                char acc[VFS_MAX_PATH];
                while (*p == '/') p++;
                if (!*p) break;
                while (*p && *p != '/' && n < 63) comp[n++] = *p++;
                comp[n] = 0;
                {
                    int tw = gfx_text_w(comp, 0);
                    if (cx + tw > c.x + c.w - 200) { gfx_text(cx, bar_y + 6, "...", t->text_mute, 0); break; }
                    /* accumulate absolute prefix for the click handler */
                    ksnprintf(acc, sizeof(acc), "%s", ex->path);
                    {
                        /* the crumb click zone is recomputed from the path */
                        (void)acc;
                    }
                    gfx_text(cx, bar_y + 6, comp, t->text, 0);
                    cx += tw + 2;
                    gfx_text(cx, bar_y + 6, "/", t->text_mute, 0);
                    cx += gfx_text_w("/", 0) + 4;
                }
            }
        }

        /* action buttons */
        {
            int ax = c.x + c.w - 178;
            const char *labels[3] = { "File", "Folder", "Delete" };
            int icons[3] = { ICON_TXT, ICON_FOLDER, ICON_CLOSE };
            for (i = 0; i < 3; i++) {
                int bw = 56;
                int hover = (g_wm.mouse_px >= ax && g_wm.mouse_px < ax + bw &&
                             g_wm.mouse_py >= bar_y && g_wm.mouse_py < bar_y + 26);
                gfx_round_rect(ax, bar_y, bw, 26, 5,
                               hover ? (i == 2 ? RGBA(200, 70, 80, 220) : t->sel)
                                     : RGBA(36, 46, 64, 220));
                gfx_draw_icon(icons[i], ax + 4, bar_y + 5, 16);
                gfx_text(ax + 22, bar_y + 7, labels[i], t->text, 0);
                ax += bw + 4;
            }
        }
    }

    /* view switch + status */
    {
        int vy = c.y + 40;
        const char *views[2] = { "Icons", "List" };
        for (i = 0; i < 2; i++) {
            int vx = c.x + 8 + i * 58;
            int active = (ex->list_view == i);
            gfx_round_rect(vx, vy, 54, 20, 5, active ? t->accent : RGBA(30, 40, 56, 200));
            gfx_text_center(vx + 27, vy + 3, views[i], active ? RGB(8, 14, 22) : t->text_dim, 0);
        }
        gfx_text(c.x + 132, vy + 3, ex->path, t->text_mute, 0);
        {
            char buf[64];
            ksnprintf(buf, sizeof(buf), "%d items", ex->nent);
            gfx_text_right(c.x + c.w - 12, vy + 3, buf, t->text_mute, 0);
        }
    }

    /* entry area */
    {
        int list_top = c.y + 66;
        gfx_hline(c.x + 6, list_top, c.w - 12, RGBA(255, 255, 255, 16));
        if (!ex->nent) {
            gfx_text_center(c.x + c.w / 2, c.y + c.h / 2 - 10, "this directory is empty", t->text_mute, 1);
        } else if (!ex->list_view) {
            for (i = 0; i < ex->nent; i++) {
                wm_rect_t r = exp_icon_rect(c, i, NULL);
                int sel = (i == ex->selected);
                int hover = (g_wm.mouse_px >= r.x && g_wm.mouse_px < r.x + r.w &&
                             g_wm.mouse_py >= r.y && g_wm.mouse_py < r.y + r.h);
                char full[VFS_MAX_PATH];
                vfs_inode_t st;
                int icon;
                if (r.y + r.h > c.y + c.h - 26) break;
                ksnprintf(full, sizeof(full), "%s/%s", kstrcmp(ex->path, "/") ? ex->path : "",
                          ex->ent[i].name);
                kmemset(&st, 0, sizeof(st));
                vfs_stat_path(full, &st);
                icon = gfx_icon_for_path(ex->ent[i].name, ex->ent[i].type == FT_DIR,
                                         ex->ent[i].type == FT_PROC, 0);
                if (sel || hover)
                    gfx_round_rect(r.x, r.y, r.w, r.h, 8, sel ? t->sel : RGBA(255, 255, 255, 18));
                if (sel) gfx_round_rect_border(r.x, r.y, r.w, r.h, 8, t->accent, 1);
                gfx_draw_icon(icon, r.x + r.w / 2 - 17, r.y + 8, 34);
                gfx_text_clip(r.x + 6, r.y + 50, r.w - 8, ex->ent[i].name, t->text, 0);
                if (ex->ent[i].type != FT_DIR) {
                    char buf[32];
                    ksnprintf(buf, sizeof(buf), "%u B", (unsigned)st.size);
                    gfx_text_right(r.x + r.w - 6, r.y + 66, buf, t->text_mute, 0);
                }
            }
        } else {
            int row_h = 22;
            gfx_text(c.x + 12, list_top + 4, "NAME", t->text_mute, 0);
            gfx_text(c.x + c.w - 190, list_top + 4, "TYPE", t->text_mute, 0);
            gfx_text(c.x + c.w - 120, list_top + 4, "SIZE", t->text_mute, 0);
            gfx_text(c.x + c.w - 56, list_top + 4, "MODE", t->text_mute, 0);
            for (i = 0; i < ex->nent; i++) {
                int ry = list_top + 22 + i * row_h;
                int sel = (i == ex->selected);
                int hover = (g_wm.mouse_px >= c.x + 8 && g_wm.mouse_px < c.x + c.w - 8 &&
                             g_wm.mouse_py >= ry && g_wm.mouse_py < ry + row_h);
                char full[VFS_MAX_PATH];
                vfs_inode_t st;
                char mode[16];
                int icon;
                if (ry + row_h > c.y + c.h - 26) break;
                ksnprintf(full, sizeof(full), "%s/%s", kstrcmp(ex->path, "/") ? ex->path : "",
                          ex->ent[i].name);
                kmemset(&st, 0, sizeof(st));
                vfs_stat_path(full, &st);
                vfs_mode_string(ex->ent[i].type, st.mode, mode);
                icon = gfx_icon_for_path(ex->ent[i].name, ex->ent[i].type == FT_DIR,
                                         ex->ent[i].type == FT_PROC, 0);
                if (sel || hover) gfx_fill_rect(c.x + 8, ry, c.w - 16, row_h, sel ? t->sel : RGBA(255, 255, 255, 14));
                gfx_draw_icon(icon, c.x + 12, ry + 3, 16);
                gfx_text_clip(c.x + 34, ry + 3, c.w - 230, ex->ent[i].name, t->text, 0);
                gfx_text(c.x + c.w - 190, ry + 3, vfs_type_name(ex->ent[i].type), t->text_dim, 0);
                {
                    char buf[32];
                    ksnprintf(buf, sizeof(buf), "%u", (unsigned)st.size);
                    gfx_text(c.x + c.w - 120, ry + 3, buf, t->text_dim, 0);
                }
                gfx_text(c.x + c.w - 56, ry + 3, mode, t->text_mute, 0);
            }
        }
    }

    /* status bar */
    {
        u32 col = ex->status_kind == 1 ? t->ok : (ex->status_kind == 2 ? t->err : t->text_dim);
        gfx_fill_rect(c.x, c.y + c.h - 22, c.w, 22, RGBA(8, 12, 18, 240));
        if (ex->status[0]) gfx_text(c.x + 10, c.y + c.h - 17, ex->status, col, 0);
        {
            u32 iu = 0, bu = 0;
            char buf[96];
            vfs_usage(&iu, &bu, NULL);
            ksnprintf(buf, sizeof(buf), "vfs %u/%u inodes  %u/%u blocks",
                      (unsigned)iu, (unsigned)VFS_INODES, (unsigned)bu, (unsigned)VFS_BLOCKS);
            gfx_text_right(c.x + c.w - 10, c.y + c.h - 17, buf, t->text_mute, 0);
        }
    }
}

static void explorer_tick(void *state, u32 dt) { (void)state; (void)dt; }

static void explorer_key(void *state, int key, int ch, int mods) {
    explorer_t *ex = (explorer_t *)state;
    (void)mods;
    if (ch == 'h' || key == 0x25) { if (ex->selected > 0) ex->selected--; }
    if (ch == 'l' || key == 0x27) { if (ex->selected < ex->nent - 1) ex->selected++; }
    if (key == 0x26) ex->selected = k_max(0, ex->selected - EXP_COLS);
    if (key == 0x28) ex->selected = k_min(ex->nent - 1, ex->selected + EXP_COLS);
    if (key == 0x0D) exp_open_entry(ex, ex->selected);
    if (ch == 8) exp_navigate(ex, "..");
    if (ch == 'n') exp_new_file(ex, 0);
    if (ch == 'N') exp_new_file(ex, 1);
    if (key == 0x2E) exp_delete(ex);
    if (ch == 'v') ex->list_view = !ex->list_view;
    if (ch == 's') { ex->sort_mode = (ex->sort_mode + 1) % 3; exp_refresh(ex); }
}

static void explorer_mouse(void *state, int event, int x, int y, int button, int wheel) {
    explorer_t *ex = (explorer_t *)state;
    wm_rect_t c = wm_input_client_rect();
    (void)button;
    if (event == APP_EV_WHEEL) { ex->scroll = k_max(0, ex->scroll - wheel); return; }
    if (event != APP_EV_MOUSE_DOWN && event != APP_EV_MOUSE_DBLCLICK) return;

    /* toolbar buttons */
    if (y < 36) {
        int bx = 8;
        if (x >= bx && x < bx + 26) { exp_navigate(ex, ".."); return; }
        {
            int ax = c.w - 178;
            if (x >= ax && x < ax + 60) { exp_new_file(ex, 0); return; }
            if (x >= ax + 60 && x < ax + 120) { exp_new_file(ex, 1); return; }
            if (x >= ax + 120 && x < ax + 180) { exp_delete(ex); return; }
        }
        /* breadcrumb: rebuild click zones */
        {
            char tmp[VFS_MAX_PATH];
            int cx = 8 + 26 + 8 + 20;
            char *p;
            kstrncpy(tmp, ex->path, sizeof(tmp) - 1);
            p = tmp;
            while (*p) {
                char comp[64];
                int n = 0;
                while (*p == '/' ) p++;
                if (!*p) break;
                while (*p && *p != '/' && n < 63) comp[n++] = *p++;
                comp[n] = 0;
                {
                    int tw = gfx_text_w(comp, 0);
                    if (x >= cx && x < cx + tw) {
                        char target[VFS_MAX_PATH];
                        char *q = tmp;
                        char *find = kstrchr(q, 0);
                        (void)find;
                        /* build the prefix up to and including this component */
                        {
                            const char *full = ex->path;
                            int idx_of = (int)((p - tmp));
                            kstrncpy(target, full, sizeof(target) - 1);
                            if (idx_of >= 0 && idx_of < (int)kstrlen(target)) target[idx_of] = 0;
                            exp_navigate(ex, target);
                        }
                        return;
                    }
                    cx += tw + 2 + gfx_text_w("/", 0) + 4;
                }
            }
        }
        return;
    }
    /* view switch */
    if (y >= 40 && y < 62 && x < 8 + 118) {
        ex->list_view = (x >= 8 + 58) ? 1 : 0;
        return;
    }
    /* entries */
    {
        int list_top = c.y >= 0 ? 66 : 66;
        (void)list_top;
        if (!ex->list_view) {
            int i;
            for (i = 0; i < ex->nent; i++) {
                wm_rect_t r = exp_icon_rect(c, i, NULL);
                if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                    int was = ex->selected;
                    ex->selected = i;
                    if (event == APP_EV_MOUSE_DBLCLICK) { exp_open_entry(ex, i); return; }
                    if (was == i && now_ms() - ex->last_click_ms < 400) {
                        exp_open_entry(ex, i);
                        return;
                    }
                    ex->last_click_ms = now_ms();
                    return;
                }
            }
        } else {
            int row_h = 22;
            int idx = (y - 88) / row_h;
            if (y >= 88 && idx >= 0 && idx < ex->nent) {
                ex->selected = idx;
                if (event == APP_EV_MOUSE_DBLCLICK) exp_open_entry(ex, idx);
            }
        }
    }
}

const app_desc_t app_explorer = {
    "explorer", "File Explorer", "browse the VFS", ICON_FILES,
    780, 520, 480, 320, APP_CAT_UTIL, 1, 1,
    explorer_create, explorer_destroy, explorer_paint, explorer_key, explorer_mouse,
    explorer_tick, NULL
};
