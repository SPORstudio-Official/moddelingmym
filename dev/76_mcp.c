/* ==========================================================================
 *  mcp.c -- Model Context Protocol client, used as the browser's search back end
 *
 *  Why MCP and not a search API key: AE ships as a single executable that any-
 *  one can run with no account, so the search back end has to work with no
 *  credentials of its own.  The You.com hosted MCP server exposes a keyless
 *  profile (you-search) at 100 queries per day that refills daily, which fits
 *  that constraint: nothing to sign up for, nothing to embed, and a documented
 *  daily allowance rather than a one-off trial balance.
 *
 *  Transport: Streamable HTTP, which for our purposes is a single JSON-RPC 2.0
 *  request over POST and a JSON-RPC response in the body.  That is exactly what
 *  the network chassis now supports (net_begin_post), so no SSE reader and no
 *  session cookie jar are needed.
 *
 *  Flow, one call per user search:
 *      POST /mcp/free   {"method":"tools/call","params":{"name":"you-search",
 *                        "arguments":{"query":"...","count":5}}}
 *      <-   {"result":{"content":[{"type":"text","text":"<json or prose>"}]}}
 *
 *  The reply body is JSON, which this module parses with a deliberately small
 *  scanner: it understands objects, arrays, strings and numbers well enough to
 *  pull title/url/snippet out of the tools/call result, and nothing else.  It
 *  is not a general JSON parser and does not try to be one -- documents larger
 *  than MCP_REPLY_MAX are truncated before parsing rather than trusted.
 * ========================================================================== */

#define MCP_HOST        "https://api.you.com/mcp/free"
#define MCP_TOOL        "you-search"
#define MCP_DAILY_LIMIT 100
#define MCP_REPLY_MAX   (96 * 1024)
#define MCP_QUERY_MAX   256
#define MCP_RESULTS_MAX 6

/* One search hit, in the order the server returned it. */
typedef enum {
    MCP_IDLE = 0,
    MCP_INITIALIZING,               /* handshake in flight                    */
    MCP_SEARCHING,                  /* tools/call in flight                   */
    MCP_READY
} mcp_phase_t;

typedef struct {
    int  slot;                      /* in-flight request, 0 when idle         */
    u32  started_ms;
    mcp_phase_t phase;
    char session[128];              /* Mcp-Session-Id from the handshake      */
    char query[MCP_QUERY_MAX];
    /* The JSON-RPC bodies live here, not on the stack: the worker thread reads
       them until the request finishes, so they must outlive the call that
       sent them. */
    char req[MCP_QUERY_MAX + 384];
    char init_req[320];
    char hdrs[MCP_QUERY_MAX + 512];
    char rhdr[768];
    int  hits;
    mcp_hit_t hit[MCP_RESULTS_MAX];
    int  done;                      /* 1 = hits are fresh, 0 = never asked    */
    int  failed;                    /* 1 = last call failed                   */
    char note[128];                 /* what to say when there is no result    */
} mcp_t;

static mcp_t g_mcp;

/* -------------------------------------------------------------------------- */
/*  String helpers                                                           */
/* -------------------------------------------------------------------------- */

/* Append a JSON string body with the escapes JSON requires.  A search query is
 * user text: quotes, backslashes and control characters must not break out of
 * the string literal, or the request becomes malformed JSON and the server
 * answers 400 with nothing useful to show. */
static void mcp_json_escape(const char *in, char *out, int cap) {
    static const char hex[] = "0123456789abcdef";
    int n = 0;
    while (in && *in && n < cap - 8) {
        unsigned char c = (unsigned char)*in++;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c == '\r') { out[n++] = '\\'; out[n++] = 'r'; }
        else if (c == '\t') { out[n++] = '\\'; out[n++] = 't'; }
        else if (c < 0x20) {
            out[n++] = '\\'; out[n++] = 'u'; out[n++] = '0'; out[n++] = '0';
            out[n++] = hex[(c >> 4) & 0xF]; out[n++] = hex[c & 0xF];
        } else out[n++] = (char)c;
    }
    out[n] = 0;
}

