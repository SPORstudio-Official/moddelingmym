/* ==========================================================================
 *  APP: calc -- calculator
 *
 *  Full four-function calculator with a genuine expression evaluator
 *  (precedence-aware recursive descent, parentheses, unary minus, %, ^),
 *  a tape history, and keyboard/button input.
 * ========================================================================== */

#define CALC_BTN_W 54
#define CALC_BTN_H 40

typedef struct {
    char entry[64];
    char display[64];
    char tape[8][80];
    int  ntape;
    int  error;
    int  just_evaluated;
    int  hover_btn;
    u32  calc_ops;
} calc_t;

typedef struct {
    const char *src;
    int pos;
    int error;
} calc_parser_t;

static double calc_expr(calc_parser_t *p);

static void calc_ws(calc_parser_t *p) {
    while (p->src[p->pos] == ' ') p->pos++;
}

static double calc_primary(calc_parser_t *p) {
    double v = 0;
    calc_ws(p);
    if (p->src[p->pos] == '(') {
        p->pos++;
        v = calc_expr(p);
        calc_ws(p);
        if (p->src[p->pos] == ')') p->pos++;
        else p->error = 1;
        return v;
    }
    if (p->src[p->pos] == '-') { p->pos++; return -calc_primary(p); }
    if (p->src[p->pos] == '+') { p->pos++; return calc_primary(p); }
    if (p->src[p->pos] == 's' && !kstrncmp(p->src + p->pos, "sqrt", 4)) {
        p->pos += 4;
        return k_sqrt(calc_primary(p));
    }
    {
        int start = p->pos;
        int any = 0;
        while ((p->src[p->pos] >= '0' && p->src[p->pos] <= '9') || p->src[p->pos] == '.') {
            p->pos++;
            any = 1;
        }
        if (!any) { p->error = 1; return 0; }
        {
            char buf[48];
            int n = p->pos - start;
            if (n > 47) n = 47;
            kmemcpy(buf, p->src + start, (size_t)n);
            buf[n] = 0;
            {
                /* small atof: integer part then fraction */
                double frac = 0.0, scale = 1.0;
                int seen_dot = 0, k;
                v = 0;
                for (k = 0; buf[k]; k++) {
                    if (buf[k] == '.') { seen_dot = 1; continue; }
                    if (!seen_dot) v = v * 10.0 + (buf[k] - '0');
                    else { scale *= 10.0; frac += (buf[k] - '0') / scale; }
                }
                v += frac;
            }
        }
    }
    return v;
}

static double calc_power(calc_parser_t *p) {
    double base = calc_primary(p);
    calc_ws(p);
    if (p->src[p->pos] == '^') {
        p->pos++;
        return k_pow(base, calc_power(p));
    }
    return base;
}

static double calc_term(calc_parser_t *p) {
    double v = calc_power(p);
    for (;;) {
        calc_ws(p);
        if (p->src[p->pos] == '*' || p->src[p->pos] == '/' || p->src[p->pos] == '%') {
            char op = p->src[p->pos++];
            double rhs = calc_power(p);
            if (op == '*') v *= rhs;
            else if (op == '/') { if (rhs == 0) p->error = 2; else v /= rhs; }
            else { if (rhs == 0) p->error = 2; else v = k_fmod(v, rhs); }
        } else break;
    }
    return v;
}

static double calc_expr(calc_parser_t *p) {
    double v = calc_term(p);
    for (;;) {
        calc_ws(p);
        if (p->src[p->pos] == '+' || p->src[p->pos] == '-') {
            char op = p->src[p->pos++];
            double rhs = calc_term(p);
            v = (op == '+') ? v + rhs : v - rhs;
        } else break;
    }
    return v;
}

static void calc_format(char *out, size_t n, double v) {
    long long iv = (long long)v;
    if (v == (double)iv && v < 1e15 && v > -1e15) ksnprintf(out, n, "%lld", iv);
    else {
        ksnprintf(out, n, "%.10g", v);
    }
}

static void calc_eval(calc_t *c) {
    calc_parser_t p;
    double v;
    kmemset(&p, 0, sizeof(p));
    p.src = c->entry;
    v = calc_expr(&p);
    calc_ws(&p);
    if (p.error || p.src[p.pos]) {
        kstrcpy(c->display, p.error == 2 ? "divide by zero" : "syntax error");
        c->error = 1;
        return;
    }
    calc_format(c->display, sizeof(c->display), v);
    if (c->ntape < 8) {
        ksnprintf(c->tape[c->ntape], sizeof(c->tape[0]), "%s = %s", c->entry, c->display);
        c->ntape++;
    } else {
        int i;
        for (i = 0; i < 7; i++) kstrcpy(c->tape[i], c->tape[i + 1]);
        ksnprintf(c->tape[7], sizeof(c->tape[0]), "%s = %s", c->entry, c->display);
    }
    kstrcpy(c->entry, c->display);
    c->just_evaluated = 1;
    c->error = 0;
    c->calc_ops++;
}

static void calc_input(calc_t *c, const char *s) {
    if (c->just_evaluated) {
        char op = s[0];
        int is_op = (op == '+' || op == '-' || op == '*' || op == '/' || op == '^' || op == '%');
        if (!is_op) kstrcpy(c->entry, "");
        c->just_evaluated = 0;
    }
    if (kstrlen(c->entry) + kstrlen(s) < sizeof(c->entry) - 1)
        kstrcat(c->entry, s);
    c->error = 0;
}

