/* ==========================================================================
 *  APP: hexview -- RAM & inode hex inspector
 *
 *  Reads raw physical RAM through the HAL (the exact same pointer the kernel
 *  allocator hands out) or any VFS file, showing 16 bytes per row with a hex
 *  grid and an ASCII gutter.  Quick jumps to the kernel base, heap base, VFS
 *  superblock and arbitrary addresses.
 * ========================================================================== */

#define HEX_BYTES_PER_ROW 16
#define HEX_ROWS 22

enum { HEX_SRC_RAM = 0, HEX_SRC_FILE = 1 };

typedef struct {
    u32  offset;                    /* byte offset into the selected source   */
    int  src;
    char path[VFS_MAX_PATH];
    int  follow;
    int  bytes_per_row;
    char addr_input[16];
    int  addr_len;
    int  editing_addr;
    int  mode;                      /* 0 bytes, 1 u32 words, 2 ascii-ish       */
    int  hover_row;
    int  hover_col;
    u32  goto_flash;
    u32  last_scroll_ms;
    u32  cached_hash;
    u32  cache_ms;
} hexview_t;

static const char *hex_source_name(int src) {
    return src == HEX_SRC_RAM ? "physical RAM" : "VFS file";
}

static u32 hex_limit(hexview_t *hv) {
    if (hv->src == HEX_SRC_RAM) return PHYS_RAM_SIZE;
    {
        vfs_inode_t st;
        if (vfs_stat_path(hv->path, &st) == 0) return st.size;
        return 0;
    }
}

static void hex_render_bytes(hexview_t *hv, u32 off, u8 *out, int n) {
    int i;
    if (hv->src == HEX_SRC_RAM) {
        u32 limit = PHYS_RAM_SIZE;
        for (i = 0; i < n; i++) out[i] = (off + i < limit) ? g_phys_ram[off + i] : 0;
    } else {
        u32 len = 0;
        const char *err = NULL;
        char *data = vfs_read_alloc(hv->path, &len, &err);
        if (!data) { kmemset(out, 0, (size_t)n); return; }
        for (i = 0; i < n; i++) out[i] = (off + i < len) ? (u8)data[off + i] : 0;
        mm_kfree(data);
    }
}

static void *hexview_create(const char *arg) {
    hexview_t *hv = (hexview_t *)mm_kmalloc(sizeof(hexview_t));
    if (!hv) return NULL;
    kmemset(hv, 0, sizeof(*hv));
    hv->bytes_per_row = 16;
    hv->offset = HEAP_BASE;
    if (arg && arg[0]) {
        hv->src = HEX_SRC_FILE;
        kstrncpy(hv->path, arg, sizeof(hv->path) - 1);
        hv->offset = 0;
    }
    return hv;
}

static void hexview_destroy(void *state) { if (state) mm_kfree(state); }

