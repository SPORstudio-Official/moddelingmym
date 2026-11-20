/* ==========================================================================
 *  CC/VM -- AetherC subset compiler + bytecode virtual machine
 *
 *  AetherC is a small C-family language:
 *      var x = 3;   x = x + 1;   x += 2;
 *      if (..) {..} else {..}    while (..) {..}
 *      for (var i = 0; i < 10; i = i + 1) {..}
 *      func add(a, b) { return a + b; }
 *      print("value = ", 42);    println("done");
 *      draw_rect(x, y, w, h, color);  clear(color);  sleep(ms);
 *      time()  rand(n)  malloc(bytes)
 *
 *  Real recursive-descent front end: lexer -> signature pre-pass -> code gen
 *  producing stack bytecode with proper call frames.  The VM is a stack
 *  machine that can run synchronously or as a scheduled OS process.
 *
 *  On-disk image (".aeb"): header | code[] | funcs[] | strings[]
 * ========================================================================== */

enum {
    OP_HALT = 0, OP_PUSH, OP_POP, OP_DUP, OP_SWAP,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR, OP_LNOT,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_AND, OP_OR,
    OP_JMP, OP_JZ, OP_JNZ, OP_CALL, OP_RET, OP_ENTER,
    OP_LOADI, OP_STOREI, OP_LOADG, OP_STOREG,
    OP_PUTS, OP_PRINTI, OP_NL,
    OP_DRAW, OP_CLEAR, OP_SLEEP, OP_TIME, OP_RAND, OP_ALLOC, OP_FREE,
    OP_COUNT
};

#define VM_STACK       768
#define VM_CALLSTACK   128
#define VM_GLOBALS     128
#define VM_HEAP_BLOCKS 16
#define CC_MAX_CODE    4096
#define CC_MAX_FUNCS   32
#define CC_MAX_VARS    64
#define CC_MAX_LOCALS  48
#define CC_MAX_STR     192
#define CC_STR_LEN     64
#define CC_MAX_LOOPS   8
#define CC_MAX_BREAKS  24
#define CC_ERR_LEN     192
#define VM_MAX_PROGS   16
#define CC_ADDR_UNSET  0xFFFFFFFFu

typedef struct { i16 op; i32 arg; } insn_t;

typedef struct {
    char name[24];
    u32  addr;
    i32  nparams;
    i32  nlocals;
} cc_fn_t;

typedef struct {
    char magic[4];              /* "AEBX"                                   */
    u32  version;
    u32  code_len;
    u32  nfuncs;
    u32  nstrings;
    u32  checksum;
} vm_header_t;

typedef struct {
    char    name[32];
    insn_t  code[CC_MAX_CODE];
    u32     code_len;
    cc_fn_t funcs[CC_MAX_FUNCS];
    int     nfuncs;
    char    strings[CC_MAX_STR][CC_STR_LEN];
    int     nstrings;
    char    error[CC_ERR_LEN];
    int     failed;
    u32     checksum;
} cc_program_t;

typedef struct {
    i32  st[VM_STACK];
    i32  globals[VM_GLOBALS];
    u32  cstack_pc[VM_CALLSTACK];
    u32  cstack_fp[VM_CALLSTACK];
    int  sp, fp, csp;
    u32  pc;
    int  steps;
    int  halted;
    i32  result;
    i32  heap_handle[VM_HEAP_BLOCKS];
    char out[6144];
    u32  out_len;
    int  overflow;
} vm_state_t;

typedef struct {
    char   name[40];
    cc_program_t *prog;
    vm_state_t   *vm;
    u32    pid;
    u32    window_id;
    int    active;
    int    finished;
} vm_process_t;

int cc_debug_dump;

static cc_program_t *g_last_prog;
static vm_process_t  g_vmprocs[VM_MAX_PROGS];
static u32 g_vm_sched_pid;

cc_program_t *cc_last_program(void) { return g_last_prog; }

vm_process_t *cc_vm_process(int i) {
    if (i < 0 || i >= VM_MAX_PROGS) return NULL;
    return g_vmprocs[i].active ? &g_vmprocs[i] : NULL;
}

/* --------------------------------------------------------------------------
 *  Draw requests from VM programs (drained by the VM console application)
 * -------------------------------------------------------------------------- */
typedef struct { i32 x, y, w, h; u32 color; } vm_draw_t;
static vm_draw_t g_vm_draw[64];
static int g_vm_draw_count;
static u32 g_vm_clear_color;
static int g_vm_clear_pending;

void cc_vm_draw_request(i32 x, i32 y, i32 w, i32 h, u32 color) {
    if (g_vm_draw_count >= 64) return;
    g_vm_draw[g_vm_draw_count].x = x;
    g_vm_draw[g_vm_draw_count].y = y;
    g_vm_draw[g_vm_draw_count].w = w;
    g_vm_draw[g_vm_draw_count].h = h;
    g_vm_draw[g_vm_draw_count].color = color;
    g_vm_draw_count++;
}
void cc_vm_clear_request(u32 color) { g_vm_clear_color = color; g_vm_clear_pending = 1; }
void cc_vm_take_draws(vm_draw_t **list, int *count, u32 *clear_color, int *clear_pending) {
    *list = g_vm_draw;
    *count = g_vm_draw_count;
    *clear_color = g_vm_clear_color;
    *clear_pending = g_vm_clear_pending;
    g_vm_draw_count = 0;
    g_vm_clear_pending = 0;
}

/* -------------------------------------------------------------------------- */
/*  Lexer                                                                    */
/* -------------------------------------------------------------------------- */

enum {
    T_EOF = 0, T_NUM, T_IDENT, T_STR,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_COMMA, T_SEMI,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_ASSIGN, T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_AND, T_OR, T_NOT, T_AMP, T_PIPE, T_CARET, T_TILDE, T_SHL, T_SHR,
    T_PLUSEQ, T_MINUSEQ, T_STAREQ
};

typedef struct {
    int  type;
    i32  num;
    char text[CC_STR_LEN];
    int  line;
} token_t;

typedef struct {
    const char *src;
    int   pos;
    int   line;
    token_t tok;
    char  error[CC_ERR_LEN];
    int   failed;
} lexer_t;

static char lx_peek(lexer_t *lx, int ahead) {
    int p = lx->pos + ahead;
    if (p < 0) return 0;
    return lx->src[p];
}

static char lx_get(lexer_t *lx) {
    char c = lx->src[lx->pos];
    if (!c) return 0;
    lx->pos++;
    if (c == '\n') lx->line++;
    return c;
}

