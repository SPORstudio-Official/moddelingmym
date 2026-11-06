/* ===========================================================================
   DEAD AIR  --  shared types / declarations
   A tactile auditory horror game.  Native Win32, software renderer, no assets
   other than the embedded PCM tape recordings.
   =========================================================================== */
#ifndef DA_H
#define DA_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  i32;

#define FB_W 640
#define FB_H 360
#define MIX_SR 22050

/* ---- colours (0xAARRGGBB) ---- */
static inline u32 argb(int a, int r, int g, int b) {
    if (a < 0) { a = 0; } else if (a > 255) { a = 255; }
    if (r < 0) { r = 0; } else if (r > 255) { r = 255; }
    if (g < 0) { g = 0; } else if (g > 255) { g = 255; }
    if (b < 0) { b = 0; } else if (b > 255) { b = 255; }
    return ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}
#define COLR(r,g,b) rgb_((r),(g),(b))
static inline u32 rgb_(int r, int g, int b) { return argb(255, r, g, b); }

typedef struct { u32 *px; int w, h; } FB;

/* ---------------------------------------------------------------- drawing */
void fb_clear(FB *fb, u32 c);
void fb_px(FB *fb, int x, int y, u32 c);
void fb_blend_px(FB *fb, int x, int y, u32 c);          /* c carries alpha */
void fb_rect(FB *fb, int x, int y, int w, int h, u32 c);
void fb_rect_a(FB *fb, int x, int y, int w, int h, u32 c);/* alpha blend    */
void fb_frame(FB *fb, int x, int y, int w, int h, u32 c);
void fb_vgrad(FB *fb, int x, int y, int w, int h, u32 top, u32 bot);
void fb_line(FB *fb, int x0, int y0, int x1, int y1, u32 c);
void fb_disc(FB *fb, int cx, int cy, int r, u32 c);
void fb_ring(FB *fb, int cx, int cy, int r, u32 c);
void fb_ellipse(FB *fb, int cx, int cy, int rx, int ry, u32 c);
void fb_ellipse_a(FB *fb, int cx, int cy, int rx, int ry, u32 c);
void fb_shade(FB *fb, int x, int y, int w, int h, float mul);   /* darken    */
void fb_scanlines(FB *fb, int alpha);
void fb_vignette(FB *fb, float strength);
void fb_grain(FB *fb, float amount, u32 *seed);
void fb_noise_block(FB *fb, int x, int y, int w, int h, float amount, u32 *seed);
void fb_glow(FB *fb, int cx, int cy, int r, u32 col, int intensity);

void txt_draw(FB *fb, int x, int y, const char *s, u32 c, int scale);
void txt_draw_glow(FB *fb, int x, int y, const char *s, u32 c, u32 glow, int scale);
void txt_draw_shadow(FB *fb, int x, int y, const char *s, u32 c, int scale);
int  txt_w(const char *s, int scale);
void txt_center(FB *fb, int cx, int y, const char *s, u32 c, int scale);
void txt_wrap(FB *fb, int x, int y, int maxchars, const char *s, u32 c, int scale,
              int linegap);

/* ---------------------------------------------------------------- random  */
typedef struct { u32 s; } RNG;
static inline u32 rng_next(RNG *r) {
    u32 x = r->s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; r->s = x; return x;
}
static inline float rng_f(RNG *r) { return (float)(rng_next(r) >> 8) * (1.0f / 16777216.0f); }
static inline float rng_range(RNG *r, float a, float b) { return a + (b - a) * rng_f(r); }
static inline int   rng_i(RNG *r, int n) { return (int)(rng_next(r) % (u32)n); }
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smoothstepf(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }

/* ---------------------------------------------------------------- audio   */
enum { APP_FAN = 0, APP_MICRO, APP_SHOWER, APP_RADIO, APP_COUNT };

typedef struct {
    /* tape */
    int   tape_playing;
    int   clip;                  /* index into audio_clips */
    float tape_rate;             /* playback speed (wow/flutter centre) */
    float low_cut, high_cut;     /* 0..1 knob positions */
    float clarity;               /* 0..1 -> brightness / loudness of voice */
    float tape_level;            /* 0..1 master tape channel */
    /* room */
    float app_level[APP_COUNT];  /* 0..1 smoothed on/off */
    float radio_freq;            /* Hz */
    float interference;          /* 0..1 entity dominance */
    float entity_freq;           /* Hz */
    float blackout;              /* 0..1 darkness */
    float jumpscare;             /* 0..1 */
    /* ui feedback */
    float static_burst;
    int   night;
} AudioParams;

extern AudioParams AP;

void  audio_reset(void);
void  audio_mix(short *out, int frames);   /* interleaved stereo */
void  sfx_creak(float vol);
void  sfx_thud(float vol);
void  sfx_click(void);
void  sfx_switch(float vol);
void  sfx_static(float vol);
void  sfx_stinger(void);
void  sfx_chime(float vol);
void  sfx_drip(float vol);
void  sfx_whisper(void);
void  sfx_breaker(void);

/* ---------------------------------------------------------------- input   */
typedef struct {
    int   mouse_x, mouse_y;
    int   mouse_down;
    int   mouse_pressed;
    int   mouse_released;
    int   key_pressed[128];
    int   key_down[128];
    float dt;
    float time;
    int   quit;
} Input;

/* ---------------------------------------------------------------- game    */
#define NLINES_MAX 8
#define BLANKS_MAX 4
#define BANK_MAX   7

typedef struct {
    const char *text;            /* '#' marks a redacted word                 */
    const char *answers[BLANKS_MAX];
} TapeLine;

typedef struct {
    const char *code;
    const char *tape_label;
    const char *subject;
    const char *brief[6];
    const char *epilogue[6];
    int   clip;
    int   nlines;
    TapeLine lines[NLINES_MAX];
    int   blanks;                /* blanks per line                           */
    float band_lo, band_hi;      /* knob targets                              */
    float freq;                  /* entity frequency (Hz)                     */
    float capacity;              /* fuse capacity                             */
    float aggression;            /* interference rise multiplier              */
    float event_gap;             /* seconds between manifestations            */
    int   event_mask;            /* which event kinds may occur               */
    int   attack_night;          /* 1 = predictive attack mechanic            */
} Night;

extern const Night NIGHTS[5];

void draw_entity_face(FB *fb, int cx, int cy, float sc, float t, int alpha);

void game_init(FB *fb);
void game_frame(Input *in);
int  game_should_quit(void);
void game_debug(char *buf, int n);
void game_debug_kill(void);
void game_debug_force_line(void);
void game_debug_night(int n);
void game_debug_interference(float v);

#endif /* DA_H */
