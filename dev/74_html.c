/* ==========================================================================
 *  HTML -- a small document renderer for the AetherBrowser
 *
 *  This is not a web engine.  It reads a fetched HTML document, throws away
 *  everything that is not content (scripts, styles, comments, head), and lays
 *  the remaining text and links out as a scrollable page with a readable type
 *  ramp.  Block level tags become paragraphs; <a href> becomes a focusable,
 *  clickable link; <b>/<strong>/<h*> become bold or a larger size.
 *
 *  Keeping it this small is deliberate: the kernel has no DOM, no CSS cascade
 *  and no JavaScript interpreter, and pretending otherwise would be a much
 *  worse browser than an honest reader.  It renders article-style pages --
 *  documentation, wikis, news, search results -- very well.
 * ========================================================================== */

/* Document capacity.  These were small enough to be a user-visible bug: a real
 * article page ran out of text at 8 KB (html_text_add() silently dropped
 * everything after that, so the page simply stopped mid-sentence with no error)
 * and a long page overflowed the u16 block coordinates below.  The buffers live
 * in dynamically allocated html_doc_t, not on the stack, so raising them costs
 * heap rather than stack -- 2 MB is affordable out of a 31 MB heap. */
#define HTML_MAX_BLOCKS   4096
#define HTML_MAX_LINKS    1024
#define HTML_TEXT_MAX     (2 * 1024 * 1024)
#define HTML_HREF_MAX     256
#define HTML_URL_MAX      512

enum { HTML_BODY = 0, HTML_HEADING = 1, HTML_BOLD = 2, HTML_LIST = 3, HTML_RULE = 4 };

typedef struct {
    char text[512];
    u16  text_off;          /* offset into g_html.text                       */
    u16  text_len;
    u8   kind;              /* HTML_*                                       */
    u8   level;             /* heading level, 1..6                          */
    u8   first_line;        /* starts an anchor span (for link runs)        */
    int  y;                 /* resolved during layout                       */
    int  h;
} html_block_t;

typedef struct {
    char label[192];
    char href[HTML_HREF_MAX];
    int  block;             /* block index this link sits on                */
    int  line;              /* wrapped line within the block                */
} html_link_t;

typedef struct {
    char  url[HTML_URL_MAX];
    char  title[192];
    char  text[HTML_TEXT_MAX];
    int   text_len;
    html_block_t blocks[HTML_MAX_BLOCKS];
    int   block_count;
    html_link_t links[HTML_MAX_LINKS];
    int   link_count;
    int   total_h;          /* laid-out height in pixels                    */
    int   truncated;        /* content was dropped: the page did not fit    */
    int   ok;
    int   error_kind;       /* 0 none, 1 network, 2 parse, 3 empty          */
    char  status[96];
} html_doc_t;

/* -------------------------------------------------------------------------- */
/*  Entity decoding                                                          */
/* -------------------------------------------------------------------------- */