static void lex_token(lexer_t *lx) {
    char c;
    kmemset(&lx->tok, 0, sizeof(lx->tok));
    for (;;) {
        c = lx_peek(lx, 0);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { lx_get(lx); continue; }
        if (c == '/' && lx_peek(lx, 1) == '/') {
            while (lx_peek(lx, 0) && lx_peek(lx, 0) != '\n') lx_get(lx);
            continue;
        }
        if (c == '/' && lx_peek(lx, 1) == '*') {
            lx_get(lx); lx_get(lx);
            while (lx_peek(lx, 0) && !(lx_peek(lx, 0) == '*' && lx_peek(lx, 1) == '/')) lx_get(lx);
            if (lx_peek(lx, 0)) { lx_get(lx); lx_get(lx); }
            continue;
        }
        if (c == '#') {
            while (lx_peek(lx, 0) && lx_peek(lx, 0) != '\n') lx_get(lx);
            continue;
        }
        break;
    }
    lx->tok.line = lx->line;
    c = lx_peek(lx, 0);
    if (!c) { lx->tok.type = T_EOF; return; }

    if (c >= '0' && c <= '9') {
        if (c == '0' && (lx_peek(lx, 1) == 'x' || lx_peek(lx, 1) == 'X')) {
            int v = 0;
            lx_get(lx); lx_get(lx);
            while ((lx_peek(lx, 0) >= '0' && lx_peek(lx, 0) <= '9') ||
                   (lx_peek(lx, 0) >= 'a' && lx_peek(lx, 0) <= 'f') ||
                   (lx_peek(lx, 0) >= 'A' && lx_peek(lx, 0) <= 'F')) {
                char h = lx_get(lx);
                int d = (h <= '9') ? h - '0' : ((h | 32) - 'a' + 10);
                v = v * 16 + d;
            }
            lx->tok.num = (i32)v;
        } else {
            i32 v = 0;
            while (lx_peek(lx, 0) >= '0' && lx_peek(lx, 0) <= '9')
                v = v * 10 + (lx_get(lx) - '0');
            lx->tok.num = v;
        }
        lx->tok.type = T_NUM;
        return;
    }
    if (c == '"') {
        int n = 0;
        lx_get(lx);
        lx->tok.type = T_STR;
        while (lx_peek(lx, 0) && lx_peek(lx, 0) != '"') {
            char ch = lx_get(lx);
            if (ch == '\\') {
                char e = lx_get(lx);
                if (e == 'n') ch = '\n';
                else if (e == 't') ch = '\t';
                else if (e == 'r') ch = '\r';
                else if (e == '0') ch = ' ';
                else ch = e;
            }
            if (n < CC_STR_LEN - 1) lx->tok.text[n++] = ch;
        }
        if (lx_peek(lx, 0) == '"') lx_get(lx);
        else if (!lx->failed) {
            lx->failed = 1;
            ksnprintf(lx->error, sizeof(lx->error), "line %d: unterminated string", lx->line);
        }
        lx->tok.text[n] = 0;
        return;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        int n = 0;
        while ((lx_peek(lx, 0) >= 'a' && lx_peek(lx, 0) <= 'z') ||
               (lx_peek(lx, 0) >= 'A' && lx_peek(lx, 0) <= 'Z') ||
               (lx_peek(lx, 0) >= '0' && lx_peek(lx, 0) <= '9') ||
               lx_peek(lx, 0) == '_') {
            char ch = lx_get(lx);
            if (n < CC_STR_LEN - 1) lx->tok.text[n++] = ch;
        }
        lx->tok.text[n] = 0;
        lx->tok.type = T_IDENT;
        return;
    }
    lx_get(lx);
    switch (c) {
    case '(': lx->tok.type = T_LPAREN; return;
    case ')': lx->tok.type = T_RPAREN; return;
    case '{': lx->tok.type = T_LBRACE; return;
    case '}': lx->tok.type = T_RBRACE; return;
    case ',': lx->tok.type = T_COMMA; return;
    case ';': lx->tok.type = T_SEMI; return;
    case '+':
        if (lx_peek(lx, 0) == '=') { lx_get(lx); lx->tok.type = T_PLUSEQ; }
        else lx->tok.type = T_PLUS;
        return;
    case '-':
        if (lx_peek(lx, 0) == '=') { lx_get(lx); lx->tok.type = T_MINUSEQ; }
        else lx->tok.type = T_MINUS;
        return;
    case '*':
        if (lx_peek(lx, 0) == '=') { lx_get(lx); lx->tok.type = T_STAREQ; }
        else lx->tok.type = T_STAR;
        return;
    case '/': lx->tok.type = T_SLASH; return;
    case '%': lx->tok.type = T_PERCENT; return;
    case '=':
        if (lx_peek(lx, 0) == '=') { lx_get(lx); lx->tok.type = T_EQ; }
        else lx->tok.type = T_ASSIGN;
        return;
    case '!':
        if (lx_peek(lx, 0) == '=') { lx_get(lx); lx->tok.type = T_NE; }
        else lx->tok.type = T_NOT;
        return;
    case '<':
        if (lx_peek(lx, 0) == '=') { lx_get(lx); lx->tok.type = T_LE; }
        else if (lx_peek(lx, 0) == '<') { lx_get(lx); lx->tok.type = T_SHL; }
        else lx->tok.type = T_LT;
        return;
    case '>':
        if (lx_peek(lx, 0) == '=') { lx_get(lx); lx->tok.type = T_GE; }
        else if (lx_peek(lx, 0) == '>') { lx_get(lx); lx->tok.type = T_SHR; }
        else lx->tok.type = T_GT;
        return;
    case '&':
        if (lx_peek(lx, 0) == '&') { lx_get(lx); lx->tok.type = T_AND; }
        else lx->tok.type = T_AMP;
        return;
    case '|':
        if (lx_peek(lx, 0) == '|') { lx_get(lx); lx->tok.type = T_OR; }
        else lx->tok.type = T_PIPE;
        return;
    case '^': lx->tok.type = T_CARET; return;
    case '~': lx->tok.type = T_TILDE; return;
    default:
        if (!lx->failed) {
            lx->failed = 1;
            ksnprintf(lx->error, sizeof(lx->error),
                      "line %d: unexpected character '%c' (0x%02X)",
                      lx->line, c, (unsigned)(u8)c);
        }
        lx->tok.type = T_EOF;
        return;
    }
}

/* -------------------------------------------------------------------------- */
/*  Compiler                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    char name[32];
    int  slot;              /* frame slot for locals, -1 for globals        */
    int  global;            /* global index, or -1                          */
} cc_var_t;

typedef struct {
    int nbreak;
    int brk[CC_MAX_BREAKS];
    int ncont;
    int cont[CC_MAX_BREAKS];
    int cont_target;        /* -1 = patch after the body                    */
} cc_loop_t;

typedef struct {
    lexer_t lx;
    cc_program_t *prog;
    cc_var_t vars[CC_MAX_VARS];
    int  nvars;
    int  in_function;
    int  loop_depth;
    cc_loop_t loops[CC_MAX_LOOPS];
    int  enter_patch;       /* position of this function's OP_ENTER         */
} cc_t;

static void cc_emit(cc_t *cc, int op, i32 arg) {
    cc_program_t *p = cc->prog;
    if (p->code_len >= CC_MAX_CODE) {
        if (!p->failed) {
            p->failed = 1;
            ksnprintf(p->error, sizeof(p->error),
                      "program exceeds %u instructions", (unsigned)CC_MAX_CODE);
        }
        return;
    }
    p->code[p->code_len].op = (i16)op;
    p->code[p->code_len].arg = arg;
    p->code_len++;
}

static u32 cc_pos(cc_t *cc) { return cc->prog->code_len; }

static void cc_patch_to_here(cc_t *cc, u32 at) {
    if (at < cc->prog->code_len) cc->prog->code[at].arg = (i32)cc->prog->code_len;
}

static void cc_error(cc_t *cc, const char *fmt, ...) {
    va_list ap;
    if (cc->prog->failed) return;
    cc->prog->failed = 1;
    va_start(ap, fmt);
    kvsnprintf(cc->prog->error, sizeof(cc->prog->error), fmt, ap);
    va_end(ap);
}

static void cc_next(cc_t *cc) {
    lex_token(&cc->lx);
    if (cc->lx.failed && !cc->prog->failed) {
        cc->prog->failed = 1;
        ksnprintf(cc->prog->error, sizeof(cc->prog->error), "%s", cc->lx.error);
    }
}

static int cc_accept(cc_t *cc, int t) {
    if (cc->lx.tok.type == t) { cc_next(cc); return 1; }
    return 0;
}

static int cc_expect(cc_t *cc, int t, const char *what) {
    if (cc->lx.tok.type == t) { cc_next(cc); return 1; }
    cc_error(cc, "line %d: expected %s", cc->lx.tok.line, what);
    return 0;
}