static void hexview_paint(void *state, window_t *w, wm_rect_t c) {
    hexview_t *hv = (hexview_t *)state;
    theme_t *t = theme_get();
    u32 limit = hex_limit(hv);
    int x = c.x + 8;
    int y = c.y + 6;
    int i;

    gfx_fill_rect(c.x, c.y, c.w, c.h, RGB(10, 13, 20));

    /* source tabs + jump buttons */
    {
        const char *srcs[2] = { "RAM", "File" };
        int bx = x;
        for (i = 0; i < 2; i++) {
            int bw = 56;
            int active = (hv->src == i);
            int hover = (g_wm.mouse_px >= bx && g_wm.mouse_px < bx + bw &&
                         g_wm.mouse_py >= y && g_wm.mouse_py < y + 22);
            gfx_round_rect(bx, y, bw, 22, 5, active ? t->accent : (hover ? t->sel : RGBA(36, 46, 64, 210)));
            gfx_text_center(bx + bw / 2, y + 4, srcs[i], active ? RGB(8, 14, 22) : t->text, 0);
            bx += bw + 4;
        }
        {
            const char *jumps[4] = { "Kernel", "Heap", "VFS", "Top" };
            int jx = bx + 8;
            for (i = 0; i < 4; i++) {
                int bw = 60;
                int hover = (g_wm.mouse_px >= jx && g_wm.mouse_px < jx + bw &&
                             g_wm.mouse_py >= y && g_wm.mouse_py < y + 22);
                gfx_round_rect(jx, y, bw, 22, 5, hover ? t->sel : RGBA(32, 42, 58, 210));
                gfx_text_center(jx + bw / 2, y + 4, jumps[i], t->text_dim, 0);
                jx += bw + 4;
            }
        }
        /* address entry */
        {
            int ax = c.x + c.w - 178;
            gfx_round_rect(ax, y, 118, 22, 5, RGBA(6, 9, 14, 240));
            gfx_round_rect_border(ax, y, 118, 22, 5, hv->editing_addr ? t->accent : RGBA(70, 90, 120, 200), 1);
            gfx_text(ax + 6, y + 4, "0x", t->text_mute, 0);
            gfx_text(ax + 24, y + 4, hv->addr_input[0] ? hv->addr_input : "go to...", 
                     hv->addr_input[0] ? t->text : t->text_mute, 0);
            gfx_round_rect(ax + 122, y, 44, 22, 5, RGBA(46, 130, 86, 220));
            gfx_text_center(ax + 144, y + 4, "Go", RGB(240, 250, 240), 0);
        }
    }

    /* file path + metadata line */
    y += 28;
    {
        char buf[320];
        if (hv->src == HEX_SRC_FILE)
            ksnprintf(buf, sizeof(buf), "%s  %s  size %u B  offset 0x%08X",
                      hex_source_name(hv->src), hv->path, (unsigned)limit, (unsigned)hv->offset);
        else
            ksnprintf(buf, sizeof(buf), "%s  64 MiB  0x00000000-0x%08X   offset 0x%08X",
                      hex_source_name(hv->src), (unsigned)PHYS_RAM_SIZE, (unsigned)hv->offset);
        /* The status word is right-aligned on the same line as the source
           description.  On a narrow window the two ran into each other, so the
           description is clipped to whatever room the word leaves. */
        {
            const char *flag = hv->follow ? "auto-refresh ON" : "auto-refresh off";
            int flag_w = gfx_text_w(flag, 0) + 16;
            gfx_text_clip(x, y, c.w - 10 - flag_w, buf, t->text_dim, 0);
            gfx_text_right(c.x + c.w - 10, y, flag, hv->follow ? t->ok : t->text_mute, 0);
        }
    }
    y += 22;

    /* dump area */
    {
        int rows = (c.y + c.h - y - 26) / 16;
        if (rows > HEX_ROWS) rows = HEX_ROWS;
        gfx_round_rect(c.x + 4, y - 4, c.w - 8, rows * 16 + 26, 4, RGBA(6, 9, 14, 240));
        gfx_text(x + 4, y, "ADDRESS   ", t->text_mute, 0);
        {
            /* kstrcat appends, so this must start empty: an uninitialised
               buffer printed whatever the previous frame left on the stack
               (which is what drew a metadata line over the column headers). */
            char hdr[80];
            int k = 0;
            hdr[0] = 0;
            for (k = 0; k < hv->bytes_per_row; k++) {
                char tmp[4];
                ksnprintf(tmp, sizeof(tmp), "%02X ", k);
                kstrcat(hdr, tmp);
            }
            gfx_text(x + 4 + 10 * FONT_TERM_W, y, hdr, t->text_mute, 0);
        }
        gfx_text(x + 4 + 10 * FONT_TERM_W + hv->bytes_per_row * 3 * FONT_TERM_W + FONT_TERM_W, y,
                 "ASCII", t->text_mute, 0);
        y += 18;
        for (i = 0; i < rows; i++) {
            u32 off = hv->offset + (u32)(i * hv->bytes_per_row);
            u8 bytes[32];
            char addr[24], hexcols[160], ascii[40];
            int k;
            if (hv->src == HEX_SRC_FILE && limit && off >= limit) break;
            if (hv->src == HEX_SRC_RAM && off >= PHYS_RAM_SIZE) break;
            hex_render_bytes(hv, off, bytes, hv->bytes_per_row);
            ksnprintf(addr, sizeof(addr), "%08X", (unsigned)off);
            hexcols[0] = 0;
            ascii[0] = 0;
            for (k = 0; k < hv->bytes_per_row; k++) {
                char tmp[8];
                ksnprintf(tmp, sizeof(tmp), "%02X", bytes[k]);
                kstrcat(hexcols, tmp);
                kstrcat(hexcols, (k == 7) ? "  " : " ");
                {
                    char a[2];
                    a[0] = (bytes[k] >= 32 && bytes[k] < 127) ? (char)bytes[k] : '.';
                    a[1] = 0;
                    kstrcat(ascii, a);
                }
            }
            /* highlight regions of interest */
            {
                u32 col = t->text_dim;
                if (hv->src == HEX_SRC_RAM) {
                    if (off < KERNEL_SIZE) col = RGB(220, 150, 250);
                    else if (off >= HEAP_BASE && off < HEAP_BASE + HEAP_SIZE) col = RGB(140, 200, 255);
                    else if (off >= DISK_BASE && off < DISK_BASE + DISK_SIZE) col = RGB(150, 230, 170);
                    else col = t->text_mute;
                }
                if (off == hv->offset) {
                    gfx_fill_rect(c.x + 6, y - 2, c.w - 12, 16, RGBA(79, 195, 247, 40));
                }
                gfx_text_term(x + 4, y, addr, col, 0, 0);
                gfx_text_term(x + 4 + 10 * FONT_TERM_W, y, hexcols, t->text, 0, 0);
                gfx_text_term(x + 4 + 10 * FONT_TERM_W + hv->bytes_per_row * 3 * FONT_TERM_W + FONT_TERM_W,
                              y, ascii, t->ok, 0, 0);
            }
            y += 16;
        }
    }

    /* footer legend */
    {
        int fy = c.y + c.h - 22;
        gfx_fill_rect(c.x, fy, c.w, 22, RGBA(8, 12, 18, 240));
        gfx_fill_rect(c.x + 8, fy + 8, 8, 6, RGB(220, 150, 250));
        gfx_text(c.x + 20, fy + 4, "kernel", t->text_mute, 0);
        gfx_fill_rect(c.x + 70, fy + 8, 8, 6, RGB(140, 200, 255));
        gfx_text(c.x + 82, fy + 4, "heap", t->text_mute, 0);
        gfx_fill_rect(c.x + 122, fy + 8, 8, 6, RGB(150, 230, 170));
        gfx_text(c.x + 134, fy + 4, "ramdisk", t->text_mute, 0);
        gfx_text_right(c.x + c.w - 10, fy + 4,
                       "wheel scroll   arrows move   type an address and press Enter",
                       t->text_mute, 0);
    }
    (void)limit;
}

