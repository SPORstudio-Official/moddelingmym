/* ==========================================================================
 *  UTIL -- freestanding-flavoured libc subset + monotonic clock + kernel log
 * ========================================================================== */


#if defined(AETHER_ON_WIN32) || defined(AETHER_WASM)
/* --------------------------------------------------------------------------
 *  Compiler-generated library calls
 *
 *  Even in freestanding mode the compiler is allowed to replace a struct
 *  assignment or an array copy with a call to memcpy/memset/memmove, and an
 *  inline strcmp with strlen.  Because the Windows executable links no C
 *  runtime, those four symbols have to exist here; they simply forward to the
 *  kernel primitives (the compiler's own memcpy cannot forward to them, so no
 *  recursion is possible: these bodies are plain loops emitted in the caller).
 * -------------------------------------------------------------------------- */
void *memcpy(void *d, const void *s, size_t n)  { return kmemcpy(d, s, n); }
void *memmove(void *d, const void *s, size_t n) { return kmemmove(d, s, n); }
void *memset(void *d, int c, size_t n)          { return kmemset(d, c, n); }
int   memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    size_t i;
    for (i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}
size_t strlen(const char *s)                    { return kstrlen(s); }
char  *strcpy(char *d, const char *s)           { return kstrcpy(d, s); }
char  *strncpy(char *d, const char *s, size_t n){ return kstrncpy(d, s, n); }
int    strcmp(const char *a, const char *b)     { return kstrcmp(a, b); }
int    strncmp(const char *a, const char *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(u8)a[i] - (int)(u8)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
#endif

void *kmemset(void *d, int c, size_t n) {
    u8 *p = (u8 *)d;
    while (n--) *p++ = (u8)c;
    return d;
}

void *kmemcpy(void *d, const void *s, size_t n) {
    u8 *dp = (u8 *)d;
    const u8 *sp = (const u8 *)s;
    if (dp == sp || n == 0) return d;
    while (n--) *dp++ = *sp++;
    return d;
}

void *kmemmove(void *d, const void *s, size_t n) {
    u8 *dp = (u8 *)d;
    const u8 *sp = (const u8 *)s;
    if (dp == sp || n == 0) return d;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}

int kmemcmp(const void *a, const void *b, size_t n) {
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 *  Why this one function carries a warning sign
 *
 *  `if (!s) return 0;` followed by `while (s[n])` is the obvious way to write a
 *  NULL-tolerant strlen, and it is what this function used to say.  clang then
 *  recognises the body as *the* strlen algorithm, folds it into the
 *  `__builtin_strlen` intrinsic, and -- because the declaration is a plain,
 *  non-`noundef` one -- decides that the *return value* may still be poison in
 *  the branch where the pointer it could not prove non-NULL was NULL.  Any
 *  caller that uses that length to shape its arguments inherits the poison.
 *
 *  winhttp_fetch_inner() is exactly such a caller: it passes
 *  `blen ? body : NO_DATA` and `(DWORD)blen` to WinHttpSendRequest().  At -O1
 *  and above clang propagated poison out of that call, concluded the whole
 *  request-building block was unreachable, deleted it, and left a single `int3`
 *  where the HTTP request should have been.  The shipped AE.exe therefore
 *  imported only six WinHttp* functions instead of eleven: every page load and
 *  every MCP search would have trapped instead of fetching.
 *
 *  The `&&` form below is semantically identical but is upstream guarded by a
 *  non-NULL check, which keeps clang from attributing an unprovable value to the
 *  emptiness test, so the intrinsic fold no longer produces poison.  `noinline`
 *  is belt and braces: it keeps this shape out of the optimiser's inliner, where
 *  the poison could be reintroduced in a future version.
 *
 *  tools/import_test.js fails the build if any function the sources call is
 *  missing from the import table, so this cannot silently come back.
 * ------------------------------------------------------------------------ */
__attribute__((noinline)) size_t kstrlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

int kstrcmp(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

int kstrncmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *a == *b) { a++; b++; }
    if (n == (size_t)-1) return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

int kstrnicmp(const char *a, const char *b, size_t n) {
    while (n--) {
        int ca = (int)(u8)*a, cb = (int)(u8)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
        a++; b++;
    }
    return 0;
}

char *kstrcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

char *kstrncpy(char *d, const char *s, size_t n) {
    char *r = d;
    while (n && (*d = *s)) { d++; s++; n--; }
    while (n--) *d++ = 0;
    return r;
}

char *kstrcat(char *d, const char *s) {
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) {}
    return r;
}

char *kstrchr(const char *s, int c) {
    while (s && *s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

const char *kstrstr(const char *h, const char *n) {
    size_t nl = kstrlen(n);
    if (!nl) return h;
    while (*h) {
        if (*h == *n && !kstrncmp(h, n, nl)) return h;
        h++;
    }
    return NULL;
}

char *kstrncat(char *d, const char *s, size_t n) {
    size_t dl = kstrlen(d);
    size_t i = 0;
    while (i < n && s[i]) { d[dl + i] = s[i]; i++; }
    d[dl + i] = 0;
    return d;
}

char *kstrrchr(const char *s, int c) {
    const char *last = NULL;
    if (!s) return NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (c == 0) ? (char *)s : (char *)last;
}

void kstrlower(char *s) {
    while (*s) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
        s++;
    }
}

int katoi(const char *s) {
    int sign = 1;
    long v = 0;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return (int)(v * sign);
}

u32 kstrtoul(const char *s, int base, int *ok) {
    u32 v = 0;
    int any = 0;
    if (ok) *ok = 0;
    if (!s) return 0;
    while (*s == ' ') s++;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    }
    for (;;) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (u32)base + (u32)d;
        any = 1;
        s++;
    }
    if (ok) *ok = any;
    return v;
}