static int html_entity(const char *s, int *advance) {
    struct { const char *name; char ch; } tbl[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
        { "apos", '\'' }, { "nbsp", ' ' }, { "ndash", '-' }, { "mdash", '-' },
        { "hellip", '.' }, { "rsquo", '\'' }, { "lsquo", '\'' },
        { "ldquo", '"' }, { "rdquo", '"' }, { "copy", 'c' }, { "reg", 'r' }
    };
    int i;
    *advance = 1;
    if (s[0] != '&') return 0;
    /* numeric: &#123; or &#x1F; */
    if (s[1] == '#') {
        int v = 0, n = 2, base = 10;
        if (s[2] == 'x' || s[2] == 'X') { base = 16; n = 3; }
        while (s[n] && s[n] != ';' && n < 10) {
            int d = -1;
            if (s[n] >= '0' && s[n] <= '9') d = s[n] - '0';
            else if (base == 16 && s[n] >= 'a' && s[n] <= 'f') d = s[n] - 'a' + 10;
            else if (base == 16 && s[n] >= 'A' && s[n] <= 'F') d = s[n] - 'A' + 10;
            if (d < 0) return 0;
            v = v * base + d;
            n++;
        }
        if (s[n] != ';') return 0;
        *advance = n + 1;
        if (v == 32 || v == 160) return ' ';
        if (v >= 32 && v < 127) return v;
        if (v >= 0x2018 && v <= 0x201F) return '"';
        return 0;   /* no glyph for it in the bitmap font */
    }
    for (i = 0; i < (int)(sizeof(tbl) / sizeof(tbl[0])); i++) {
        int n = (int)kstrlen(tbl[i].name);
        if (!kstrncmp(s + 1, tbl[i].name, (size_t)n) && s[1 + n] == ';') {
            *advance = n + 2;
            return tbl[i].ch;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  Tag handling                                                             */
/* -------------------------------------------------------------------------- */

static const char *const HTML_SKIP[] = {
    "script", "style", "head", "noscript", "svg", "iframe", "template", "nav",
    "footer", "form", "select", "textarea", "canvas", "object"
};
#define HTML_SKIP_COUNT ((int)(sizeof(HTML_SKIP) / sizeof(HTML_SKIP[0])))

static int html_is_block(const char *tag) {
    static const char *const b[] = {
        "p", "div", "section", "article", "header", "main", "aside",
        "li", "br", "tr", "td", "th", "blockquote", "pre", "figure",
        "figcaption", "dl", "dt", "dd", "table", "ul", "ol", "span", "a",
        "h1", "h2", "h3", "h4", "h5", "h6", "b", "strong", "em", "i", "code"
    };
    int i;
    for (i = 0; i < (int)(sizeof(b) / sizeof(b[0])); i++)
        if (!kstrcmp(tag, b[i])) return 1;
    return 0;
}

static void html_block_push(html_doc_t *d, int kind, int level) {
    html_block_t *b;
    (void)0;
    if (d->block_count >= HTML_MAX_BLOCKS) { d->truncated = 1; return; }
    b = &d->blocks[d->block_count++];
    kmemset(b, 0, sizeof(*b));
    b->kind = (u8)kind;
    b->level = (u8)level;
    b->text_off = (u16)d->text_len;
}

/* Append to the current block's text, collapsing runs of whitespace. */
static void html_text_add(html_doc_t *d, char ch) {
    html_block_t *b;
    int len;
    if (d->block_count == 0) html_block_push(d, HTML_BODY, 0);
    b = &d->blocks[d->block_count - 1];
    if (d->text_len >= HTML_TEXT_MAX - 1) { d->truncated = 1; return; }
    len = d->text_len - b->text_off;
    if (len >= (int)sizeof(b->text) - 1) return;
    if (ch == ' ' && len > 0 && d->text[d->text_len - 1] == ' ') return;
    d->text[d->text_len++] = ch;
    d->text[d->text_len] = 0;
    b->text_len = (u16)(d->text_len - b->text_off);
}

/* -------------------------------------------------------------------------- */
/*  Parser                                                                   */
/* -------------------------------------------------------------------------- */

static void html_parse(html_doc_t *d, const char *src) {
    const char *p = src;
    int skip_tag = -1;          /* index into HTML_SKIP while inside one       */
    int skip_depth = 0;
    int in_anchor = 0;
    int anchor_block = -1;
    int cur_kind = HTML_BODY, cur_level = 0;
    int title_done = 0;

    while (*p) {
        /* ---- markup ---- */
        if (*p == '<') {
            char tag[32];
            int tn = 0;
            int closing = 0;
            const char *q = p + 1;
            int i;

            if (q[0] == '!') {                  /* comment or doctype */
                while (*p && *p != '>') p++;
                if (*p) p++;
                continue;
            }
            if (*q == '/') { closing = 1; q++; }
            while (*q && *q != '>' && *q != ' ' && tn < 31) {
                char c = *q;
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                tag[tn++] = c;
                q++;
            }
            tag[tn] = 0;

            /* skip element entirely, tags and all */
            if (skip_tag >= 0) {
                if (!closing && !kstrcmp(tag, HTML_SKIP[skip_tag])) skip_depth++;
                else if (closing && !kstrcmp(tag, HTML_SKIP[skip_tag])) {
                    if (--skip_depth <= 0) skip_tag = -1;
                }
            } else if (!closing && tn) {
                for (i = 0; i < HTML_SKIP_COUNT; i++) {
                    if (!kstrcmp(tag, HTML_SKIP[i])) {
                        /* self-closing forms never open a scope */
                        if (p[kstrlen(tag) + 1] == ' ' || q[-1] != '/') {
                            skip_tag = i;
                            skip_depth = 1;
                        }
                        break;
                    }
                }
            }

            /* <title> lives in <head>, which is skipped -- grab it separately */
            if (!title_done && !closing && !kstrcmp(tag, "title")) {
                const char *e = kstrchr(q, '<');
                int n = e ? (int)(e - q) : 0;
                if (n > 0 && n < (int)sizeof(d->title) - 1) {
                    int j, k = 0;
                    for (j = 0; j < n; j++) {
                        char c = q[j];
                        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                        d->title[k++] = c;
                    }
                    d->title[k] = 0;
                    title_done = 1;
                }
            }

            if (skip_tag < 0) {
                if (closing) {
                    if (!kstrcmp(tag, "b") || !kstrcmp(tag, "strong") ||
                        !kstrcmp(tag, "i") || !kstrcmp(tag, "em") ||
                        !kstrcmp(tag, "code")) {
                        cur_kind = HTML_BODY;
                    }
                    if (!kstrcmp(tag, "a")) in_anchor = 0;
                    if (!kstrcmp(tag, "h1") || !kstrcmp(tag, "h2") ||
                        !kstrcmp(tag, "h3") || !kstrcmp(tag, "h4") ||
                        !kstrcmp(tag, "h5") || !kstrcmp(tag, "h6")) {
                        cur_kind = HTML_BODY; cur_level = 0;
                        html_block_push(d, HTML_BODY, 0);   /* breath after */
                    } else if (!kstrcmp(tag, "p") || !kstrcmp(tag, "div") ||
                               !kstrcmp(tag, "li") || !kstrcmp(tag, "tr") ||
                               !kstrcmp(tag, "blockquote") || !kstrcmp(tag, "pre")) {
                        html_block_push(d, HTML_BODY, 0);
                    }
                } else {
                    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2]) {
                        html_block_push(d, HTML_HEADING, tag[1] - '0');
                        cur_kind = HTML_HEADING; cur_level = tag[1] - '0';
                    } else if (!kstrcmp(tag, "b") || !kstrcmp(tag, "strong")) {
                        cur_kind = HTML_BOLD;
                    } else if (!kstrcmp(tag, "hr")) {
                        html_block_push(d, HTML_RULE, 0);
                    } else if (!kstrcmp(tag, "li")) {
                        html_block_push(d, HTML_LIST, 0);
                    } else if (!kstrcmp(tag, "br")) {
                        html_block_push(d, HTML_BODY, 0);
                    } else if (!kstrcmp(tag, "p") || !kstrcmp(tag, "div") ||
                               !kstrcmp(tag, "section") || !kstrcmp(tag, "article") ||
                               !kstrcmp(tag, "tr") || !kstrcmp(tag, "blockquote") ||
                               !kstrcmp(tag, "pre") || !kstrcmp(tag, "figcaption") ||
                               !kstrcmp(tag, "dt") || !kstrcmp(tag, "dd") ||
                               !kstrcmp(tag, "header") || !kstrcmp(tag, "main") ||
                               !kstrcmp(tag, "table")) {
                        /* start a fresh paragraph if the current one has text */
                        if (d->block_count &&
                            d->blocks[d->block_count - 1].text_len) {
                            html_block_push(d, cur_kind, cur_level);
                        }
                    }
                }

                /* ---- <a href="..."> ---- */
                if (!closing && !kstrcmp(tag, "a") && d->link_count < HTML_MAX_LINKS) {
                    const char *h = q;
                    int found = 0;
                    while (*h && *h != '>') {
                        if ((h[0] == 'h' || h[0] == 'H') &&
                            (h[1] == 'r' || h[1] == 'R') &&
                            (h[2] == 'e' || h[2] == 'E') &&
                            (h[3] == 'f' || h[3] == 'F') &&
                            h[4] == '=') {
                            const char *v = h + 5;
                            char quote = 0;
                            int n = 0;
                            if (*v == '"' || *v == '\'') { quote = *v; v++; }
                            while (*v && n < HTML_HREF_MAX - 1 &&
                                   (quote ? *v != quote : (*v != ' ' && *v != '>'))) {
                                d->links[d->link_count].href[n++] = *v++;
                            }
                            d->links[d->link_count].href[n] = 0;
                            /* keep it simple: only http(s) and relative */
                            if (!kstrncmp(d->links[d->link_count].href, "javascript:", 11) ||
                                !kstrncmp(d->links[d->link_count].href, "#", 1) ||
                                !kstrncmp(d->links[d->link_count].href, "mailto:", 7)) {
                                d->links[d->link_count].href[0] = 0;
                            } else {
                                found = 1;
                            }
                            break;
                        }
                        h++;
                    }
                    if (found) {
                        html_link_t *l = &d->links[d->link_count];
                        l->label[0] = 0;
                        l->block = d->block_count ? d->block_count - 1 : 0;
                        in_anchor = 1;
                        anchor_block = l->block;
                        d->link_count++;
                    } else {
                        in_anchor = 0;
                        anchor_block = -1;
                    }
                    (void)anchor_block;
                }
            }
            while (*p && *p != '>') p++;
            if (*p) p++;
            continue;
        }

        /* ---- text ---- */
        if (skip_tag >= 0) { p++; continue; }

        {
            char ch = *p;
            int adv = 1;
            if (ch == '&') {
                int e = html_entity(p, &adv);
                if (adv > 1) {
                    if (e) html_text_add(d, (char)e);
                    p += adv;
                    continue;
                }
            }
            if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
            if (ch >= 32 || ch == ' ') {
                html_text_add(d, ch);
                /* grow the anchor label with the text it wraps */
                if (in_anchor && d->link_count) {
                    html_link_t *l = &d->links[d->link_count - 1];
                    int n = (int)kstrlen(l->label);
                    if (n < (int)sizeof(l->label) - 1 && ch != ' ')
                        l->label[n] = ch, l->label[n + 1] = 0;
                    else if (n > 0 && n < (int)sizeof(l->label) - 1 &&
                             l->label[n - 1] != ' ')
                        l->label[n] = ' ', l->label[n + 1] = 0;
                }
            }
            p++;
        }
    }

    /* drop blocks with no text (except rules) and trim trailing blanks */
    {
        int i, w = 0;
        for (i = 0; i < d->block_count; i++) {
            html_block_t *b = &d->blocks[i];
            if (b->kind != HTML_RULE && b->text_len == 0) continue;
            while (b->text_len && d->text[b->text_off + b->text_len - 1] == ' ')
                b->text_len--;
            d->blocks[w++] = *b;
        }
        d->block_count = w;
    }

    /* Compaction renumbers the blocks, so the block index recorded for each link
       is now stale.  Re-home every link by finding its label in the remaining
       text; a link whose label did not survive is dropped.  Without this, links
       parse correctly but never match a line, and so are never highlighted or
       clickable. */
    {
        int i, w = 0;
        char tmp[512];
        for (i = 0; i < d->link_count; i++) {
            html_link_t *l = &d->links[i];
            int bi, home = -1;
            /* trim the trailing space the label accumulator leaves behind */
            int ln = (int)kstrlen(l->label);
            while (ln > 0 && l->label[ln - 1] == ' ') l->label[--ln] = 0;
            if (!ln) continue;
            for (bi = 0; bi < d->block_count; bi++) {
                html_block_t *b = &d->blocks[bi];
                int n = b->text_len;
                if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
                kmemcpy(tmp, d->text + b->text_off, (size_t)n);
                tmp[n] = 0;
                if (kstrstr(tmp, l->label)) { home = bi; break; }
            }
            if (home < 0) continue;
            l->block = home;
            d->links[w++] = *l;
        }
        d->link_count = w;
    }
}

/* -------------------------------------------------------------------------- */
/*  Layout                                                                   */
/* -------------------------------------------------------------------------- */

static int html_heading_size(int level) {
    switch (level) {
    case 1: return 2;
    case 2: return 2;
    case 3: return 1;
    default: return 1;
    }
}

static int html_line_h(const html_block_t *b) {
    if (b->kind == HTML_HEADING) return html_heading_size(b->level) == 2 ? 26 : 20;
    if (b->kind == HTML_RULE) return 14;
    return 16;
}

/* Wrap a block's text to `width` pixels.  Returns the number of lines and fills
 * line_start[] with the character offset of each line. */
static int html_wrap(const html_doc_t *d, const html_block_t *b, int width,
                     int *line_start, int *line_len, int max_lines) {
    int size = (b->kind == HTML_HEADING) ? html_heading_size(b->level) : 0;
    int lines = 0, i = 0;
    int start = 0, last_space = -1, w = 0;

    for (i = 0; i < b->text_len && lines < max_lines; i++) {
        char ch = d->text[b->text_off + i];
        int cw;
        if (ch == ' ') last_space = i;
        cw = gfx_char_w(ch, size);
        if (w + cw > width && i > start) {
            int brk = (last_space > start) ? last_space : i;
            line_start[lines] = start;
            line_len[lines] = brk - start;
            lines++;
            start = (last_space > start) ? brk + 1 : brk;
            last_space = -1;
            w = 0;
            i = start - 1;
            continue;
        }
        w += cw;
    }
    if (lines < max_lines) {
        line_start[lines] = start;
        line_len[lines] = (b->text_len > start) ? b->text_len - start : 0;
        lines++;
    }
    return lines;
}

static void html_layout(html_doc_t *d, int width) {
    int i, y = 0;
    for (i = 0; i < d->block_count; i++) {
        html_block_t *b = &d->blocks[i];
        int starts[128], lens[128];
        int n = html_wrap(d, b, width - 24, starts, lens, 128);
        int lh = html_line_h(b);
        b->y = (u16)y;
        b->h = (u16)(n * lh + (b->kind == HTML_HEADING ? 10 : 6));
        y += b->h;
    }
    d->total_h = y + 16;
}

/* -------------------------------------------------------------------------- */
/*  Rendering                                                               */
/* -------------------------------------------------------------------------- */

/* Which link (if any) owns this block and line?  Links are matched by the
 * order in which they were seen, and a link's label is searched for in the
 * block text so the renderer can colour it. */
static int html_link_span(const html_doc_t *d, int block, const char *line,
                          int line_len, int *off, int *len) {
    int i;
    for (i = 0; i < d->link_count; i++) {
        const html_link_t *l = &d->links[i];
        const char *pos;
        if (l->block != block || !l->label[0]) continue;
        if ((int)kstrlen(l->label) > line_len + 2) continue;
        pos = kstrstr(line, l->label);
        if (pos) {
            *off = (int)(pos - line);
            *len = (int)kstrlen(l->label);
            return i;
        }
    }
    return -1;
}

static void html_render(const html_doc_t *d, theme_t *t, int x, int y, int w,
                        int h, int scroll, int hover_link) {
    int i;
    gfx_fill_rect(x, y, w, h, RGBA(14, 18, 26, 250));

    for (i = 0; i < d->block_count; i++) {
        const html_block_t *b = &d->blocks[i];
        int by = y + 8 + b->y - scroll;
        int starts[128], lens[128];
        int n, ln;
        if (by > y + h || by + b->h < y) continue;

        if (b->kind == HTML_RULE) {
            gfx_fill_rect(x + 12, by + 6, w - 24, 1, t->border);
            continue;
        }

        n = html_wrap(d, b, w - 24, starts, lens, 128);
        for (ln = 0; ln < n; ln++) {
            char line[520];
            int off, llen, li;
            int ly = by + 2 + ln * html_line_h(b);
            int size = (b->kind == HTML_HEADING) ? html_heading_size(b->level) : 0;
            int tx = x + 12;
            u32 col = (b->kind == HTML_HEADING) ? t->text : t->text_dim;

            if (ly + 16 < y || ly > y + h) continue;
            llen = lens[ln];
            if (llen > (int)sizeof(line) - 1) llen = (int)sizeof(line) - 1;
            kstrncpy(line, d->text + b->text_off + starts[ln], llen);
            line[llen] = 0;

            if (b->kind == HTML_HEADING) {
                gfx_text_clip(tx, ly, w - 24, line, col, size);
                continue;
            }

            if (b->kind == HTML_LIST) {
                gfx_fill_rect(tx + 3, ly + 6, 3, 3, t->accent);
                tx += 14;
            } else if (b->kind == HTML_BOLD) {
                col = t->text;
            }

            li = html_link_span(d, i, line, llen, &off, &llen);
            if (li >= 0) {
                /* draw the text before the link, the link itself, then the rest */
                char pre[520];
                int pre_len = off;
                u32 lc = (li == hover_link) ? t->accent : t->info;
                if (pre_len > 0) {
                    kstrncpy(pre, line, pre_len);
                    pre[pre_len] = 0;
                    gfx_text(tx, ly, pre, col, 0);
                    tx += gfx_text_w(pre, 0);
                }
                gfx_text_n(tx, ly, line + off, llen, lc, 0);
                tx += gfx_text_w_n(line + off, llen, 0);
                if (li == hover_link) {
                    gfx_fill_rect(tx - gfx_text_w_n(line + off, llen, 0),
                                  ly + 13, gfx_text_w_n(line + off, llen, 0), 1, lc);
                }
                if (line[off + llen]) gfx_text(tx, ly, line + off + llen, col, 0);
            } else {
                gfx_text_clip(tx, ly, w - (tx - x) - 12, line, col, 0);
            }
        }
    }
}

/* Resolve a possibly-relative href against the page it was found on. */
static void html_resolve(const char *base, const char *href, char *out, int cap) {
    if (!kstrncmp(href, "http://", 7) || !kstrncmp(href, "https://", 8)) {
        kstrncpy(out, href, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    /* split base into scheme://host and path */
    {
        const char *host_end = kstrstr(base, "://");
        const char *path;
        int n;
        if (!host_end) { kstrncpy(out, href, cap - 1); out[cap - 1] = 0; return; }
        host_end += 3;
        path = kstrchr(host_end, '/');
        if (!path) path = base + kstrlen(base);

        if (href[0] == '/') {
            n = (int)(path - base);
            if (n > cap - 1) n = cap - 1;
            kmemcpy(out, base, (size_t)n);
            out[n] = 0;
            kstrncat(out, href, cap - 1 - n);
            return;
        }
        /* relative: keep everything up to the last slash of the path */
        {
            const char *slash = kstrrchr(path, '/');
            n = slash ? (int)(slash - base) + 1 : (int)(path - base);
            if (n > cap - 1) n = cap - 1;
            kmemcpy(out, base, (size_t)n);
            out[n] = 0;
            kstrncat(out, href, cap - 1 - n);
        }
    }
}