static void hexview_tick(void *state, u32 dt) { (void)state; (void)dt; }

static void hexview_key(void *state, int key, int ch, int mods) {
    hexview_t *hv = (hexview_t *)state;
    u32 step = (u32)hv->bytes_per_row;
    (void)mods;
    if (hv->editing_addr) {
        if (key == 0x0D || ch == '\n') {
            int ok = 0;
            u32 v = kstrtoul(hv->addr_input, 16, &ok);
            if (ok) {
                hv->offset = (hv->src == HEX_SRC_RAM) ? (v & ~(step - 1)) : (v & ~(step - 1));
                hv->goto_flash = now_ms();
            }
            hv->editing_addr = 0;
            hv->addr_len = 0;
            return;
        }
        if (key == 0x08) {
            if (hv->addr_len > 0) hv->addr_input[--hv->addr_len] = 0;
            return;
        }
        if (key == 0x1B) { hv->editing_addr = 0; hv->addr_input[0] = 0; hv->addr_len = 0; return; }
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) {
            if (hv->addr_len < 8) {
                hv->addr_input[hv->addr_len++] = (char)ch;
                hv->addr_input[hv->addr_len] = 0;
            }
        }
        return;
    }
    if (key == 0x26) hv->offset = (hv->offset > step * 4) ? hv->offset - step * 4 : 0;
    if (key == 0x28) hv->offset += step * 4;
    if (key == 0x21) hv->offset = (hv->offset > step * 30) ? hv->offset - step * 30 : 0;
    if (key == 0x22) hv->offset += step * 30;
    if (key == 0x25) hv->offset = (hv->offset > step) ? hv->offset - step : 0;
    if (key == 0x27) hv->offset += step;
    if (key == 0x24) hv->offset = 0;
    if (ch == 'r') hv->follow = !hv->follow;
    if (ch == '/') { hv->editing_addr = 1; hv->addr_input[0] = 0; hv->addr_len = 0; }
    {
        u32 limit = hex_limit(hv);
        if (limit && hv->offset >= limit) hv->offset = (limit > step) ? limit - step : 0;
    }
}