static int cc_is(cc_t *cc, const char *kw) {
    return cc->lx.tok.type == T_IDENT && !kstrcmp(cc->lx.tok.text, kw);
}

static int cc_find_var(cc_t *cc, const char *name) {
    int i;
    for (i = 0; i < cc->nvars; i++)
        if (!kstrcmp(cc->vars[i].name, name)) return i;
    return -1;
}

static int cc_declare(cc_t *cc, const char *name) {
    int i = cc_find_var(cc, name);
    if (i >= 0) {
        cc_error(cc, "line %d: '%s' is already declared", cc->lx.tok.line, name);
        return -1;
    }
    if (cc->nvars >= CC_MAX_VARS) { cc_error(cc, "too many variables in scope"); return -1; }
    kstrncpy(cc->vars[cc->nvars].name, name, 31);
    if (cc->in_function) {
        int slots = 0, j;
        for (j = 0; j < cc->nvars; j++)
            if (cc->vars[j].slot >= slots) slots = cc->vars[j].slot + 1;
        if (slots >= CC_MAX_LOCALS) { cc_error(cc, "too many locals (max %d)", CC_MAX_LOCALS); return -1; }
        cc->vars[cc->nvars].slot = slots;
        cc->vars[cc->nvars].global = -1;
    } else {
        cc->vars[cc->nvars].slot = -1;
        cc->vars[cc->nvars].global = cc->nvars;
    }
    return cc->nvars++;
}

static int cc_find_func(cc_t *cc, const char *name) {
    int i;
    for (i = 0; i < cc->prog->nfuncs; i++)
        if (!kstrcmp(cc->prog->funcs[i].name, name)) return i;
    return -1;
}

static int cc_add_string(cc_t *cc, const char *s) {
    cc_program_t *p = cc->prog;
    int i;
    for (i = 0; i < p->nstrings; i++)
        if (!kstrcmp(p->strings[i], s)) return i;
    if (p->nstrings >= CC_MAX_STR) return 0;
    kstrncpy(p->strings[p->nstrings], s, CC_STR_LEN - 1);
    return p->nstrings++;
}

static void cc_expr(cc_t *cc);
static void cc_stmt(cc_t *cc);
static void cc_print_args(cc_t *cc);

static int cc_binop_prec(int t) {
    switch (t) {
    case T_OR: return 1;
    case T_AND: return 2;
    case T_PIPE: return 3;
    case T_CARET: return 4;
    case T_AMP: return 5;
    case T_EQ: case T_NE: return 6;
    case T_LT: case T_LE: case T_GT: case T_GE: return 7;
    case T_SHL: case T_SHR: return 8;
    case T_PLUS: case T_MINUS: return 9;
    case T_STAR: case T_SLASH: case T_PERCENT: return 10;
    default: return -1;
    }
}

static int cc_binop_code(int t) {
    switch (t) {
    case T_PLUS: return OP_ADD;
    case T_MINUS: return OP_SUB;
    case T_STAR: return OP_MUL;
    case T_SLASH: return OP_DIV;
    case T_PERCENT: return OP_MOD;
    case T_EQ: return OP_EQ;
    case T_NE: return OP_NE;
    case T_LT: return OP_LT;
    case T_LE: return OP_LE;
    case T_GT: return OP_GT;
    case T_GE: return OP_GE;
    case T_AND: return OP_AND;
    case T_OR: return OP_OR;
    case T_AMP: return OP_BAND;
    case T_PIPE: return OP_BOR;
    case T_CARET: return OP_BXOR;
    case T_SHL: return OP_SHL;
    case T_SHR: return OP_SHR;
    default: return OP_HALT;
    }
}

static void cc_emit_primary(cc_t *cc) {
    cc_program_t *p = cc->prog;
    switch (cc->lx.tok.type) {
    case T_NUM:
        cc_emit(cc, OP_PUSH, cc->lx.tok.num);
        cc_next(cc);
        return;
    case T_STR:
        cc_emit(cc, OP_PUTS, cc_add_string(cc, cc->lx.tok.text));
        cc_emit(cc, OP_PUSH, 0);
        cc_next(cc);
        return;
    case T_LPAREN:
        cc_next(cc);
        cc_expr(cc);
        cc_expect(cc, T_RPAREN, "')'");
        return;
    case T_MINUS:
        cc_next(cc);
        cc_emit_primary(cc);
        cc_emit(cc, OP_NEG, 0);
        return;
    case T_NOT:
        cc_next(cc);
        cc_emit_primary(cc);
        cc_emit(cc, OP_LNOT, 0);
        return;
    case T_TILDE:
        cc_next(cc);
        cc_emit_primary(cc);
        cc_emit(cc, OP_PUSH, -1);
        cc_emit(cc, OP_BXOR, 0);
        return;
    case T_PLUS:
        cc_next(cc);
        cc_emit_primary(cc);
        return;
    case T_IDENT: {
        char name[32];
        kstrncpy(name, cc->lx.tok.text, 31);
        name[31] = 0;
        cc_next(cc);
        if (cc->lx.tok.type == T_LPAREN) {
            /* -------- call -------- */
            int argc = 0;
            int fidx = cc_find_func(cc, name);
            int is_builtin =
                !kstrcmp(name, "print") || !kstrcmp(name, "println") ||
                !kstrcmp(name, "draw_rect") || !kstrcmp(name, "clear") ||
                !kstrcmp(name, "sleep") || !kstrcmp(name, "time") ||
                !kstrcmp(name, "rand") || !kstrcmp(name, "malloc");
            if (!kstrcmp(name, "print") || !kstrcmp(name, "println")) {
                int nl = !kstrcmp(name, "println");
                cc_print_args(cc);
                if (nl) cc_emit(cc, OP_NL, 0);
                cc_emit(cc, OP_PUSH, 0);
                return;
            }
            cc_next(cc);        /* consume '(' */
            while (cc->lx.tok.type != T_RPAREN && cc->lx.tok.type != T_EOF && !p->failed) {
                cc_expr(cc);
                argc++;
                if (!cc_accept(cc, T_COMMA)) break;
            }
            cc_expect(cc, T_RPAREN, "')'");
            if (p->failed) return;
            {
                int leaves_value = 0;
            if (!kstrcmp(name, "draw_rect")) {
                if (argc != 5) cc_error(cc, "draw_rect() takes 5 arguments (x,y,w,h,color)");
                else cc_emit(cc, OP_DRAW, 0);
            } else if (!kstrcmp(name, "clear")) {
                if (argc != 1) cc_error(cc, "clear() takes 1 argument (color)");
                else cc_emit(cc, OP_CLEAR, 0);
            } else if (!kstrcmp(name, "sleep")) {
                if (argc != 1) cc_error(cc, "sleep() takes 1 argument (milliseconds)");
                else cc_emit(cc, OP_SLEEP, 0);
            } else if (!kstrcmp(name, "time")) {
                cc_emit(cc, OP_TIME, 0);
            } else if (!kstrcmp(name, "rand")) {
                if (argc > 1) cc_error(cc, "rand() takes 0 or 1 argument");
                cc_emit(cc, OP_RAND, argc);
            } else if (!kstrcmp(name, "malloc")) {
                if (argc != 1) cc_error(cc, "malloc() takes 1 argument (bytes)");
                cc_emit(cc, OP_ALLOC, 0);
            } else if (is_builtin) {
                cc_error(cc, "unknown builtin '%s'", name);
            } else if (fidx < 0) {
                cc_error(cc, "line %d: call to undefined function '%s'",
                         cc->lx.tok.line, name);
            } else {
                if (argc != (int)p->funcs[fidx].nparams)
                    cc_error(cc, "%s() expects %d argument(s), got %d", name,
                             (int)p->funcs[fidx].nparams, argc);
                cc_emit(cc, OP_CALL, (i32)fidx);
                leaves_value = 1;
            }
            if (!leaves_value) cc_emit(cc, OP_PUSH, 0);   /* builtins: dummy */
            }
            return;
        }
        if (cc->lx.tok.type == T_ASSIGN || cc->lx.tok.type == T_PLUSEQ ||
            cc->lx.tok.type == T_MINUSEQ || cc->lx.tok.type == T_STAREQ) {
            int op = cc->lx.tok.type;
            int vi = cc_find_var(cc, name);
            if (vi < 0) {
                cc_error(cc, "line %d: assignment to undeclared variable '%s'",
                         cc->lx.tok.line, name);
                return;
            }
            cc_next(cc);
            if (op != T_ASSIGN) {
                if (cc->vars[vi].global >= 0) cc_emit(cc, OP_LOADG, cc->vars[vi].global);
                else cc_emit(cc, OP_LOADI, cc->vars[vi].slot);
            }
            cc_expr(cc);
            if (op == T_PLUSEQ) cc_emit(cc, OP_ADD, 0);
            else if (op == T_MINUSEQ) cc_emit(cc, OP_SUB, 0);
            else if (op == T_STAREQ) cc_emit(cc, OP_MUL, 0);
            if (cc->vars[vi].global >= 0) cc_emit(cc, OP_STOREG, cc->vars[vi].global);
            else cc_emit(cc, OP_STOREI, cc->vars[vi].slot);
            return;
        }
        {
            int vi = cc_find_var(cc, name);
            if (vi < 0) {
                cc_error(cc, "line %d: undefined variable '%s' (declare with var)",
                         cc->lx.tok.line, name);
                return;
            }
            if (cc->vars[vi].global >= 0) cc_emit(cc, OP_LOADG, cc->vars[vi].global);
            else cc_emit(cc, OP_LOADI, cc->vars[vi].slot);
        }
        return;
    }
    default:
        cc_error(cc, "line %d: unexpected token in expression", cc->lx.tok.line);
        return;
    }
}