static void *calc_create(const char *arg) {
    calc_t *c = (calc_t *)mm_kmalloc(sizeof(calc_t));
    (void)arg;
    if (!c) return NULL;
    kmemset(c, 0, sizeof(*c));
    kstrcpy(c->display, "0");
    return c;
}

static void calc_destroy(void *state) { if (state) mm_kfree(state); }

static const char *g_calc_buttons[5][4] = {
    { "7", "8", "9", "/" },
    { "4", "5", "6", "*" },
    { "1", "2", "3", "-" },
    { "0", ".", "=", "+" },
    { "C", "(", ")", "^" }
};

static void calc_paint(void *state, window_t *w, wm_rect_t c) {
    calc_t *calc = (calc_t *)state;
    theme_t *t = theme_get();
    int i, j;
    int bx0 = c.x + 12, by0 = c.y + 92;

    gfx_fill_rect(c.x, c.y, c.w, c.h, t->win_bg);

    /* display */
    {
        int dx = c.x + 12, dy = c.y + 10, dw = c.w - 24, dh = 56;
        gfx_round_rect(dx, dy, dw, dh, 8, RGBA(6, 10, 16, 240));
        gfx_round_rect_border(dx, dy, dw, dh, 8, calc->error ? t->err : t->border, 1);
        gfx_text(dx + 10, dy + 6, calc->entry[0] ? calc->entry : "0", t->text_mute, 0);
        gfx_text_right(dx + dw - 12, dy + 20, calc->display, calc->error ? t->err : t->text, 2);
    }
    /* tape */
    {
        int ty = c.y + c.h - 92;
        gfx_text(c.x + 14, ty - 16, "tape", t->text_mute, 0);
        for (i = 0; i < calc->ntape && i < 4; i++)
            gfx_text(c.x + 14, ty + i * 16, calc->tape[calc->ntape - 1 - i], t->text_dim, 0);
    }
    /* keypad */
    for (i = 0; i < 5; i++) {
        for (j = 0; j < 4; j++) {
            const char *lbl = g_calc_buttons[i][j];
            int bx = bx0 + j * (CALC_BTN_W + 6);
            int by = by0 + i * (CALC_BTN_H + 6);
            int hover = (g_wm.mouse_px >= bx && g_wm.mouse_px < bx + CALC_BTN_W &&
                         g_wm.mouse_py >= by && g_wm.mouse_py < by + CALC_BTN_H);
            u32 bg;
            if (!kstrcmp(lbl, "=")) bg = hover ? RGB(70, 180, 120) : RGB(46, 140, 90);
            else if (!kstrcmp(lbl, "C")) bg = hover ? RGB(210, 80, 90) : RGB(150, 50, 60);
            else if (lbl[0] == '/' || lbl[0] == '*' || lbl[0] == '-' || lbl[0] == '+' || lbl[0] == '^')
                bg = hover ? RGBA(90, 120, 170, 255) : RGBA(50, 66, 92, 255);
            else bg = hover ? t->sel : RGBA(34, 44, 60, 240);
            gfx_round_rect_grad(bx, by, CALC_BTN_W, CALC_BTN_H, 6, bg,
                                RGBA(RED(bg) * 3 / 4, GREEN(bg) * 3 / 4, BLUE(bg) * 3 / 4, 255));
            gfx_text_center(bx + CALC_BTN_W / 2, by + 12, lbl, t->text, 1);
        }
    }
    (void)w;
}

static void calc_tick(void *state, u32 dt) { (void)state; (void)dt; }

static void calc_key(void *state, int key, int ch, int mods) {
    calc_t *c = (calc_t *)state;
    char s[2];
    (void)mods;
    if (key == 0x0D) { calc_eval(c); return; }
    if (key == 0x08) {
        int n = (int)kstrlen(c->entry);
        if (n > 0) c->entry[n - 1] = 0;
        return;
    }
    s[0] = (char)ch;
    s[1] = 0;
    if ((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '*' ||
        ch == '/' || ch == '(' || ch == ')' || ch == '.' || ch == '^' || ch == '%') {
        calc_input(c, s);
        return;
    }
    if (ch == 'c' || ch == 'C') { kstrcpy(c->entry, ""); kstrcpy(c->display, "0"); return; }
}

static void calc_mouse(void *state, int event, int x, int y, int button, int wheel) {
    calc_t *c = (calc_t *)state;
    wm_rect_t cc = wm_input_client_rect();
    int i, j;
    (void)button; (void)wheel;
    if (event != APP_EV_MOUSE_DOWN) return;
    for (i = 0; i < 5; i++) {
        for (j = 0; j < 4; j++) {
            int bx = cc.x + 12 + j * (CALC_BTN_W + 6);
            int by = cc.y + 92 + i * (CALC_BTN_H + 6);
            if (x >= bx && x < bx + CALC_BTN_W && y >= by && y < by + CALC_BTN_H) {
                const char *lbl = g_calc_buttons[i][j];
                if (!kstrcmp(lbl, "=")) calc_eval(c);
                else if (!kstrcmp(lbl, "C")) { kstrcpy(c->entry, ""); kstrcpy(c->display, "0"); c->error = 0; }
                else calc_input(c, lbl);
                return;
            }
        }
    }
}

const app_desc_t app_calc = {
    "calc", "Calculator", "expression math", ICON_CALC,
    272, 380, 248, 340, APP_CAT_UTIL, 0, 1,
    calc_create, calc_destroy, calc_paint, calc_key, calc_mouse, calc_tick, NULL
};