/* -------------------------------------------------------------------------- */
/*  Formatted output (subset: %s %c %d %i %u %x %X %p %ld %lu %llu %-+0# width)  */
/* -------------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} strbuf_t;

static void sb_putc(strbuf_t *sb, char c) {
    if (sb->len + 1 < sb->cap) sb->buf[sb->len] = c;
    sb->len++;
}

static void sb_putnum(strbuf_t *sb, u64 v, int base, int upper, int width,
                      char pad, int left, int prefix, int neg) {
    char tmp[72];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % (u32)base]; v /= (u32)base; }
    if (neg) tmp[n++] = '-';
    else if (prefix) { tmp[n++] = 'x'; tmp[n++] = '0'; }
    while (n < width) {
        if (left) tmp[n++] = ' ';
        else { sb_putc(sb, pad); width--; }
    }
    while (n) sb_putc(sb, tmp[--n]);
}

int kvsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    strbuf_t sb;
    sb.buf = buf;
    sb.cap = n;
    sb.len = 0;
    if (!fmt) fmt = "";
    while (*fmt) {
        if (*fmt != '%') { sb_putc(&sb, *fmt++); continue; }
        fmt++;
        int left = 0, plus = 0, zero = 0, alt = 0;
        int width = 0, prec = -1, lng = 0;
        char pad = ' ';
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '#') alt = 1;
            else if (*fmt == ' ') { if (!plus) plus = 0; }
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
        }
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h') {
            if (*fmt == 'l') lng++;
            fmt++;
        }
        char conv = *fmt ? *fmt++ : 0;
        if (zero && !left) pad = '0';
        switch (conv) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)kstrlen(s);
            int lim = (prec >= 0 && prec < len) ? prec : len;
            if (!left) for (int i = lim; i < width; i++) sb_putc(&sb, pad);
            for (int i = 0; i < lim; i++) sb_putc(&sb, s[i]);
            if (left) for (int i = lim; i < width; i++) sb_putc(&sb, ' ');
            break;
        }
        case 'c':
            if (!left) for (int i = 1; i < width; i++) sb_putc(&sb, ' ');
            sb_putc(&sb, (char)va_arg(ap, int));
            if (left) for (int i = 1; i < width; i++) sb_putc(&sb, ' ');
            break;
        case 'd': case 'i': {
            i64 v = (lng >= 2) ? (i64)va_arg(ap, long long)
                              : (lng == 1 ? (i64)va_arg(ap, long) : (i64)va_arg(ap, int));
            if (v < 0) sb_putnum(&sb, (u64)(-v), 10, 0, width, pad, left, 0, 1);
            else       sb_putnum(&sb, (u64)v, 10, 0, width, pad, left, plus ? 0 : 0, 0);
            break;
        }
        case 'u': {
            u64 v = (lng >= 2) ? (u64)va_arg(ap, unsigned long long)
                              : (lng == 1 ? (u64)va_arg(ap, unsigned long) : (u64)va_arg(ap, unsigned int));
            sb_putnum(&sb, v, 10, 0, width, pad, left, 0, 0);
            break;
        }
        case 'x': case 'X': {
            u64 v = (lng >= 2) ? (u64)va_arg(ap, unsigned long long)
                              : (lng == 1 ? (u64)va_arg(ap, unsigned long) : (u64)va_arg(ap, unsigned int));
            sb_putnum(&sb, v, 16, conv == 'X', width, pad, left, alt && v, 0);
            break;
        }
        case 'p': {
            u64 v = (u64)(uintptr_t)va_arg(ap, void *);
            sb_putnum(&sb, v, 16, 0, width, pad, left, 1, 0);
            break;
        }
        case '%': sb_putc(&sb, '%'); break;
        case 0: fmt--; break;
        default: sb_putc(&sb, '%'); sb_putc(&sb, conv); break;
        }
    }
    if (sb.cap) buf[(sb.len < sb.cap) ? sb.len : sb.cap - 1] = 0;
    return (int)sb.len;
}

int ksnprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = kvsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* -------------------------------------------------------------------------- */
/*  Random / math                                                             */
/* -------------------------------------------------------------------------- */