static void cc_expr_prec(cc_t *cc, int minprec) {
    cc_emit_primary(cc);
    while (!cc->prog->failed) {
        int prec = cc_binop_prec(cc->lx.tok.type);
        int code;
        if (prec < minprec) break;
        code = cc_binop_code(cc->lx.tok.type);
        cc_next(cc);
        cc_expr_prec(cc, prec + 1);
        cc_emit(cc, code, 0);
    }
}

static void cc_expr(cc_t *cc) { cc_expr_prec(cc, 1); }

/* print(a, b, ...) -- strings are emitted inline, values are int-printed. */
static void cc_print_args(cc_t *cc) {
    cc_expect(cc, T_LPAREN, "'('");
    if (cc->lx.tok.type == T_RPAREN) {
        cc_emit(cc, OP_NL, 0);
        cc_next(cc);
        return;
    }
    for (;;) {
        if (cc->lx.tok.type == T_STR) {
            cc_emit(cc, OP_PUTS, cc_add_string(cc, cc->lx.tok.text));
            cc_next(cc);
        } else {
            cc_expr(cc);
            cc_emit(cc, OP_PRINTI, 0);
        }
        if (!cc_accept(cc, T_COMMA)) break;
    }
    cc_expect(cc, T_RPAREN, "')'");
}

static void cc_loop_begin(cc_t *cc, int cont_target) {
    if (cc->loop_depth >= CC_MAX_LOOPS) {
        cc_error(cc, "loops nested too deeply");
        return;
    }
    cc->loops[cc->loop_depth].nbreak = 0;
    cc->loops[cc->loop_depth].ncont = 0;
    cc->loops[cc->loop_depth].cont_target = cont_target;
    cc->loop_depth++;
}

static void cc_loop_end(cc_t *cc) {
    cc_loop_t *l;
    int i;
    if (cc->loop_depth <= 0) return;
    l = &cc->loops[cc->loop_depth - 1];
    for (i = 0; i < l->ncont; i++) {
        if (l->cont_target >= 0) cc->prog->code[l->cont[i]].arg = l->cont_target;
        else cc->prog->code[l->cont[i]].arg = (i32)cc->prog->code_len;
    }
    for (i = 0; i < l->nbreak; i++) cc_patch_to_here(cc, (u32)l->brk[i]);
    cc->loop_depth--;
}

/* for-loop step code is compiled into per-nesting-level scratch space and then
 * appended after the loop body (innermost loops cannot clobber outer steps). */
static cc_program_t g_cc_scratch[CC_MAX_LOOPS];