/* Decode the escapes JSON allows back into plain text, in place.  \u is handled
 * for the Latin-1 range and mapped to '?' above it: AE renders 7-bit glyphs, so
 * a faithful UTF-8 conversion would buy nothing a reader can see. */
static void mcp_json_unescape(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r != '\\') { *w++ = *r++; continue; }
        r++;
        switch (*r) {
        case 'n': *w++ = ' '; r++; break;
        case 'r': case 't': *w++ = ' '; r++; break;
        case 'b': case 'f': r++; break;
        case 'u': {
            int i, v = 0, ok = 1;
            for (i = 0; i < 4; i++) {
                char c = r[1 + i];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= c - '0';
                else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
                else { ok = 0; break; }
            }
            if (!ok) break;
            *w++ = (v >= 32 && v < 127) ? (char)v : ' ';
            r += 5;
            break;
        }
        case 0: return;
        default: *w++ = *r++; break;
        }
    }
    *w = 0;
}

/* Copy a JSON string literal starting at p (which must point at the opening
 * quote) into out, returns the position just past the closing quote. */
static const char *mcp_take_string(const char *p, char *out, int cap) {
    int n = 0;
    if (*p != '"') { if (out && cap) out[0] = 0; return p; }
    p++;
    while (*p && *p != '"') {
        if (n < cap - 1) out[n++] = *p;
        p++;
    }
    if (out && cap) {
        out[n] = 0;
        mcp_json_unescape(out);
    }
    return (*p == '"') ? p + 1 : p;
}

/* Find a key at any depth and return a pointer to its value.  Searching by key
 * rather than by path is deliberate: the MCP result shape varies (web vs news
 * arrays, one text block vs several), but the leaf keys are stable. */