static u32 g_rand_state = 0x1234ABCDu;

void krand_seed(u32 s) { g_rand_state = s ? s : 1u; }

u32 krandom(void) {
    /* xorshift32 */
    u32 x = g_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rand_state = x;
    return x;
}

u32 k_abs(i32 v) { return (u32)(v < 0 ? -v : v); }
int k_min(int a, int b) { return a < b ? a : b; }
int k_max(int a, int b) { return a > b ? a : b; }
int k_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Integer square root by binary search.  Used by the software renderer, which
 * must not pull in libm's double-precision sqrt for a per-scanline width --
 * that single call was the hottest instruction stream in the compositor. */
u32 k_isqrt(u32 v) {
    u32 r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* -------------------------------------------------------------------------- */
/*  Monotonic clock                                                           */
/* -------------------------------------------------------------------------- */

u64 now_us(void) {
#if defined(AETHER_ON_WIN32)
    static LARGE_INTEGER freq;
    static int init = 0;
    LARGE_INTEGER c;
    if (!init) { QueryPerformanceFrequency(&freq); init = 1; }
    QueryPerformanceCounter(&c);
    return (u64)((c.QuadPart * 1000000ULL) / (u64)freq.QuadPart);
#elif defined(AETHER_WASM)
    /* performance.now() in the shell, already in microseconds. */
    return js_now_us();
#else
    extern u64 host_now_us(void);
    return host_now_us();
#endif
}

u32 now_ms(void) {
    return (u32)(now_us() / 1000ULL);
}

/* -------------------------------------------------------------------------- */
/*  Kernel log ring buffer ("dmesg")                                          */
/* -------------------------------------------------------------------------- */

static char g_log_buf[LOG_BUF_SIZE];
static u32  g_log_len;
static int  g_log_lock_dummy;

void klog_clear(void) {
    g_log_len = 0;
    g_log_buf[0] = 0;
    g_log_lock_dummy = 0;
}

void klog(const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = kvsnprintf(tmp, sizeof(tmp) - 3, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(tmp) - 3) n = (int)sizeof(tmp) - 3;
    tmp[n++] = '\n';
    tmp[n] = 0;
    if (g_log_len + (u32)n >= LOG_BUF_SIZE - 1) {
        /* drop the oldest 25% of the log so dmesg keeps rolling */
        u32 drop = LOG_BUF_SIZE / 4;
        g_log_len -= drop;
        kmemcpy(g_log_buf, g_log_buf + drop, g_log_len);
    }
    kmemcpy(g_log_buf + g_log_len, tmp, (size_t)n);
    g_log_len += (u32)n;
    g_log_buf[g_log_len] = 0;
}

const char *klog_data(u32 *len) {
    if (len) *len = g_log_len;
    return g_log_buf;
}

/* --------------------------------------------------------------------------
 *  Floating point maths
 *
 *  The calculator evaluates expressions in double precision.  These routines
 *  are implemented here rather than linked from the C runtime so the Windows
 *  executable depends on nothing but the four Win32 system libraries.
 * -------------------------------------------------------------------------- */

double k_sin(double x);
double k_cos(double x);

#if !defined(AETHER_ON_WIN32) && !defined(AETHER_WASM) && !defined(AETHER_HOST_MATH_SHIM)
/* The host build keeps the libm versions (identical semantics).
   Define AETHER_HOST_MATH_SHIM to build the freestanding versions on the host
   instead, which is how tools/math_check.sh validates them against libm. */
double k_sqrt(double x) { return sqrt(x); }
double k_pow(double b, double e) { return pow(b, e); }
double k_fmod(double a, double b) { return fmod(a, b); }
double k_sin(double x) { return sin(x); }
double k_cos(double x) { return cos(x); }
#else
double k_fmod(double a, double b) {
    double q;
    if (b == 0.0) return 0.0;
    q = (double)(i64)(a / b);
    return a - q * b;
}

double k_sqrt(double x) {
    double g;
    int i;
    if (x <= 0.0) return 0.0;
    /* Newton-Raphson on y^2 = x.  Start from a scaling by powers of four so the
       guess is within a factor of two, then converge quadratically. */
    g = x;
    while (g > 4.0) g *= 0.25;
    while (g < 0.25) g *= 4.0;
    g *= 0.75;                      /* cheap starting point */
    for (i = 0; i < 40; i++) {
        double ng = 0.5 * (g + x / g);
        if (ng == g) break;
        g = ng;
    }
    return g;
}

double k_exp(double x) {
    /* e^x = 2^k * e^r with |r| <= ln(2)/2, Taylor series for e^r, then scale.
       The reduction constant is written to full double precision: truncating
       it to 8 digits costs ~6e-9 relative error per halving. */
    const double ln2 = 0.69314718055994530942;
    const double half_ln2 = 0.34657359027997265471;
    double r = x;
    double term = 1.0, sum = 1.0;
    int k = 0, i;
    if (x > 709.0) return 1.0e308;
    if (x < -709.0) return 0.0;
    while (r > half_ln2) { r -= ln2; k++; }
    while (r < -half_ln2) { r += ln2; k--; }
    for (i = 1; i < 24; i++) {
        term *= r / (double)i;
        sum += term;
        if (term < 1e-18 && term > -1e-18) break;
    }
    while (k > 0) { sum *= 2.0; k--; }
    while (k < 0) { sum *= 0.5; k++; }
    return sum;
}

double k_log(double x) {
    /* natural log by range reduction to [1,2) then atanh series */
    double y, z, sum, term;
    int e = 0, i;
    if (x <= 0.0) return -1.0e308;
    while (x >= 2.0) { x *= 0.5; e++; }
    while (x < 1.0)  { x *= 2.0; e--; }
    z = (x - 1.0) / (x + 1.0);
    y = z * z;
    sum = 0.0;
    term = z;
    for (i = 0; i < 40; i++) {
        sum += term / (double)(2 * i + 1);
        term *= y;
    }
    return 2.0 * sum + (double)e * 0.693147180559945;
}

double k_pow(double b, double e) {
    i64 ie;
    double r = 1.0, base = b;
    if (e == 0.0) return 1.0;
    if (b == 0.0) return 0.0;
    ie = (i64)e;
    if ((double)ie == e && ie > -1024 && ie < 1024) {
        /* integer exponent: square and multiply */
        if (ie < 0) { base = 1.0 / base; ie = -ie; }
        while (ie) {
            if (ie & 1) r *= base;
            base *= base;
            ie >>= 1;
        }
        return r;
    }
    if (e == 0.5) return k_sqrt(b); /* exact, and the common case in the calc */
    if (b < 0.0) return 0.0;        /* fractional power of a negative base */
    return k_exp(e * k_log(b));
}
#endif

#if (defined(AETHER_ON_WIN32) || defined(AETHER_WASM) || defined(AETHER_HOST_MATH_SHIM))
/* Reduce to [-pi, pi], then fold into [-pi/2, pi/2] and use the Taylor series
   for sine; cosine is the same series shifted by a quarter period.  Both are
   accurate to well under a pixel at the scales the compositor uses. */
static double k_sin_series(double x) {
    double term = x, sum = x, x2 = x * x;
    int i;
    for (i = 1; i < 12; i++) {
        term *= -x2 / (double)((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}

double k_sin(double x) {
    const double pi = 3.14159265358979323846;
    const double two_pi = 6.28318530717958647692;
    const double half_pi = 1.57079632679489661923;
    int neg = 0;
    /* fold into [0, 2pi) */
    x = k_fmod(x, two_pi);
    if (x < 0.0) x += two_pi;
    if (x > pi) { x -= pi; neg = 1; }
    if (x > half_pi) x = pi - x;
    {
        double r = k_sin_series(x);
        return neg ? -r : r;
    }
}

double k_cos(double x) {
    return k_sin(x + 1.57079632679489661923);
}
#endif