static void cc_stmt(cc_t *cc) {
    cc_program_t *p = cc->prog;
    if (p->failed) return;

    if (cc->lx.tok.type == T_SEMI) { cc_next(cc); return; }
    if (cc->lx.tok.type == T_LBRACE) {
        cc_next(cc);
        while (cc->lx.tok.type != T_RBRACE && cc->lx.tok.type != T_EOF && !p->failed)
            cc_stmt(cc);
        cc_expect(cc, T_RBRACE, "'}'");
        return;
    }
    if (cc->lx.tok.type == T_IDENT) {
        const char *kw = cc->lx.tok.text;

        if (!kstrcmp(kw, "var") || !kstrcmp(kw, "int")) {
            char name[32];
            int vi;
            cc_next(cc);
            if (cc->lx.tok.type != T_IDENT) { cc_error(cc, "expected a variable name"); return; }
            kstrncpy(name, cc->lx.tok.text, 31);
            name[31] = 0;
            cc_next(cc);
            vi = cc_declare(cc, name);
            if (vi < 0) return;
            if (cc_accept(cc, T_ASSIGN)) cc_expr(cc);
            else cc_emit(cc, OP_PUSH, 0);
            if (cc->vars[vi].global >= 0) cc_emit(cc, OP_STOREG, cc->vars[vi].global);
            else cc_emit(cc, OP_STOREI, cc->vars[vi].slot);
            cc_emit(cc, OP_POP, 1);
            cc_expect(cc, T_SEMI, "';'");
            return;
        }
        if (!kstrcmp(kw, "if")) {
            u32 jz;
            cc_next(cc);
            cc_expect(cc, T_LPAREN, "'('");
            cc_expr(cc);
            cc_expect(cc, T_RPAREN, "')'");
            jz = cc_pos(cc);
            cc_emit(cc, OP_JZ, 0);
            cc_stmt(cc);
            if (cc_is(cc, "else")) {
                u32 jend;
                cc_next(cc);
                jend = cc_pos(cc);
                cc_emit(cc, OP_JMP, 0);
                cc_patch_to_here(cc, jz);
                cc_stmt(cc);
                cc_patch_to_here(cc, jend);
            } else {
                cc_patch_to_here(cc, jz);
            }
            return;
        }
        if (!kstrcmp(kw, "while")) {
            u32 top = cc_pos(cc);
            u32 jz;
            cc_next(cc);
            cc_expect(cc, T_LPAREN, "'('");
            cc_expr(cc);
            cc_expect(cc, T_RPAREN, "')'");
            jz = cc_pos(cc);
            cc_emit(cc, OP_JZ, 0);
            cc_loop_begin(cc, (int)top);
            cc_stmt(cc);
            cc_loop_end(cc);
            cc_emit(cc, OP_JMP, (i32)top);
            cc_patch_to_here(cc, jz);
            return;
        }
        if (!kstrcmp(kw, "for")) {
            u32 top, jz;
            cc_program_t *real = cc->prog;
            int loop_slot;
            cc_next(cc);
            cc_expect(cc, T_LPAREN, "'('");
            /* init */
            if (cc->lx.tok.type != T_SEMI) {
                if (cc_is(cc, "var") || cc_is(cc, "int")) {
                    char name[32];
                    int vi;
                    cc_next(cc);
                    if (cc->lx.tok.type != T_IDENT) { cc_error(cc, "expected loop variable"); return; }
                    kstrncpy(name, cc->lx.tok.text, 31);
                    name[31] = 0;
                    cc_next(cc);
                    vi = cc_declare(cc, name);
                    if (vi < 0) return;
                    if (cc_accept(cc, T_ASSIGN)) cc_expr(cc);
                    else cc_emit(cc, OP_PUSH, 0);
                    if (cc->vars[vi].global >= 0) cc_emit(cc, OP_STOREG, cc->vars[vi].global);
                    else cc_emit(cc, OP_STOREI, cc->vars[vi].slot);
                    cc_emit(cc, OP_POP, 1);
                } else {
                    cc_expr(cc);
                    cc_emit(cc, OP_POP, 1);
                }
            }
            cc_expect(cc, T_SEMI, "';'");
            /* cond */
            top = cc_pos(cc);
            if (cc->lx.tok.type != T_SEMI) cc_expr(cc);
            else cc_emit(cc, OP_PUSH, 1);
            cc_expect(cc, T_SEMI, "';'");
            jz = cc_pos(cc);
            cc_emit(cc, OP_JZ, 0);
            /* step -> scratch (one buffer per nesting level) */
            loop_slot = cc->loop_depth;
            if (loop_slot >= CC_MAX_LOOPS) { cc_error(cc, "loops nested too deeply"); return; }
            kmemset(&g_cc_scratch[loop_slot], 0, sizeof(g_cc_scratch[loop_slot]));
            cc->prog = &g_cc_scratch[loop_slot];
            if (cc->lx.tok.type != T_RPAREN) {
                cc_expr(cc);
                cc_emit(cc, OP_POP, 1);
            }
            cc->prog = real;
            if (g_cc_scratch[loop_slot].failed && !p->failed) {
                p->failed = 1;
                ksnprintf(p->error, sizeof(p->error), "%s", g_cc_scratch[loop_slot].error);
            }
            cc_expect(cc, T_RPAREN, "')'");
            cc_loop_begin(cc, -1);
            cc_stmt(cc);
            /* append the step, then patch continue to point at it */
            {
                u32 i;
                if (loop_slot < CC_MAX_LOOPS) {
                    int j;
                    for (j = 0; j < cc->loops[loop_slot].ncont; j++)
                        cc->prog->code[cc->loops[loop_slot].cont[j]].arg = (i32)cc_pos(cc);
                }
                for (i = 0; i < g_cc_scratch[loop_slot].code_len; i++)
                    cc_emit(cc, g_cc_scratch[loop_slot].code[i].op,
                            g_cc_scratch[loop_slot].code[i].arg);
            }
            cc_loop_end(cc);
            cc_emit(cc, OP_JMP, (i32)top);
            cc_patch_to_here(cc, jz);
            return;
        }
        if (!kstrcmp(kw, "break") || !kstrcmp(kw, "continue")) {
            int is_break = !kstrcmp(kw, "break");
            cc_next(cc);
            cc_expect(cc, T_SEMI, "';'");
            if (cc->loop_depth <= 0) {
                cc_error(cc, "'%s' used outside a loop", is_break ? "break" : "continue");
                return;
            }
            {
                cc_loop_t *l = &cc->loops[cc->loop_depth - 1];
                if (is_break) {
                    if (l->nbreak < CC_MAX_BREAKS) {
                        l->brk[l->nbreak++] = (int)cc_pos(cc);
                        cc_emit(cc, OP_JMP, 0);
                    }
                } else {
                    if (l->ncont < CC_MAX_BREAKS) {
                        l->cont[l->ncont++] = (int)cc_pos(cc);
                        cc_emit(cc, OP_JMP, 0);
                    }
                }
            }
            return;
        }
        if (!kstrcmp(kw, "return")) {
            cc_next(cc);
            if (cc->lx.tok.type == T_SEMI) cc_emit(cc, OP_PUSH, 0);
            else cc_expr(cc);
            cc_expect(cc, T_SEMI, "';'");
            cc_emit(cc, OP_RET, 0);
            return;
        }
        if (!kstrcmp(kw, "print") || !kstrcmp(kw, "println")) {
            int nl = !kstrcmp(kw, "println");
            cc_next(cc);
            cc_print_args(cc);
            if (nl) cc_emit(cc, OP_NL, 0);
            cc_expect(cc, T_SEMI, "';'");
            return;
        }
        if (!kstrcmp(kw, "func")) {
            cc_error(cc, "line %d: nested functions are not supported", cc->lx.tok.line);
            return;
        }
    }
    cc_expr(cc);
    cc_emit(cc, OP_POP, 1);
    cc_expect(cc, T_SEMI, "';'");
}

static void cc_signature_pass(cc_t *cc) {
    cc->lx.pos = 0;
    cc->lx.line = 1;
    cc->lx.failed = 0;
    cc_next(cc);
    while (cc->lx.tok.type != T_EOF && !cc->prog->failed) {
        if (cc_is(cc, "func")) {
            char name[32];
            int nparams = 0;
            cc_next(cc);
            if (cc->lx.tok.type != T_IDENT) { cc_error(cc, "expected a function name"); return; }
            kstrncpy(name, cc->lx.tok.text, 31);
            name[31] = 0;
            cc_next(cc);
            if (cc->lx.tok.type != T_LPAREN) { cc_error(cc, "expected '(' after function name"); return; }
            cc_next(cc);
            while (cc->lx.tok.type == T_IDENT) {
                nparams++;
                cc_next(cc);
                if (cc->lx.tok.type == T_COMMA) { cc_next(cc); continue; }
                break;
            }
            if (cc_find_func(cc, name) >= 0) {
                cc_error(cc, "function '%s' defined twice", name);
                return;
            }
            if (cc->prog->nfuncs >= CC_MAX_FUNCS) { cc_error(cc, "too many functions"); return; }
            {
                cc_fn_t *f = &cc->prog->funcs[cc->prog->nfuncs++];
                kstrncpy(f->name, name, 23);
                f->addr = CC_ADDR_UNSET;
                f->nparams = nparams;
                f->nlocals = 0;
            }
        } else {
            cc_next(cc);
        }
    }
    cc->lx.failed = 0;
}

static u32 cc_checksum(const cc_program_t *p) {
    u32 h = 0x811C9DC5u;
    u32 i;
    for (i = 0; i < p->code_len; i++) {
        h = (h ^ (u32)(u16)p->code[i].op) * 16777619u;
        h = (h ^ (u32)p->code[i].arg) * 16777619u;
    }
    return h;
}

