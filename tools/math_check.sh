#!/usr/bin/env bash
# Validate the freestanding maths (k_sqrt/k_sin/k_cos/k_pow/k_fmod/k_exp/k_log)
# against the host libm.  The Windows build links no C runtime, so these routines
# are what the compositor and the calculator actually execute -- this harness is
# the only way to prove they agree with the reference implementation.
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cat > "$T/stub.c" <<'C'
#include "dev/00_prelude.c"
u64 host_now_us(void) { return 0; }
C
cat > "$T/main.c" <<'C'
#include "dev/00_prelude.c"
#include "dev/10_util.c"
#include <math.h>
double k_sqrt(double), k_pow(double, double), k_fmod(double, double);
double k_sin(double), k_cos(double), k_exp(double), k_log(double);
static int bad;
static void rel(const char *name, double a, double b, double tol) {
    double e = (b != 0.0) ? (a - b) / b : (a - b);
    if (e > tol || e < -tol) {
        printf("  %s: got %.15g want %.15g (rel %.3g)\n", name, a, b, e);
        bad++;
    }
}
int main(void) {
    static const double sq[] = {0.5, 1, 2, 4, 9, 81, 400, 1e6, 1e-4, 12345.678};
    static const double pw[][2] = {{2,10},{2,-3},{9,0.5},{1.5,4},{10,3},{2,0.5},{3,7}};
    static const double fm[][2] = {{10,3},{7.5,2},{1,0.3},{-7,2},{100,7}};
    char buf[64];
    int i;
    for (i = 0; i < (int)(sizeof sq / sizeof sq[0]); i++) {
        snprintf(buf, sizeof buf, "sqrt(%g)", sq[i]);
        rel(buf, k_sqrt(sq[i]), sqrt(sq[i]), 1e-12);
    }
    for (i = 0; i < 1257; i += 3) {           /* -2pi .. 2pi in 5 mrad steps */
        double x = (i - 628) / 100.0;
        snprintf(buf, sizeof buf, "sin(%g)", x);
        rel(buf, k_sin(x), sin(x), 1e-9);
        snprintf(buf, sizeof buf, "cos(%g)", x);
        rel(buf, k_cos(x), cos(x), 1e-9);
    }
    for (i = 0; i < (int)(sizeof pw / sizeof pw[0]); i++) {
        snprintf(buf, sizeof buf, "pow(%g,%g)", pw[i][0], pw[i][1]);
        rel(buf, k_pow(pw[i][0], pw[i][1]), pow(pw[i][0], pw[i][1]), 1e-9);
    }
    for (i = 0; i < (int)(sizeof fm / sizeof fm[0]); i++) {
        snprintf(buf, sizeof buf, "fmod(%g,%g)", fm[i][0], fm[i][1]);
        rel(buf, k_fmod(fm[i][0], fm[i][1]), fmod(fm[i][0], fm[i][1]), 1e-9);
    }
    for (i = 1; i <= 40; i++) {
        double x = i * 0.25;
        snprintf(buf, sizeof buf, "exp(%g)", x);
        rel(buf, k_exp(x), exp(x), 1e-9);
        snprintf(buf, sizeof buf, "log(%g)", x);
        rel(buf, k_log(x), log(x), 1e-9);
    }
    if (bad) { printf("math_check: %d FAILURES\n", bad); return 1; }
    printf("math_check: all routines match libm\n");
    return 0;
}
C
gcc -O2 -std=gnu11 -DAETHER_HOST_MATH_SHIM -I. -w "$T/main.c" "$T/stub.c" -o "$T/t" -lm
"$T/t"
