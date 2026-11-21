/* ==========================================================================
 *  APP: browser -- AE Browser
 *
 *  A real web browser for AE: address bar, back/forward/reload, a
 *  scrollable document view and clickable links.
 *
 *  Where the page comes from is the host's business.  The kernel calls
 *  net_fetch() in 20_hal.c, which the WebAssembly shell implements with
 *  fetch(); the Win32 build has no network stack, so there it reports that
 *  networking is unavailable and the browser still renders a built-in start
 *  page of local documentation.  Nothing in this file knows the difference.
 * ========================================================================== */

#define BR_HISTORY     32
#define BR_ADDR_MAX    512

typedef struct {
    char addr[BR_ADDR_MAX];         /* what the user typed / the current URL  */
    char status[128];               /* status bar text                        */
    int  status_kind;               /* 0 info, 1 ok, 2 warn, 3 error          */
    int  editing;                   /* address bar has keyboard focus        */
    int  addr_sel;                  /* whole address selected: typing replaces it */
    int  loading;                   /* a request is in flight                */
    int  scroll;
    int  hover_link;
    int  hist_len;
    int  hist_pos;
    char hist[BR_HISTORY][BR_ADDR_MAX];
    u32  started_ms;
    int  last_width;
    int  req_slot;                  /* in-flight request, 0 when idle        */
    u32  req_started_ms;
    char req_url[BR_ADDR_MAX];
    int  search_mode;               /* showing MCP results, not a document    */
    int  search_scroll;
    int  search_hover;
    html_doc_t *doc;
} browser_t;

/* -------------------------------------------------------------------------- */
/*  URL helpers                                                            */
/* -------------------------------------------------------------------------- */

/* Accept "example.com", "https://example.com/x" or a bare host and return a
 * fetchable URL.  A search-looking string goes to a plain-text search endpoint
 * so it renders in the reader. */