int cc_compile(const char *source, const char *name, cc_program_t *out) {
    cc_t *cc;
    cc_program_t *p = out;
    int i;

    if (!p) return -1;
    kmemset(p, 0, sizeof(*p));
    kstrncpy(p->name, name ? name : "program", sizeof(p->name) - 1);
    if (!source) source = "";
    cc = (cc_t *)mm_kmalloc(sizeof(cc_t));
    if (!cc) {
        ksnprintf(p->error, sizeof(p->error), "compiler out of memory");
        p->failed = 1;
        return -1;
    }
    kmemset(cc, 0, sizeof(*cc));
    cc->prog = p;
    cc->lx.src = source;
    cc->lx.line = 1;

    cc_signature_pass(cc);

    if (!p->failed) {
        cc->lx.pos = 0;
        cc->lx.line = 1;
        cc->lx.failed = 0;
        cc->nvars = 0;
        cc->loop_depth = 0;
        cc->in_function = 0;
        cc_next(cc);

        while (cc->lx.tok.type != T_EOF && !p->failed) {
            if (cc_is(cc, "func")) {
                char fname[32];
                int nparams = 0;
                int fidx;
                u32 enter_at;
                cc_next(cc);
                if (cc->lx.tok.type != T_IDENT) { cc_error(cc, "expected a function name"); break; }
                kstrncpy(fname, cc->lx.tok.text, 31);
                fname[31] = 0;
                cc_next(cc);
                fidx = cc_find_func(cc, fname);
                if (cc_debug_dump) klog("  [dbg] body pass enters func '%s' fidx=%d nfuncs=%d pos=%u",
                                        fname, fidx, p->nfuncs, (unsigned)cc_pos(cc));
                if (!cc_expect(cc, T_LPAREN, "'('")) break;
                cc->nvars = 0;
                cc->in_function = 1;
                cc->loop_depth = 0;
                while (cc->lx.tok.type == T_IDENT) {
                    if (cc->nvars >= CC_MAX_VARS) { cc_error(cc, "too many parameters"); break; }
                    kstrncpy(cc->vars[cc->nvars].name, cc->lx.tok.text, 31);
                    cc->vars[cc->nvars].slot = nparams;
                    cc->vars[cc->nvars].global = -1;
                    cc->nvars++;
                    nparams++;
                    cc_next(cc);
                    if (cc->lx.tok.type == T_COMMA) { cc_next(cc); continue; }
                    break;
                }
                if (!cc_expect(cc, T_RPAREN, "')'")) break;
                if (fidx >= 0) {
                    p->funcs[fidx].addr = cc_pos(cc);
                    p->funcs[fidx].nparams = nparams;
                }
                if (!cc_expect(cc, T_LBRACE, "'{'")) break;
                enter_at = cc_pos(cc);
                cc_emit(cc, OP_ENTER, 0);           /* patched below */
                while (cc->lx.tok.type != T_RBRACE && cc->lx.tok.type != T_EOF && !p->failed)
                    cc_stmt(cc);
                if (!cc_expect(cc, T_RBRACE, "'}'")) break;
                cc_emit(cc, OP_PUSH, 0);
                cc_emit(cc, OP_RET, 0);
                {
                    int nlocals = 0, j;
                    for (j = 0; j < cc->nvars; j++)
                        if (cc->vars[j].slot >= nlocals) nlocals = cc->vars[j].slot + 1;
                    if (nlocals < nparams) nlocals = nparams;
                    p->code[enter_at].arg = nlocals;
                    if (fidx >= 0) p->funcs[fidx].nlocals = nlocals;
                }
                cc->in_function = 0;
                cc->nvars = 0;
            } else {
                cc->in_function = 0;
                cc_stmt(cc);
            }
        }
    }

    if (!p->failed) {
        int main_idx = -1;
        for (i = 0; i < p->nfuncs; i++)
            if (!kstrcmp(p->funcs[i].name, "main")) { main_idx = i; break; }
        if (p->code_len == 0 && main_idx < 0) {
            cc_error(cc, "nothing to compile: write a main() function or statements");
        } else if (p->code_len + 3 <= CC_MAX_CODE) {
            /* prologue: call main (if any), drop its result, halt */
            kmemmove(&p->code[3], &p->code[0], sizeof(insn_t) * p->code_len);
            p->code[0].op = (i16)OP_CALL;
            p->code[0].arg = (i32)(main_idx >= 0 ? main_idx : 0);
            p->code[1].op = (i16)OP_POP;
            p->code[1].arg = 1;
            p->code[2].op = (i16)OP_HALT;
            p->code[2].arg = 0;
            if (main_idx < 0) {
                /* script body: no call, just run the statements then halt */
                p->code[0].op = (i16)OP_PUSH;
                p->code[0].arg = 0;
                p->code[1].op = (i16)OP_POP;
                p->code[1].arg = 1;
            }
            p->code_len += 3;
            for (i = 0; i < (int)p->code_len; i++) {
                if (p->code[i].op == OP_JMP || p->code[i].op == OP_JZ || p->code[i].op == OP_JNZ)
                    p->code[i].arg += 3;
            }
            for (i = 0; i < p->nfuncs; i++)
                if (p->funcs[i].addr != CC_ADDR_UNSET) p->funcs[i].addr += 3;
        } else {
            cc_error(cc, "program too large for the entry prologue");
        }
    }

    if (cc_debug_dump && !p->failed) {
        static const char *names[OP_COUNT] = {
            "HALT","PUSH","POP","DUP","SWAP","ADD","SUB","MUL","DIV","MOD","NEG",
            "BAND","BOR","BXOR","SHL","SHR","LNOT","EQ","NE","LT","LE","GT","GE",
            "AND","OR","JMP","JZ","JNZ","CALL","RET","ENTER","LOADI","STOREI",
            "LOADG","STOREG","PUTS","PRINTI","NL","DRAW","CLEAR","SLEEP","TIME",
            "RAND","ALLOC","FREE"
        };
        u32 d;
        for (d = 0; d < p->code_len; d++)
            klog("  %04u: %-7s %d", (unsigned)d,
                 (p->code[d].op >= 0 && p->code[d].op < OP_COUNT) ? names[p->code[d].op] : "?",
                 (int)p->code[d].arg);
        for (i = 0; i < p->nfuncs; i++)
            klog("  func %s addr %u params %d locals %d", p->funcs[i].name,
                 (unsigned)p->funcs[i].addr, (int)p->funcs[i].nparams, (int)p->funcs[i].nlocals);
    }
    if (!p->failed) {
        p->checksum = cc_checksum(p);
        klog("cc: %s compiled: %u insns, %d funcs, %d strings, sum 0x%08X",
             p->name, (unsigned)p->code_len, p->nfuncs, p->nstrings,
             (unsigned)p->checksum);
    } else {
        klog("cc: %s: error: %s", p->name, p->error);
    }
    mm_kfree(cc);
    return p->failed ? -1 : 0;
}

/* -------------------------------------------------------------------------- */
/*  Image serialisation                                                      */
/* -------------------------------------------------------------------------- */

u32 cc_image_size(cc_program_t *p) {
    u32 sz = (u32)sizeof(vm_header_t);
    u32 i;
    sz += p->code_len * (u32)sizeof(insn_t);
    sz += (u32)p->nfuncs * (u32)sizeof(cc_fn_t);
    for (i = 0; i < (u32)p->nstrings; i++) sz += (u32)kstrlen(p->strings[i]) + 1;
    return sz;
}

u32 cc_serialise(cc_program_t *p, u8 *buf, u32 cap) {
    vm_header_t h;
    u32 off, i;
    if (!buf || cap < cc_image_size(p)) return 0;
    kmemset(&h, 0, sizeof(h));
    kstrcpy(h.magic, "AEBX");
    h.version = 1;
    h.code_len = p->code_len;
    h.nfuncs = (u32)p->nfuncs;
    h.nstrings = (u32)p->nstrings;
    h.checksum = p->checksum;
    kmemcpy(buf, &h, sizeof(h));
    off = (u32)sizeof(h);
    kmemcpy(buf + off, p->code, p->code_len * (u32)sizeof(insn_t));
    off += p->code_len * (u32)sizeof(insn_t);
    kmemcpy(buf + off, p->funcs, (u32)p->nfuncs * (u32)sizeof(cc_fn_t));
    off += (u32)p->nfuncs * (u32)sizeof(cc_fn_t);
    for (i = 0; i < (u32)p->nstrings; i++) {
        u32 n = (u32)kstrlen(p->strings[i]) + 1;
        kmemcpy(buf + off, p->strings[i], n);
        off += n;
    }
    return off;
}