static void hexview_mouse(void *state, int event, int x, int y, int button, int wheel) {
    hexview_t *hv = (hexview_t *)state;
    wm_rect_t c = wm_input_client_rect();
    u32 step = (u32)hv->bytes_per_row;
    (void)button;
    if (event == APP_EV_WHEEL) {
        if (wheel > 0) hv->offset = (hv->offset > step * 3 * (u32)wheel) ? hv->offset - step * 3 * (u32)wheel : 0;
        else hv->offset += step * 3 * (u32)(-wheel);
        return;
    }
    if (event != APP_EV_MOUSE_DOWN) return;
    if (y < c.y + 32) {
        int bx = c.x + 8;
        int i;
        if (x >= bx && x < bx + 60) { hv->src = HEX_SRC_RAM; hv->offset = HEAP_BASE; return; }
        if (x >= bx + 60 && x < bx + 120) {
            if (hv->src != HEX_SRC_FILE) {
                hv->src = HEX_SRC_FILE;
                kstrcpy(hv->path, "/home/user/hello.c");
                hv->offset = 0;
            }
            return;
        }
        {
            int jx = bx + 130;
            static const u32 addrs[4] = { KERNEL_BASE, HEAP_BASE, DISK_BASE, 0 };
            for (i = 0; i < 4; i++) {
                if (x >= jx && x < jx + 64) {
                    if (i == 3) hv->offset = 0;
                    else {
                        hv->src = HEX_SRC_RAM;
                        hv->offset = addrs[i];
                    }
                    hv->goto_flash = now_ms();
                    return;
                }
                jx += 64;
            }
        }
        {
            int ax = c.x + c.w - 178;
            if (x >= ax && x < ax + 118) { hv->editing_addr = 1; hv->addr_input[0] = 0; hv->addr_len = 0; return; }
            if (x >= ax + 122 && x < ax + 166) {
                int ok = 0;
                u32 v = kstrtoul(hv->addr_input, 16, &ok);
                if (ok) hv->offset = v & ~(step - 1);
                hv->editing_addr = 0;
                return;
            }
        }
    }
}

const app_desc_t app_hexview = {
    "hexview", "Hex Inspector", "RAM + inode dump", ICON_HEX,
    760, 520, 520, 320, APP_CAT_DEV, 1, 0,
    hexview_create, hexview_destroy, hexview_paint, hexview_key, hexview_mouse,
    hexview_tick, NULL
};