static const char *mcp_find_key(const char *json, const char *key) {
    char pat[40];
    int plen, i;
    ksnprintf(pat, sizeof(pat), "\"%s\"", key);
    plen = (int)kstrlen(pat);
    for (i = 0; json[i]; i++) {
        if (json[i] != '"') continue;
        if (kstrncmp(json + i, pat, plen)) continue;
        {
            int j = i + plen;
            while (json[j] == ' ' || json[j] == '\t' || json[j] == '\n' || json[j] == '\r') j++;
            if (json[j] == ':') {
                j++;
                while (json[j] == ' ' || json[j] == '\t' || json[j] == '\n' || json[j] == '\r') j++;
                return json + j;
            }
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/*  Result extraction                                                        */
/* -------------------------------------------------------------------------- */

/* The tools/call result wraps its payload in a text block whose content is
 * itself JSON for you-search.  Unwrap so the key search sees the real object,
 * falling back to the outer document when the text is prose. */
static const char *mcp_payload(const char *body) {
    const char *t = mcp_find_key(body, "text");
    if (t && *t == '"') return t;
    return body;
}

static void mcp_copy_trim(char *dst, int cap, const char *src) {
    int n = 0;
    if (!src) { if (cap) dst[0] = 0; return; }
    while (*src == ' ' || *src == '\n') src++;
    while (*src && n < cap - 1) dst[n++] = *src++;
    dst[n] = 0;
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\n')) dst[--n] = 0;
}

/* Pull up to MCP_RESULTS_MAX hits out of the reply.  Returns the count. */
static int mcp_extract(const char *body, mcp_hit_t *out, int max) {
    const char *payload = mcp_payload(body);
    const char *p = payload;
    int n = 0;
    char tmp[512];

    /* The text block is a JSON string, so its inner quotes are escaped as \" and
     * the key scanner above cannot see them.  Unescape into a scratch copy
     * first when the payload still looks escaped. */
    static char inner[MCP_REPLY_MAX];
    if (payload != body && payload[0] == '"') {
        int i = 0;
        payload++;
        while (*payload && i < (int)sizeof(inner) - 1) {
            if (payload[0] == '\\' && payload[1] == '"') { inner[i++] = '"'; payload += 2; continue; }
            if (payload[0] == '"') break;
            inner[i++] = *payload++;
        }
        inner[i] = 0;
        /* The inner text may itself be prose; only use it when it looks like a
         * JSON object, otherwise fall through to the outer body. */
        if (i > 0 && inner[0] == '{') payload = inner;
        else payload = body;
    }

    p = payload;
    while (n < max) {
        const char *u = mcp_find_key(p, "url");
        const char *t, *d;
        if (!u) break;
        t = mcp_find_key(p, "title");
        mcp_take_string(u, tmp, sizeof(tmp));
        if (!tmp[0]) break;
        mcp_copy_trim(out[n].url, sizeof(out[n].url), tmp);
        if (t) {
            mcp_take_string(t, tmp, sizeof(tmp));
            mcp_copy_trim(out[n].title, sizeof(out[n].title), tmp);
        } else out[n].title[0] = 0;
        if (!out[n].title[0]) mcp_copy_trim(out[n].title, sizeof(out[n].title), out[n].url);

        /* The one-line summary is called "description" by you-search and
           "snippet" by most other servers; take whichever appears before the
           next result, so a hit never borrows its neighbour's text. */
        out[n].snippet[0] = 0;
        {
            const char *rest = u + 1;
            const char *nexturl = mcp_find_key(rest, "url");
            const char *d1 = mcp_find_key(rest, "description");
            const char *d2 = mcp_find_key(rest, "snippet");
            if (!d1 || (d2 && d2 < d1)) d = d2; else d = d1;
            if (d && (!nexturl || d < nexturl) && *d == '"') {
                mcp_take_string(d, tmp, sizeof(tmp));
                mcp_copy_trim(out[n].snippet, sizeof(out[n].snippet), tmp);
            }
        }
        n++;
        /* Advance past this url occurrence so the next scan finds the next hit
         * rather than the same one forever. */
        p = u + 1;
    }
    return n;
}

/* -------------------------------------------------------------------------- */
/*  Reply decoding                                                           */
/* -------------------------------------------------------------------------- */

/* An MCP server may answer a POST either with plain JSON or with a one-shot
 * server-sent-event stream whose payload is on the "data:" lines.  Both carry
 * the same JSON-RPC message, so normalise to the JSON text before parsing. */
static const char *mcp_opensse(const char *body, char *scratch, int cap) {
    const char *p = body;
    int n = 0;
    if (!body) return body;
    if (kstrncmp(body, "data:", 5) && kstrncmp(body, "event:", 6) && kstrncmp(body, ":", 1))
        return body;
    while (*p && n < cap - 1) {
        const char *nl = kstrchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)kstrlen(p);
        if (len > 5 && !kstrncmp(p, "data:", 5)) {
            int k = 5;
            while (k < len && (p[k] == ' ' || p[k] == '\t')) k++;
            while (k < len && n < cap - 2) scratch[n++] = p[k++];
            scratch[n++] = '\n';
        }
        if (!nl) break;
        p = nl + 1;
    }
    scratch[n] = 0;
    return n ? scratch : body;
}

/* Pull "Name: value" out of a raw response-header block, case-insensitively. */
static int mcp_header(const char *raw, const char *name, char *out, int cap) {
    int nlen = (int)kstrlen(name);
    const char *p = raw;
    if (out && cap) out[0] = 0;
    while (p && *p) {
        const char *e = kstrchr(p, '\n');
        int len = e ? (int)(e - p) : (int)kstrlen(p);
        if (len > nlen && p[nlen] == ':') {
            int i, match = 1;
            for (i = 0; i < nlen; i++) {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) { match = 0; break; }
            }
            if (match) {
                int k = nlen + 1, n = 0;
                while (k < len && (p[k] == ' ' || p[k] == '\t')) k++;
                while (k < len && p[k] != '\r' && p[k] != '\n' && n < cap - 1)
                    out[n++] = p[k++];
                out[n] = 0;
                return n;
            }
        }
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  Request lifecycle                                                        */
/* -------------------------------------------------------------------------- */

int mcp_available(void) { return net_available(); }

void mcp_init(void) {
    kmemset(&g_mcp, 0, sizeof(g_mcp));
    kstrcpy(g_mcp.note, "search is idle");
}

/* The handshake.  MCP over Streamable HTTP starts with initialize, and the
 * server hands back a session id that every later call must echo; servers that
 * do not use sessions simply omit the header and the client carries on. */
static int mcp_send_initialize(void) {
    ksnprintf(g_mcp.init_req, sizeof(g_mcp.init_req),
              "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
              "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
              "\"clientInfo\":{\"name\":\"AE\",\"version\":\"1.0\"}}}");
    kstrcpy(g_mcp.hdrs, "Accept: application/json, text/event-stream\r\n"
                        "MCP-Protocol-Version: 2025-06-18\r\n");
    g_mcp.phase = MCP_INITIALIZING;
    g_mcp.slot = net_begin_full(MCP_HOST, "POST", "application/json",
                                g_mcp.init_req, g_mcp.hdrs);
    g_mcp.started_ms = now_ms();
    if (g_mcp.slot <= 0) {
        int r = g_mcp.slot;
        g_mcp.slot = 0;
        g_mcp.phase = MCP_IDLE;
        g_mcp.failed = 1;
        kstrcpy(g_mcp.note, (r == NET_ERR_UNSUPPORTED)
                ? "no network on this chassis" : "could not reach the search server");
        return r;
    }
    return 0;
}

/* Start a search.  Returns 0 when a request is in flight or the handshake was
 * kicked off, else a NET_ERR_* code or -100.  Never blocks. */
int mcp_search(const char *query) {
    char esc[MCP_QUERY_MAX];
    int n;

    if (!query) return -100;
    while (*query == ' ') query++;
    if (!*query) return -100;

    if (g_mcp.slot > 0) net_abort(g_mcp.slot);
    g_mcp.slot = 0;
    g_mcp.hits = 0;
    g_mcp.failed = 0;
    g_mcp.done = 0;
    kstrncpy(g_mcp.query, query, sizeof(g_mcp.query) - 1);
    g_mcp.query[sizeof(g_mcp.query) - 1] = 0;
    if (g_mcp.phase == MCP_SEARCHING || g_mcp.phase == MCP_INITIALIZING)
        g_mcp.phase = MCP_IDLE;

    /* A session is needed before tools/call is accepted, so the first search on
       a fresh process pays for the handshake and every later one reuses it. */
    if (g_mcp.phase != MCP_READY) return mcp_send_initialize();

    mcp_json_escape(query, esc, sizeof(esc));
    n = ksnprintf(g_mcp.req, sizeof(g_mcp.req),
                  "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                  "\"params\":{\"name\":\"%s\",\"arguments\":{\"query\":\"%s\","
                  "\"count\":%d}}}",
                  MCP_TOOL, esc, MCP_RESULTS_MAX);
    if (n <= 0) return -100;

    /* The session id goes back as a request header, quoted verbatim. */
    ksnprintf(g_mcp.hdrs, sizeof(g_mcp.hdrs),
              "Accept: application/json, text/event-stream\r\n"
              "MCP-Protocol-Version: 2025-06-18\r\n"
              "Mcp-Session-Id: %s\r\n", g_mcp.session);
    g_mcp.phase = MCP_SEARCHING;
    g_mcp.slot = net_begin_full(MCP_HOST, "POST", "application/json",
                                g_mcp.req, g_mcp.hdrs);
    g_mcp.started_ms = now_ms();
    if (g_mcp.slot <= 0) {
        int r = g_mcp.slot;
        g_mcp.slot = 0;
        g_mcp.phase = MCP_IDLE;
        g_mcp.failed = 1;
        kstrcpy(g_mcp.note, "could not reach the search server");
        return r;
    }
    return 0;
}

/* 0 while waiting, 1 when the current stage finished (success or failure). */
int mcp_poll(void) {
    if (g_mcp.slot <= 0) return 1;
    {
        int r = net_poll(g_mcp.slot);
        if (r == 0) {
            if (now_ms() - g_mcp.started_ms > 25000) {
                net_abort(g_mcp.slot);
                g_mcp.slot = 0;
                g_mcp.phase = MCP_IDLE;
                g_mcp.failed = 1;
                kstrcpy(g_mcp.note, "search timed out");
            }
            return 0;
        }
        return 1;
    }
}

/* Collect the reply for the stage that is in flight.  Returns the number of
 * hits when a search completed, 0 when only the handshake completed, or a
 * negative code. */
int mcp_finish(void) {
    static char body[MCP_REPLY_MAX];
    static char scratch[MCP_REPLY_MAX + 256];
    int len, slot;

    if (g_mcp.slot <= 0) return -100;
    slot = g_mcp.slot;
    net_headers(slot, g_mcp.rhdr, sizeof(g_mcp.rhdr));
    len = net_finish(slot, body, sizeof(body));
    g_mcp.slot = 0;

    if (len < 0) {
        g_mcp.failed = 1;
        g_mcp.phase = MCP_IDLE;
        kstrcpy(g_mcp.note, "search failed");
        return len;
    }
    body[MCP_REPLY_MAX - 1] = 0;

    if (g_mcp.phase == MCP_INITIALIZING) {
        /* Keep the session id; it is what makes the next call authorised. */
        mcp_header(g_mcp.rhdr, "Mcp-Session-Id", g_mcp.session, sizeof(g_mcp.session));
        g_mcp.phase = MCP_READY;
        /* Re-issue the search that prompted the handshake. */
        {
            char q[MCP_QUERY_MAX];
            kstrcpy(q, g_mcp.query);
            return mcp_search(q) == 0 ? 0 : -101;
        }
    }

    {
        const char *json = mcp_opensse(body, scratch, sizeof(scratch));
        const char *err = mcp_find_key(json, "error");
        /* A rate-limit or usage reply is a normal JSON-RPC error object, so it
           must be checked before looking for results. */
        if (err && *err == '{') {
            const char *m = mcp_find_key(err, "message");
            char tmp[160];
            g_mcp.failed = 1;
            g_mcp.phase = MCP_IDLE;
            if (m) {
                static char unesc[160];
                mcp_take_string(m, unesc, sizeof(unesc));
                kstrcpy(tmp, unesc);
                kstrncpy(g_mcp.note, tmp, sizeof(g_mcp.note) - 1);
            } else {
                kstrcpy(g_mcp.note, "search server returned an error");
            }
            return -101;
        }
        if (mcp_find_key(json, "result")) {
            g_mcp.hits = mcp_extract(json, g_mcp.hit, MCP_RESULTS_MAX);
        } else {
            g_mcp.hits = mcp_extract(body, g_mcp.hit, MCP_RESULTS_MAX);
        }
    }

    g_mcp.done = 1;
    g_mcp.failed = 0;
    g_mcp.phase = MCP_READY;
    if (g_mcp.hits == 0) kstrcpy(g_mcp.note, "no results");
    else g_mcp.note[0] = 0;
    return g_mcp.hits;
}

int mcp_hits(void) { return g_mcp.hits; }
const mcp_hit_t *mcp_hit(int i) {
    if (i < 0 || i >= g_mcp.hits) return NULL;
    return &g_mcp.hit[i];
}
int mcp_busy(void) { return g_mcp.slot > 0; }
const char *mcp_note(void) { return g_mcp.note; }
const char *mcp_query(void) { return g_mcp.query; }
int mcp_daily_limit(void) { return MCP_DAILY_LIMIT; }

/* Does this text look like a search rather than an address?  The browser asks
 * before choosing a back end: a scheme or a single dotted word is a URL,
 * everything else is a query. */
int mcp_is_search(const char *s) {
    const char *p = s;
    if (!s) return 0;
    while (*p == ' ') p++;
    if (!*p) return 0;
    if (!kstrncmp(p, "http://", 7) || !kstrncmp(p, "https://", 8)) return 0;
    if (!kstrncmp(p, "ae://", 5) || !kstrncmp(p, "about:", 6)) return 0;
    {
        const char *q = p;
        while (*q) {
            if (*q == ' ') return 1;
            q++;
        }
    }
    return kstrchr(p, '.') ? 0 : 1;
}