int cc_deserialise(const u8 *buf, u32 len, cc_program_t *p) {
    vm_header_t h;
    u32 off, i;
    if (!buf || len < sizeof(h) || !p) return -1;
    kmemcpy(&h, buf, sizeof(h));
    if (kstrncmp(h.magic, "AEBX", 4)) return -1;
    if (h.code_len > CC_MAX_CODE || h.nfuncs > CC_MAX_FUNCS || h.nstrings > CC_MAX_STR) return -1;
    kmemset(p, 0, sizeof(*p));
    off = (u32)sizeof(h);
    if (off + h.code_len * (u32)sizeof(insn_t) > len) return -1;
    kmemcpy(p->code, buf + off, h.code_len * (u32)sizeof(insn_t));
    p->code_len = h.code_len;
    off += h.code_len * (u32)sizeof(insn_t);
    if (off + h.nfuncs * (u32)sizeof(cc_fn_t) > len) return -1;
    kmemcpy(p->funcs, buf + off, h.nfuncs * (u32)sizeof(cc_fn_t));
    p->nfuncs = (int)h.nfuncs;
    off += h.nfuncs * (u32)sizeof(cc_fn_t);
    for (i = 0; i < h.nstrings && off < len; i++) {
        u32 n = 0;
        while (off + n < len && buf[off + n] && n < CC_STR_LEN - 1) n++;
        kmemcpy(p->strings[i], buf + off, n);
        p->strings[i][n] = 0;
        off += n + 1;
        p->nstrings = (int)i + 1;
    }
    p->checksum = h.checksum;
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  VM                                                                       */
/* -------------------------------------------------------------------------- */

static void vm_put(vm_state_t *vm, const char *s, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (vm->out_len < sizeof(vm->out) - 2) vm->out[vm->out_len++] = s[i];
        else vm->overflow = 1;
    }
    vm->out[vm->out_len] = 0;
}
static void vm_puts(vm_state_t *vm, const char *s) { vm_put(vm, s, (int)kstrlen(s)); }
static void vm_puti(vm_state_t *vm, i32 v) {
    char tmp[24];
    ksnprintf(tmp, sizeof(tmp), "%d", (int)v);
    vm_puts(vm, tmp);
}

#define VM_PUSH(v) do { if (vm->sp < VM_STACK) vm->st[vm->sp++] = (i32)(v); } while (0)
#define VM_POP(dst) do { (dst) = (vm->sp > 0) ? vm->st[--vm->sp] : 0; } while (0)

int vm_run(vm_state_t *vm, cc_program_t *p, int step_budget) {
    int steps = 0;
    if (!vm || !p || p->failed) return -1;
    while (vm->pc < p->code_len && !vm->halted) {
        insn_t in = p->code[vm->pc];
        i32 a, b;
        if (++steps > step_budget) break;
        vm->pc++;
        switch (in.op) {
        case OP_HALT:
            vm->halted = 1;
            break;
        case OP_PUSH:
            VM_PUSH(in.arg);
            break;
        case OP_POP:
            if (in.arg < 0) { vm->sp += in.arg; if (vm->sp < 0) vm->sp = 0; }
            else { vm->sp -= in.arg; if (vm->sp < 0) vm->sp = 0; }
            break;
        case OP_DUP:
            if (vm->sp > 0) { i32 t = vm->st[vm->sp - 1]; VM_PUSH(t); }
            break;
        case OP_SWAP:
            if (vm->sp >= 2) {
                i32 t = vm->st[vm->sp - 1];
                vm->st[vm->sp - 1] = vm->st[vm->sp - 2];
                vm->st[vm->sp - 2] = t;
            }
            break;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
        case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR:
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
        case OP_AND: case OP_OR:
            VM_POP(b);
            VM_POP(a);
            switch (in.op) {
            case OP_ADD: VM_PUSH(a + b); break;
            case OP_SUB: VM_PUSH(a - b); break;
            case OP_MUL: VM_PUSH(a * b); break;
            case OP_DIV:
                if (!b) { vm_puts(vm, "\n[vm] divide by zero\n"); vm->halted = 1; }
                else VM_PUSH(a / b);
                break;
            case OP_MOD:
                if (!b) { vm_puts(vm, "\n[vm] modulo by zero\n"); vm->halted = 1; }
                else VM_PUSH(a % b);
                break;
            case OP_BAND: VM_PUSH(a & b); break;
            case OP_BOR: VM_PUSH(a | b); break;
            case OP_BXOR: VM_PUSH(a ^ b); break;
            case OP_SHL: VM_PUSH((i32)((u32)a << (b & 31))); break;
            case OP_SHR: VM_PUSH((i32)((u32)a >> (b & 31))); break;
            case OP_EQ: VM_PUSH(a == b); break;
            case OP_NE: VM_PUSH(a != b); break;
            case OP_LT: VM_PUSH(a < b); break;
            case OP_LE: VM_PUSH(a <= b); break;
            case OP_GT: VM_PUSH(a > b); break;
            case OP_GE: VM_PUSH(a >= b); break;
            case OP_AND: VM_PUSH(a && b); break;
            case OP_OR: VM_PUSH(a || b); break;
            default: break;
            }
            break;
        case OP_NEG:
            if (vm->sp > 0) vm->st[vm->sp - 1] = -vm->st[vm->sp - 1];
            break;
        case OP_LNOT:
            if (vm->sp > 0) vm->st[vm->sp - 1] = vm->st[vm->sp - 1] ? 0 : 1;
            break;
        case OP_JMP:
            vm->pc = (u32)in.arg;
            break;
        case OP_JZ:
            VM_POP(a);
            if (!a) vm->pc = (u32)in.arg;
            break;
        case OP_JNZ:
            VM_POP(a);
            if (a) vm->pc = (u32)in.arg;
            break;
        case OP_CALL: {
            i32 fid = in.arg;
            if (fid < 0 || fid >= p->nfuncs || p->funcs[fid].addr == CC_ADDR_UNSET) {
                vm_puts(vm, "\n[vm] bad call target\n");
                vm->halted = 1;
                break;
            }
            if (vm->csp >= VM_CALLSTACK) {
                vm_puts(vm, "\n[vm] call stack overflow\n");
                vm->halted = 1;
                break;
            }
            vm->cstack_pc[vm->csp] = vm->pc;
            vm->cstack_fp[vm->csp] = (u32)vm->fp;
            vm->csp++;
            vm->fp = vm->sp - (int)p->funcs[fid].nparams;
            if (vm->fp < 0) vm->fp = 0;
            vm->pc = p->funcs[fid].addr;
            break;
        }
        case OP_ENTER: {
            int top = vm->fp + in.arg;
            if (top < 0) top = 0;
            if (top > VM_STACK) top = VM_STACK;
            vm->sp = top;
            break;
        }
        case OP_RET: {
            i32 rv;
            VM_POP(rv);
            vm->result = rv;
            if (vm->csp <= 0) { vm->halted = 1; break; }
            vm->csp--;
            vm->sp = vm->fp;
            if (vm->sp < 0) vm->sp = 0;
            vm->fp = (int)vm->cstack_fp[vm->csp];
            vm->pc = vm->cstack_pc[vm->csp];
            VM_PUSH(rv);
            break;
        }
        case OP_LOADI: {
            int idx = vm->fp + in.arg;
            VM_PUSH((idx >= 0 && idx < VM_STACK) ? vm->st[idx] : 0);
            break;
        }
        case OP_STOREI: {
            int idx = vm->fp + in.arg;
            a = (vm->sp > 0) ? vm->st[vm->sp - 1] : 0;   /* peek: assignments are expressions */
            if (idx >= 0 && idx < VM_STACK) vm->st[idx] = a;
            break;
        }
        case OP_LOADG:
            VM_PUSH(vm->globals[in.arg & (VM_GLOBALS - 1)]);
            break;
        case OP_STOREG:
            a = (vm->sp > 0) ? vm->st[vm->sp - 1] : 0;   /* peek */
            vm->globals[in.arg & (VM_GLOBALS - 1)] = a;
            break;
        case OP_PUTS:
            if (in.arg >= 0 && in.arg < p->nstrings) vm_puts(vm, p->strings[in.arg]);
            break;
        case OP_PRINTI:
            VM_POP(a);
            vm_puti(vm, a);
            break;
        case OP_NL:
            vm_puts(vm, "\n");
            break;
        case OP_DRAW: {
            i32 args[5];
            int i;
            for (i = 4; i >= 0; i--) VM_POP(args[i]);
            {
                char tmp[80];
                ksnprintf(tmp, sizeof(tmp), "[vm] draw_rect %d,%d %dx%d #%06X\n",
                          (int)args[0], (int)args[1], (int)args[2], (int)args[3],
                          (unsigned)args[4] & 0xFFFFFFu);
                vm_puts(vm, tmp);
                cc_vm_draw_request(args[0], args[1], args[2], args[3],
                                   ((u32)args[4] & 0xFFFFFFu) | 0xFF000000u);
            }
            break;
        }
        case OP_CLEAR:
            VM_POP(a);
            cc_vm_clear_request(((u32)a & 0xFFFFFFu) | 0xFF000000u);
            vm_puts(vm, "[vm] clear()\n");
            break;
        case OP_SLEEP:
            VM_POP(a);
            if (a > 0 && a < 5000) hal_sleep_ms((int)a);
            if (g_vm_sched_pid) proc_sleep_ms(g_vm_sched_pid, (u32)(a < 0 ? 0 : a));
            break;
        case OP_TIME:
            VM_PUSH((i32)(irq_ticks() / 10));
            break;
        case OP_RAND:
            if (in.arg >= 1) {
                VM_POP(a);
                if (a <= 0) a = 1;
                VM_PUSH((i32)(krandom() % (u32)a));
            } else {
                VM_PUSH((i32)(krandom() & 0x7FFF));
            }
            break;
        case OP_ALLOC: {
            int slot = -1, i;
            VM_POP(a);
            if (a <= 0) a = 64;
            for (i = 0; i < VM_HEAP_BLOCKS; i++)
                if (!vm->heap_handle[i]) { slot = i; break; }
            if (slot >= 0) {
                void *blk = mm_kmalloc((u32)a);
                if (blk) {
                    vm->heap_handle[slot] = 1;
                    VM_PUSH(slot);
                } else VM_PUSH(-1);
            } else VM_PUSH(-1);
            break;
        }
        case OP_FREE: {
            VM_POP(a);
            if (a >= 0 && a < VM_HEAP_BLOCKS) vm->heap_handle[a] = 0;
            break;
        }
        default:
            vm_puts(vm, "\n[vm] illegal opcode\n");
            vm->halted = 1;
            break;
        }
    }
    vm->steps += steps;
    if (vm->pc >= p->code_len) vm->halted = 1;
    return steps;
}