static void br_normalize(const char *in, char *out, int cap) {
    char tmp[BR_ADDR_MAX];
    int i, n = 0;
    while (*in == ' ') in++;
    for (i = 0; in[i] && n < (int)sizeof(tmp) - 1; i++) tmp[n++] = in[i];
    while (n > 0 && tmp[n - 1] == ' ') n--;
    tmp[n] = 0;

    if (!n) { kstrncpy(out, "https://example.com/", cap - 1); out[cap - 1] = 0; return; }

    if (!kstrncmp(tmp, "http://", 7) || !kstrncmp(tmp, "https://", 8)) {
        kstrncpy(out, tmp, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    if (!kstrncmp(tmp, "about:", 6) || !kstrncmp(tmp, "aether:", 7)) {
        kstrcpy(out, tmp);
        return;
    }
    /* A word with no dot used to be rewritten into a DuckDuckGo Lite URL, which
     * meant every search was really a page fetch of a SERP rendered as prose.
     * Searches now go to the MCP back end (br_search), so anything that reaches
     * this point is a host name to be tried as-is. */
    kstrcpy(out, "https://");
    kstrncat(out, tmp, cap - 1 - (int)kstrlen(out));
}

/* -------------------------------------------------------------------------- */
/*  Built-in start page                                                     */
/* -------------------------------------------------------------------------- */

static const char *const BR_START_PAGE =
    "<html><head><title>AE Browser</title></head><body>"
    "<h1>AE Browser</h1>"
    "<p>This is AE's own browser. Type an address above and press Enter, "
    "or pick one of the pages below.</p>"
    "<p>Type words instead of an address and you get search results. The search "
    "back end is a free Model Context Protocol server that needs no account, "
    "and it allows 100 searches a day.</p>"
    "<hr>"
    "<h3>Try these</h3>"
    "<p><a href=\"https://example.com\">example.com</a> - the smallest possible page</p>"
    "<h3>Search</h3>"
    "<p>Type a phrase such as <i>operating system history</i> into the address bar. "
    "Search results come from the MCP back end and open here as a reading list.</p>"
    "<p><a href=\"https://en.m.wikipedia.org/wiki/Operating_system\">Wikipedia</a> - "
    "article pages, mobile versions are tidier</p>"
    "<hr>"
    "<h3>How it works</h3>"
    "<p>The page is fetched by the host process - the browser runs sandboxed, so "
    "it asks the outside world for the bytes. What you see is a reading view: "
    "text, headings and links, with scripts and styling dropped. This is a "
    "reader, not a rendering engine, and it is honest about that.</p>"
    "<h3>Offline pages</h3>"
    "<p><a href=\"ae://about\">ae://about</a> - this system's details</p>"
    "<p><a href=\"ae://files\">ae://files</a> - the virtual file system</p>"
    "<p><a href=\"ae://commands\">ae://commands</a> - shell reference</p>"
    "<p><a href=\"ae://keys\">ae://keys</a> - keyboard shortcuts</p>"
    "<p>The full documentation shelf is the <b>AE64</b> folder in your home "
    "directory - open Files and look for it.</p>"
    "</body></html>";

static const char *br_local_page(const char *url) {
    static char buf[4096];
    if (!kstrncmp(url, "ae://about", 14)) {
        ksnprintf(buf, sizeof(buf),
            "<html><body><h1>AE %s</h1>"
            "<p>%s (%s)</p><hr>"
            "<p>Screen: %dx%d at 32bpp</p>"
            "<p>Physical RAM: %u MB</p>"
            "<p>Kernel heap: %u MB</p>"
            "<p>Disk: %u MB ramdisk</p>"
            "<p>Host: %s</p>"
            "<p>CPU: %s</p>"
            "<hr><p><a href=\"ae://\">back to the start page</a></p></body></html>",
            AE_VERSION_STR, AE_CODENAME, AE_VERSION_STR,
            SCREEN_W, SCREEN_H,
            (unsigned)(PHYS_RAM_SIZE / (1024u * 1024u)),
            (unsigned)(HEAP_SIZE / (1024u * 1024u)),
            (unsigned)(DISK_SIZE / (1024u * 1024u)),
            g_machine.host_os, g_machine.cpu_name);
        return buf;
    }
    if (!kstrncmp(url, "ae://keys", 13)) {
        /* The shortcut reference, generated from the same list the desktop
           implements, so it cannot drift out of date unnoticed. */
        static char keys[4096];
        int n = ksnprintf(keys, sizeof(keys),
            "<html><body><h1>Keyboard shortcuts</h1>"
            "<p>These are the bindings this build actually implements.</p><hr>"
            "<h3>Desktop</h3>"
            "<p>T - open a terminal &nbsp; B - browser &nbsp; F - file manager</p>"
            "<h3>Windows</h3>"
            "<p>Alt+Tab / Alt+Shift+Tab - switch window</p>"
            "<p>Alt+Q or Alt+F4 - close &nbsp; Alt+M - minimise</p>"
            "<p>Alt+Enter - maximise or restore</p>"
            "<p>Alt+Left / Alt+Right - snap to the left or right half</p>"
            "<h3>Menus</h3>"
            "<p>Right-click opens a context menu for what is under the pointer.</p>"
            "<p>Up/Down move the highlight, Enter or Space activates, Escape dismisses.</p>"
            "<p>Ctrl+Esc or the Windows key opens the Start menu.</p>"
            "<h3>Shell</h3>"
            "<p>Up/Down - history &nbsp; Tab - complete &nbsp; Ctrl+C - abandon line</p>"
            "<p>Ctrl+L - clear screen &nbsp; Home/End - line ends</p>"
            "<h3>Browser</h3>"
            "<p>Enter - go or search &nbsp; Alt+Left / Alt+Right - back / forward</p>"
            "<p>F5 - reload &nbsp; Tab - address bar &nbsp; F8 - open on the host</p>"
            "<p>Ctrl+letter works: Ctrl+C copies, Ctrl+V pastes, Ctrl+N makes a file</p>"
            "<h3>System</h3>"
            "<p>Ctrl+F1 - this page &nbsp; Ctrl+` - toggle scanlines</p>"
            "<p>F4 - deliberately panic the kernel, to demonstrate the handler</p>"
            "<hr><p>The same reference is on disk at "
            "<b>/home/user/AE64/keyboard.txt</b></p>"
            "<p><a href=\"ae://\">start page</a></p></body></html>");
        (void)n;
        return keys;
    }
    if (!kstrncmp(url, "ae://files", 14)) {
        static char listing[4096];
        int n = (int)kstrlen("<html><body><h1>The virtual file system</h1><p>"
                             "Paths that exist in this machine:</p><hr>");
        kstrcpy(listing, "<html><body><h1>The virtual file system</h1>"
                         "<p>Paths that exist in this machine:</p><hr>");
        {
            static const char *const paths[] = {
                "/", "/home", "/home/user", "/bin", "/etc", "/etc/motd",
                "/var", "/var/log", "/boot", "/proc", "/proc/cpuinfo",
                "/proc/meminfo", "/tmp", "/usr", "/usr/share",
                "/home/user/AE64", "/home/user/AE64/keyboard.txt",
                "/home/user/AE64/shell.txt", "/etc/desktop.cfg"
            };
            int i;
            for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
                char line[128];
                int is_dir = (paths[i][kstrlen(paths[i]) - 1] != 'o' ||
                              !kstrcmp(paths[i], "/proc")) ;
                (void)is_dir;
                ksnprintf(line, sizeof(line), "<p>%s</p>", paths[i]);
                n += (int)kstrlen(line);
                if (n < (int)sizeof(listing) - 2) kstrncat(listing, line, sizeof(listing) - 1 - (int)kstrlen(listing));
            }
            kstrncat(listing, "<hr><p><a href=\"ae://\">start page</a></p></body></html>",
                     sizeof(listing) - 1 - (int)kstrlen(listing));
        }
        return listing;
    }
    if (!kstrncmp(url, "ae://commands", 17)) {
        kstrcpy(buf, "<html><body><h1>Shell reference</h1>"
                     "<p>Open Crommand from the Start menu and type:</p><hr>"
                     "<p>ls [-l] [path] - list a directory</p>"
                     "<p>cd &lt;dir&gt; - change directory</p>"
                     "<p>cat &lt;file&gt; - print a file</p>"
                     "<p>write &lt;file&gt; &lt;text&gt; - create a file</p>"
                     "<p>mkdir / rm / cp / mv - file operations</p>"
                     "<p>ps / kill / free / memmap - processes and memory</p>"
                     "<p>cc &lt;src&gt; - compile C in the bytecode VM</p>"
                     "<p>theme &lt;0-3&gt; - switch themes</p>"
                     "<p>wallpaper &lt;0-3&gt; - switch wallpapers</p>"
                     "<p>browser [url] - open this browser</p>"
                     "<hr><h3>Crommand\'s own commands</h3>"
                     "<p>peek / grep / where / biggest / since / bench / grab / note</p>"
                     "<p>Type <b>help</b> in Crommand for the full list.</p>"
                     "<hr><p><a href=\"ae://\">start page</a></p></body></html>");
        return buf;
    }
    return BR_START_PAGE;
}

/* -------------------------------------------------------------------------- */
/*  Navigation                                                             */
/* -------------------------------------------------------------------------- */

/* Giving the address bar focus selects the address, exactly like a desktop
   browser: the next character typed starts a new address rather than being
   appended to the current one. */
static void br_focus_addr(browser_t *b, int select_all) {
    b->editing = 1;
    b->addr_sel = select_all && b->addr[0] ? 1 : 0;
}

static void br_load(browser_t *b, const char *url);

/* Width of the text column inside the current window, in pixels. */
static int br_text_width(const html_doc_t *d) {
    window_t *w = wm_focused_window();
    int cw = w ? wm_input_client_rect().w : 900;
    (void)d;
    if (cw < 240) cw = 900;
    return cw - 2 - 24;
}

/* -------------------------------------------------------------------------- */
/*  Search (MCP back end)                                                   */
/* -------------------------------------------------------------------------- */

/* A search result page is built as a local document: the reader already knows
 * how to lay out headings, links and paragraphs, so results go through exactly
 * the same path as any other page and get clickable links for free. */
static void br_show_results(browser_t *b) {
    static char html[8192];
    int n = 0, i;
    int hits = mcp_hits();

    n += ksnprintf(html + n, sizeof(html) - n,
                   "<html><head><title>search</title></head><body>"
                   "<h1>Search</h1><p>query: %s</p><hr>",
                   mcp_query());
    if (hits <= 0) {
        n += ksnprintf(html + n, sizeof(html) - n,
                       "<p>%s</p><p>The search back end is the You.com MCP server, "
                       "which needs no account and allows %d searches a day.</p>",
                       mcp_note(), mcp_daily_limit());
    } else {
        for (i = 0; i < hits && n < (int)sizeof(html) - 700; i++) {
            const mcp_hit_t *h = mcp_hit(i);
            if (!h) continue;
            n += ksnprintf(html + n, sizeof(html) - n,
                           "<h3><a href=\"%s\">%s</a></h3><p>%s</p>",
                           h->url,
                           h->title[0] ? h->title : h->url,
                           h->snippet[0] ? h->snippet : h->url);
        }
        n += ksnprintf(html + n, sizeof(html) - n,
                       "<hr><p>%d result%s from the MCP search back end</p>",
                       hits, hits == 1 ? "" : "s");
    }
    n += ksnprintf(html + n, sizeof(html) - n, "</body></html>");

    if (!b->doc) return;
    b->search_mode = 1;
    b->search_scroll = 0;
    b->scroll = 0;
    html_parse(b->doc, html);
    html_layout(b->doc, br_text_width(b->doc));
    b->doc->ok = b->doc->block_count > 0;
    b->status_kind = 1;
    ksnprintf(b->status, sizeof(b->status), "%d result%s",
              hits, hits == 1 ? "" : "s");
}

static void br_search(browser_t *b, const char *query) {
    char url[BR_ADDR_MAX];
    int r;

    /* Ask the MCP server, unless this chassis has no network -- then the reader
       says so plainly instead of pretending the search ran. */
    r = mcp_search(query);
    ksnprintf(url, sizeof(url), "search: %s", query);
    kstrncpy(b->addr, url, BR_ADDR_MAX - 1);
    b->addr[BR_ADDR_MAX - 1] = 0;
    b->addr_sel = 0;
    b->editing = 0;
    b->scroll = 0;
    b->search_scroll = 0;
    b->search_mode = 1;

    if (r == 0) {
        b->loading = 1;
        b->status_kind = 0;
        ksnprintf(b->status, sizeof(b->status),
                  "searching with MCP: %s", query);
        /* The results page is what gets parsed when the reply lands. */
        if (b->req_slot) { net_abort(b->req_slot); b->req_slot = 0; }
    } else {
        b->loading = 0;
        b->status_kind = 3;
        ksnprintf(b->status, sizeof(b->status), "%s", mcp_note());
        br_show_results(b);
    }
}

static void br_history_push(browser_t *b, const char *url) {
    int i;
    /* Truncate any forward history, like a real browser. */
    if (b->hist_pos < b->hist_len - 1) b->hist_len = b->hist_pos + 1;
    if (b->hist_len < BR_HISTORY) {
        kstrncpy(b->hist[b->hist_len], url, BR_ADDR_MAX - 1);
        b->hist[b->hist_len][BR_ADDR_MAX - 1] = 0;
        b->hist_pos = b->hist_len;
        b->hist_len++;
    } else {
        for (i = 1; i < BR_HISTORY; i++) kstrcpy(b->hist[i - 1], b->hist[i]);
        kstrncpy(b->hist[BR_HISTORY - 1], url, BR_ADDR_MAX - 1);
        b->hist_pos = BR_HISTORY - 1;
    }
}

static void br_search(browser_t *b, const char *query);

static void br_load(browser_t *b, const char *raw) {
    char url[BR_ADDR_MAX];
    html_doc_t *d = b->doc;

    /* A phrase, or a word that is not a host name, is a search: send it to the
       MCP back end and show the results in a list, which is far more useful in a
       text reader than a SERP page parsed as prose. */
    if (mcp_is_search(raw)) { br_search(b, raw); return; }

    br_normalize(raw, url, sizeof(url));
    kstrncpy(b->addr, url, BR_ADDR_MAX - 1);
    b->addr_sel = 0;
    b->scroll = 0;
    b->hover_link = -1;

    if (!d) return;
    kmemset(d, 0, sizeof(*d));
    kstrncpy(d->url, url, HTML_URL_MAX - 1);

    /* Local pages never touch the network. */
    if (!kstrncmp(url, "ae://", 9)) {
        const char *page = br_local_page(url);
        b->search_mode = 0;
        html_parse(d, page);
        html_layout(d, br_text_width(d));
        d->ok = d->block_count > 0;
        b->status_kind = 1;
        ksnprintf(b->status, sizeof(b->status), "%d blocks", d->block_count);
        br_history_push(b, url);
        return;
    }

    /* ---- real request ----------------------------------------------
       Started, not awaited: blocking here would freeze the window while the
       network answered.  browser_tick() polls the slot each frame and calls
       br_complete() when the body is ready. */
    if (b->req_slot) net_abort(b->req_slot);
    b->search_mode = 0;
    b->req_slot = net_begin(url);
    b->req_started_ms = now_ms();
    b->loading = 1;
    b->status_kind = 0;
    ksnprintf(b->status, sizeof(b->status), "loading %s", url);
    if (b->req_slot <= 0) {
        int r = b->req_slot;
        b->req_slot = 0;
        b->loading = 0;
        d->error_kind = 1;
        d->ok = 0;
        b->status_kind = 3;
        if (r == NET_ERR_UNSUPPORTED)
            ksnprintf(b->status, sizeof(b->status),
                      "networking is not available on this chassis");
        else
            ksnprintf(b->status, sizeof(b->status), "could not load %s", url);
        return;
    }
    kstrncpy(b->req_url, url, BR_ADDR_MAX - 1);
    b->req_url[BR_ADDR_MAX - 1] = 0;
}

/* Called once the request body is in hand: parse, lay out and record history. */
static void br_complete(browser_t *b, int len) {
    html_doc_t *d = b->doc;
    if (!d) return;
    if (len < 0) {
        d->error_kind = 1;
        d->ok = 0;
        b->status_kind = 3;
        if (len == NET_ERR_TIMEOUT)
            ksnprintf(b->status, sizeof(b->status),
                      "the site did not respond -- it may be offline or blocking this machine");
        else if (len == NET_ERR_UNSUPPORTED)
            ksnprintf(b->status, sizeof(b->status), "networking is not available here");
        else
            ksnprintf(b->status, sizeof(b->status),
                      "could not load this site -- press Ctrl+L to open it in a real window");
        return;
    }
    d->text_len = len;
    d->text[len] = 0;
    /* A plain-text document (many sites, every proxy, README files) has no
       markup at all, and the reader would run every line together into one
       paragraph.  Wrapping it in <pre> preserves the line breaks. */
    if (!kstrstr(d->text, "<html") && !kstrstr(d->text, "<HTML") &&
        !kstrstr(d->text, "<!DOCTYPE") && !kstrstr(d->text, "<body")) {
        /* In place, so no second copy of a 2 MB buffer is needed: shift the text
           up by the length of the opening tag and drop the closing tag in
           behind it. */
        static const char pre[] = "<html><body><pre>";
        static const char post[] = "</pre></body></html>";
        int head = (int)sizeof(pre) - 1;
        int tail = (int)sizeof(post) - 1;
        if (len + head + tail < HTML_TEXT_MAX) {
            kmemmove(d->text + head, d->text, (size_t)len + 1);
            kmemcpy(d->text, pre, (size_t)head);
            kmemcpy(d->text + head + len, post, (size_t)tail);
            d->text[head + len + tail] = 0;
            d->text_len = len + head + tail;
        }
        html_parse(d, d->text);
    } else {
        html_parse(d, d->text);
    }
    html_layout(d, br_text_width(d));
    d->ok = d->block_count > 0;
    if (!d->ok) {
        d->error_kind = 3;
        b->status_kind = 2;
        ksnprintf(b->status, sizeof(b->status), "the page had no readable text");
    } else if (d->truncated) {
        /* The page exceeded the readable-document budget.  Saying so is better
           than silently stopping mid-sentence, which is what used to happen. */
        b->status_kind = 2;
        ksnprintf(b->status, sizeof(b->status),
                  "%d KB -- page truncated, showing the first part", len / 1024);
    } else {
        b->status_kind = 1;
        ksnprintf(b->status, sizeof(b->status), "%d KB, %d blocks",
                  len / 1024, d->block_count);
    }
    br_history_push(b, d->url);
}

/* -------------------------------------------------------------------------- */
/*  Layout constants                                                       */
/* -------------------------------------------------------------------------- */

#define BR_BAR_H     38
#define BR_ADDR_X    96
#define BR_ADDR_W    (-220)     /* resolved against the client width          */

static void br_nav_buttons(wm_rect_t c, int *bx) {
    bx[0] = c.x + 10;    /* back    */
    bx[1] = c.x + 42;    /* forward */
    bx[2] = c.x + 74;    /* reload  */
}

/* -------------------------------------------------------------------------- */
/*  App callbacks                                                          */
/* -------------------------------------------------------------------------- */

static void *browser_create(const char *arg) {
    browser_t *b = (browser_t *)mm_kmalloc(sizeof(browser_t));
    if (!b) return NULL;
    kmemset(b, 0, sizeof(*b));
    b->doc = (html_doc_t *)mm_kmalloc(sizeof(html_doc_t));
    if (!b->doc) { mm_kfree(b); return NULL; }
    kmemset(b->doc, 0, sizeof(*b->doc));
    b->hover_link = -1;
    kstrcpy(b->status, "type an address and press Enter");
    /* Load first, focus second: br_load() clears the selection, so focusing
       before it would leave the address bar in append mode and every URL
       typed on open became "ae://<what was typed>". */
    if (arg && arg[0]) br_load(b, arg);
    else br_load(b, "ae://");
    br_focus_addr(b, 1);
    return b;
}

static void browser_destroy(void *state) {
    browser_t *b = (browser_t *)state;
    if (!b) return;
    if (b->req_slot) net_abort(b->req_slot);
    if (b->doc) mm_kfree(b->doc);
    mm_kfree(b);
}

/* Work out which link (if any) is under a client point.  Mirrors the layout in
 * html_render() so clicking always hits what you see. */
static int br_link_at(browser_t *b, wm_rect_t c, int px, int py) {
    html_doc_t *d = b->doc;
    int view_y = c.y + BR_BAR_H;
    int view_h = c.h - BR_BAR_H - 20;
    int i, rel;
    if (!d || !d->ok) return -1;
    if (py < view_y || py > view_y + view_h) return -1;
    rel = py - view_y - 8 + b->scroll;

    for (i = 0; i < d->block_count; i++) {
        html_block_t *blk = &d->blocks[i];
        int starts[128], lens[128];
        int n, ln;
        if (rel < blk->y || rel >= blk->y + blk->h) continue;
        n = html_wrap(d, blk, (c.w - 2) - 24, starts, lens, 128);
        ln = (rel - blk->y) / html_line_h(blk);
        if (ln < 0 || ln >= n) return -1;
        {
            char line[520];
            int off, llen;
            int lli = lens[ln];
            if (lli > 519) lli = 519;
            kstrncpy(line, d->text + blk->text_off + starts[ln], lli);
            line[lli] = 0;
            {
                int li = html_link_span(d, i, line, lli, &off, &llen);
                if (li < 0) return -1;
                /* is the pointer inside the link's glyph run? */
                {
                    int tx = c.x + 12 + (blk->kind == HTML_LIST ? 14 : 0);
                    int x0, x1;
                    if (blk->kind == HTML_LIST) { /* list bullets shift text */ }
                    x0 = tx + gfx_text_w_n(line, off, 0);
                    x1 = x0 + gfx_text_w_n(line + off, llen, 0);
                    if (px >= x0 - 2 && px <= x1 + 2) return li;
                }
            }
        }
        return -1;
    }
    return -1;
}

static void br_follow(browser_t *b, int li) {
    char target[HTML_URL_MAX];
    if (!b->doc || li < 0 || li >= b->doc->link_count) return;
    html_resolve(b->doc->url, b->doc->links[li].href, target, sizeof(target));
    br_load(b, target);
}

static void browser_paint(void *state, window_t *w, wm_rect_t c) {
    browser_t *b = (browser_t *)state;
    theme_t *t = theme_get();
    int bx[3];
    int addr_x, addr_w;
    (void)w;

    gfx_fill_rect(c.x, c.y, c.w, c.h, t->win_bg);

    /* ---- toolbar ---------------------------------------------------- */
    gfx_fill_rect(c.x, c.y, c.w, BR_BAR_H, t->win_chrome);
    br_nav_buttons(c, bx);
    {
        int i;
        const char *glyph[3] = { "<", ">", "O" };
        int enabled[3];
        enabled[0] = b->hist_pos > 0;
        enabled[1] = b->hist_pos + 1 < b->hist_len;
        enabled[2] = 1;
        for (i = 0; i < 3; i++) {
            int hover = (g_wm.mouse_px >= bx[i] && g_wm.mouse_px < bx[i] + 28 &&
                         g_wm.mouse_py >= c.y + 6 && g_wm.mouse_py < c.y + 32);
            u32 bg = hover && enabled[i] ? t->sel : RGBA(28, 36, 50, 220);
            gfx_round_rect(bx[i], c.y + 6, 28, 26, 6, bg);
            gfx_text_center(bx[i] + 14, c.y + 13, glyph[i],
                            enabled[i] ? t->text : t->text_mute, 1);
        }
    }

    addr_x = c.x + BR_ADDR_X;
    addr_w = c.w + BR_ADDR_W;
    if (addr_w < 120) addr_w = 120;
    gfx_round_rect(addr_x, c.y + 6, addr_w, 26, 13, RGBA(8, 12, 18, 240));
    gfx_round_rect_border(addr_x, c.y + 6, addr_w, 26, 13,
                          b->editing ? t->accent : t->border, 1);
    {
        int pad = 10;
        int shown = gfx_text_w(b->addr, 0);
        int off = 0;
        if (shown > addr_w - 2 * pad) {
            /* right-align the tail of a long URL, like a real address bar */
            while (b->addr[off] && gfx_text_w(b->addr + off, 0) > addr_w - 2 * pad) off++;
        }
        if (b->editing && b->addr_sel) {
            /* draw the selection, the way a focused address bar looks */
            int tw = gfx_text_w(b->addr + off, 0);
            if (tw > addr_w - 2 * pad) tw = addr_w - 2 * pad;
            gfx_fill_rect(addr_x + pad - 1, c.y + 11, tw + 2, 16, RGBA(58, 110, 190, 190));
        }
        gfx_text_clip(addr_x + pad, c.y + 13, addr_w - 2 * pad,
                      b->addr + off, b->editing && b->addr_sel ? CLR_WHITE : t->text, 0);
        if (b->editing && ((now_ms() / 500) & 1)) {
            int cx = addr_x + pad + gfx_text_w(b->addr + off, 0) + 1;
            if (cx < addr_x + addr_w - pad)
                gfx_fill_rect(cx, c.y + 11, 1, 15, t->accent);
        }
    }
    /* loading pip */
    if (b->loading) {
        int cx = c.x + c.w - 18, cy = c.y + 19;
        gfx_fill_circle(cx, cy, 5, t->warn);
    }

    /* ---- document -------------------------------------------------- */
    {
        int view_y = c.y + BR_BAR_H;
        int view_h = c.h - BR_BAR_H - 20;
        int view_w = c.w - 2;
        if (view_h < 10) view_h = 10;

        if (b->doc && b->doc->ok) {
            html_render(b->doc, t, c.x + 1, view_y, view_w, view_h,
                        b->scroll, b->hover_link);
        } else if (b->doc && b->doc->error_kind) {
            gfx_fill_rect(c.x + 1, view_y, view_w, view_h, RGBA(14, 18, 26, 250));
            gfx_text_center(c.x + view_w / 2, view_y + view_h / 2 - 20,
                            "This page could not be displayed", t->err, 1);
            gfx_text_center(c.x + view_w / 2, view_y + view_h / 2 + 2,
                            b->status, t->text_dim, 0);
            gfx_text_center(c.x + view_w / 2, view_y + view_h / 2 + 26,
                            "Press F8 to open it in a real browser window", t->text_mute, 0);
            gfx_text_center(c.x + view_w / 2, view_y + view_h / 2 + 48,
                            "or press Back and try another address", t->text_mute, 0);
        } else {
            gfx_fill_rect(c.x + 1, view_y, view_w, view_h, RGBA(14, 18, 26, 250));
            gfx_text_center(c.x + view_w / 2, view_y + view_h / 2,
                            "Loading...", t->text_dim, 0);
        }
    }

    /* ---- status bar ------------------------------------------------- */
    {
        int sy = c.y + c.h - 19;
        u32 col = b->status_kind == 3 ? t->err :
                  b->status_kind == 2 ? t->warn :
                  b->status_kind == 1 ? t->text_dim : t->text_mute;
        gfx_fill_rect(c.x, sy, c.w, 19, RGBA(8, 11, 17, 250));
        gfx_text_clip(c.x + 10, sy + 5, c.w - 20, b->status, col, 0);
        if (b->doc && b->doc->ok && b->doc->total_h > 0) {
            char pos[48];
            int pct = (b->scroll * 100) / (b->doc->total_h > 0 ? b->doc->total_h : 1);
            if (pct > 100) pct = 100;
            ksnprintf(pos, sizeof(pos), "%d%%", pct);
            gfx_text_right(c.x + c.w - 10, sy + 5, pos, t->text_mute, 0);
        }
    }
}

static void browser_key(void *state, int key, int ch, int mods) {
    browser_t *b = (browser_t *)state;
    html_doc_t *d = b->doc;
    (void)mods;

    /* Alt+Left / Alt+Right are Back / Forward, as in every browser. */
    if (mods & MOD_ALT) {
        if (key == 0x25 && b->hist_pos > 0) {   /* left  */
            b->hist_pos--;
            br_load(b, b->hist[b->hist_pos]);
            return;
        }
        if (key == 0x27 && b->hist_pos + 1 < b->hist_len) {  /* right */
            b->hist_pos++;
            br_load(b, b->hist[b->hist_pos]);
            return;
        }
    }

    /* Ctrl+L or F8: this page is beyond what a reader can lay out, so open it
       in a real window on the host.  This is the honest escape hatch instead of
       a dead end -- the reader is not a web engine and never will be. */
    if (key == 0x77 ||
        ((mods & MOD_CTRL) && (ch == 'l' || ch == 'L'))) {
        char u[BR_ADDR_MAX];
        kstrcpy(u, b->addr);
        hal_open_external(u);
        b->status_kind = 1;
        ksnprintf(b->status, sizeof(b->status),
                  "opened in a real browser window: %s", u);
        return;
    }

    if (key == 0x74) {           /* F5: reload */
        char u[BR_ADDR_MAX];
        kstrcpy(u, b->addr);
        br_load(b, u);
        return;
    }

    if (b->editing) {
        int n = (int)kstrlen(b->addr);
        /* Enter arrives twice: once as the key code and once as the '\n' glyph,
           depending on the chassis.  Both must navigate, and navigating twice
           must not happen -- so navigation clears `editing` first and the glyph
           path only fires when the key path did not already run. */
        if (key == 0x0D || ch == '\n' || ch == '\r') {
            char u[BR_ADDR_MAX];
            if (!b->editing) return;
            b->editing = 0;
            kstrcpy(u, b->addr);
            br_load(b, u);
            return;
        }
        if (key == 0x1B) { b->editing = 0; b->addr_sel = 0; return; }   /* Escape */
        if (key == 0x08) {                             /* Backspace */
            if (b->addr_sel) { b->addr[0] = 0; b->addr_sel = 0; return; }
            if (n > 0) b->addr[n - 1] = 0;
            return;
        }
        if (key == 0x2E) { b->addr[0] = 0; b->addr_sel = 0; return; }   /* Delete */
        if (ch >= 32 && ch < 127 && n < BR_ADDR_MAX - 1) {
            /* A focused address bar starts out with the whole address selected,
               so the first character typed replaces it.  Appending instead is
               what turned every typed URL into "ae://test.example/page".
               Clicking into the bar, Escape or any editing key clears the
               selection and returns to ordinary append-at-the-end behaviour. */
            if (b->addr_sel) { b->addr[0] = 0; b->addr_sel = 0; n = 0; }
            b->addr[n] = (char)ch;
            b->addr[n + 1] = 0;
        }
        return;
    }

    /* A printable character with the document focused means the user started
       typing an address -- hand the keys to the address bar instead of dropping
       them.  There is nothing else on the page that accepts text. */
    if (ch >= 32 && ch < 127) {
        br_focus_addr(b, 0);
        b->addr[0] = 0;
        b->addr[0] = (char)ch;
        b->addr[1] = 0;
        return;
    }

    /* document navigation */
    if (key == 0x26) { b->scroll -= 48; }            /* Up    */
    if (key == 0x28) { b->scroll += 48; }            /* Down  */
    if (key == 0x21) { b->scroll -= 320; }           /* PgUp  */
    if (key == 0x22) { b->scroll += 320; }           /* PgDn  */
    if (key == 0x23) {                               /* End   */
        b->scroll = d ? d->total_h : 0;
    }
    if (key == 0x24) { b->scroll = 0; }              /* Home  */
    if (key == 0x09) { br_focus_addr(b, 1); }        /* Tab: back to address */
    if (key == 0x0D && b->hover_link >= 0) br_follow(b, b->hover_link);
    if (b->scroll < 0) b->scroll = 0;
}

static void browser_mouse(void *state, int event, int x, int y, int button, int wheel) {
    browser_t *b = (browser_t *)state;
    wm_rect_t cc = wm_input_client_rect();
    int bx[3];
    (void)button;
    if (cc.w <= 0) return;

    if (wheel) {
        b->scroll += (wheel > 0 ? 1 : -1) * 64;
    }

    if (event == APP_EV_MOUSE_MOVE) {
        b->hover_link = br_link_at(b, cc, x, y);
        return;
    }
    if (event != APP_EV_MOUSE_DOWN) return;

    br_nav_buttons(cc, bx);
    if (y >= cc.y + 6 && y < cc.y + 32) {
        if (x >= bx[0] && x < bx[0] + 28) {
            if (b->hist_pos > 0) { b->hist_pos--; br_load(b, b->hist[b->hist_pos]); }
            return;
        }
        if (x >= bx[1] && x < bx[1] + 28) {
            if (b->hist_pos + 1 < b->hist_len) { b->hist_pos++; br_load(b, b->hist[b->hist_pos]); }
            return;
        }
        if (x >= bx[2] && x < bx[2] + 28) {
            char u[BR_ADDR_MAX];
            kstrcpy(u, b->addr);
            br_load(b, u);
            return;
        }
        {
            int addr_x = cc.x + BR_ADDR_X;
            int addr_w = cc.w + BR_ADDR_W;
            if (addr_w < 120) addr_w = 120;
            if (x >= addr_x && x < addr_x + addr_w) { br_focus_addr(b, 1); return; }
        }
    }

    {
        int li = br_link_at(b, cc, x, y);
        if (li >= 0) { br_follow(b, li); return; }
    }
    b->editing = 0;
    b->addr_sel = 0;
}

static void browser_layout(void *state, wm_rect_t client) {
    browser_t *b = (browser_t *)state;
    int width = client.w - 2 - 24;
    if (!b->doc || !b->doc->ok) return;
    if (width < 240) width = 240;
    /* Re-flow only when the column width actually changed, and keep the reader
       at roughly the same place in the document. */
    if (width != b->last_width) {
        int maxs;
        html_layout(b->doc, width);
        b->last_width = width;
        maxs = b->doc->total_h - 40;
        if (maxs < 0) maxs = 0;
        if (b->scroll > maxs) b->scroll = maxs;
    }
}

static void browser_tick(void *state, u32 dt_ms) {
    browser_t *b = (browser_t *)state;
    (void)dt_ms;

    /* A search is its own request, tracked by the MCP client rather than by
       req_slot: it polls its own slot and reports when the reply is parsed. */
    if (b->loading && mcp_busy()) {
        if (mcp_poll() != 0) {
            int before = mcp_hits();
            int r = mcp_finish();
            /* A handshake answers with 0 hits and immediately starts the search
               it was holding, so loading does not end until a reply that was
               actually a search comes back. */
            if (r == 0 && mcp_busy()) {
                b->status_kind = 0;
                ksnprintf(b->status, sizeof(b->status),
                          "connected -- searching for %s", mcp_query());
            } else {
                b->loading = 0;
                b->status_kind = (mcp_hits() > 0) ? 1 : 3;
                ksnprintf(b->status, sizeof(b->status), "%s",
                          mcp_hits() > 0 ? "search results" : mcp_note());
                br_show_results(b);
            }
            (void)before;
        }
    }

    /* Finish a request that the host has completed since the last frame. */
    if (b->req_slot) {
        int r = net_poll(b->req_slot);
        if (r < 0) {
            net_abort(b->req_slot);
            b->req_slot = 0;
            b->loading = 0;
            br_complete(b, r);
        } else if (r > 0) {
            int slot = b->req_slot;
            int n;
            b->req_slot = 0;
            b->loading = 0;
            n = net_finish(slot, b->doc->text, HTML_TEXT_MAX - 1);
            br_complete(b, n);
        } else if (now_ms() - b->req_started_ms > 25000) {
            net_abort(b->req_slot);
            b->req_slot = 0;
            b->loading = 0;
            br_complete(b, NET_ERR_TIMEOUT);
        }
    }
    if (b->scroll < 0) b->scroll = 0;
    if (b->doc && b->doc->total_h > 0) {
        int maxs = b->doc->total_h - 40;
        if (maxs < 0) maxs = 0;
        if (b->scroll > maxs) b->scroll = maxs;
    }
}

const app_desc_t app_browser = {
    "browser", "AE Browser", "browse the web", ICON_BROWSER,
    900, 620, 520, 360, APP_CAT_UTIL, 1, 1,
    browser_create, browser_destroy, browser_paint, browser_key, browser_mouse,
    browser_tick, browser_layout
};