/* -------------------------------------------------------------------------- */
/*  Process integration                                                      */
/* -------------------------------------------------------------------------- */

void cc_init(void) {
    kmemset(g_vmprocs, 0, sizeof(g_vmprocs));
    g_last_prog = NULL;
    klog("cc: AetherC compiler v1 ready: %d opcodes, %u insn budget",
         OP_COUNT, (unsigned)CC_MAX_CODE);
    klog("vm: stack machine ready: %d frames, %d slots, %d globals",
         VM_CALLSTACK, VM_STACK, VM_GLOBALS);
}

int cc_spawn_program(cc_program_t *p, const char *name, u32 *pid_out) {
    int slot;
    vm_process_t *vp;
    vm_state_t *vm;
    if (!p || p->failed) return -1;
    for (slot = 0; slot < VM_MAX_PROGS; slot++)
        if (!g_vmprocs[slot].active) break;
    if (slot >= VM_MAX_PROGS) return -2;
    vm = (vm_state_t *)mm_kmalloc(sizeof(vm_state_t));
    if (!vm) return -3;
    kmemset(vm, 0, sizeof(*vm));
    vp = &g_vmprocs[slot];
    kmemset(vp, 0, sizeof(*vp));
    vp->active = 1;
    kstrncpy(vp->name, name ? name : p->name, sizeof(vp->name) - 1);
    vp->prog = p;
    vp->vm = vm;
    vp->pid = proc_spawn(vp->name, 1, 12, PROC_FLAG_APP | PROC_FLAG_VM, 0);
    g_last_prog = p;
    if (pid_out) *pid_out = vp->pid;
    return slot;
}

int cc_step_all(int budget) {
    int i, ran = 0;
    for (i = 0; i < VM_MAX_PROGS; i++) {
        vm_process_t *vp = &g_vmprocs[i];
        pcb_t *pc;
        if (!vp->active) continue;
        if (vp->vm->halted) {
            if (!vp->finished) {
                vp->finished = 1;
                proc_exit(vp->pid, 0);
                klog("vm: pid %u (%s) finished after %d insns", (unsigned)vp->pid,
                     vp->name, vp->vm->steps);
            }
            continue;
        }
        pc = proc_pcb(vp->pid);
        if (!pc || pc->state == PROC_ZOMBIE) { vp->active = 0; continue; }
        if (pc->state == PROC_SLEEPING) continue;
        g_vm_sched_pid = vp->pid;
        vm_run(vp->vm, vp->prog, budget);
        g_vm_sched_pid = 0;
        ran++;
    }
    return ran;
}

void cc_vm_destroy(int slot) {
    vm_process_t *vp;
    if (slot < 0 || slot >= VM_MAX_PROGS) return;
    vp = &g_vmprocs[slot];
    if (!vp->active) return;
    if (vp->vm) mm_kfree(vp->vm);
    kmemset(vp, 0, sizeof(*vp));
}

const char *cc_vm_output(int slot, u32 *len) {
    if (slot < 0 || slot >= VM_MAX_PROGS || !g_vmprocs[slot].active) return NULL;
    if (len) *len = g_vmprocs[slot].vm->out_len;
    return g_vmprocs[slot].vm->out;
}

int cc_run_source(const char *source, const char *name, char *out, u32 outcap,
                  int *insns, char *err, u32 errcap) {
    cc_program_t *p = (cc_program_t *)mm_kmalloc(sizeof(cc_program_t));
    vm_state_t *vm;
    int rc;
    if (!p) return -1;
    if (cc_compile(source, name, p) != 0) {
        if (err) ksnprintf(err, errcap, "%s", p->error);
        mm_kfree(p);
        return -1;
    }
    vm = (vm_state_t *)mm_kmalloc(sizeof(vm_state_t));
    if (!vm) { mm_kfree(p); return -1; }
    kmemset(vm, 0, sizeof(*vm));
    vm_run(vm, p, 400000);
    if (out) {
        u32 n = vm->out_len < outcap - 1 ? vm->out_len : outcap - 1;
        kmemcpy(out, vm->out, n);
        out[n] = 0;
    }
    if (insns) *insns = vm->steps;
    if (err) ksnprintf(err, errcap, "%s", vm->overflow ? "output truncated" : "");
    rc = vm->halted ? 0 : -2;
    mm_kfree(vm);
    mm_kfree(p);
    return rc;
}
