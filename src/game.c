/* ===========================================================================
   DEAD AIR -- game state, rules and rendering
   =========================================================================== */
#include "da.h"
#include "font_data.h"

enum { SCR_TITLE = 0, SCR_HELP, SCR_BRIEF, SCR_DESK, SCR_ROOM, SCR_DEAD,
       SCR_NIGHTEND, SCR_ENDING };

enum {
    HIT_NONE = 0,
    HIT_KNOB_LOW = 10, HIT_KNOB_HIGH, HIT_TRANSPORT,
    HIT_BANK = 20,
    HIT_STAND = 40, HIT_SIT, HIT_FAN, HIT_MICRO, HIT_SHOWER, HIT_BATH,
    HIT_RADIO, HIT_DIAL, HIT_BREAKER, HIT_CLOSET_HIT,
    HIT_MENU = 60,
    HIT_RETRY = 70, HIT_NEXT
};

enum { EV_NONE = 0, EV_CREAK, EV_SHADOW, EV_BREATH, EV_MIMIC, EV_ATTACK };

typedef struct { int x, y, w, h, id; } Hit;

typedef struct {
    FB   fb;
    RNG  rng;
    float t;          /* wall clock since launch */
    float scr_t;      /* time in current screen  */
    int   scr;
    int   night;
    int   paused;
    int   menu_sel;
    int   quit_requested;
    u32   grain_seed;
    float grain_phase;
    int   mx, my;

    Hit   hits[96];
    int   nhits;

    int   drag;              /* hit id being dragged */
    float drag_val, drag_y, drag_x;

    float tw;                /* typewriter progress (chars) */

    /* ---- deck ---- */
    float low_cut, high_cut, target_lo, target_hi;
    int   playing;
    float reel_a, reel_b;
    float clarity, signal;
    int   line, filled;
    char  bank[BANK_MAX][24];
    char  chosen[BLANKS_MAX][24];
    int   nbank, bank_used[BANK_MAX];
    float flash_ok, flash_bad;
    int   solving;           /* brief pause after a line is accepted */

    /* ---- room ---- */
    float app_on[APP_COUNT], app_lvl[APP_COUNT];
    float radio_freq;
    float load, capacity, over_t;
    int   blackout;
    float blackout_t;
    int   bath_open;

    /* ---- entity ---- */
    float interference, red_t, dead_t;
    float shadow, closet, drip_t;
    int   ev_kind, ev_app;
    float ev_t, ev_dur;
    int   attack_active, attack_app;
    float attack_t;
    float whisper_t;

    /* ---- fx ---- */
    float shake, flash;
} Game;

static Game G;
static void draw_crt(FB *fb);

/* ---------------------------------------------------------------- helpers */
static void hits_reset(void) { G.nhits = 0; }
static void hit(int x, int y, int w, int h, int id) {
    if (G.nhits >= 96) return;
    Hit *h2 = &G.hits[G.nhits++];
    h2->x = x; h2->y = y; h2->w = w; h2->h = h; h2->id = id;
}
static int hit_test(int x, int y) {
    for (int i = G.nhits - 1; i >= 0; i--) {
        Hit *h = &G.hits[i];
        if (x >= h->x && y >= h->y && x < h->x + h->w && y < h->y + h->h) return i;
    }
    return -1;
}
static float gauss(float d, float w) { return expf(-(d * d) / (2.0f * w * w)); }

static void set_screen(int s) { G.scr = s; G.scr_t = 0; G.tw = 0; hits_reset(); }

static int hot(int x, int y, int w, int h) {
    return G.mx >= x && G.mx < x + w && G.my >= y && G.my < y + h;
}
static void hover_ring(FB *fb, int x, int y, int w, int h, int on) {
    if (!on) return;
    fb_frame(fb, x - 1, y - 1, w + 2, h + 2, argb(70, 240, 230, 180));
    fb_frame(fb, x, y, w, h, argb(50, 240, 230, 180));
}

/* ------------------------------------------------------------- visual kit */
static void bevel(FB *fb, int x, int y, int w, int h, u32 base) {
    fb_rect(fb, x, y, w, h, base);
    fb_rect_a(fb, x, y, w, 1, argb(60, 255, 255, 255));
    fb_rect_a(fb, x, y, 1, h, argb(40, 255, 255, 255));
    fb_rect_a(fb, x, y + h - 1, w, 1, argb(90, 0, 0, 0));
    fb_rect_a(fb, x + w - 1, y, 1, h, argb(70, 0, 0, 0));
}

static void led(FB *fb, int x, int y, int r, u32 col, int on) {
    fb_disc(fb, x, y, r, rgb_(10, 10, 10));
    if (on) {
        fb_disc(fb, x, y, r - 1, col);
        for (int i = 1; i <= 3; i++)
            for (int a = 0; a < 8; a++) {
                float ang = (float)a / 8.0f * 6.2831853f;
                fb_blend_px(fb, x + (int)(cosf(ang) * (r + i)),
                            y + (int)(sinf(ang) * (r + i)), argb(46 - i * 12,
                            (col >> 16) & 0xff, (col >> 8) & 0xff, col & 0xff));
            }
    }
}

static void draw_window(FB *fb, int x, int y, int w, int h, float t, float threat) {
    fb_rect(fb, x - 4, y - 4, w + 8, h + 8, rgb_(24, 26, 30));
    fb_vgrad(fb, x, y, w, h, rgb_(14, 20, 33), rgb_(30, 24, 34));
    /* far away city glow */
    for (int i = 0; i < 9; i++) {
        int bx = x + ((i * 37 + 11) % w);
        int bw = 8 + (i * 13) % 22;
        int bh = 10 + (i * 29) % 34;
        fb_rect_a(fb, bx, y + h - bh, bw, bh, argb(120, 14, 16, 24));
        for (int wy = 0; wy < bh - 4; wy += 4)
            for (int wx = 2; wx < bw - 2; wx += 4)
                if (((i * 7 + wy + wx) % 11) == 0)
                    fb_rect_a(fb, bx + wx, y + h - bh + 2 + wy, 1, 1,
                              argb(180, 220, 170, 90));
    }
    fb_rect_a(fb, x, y + h - 26, w, 26, argb(40, 30, 40, 60));
    /* rain */
    for (int i = 0; i < 70; i++) {
        float sp = 150.0f + (float)((i * 53) % 120);
        float yy = fmodf((float)((i * 37) % 1000) + t * sp, (float)h + 20.0f) - 10.0f;
        int xx = x + ((i * 67 + (int)(t * 6.0f)) % w);
        int len = 3 + (i % 4);
        fb_line(fb, xx, y + (int)yy, xx - 1, y + (int)yy + len, argb(110, 175, 200, 235));
    }
    /* streaks on the glass */
    for (int i = 0; i < 12; i++) {
        int sx = x + ((i * 97) % w);
        fb_rect_a(fb, sx, y, 1, h, argb(16, 200, 220, 255));
    }
    /* frame bars */
    fb_rect(fb, x + w / 2 - 2, y, 4, h, rgb_(18, 19, 22));
    fb_rect(fb, x, y + h / 3 - 1, w, 3, rgb_(18, 19, 22));
    /* a silhouette outside, when it is watching */
    if (threat > 0.55f) {
        float a = (threat - 0.55f) / 0.45f;
        int cx = x + w - 34, cy = y + h - 30;
        fb_disc(fb, cx, cy - 26, 7, argb((int)(150 * a), 2, 2, 3));
        fb_rect_a(fb, cx - 9, cy - 18, 18, 30, argb((int)(140 * a), 2, 2, 3));
    }
    (void)threat;
}

static void draw_rain_inside(FB *fb, float t) { (void)fb; (void)t; }

/* ------------------------------------------------------------- night data */
static const Night *night(void) { return &NIGHTS[G.night]; }

static void build_bank(void) {
    static const char *pool[] = {
        "DOOR", "HULL", "WATER", "SIGNAL", "MACHINE", "LISTEN", "SHADOW",
        "MOUTH", "SOUND", "ROOM", "EYES", "WINDOW", "FOOTSTEP", "CLOSET",
        "SLEEP", "MOTHER", "STATIC", "CEILING", "MOUTH", "TEETH"
    };
    const TapeLine *ln = &night()->lines[G.line];
    int nb = 0;
    int na = night()->blanks;
    for (int i = 0; i < BLANKS_MAX; i++) {
        const char *ans = ln->answers[i];
        if (!ans || !ans[0] || nb >= BANK_MAX) continue;
        int dup = 0;
        for (int k = 0; k < nb; k++) if (!strcmp(G.bank[k], ans)) dup = 1;
        if (dup) continue;
        strncpy(G.bank[nb], ans, 23); G.bank[nb][23] = 0; nb++;
    }
    int want = nb + 4;
    if (want > BANK_MAX) want = BANK_MAX;
    int guard = 0;
    while (nb < want && guard++ < 400) {
        const char *w = pool[rng_i(&G.rng, (int)(sizeof(pool) / sizeof(pool[0])))];
        int dup = 0;
        for (int k = 0; k < nb; k++) if (!strcmp(G.bank[k], w)) dup = 1;
        if (!dup) { strncpy(G.bank[nb], w, 23); G.bank[nb][23] = 0; nb++; }
    }
    /* shuffle */
    for (int i = nb - 1; i > 0; i--) {
        int j = rng_i(&G.rng, i + 1);
        char tmp[24]; memcpy(tmp, G.bank[i], 24);
        memcpy(G.bank[i], G.bank[j], 24);
        memcpy(G.bank[j], tmp, 24);
    }
    G.nbank = nb;
    for (int i = 0; i < BANK_MAX; i++) G.bank_used[i] = 0;
    G.filled = 0;
    (void)na;
}

static void start_night(int n) {
    G.night = n;
    const Night *N = &NIGHTS[n];
    G.low_cut = 0.1f; G.high_cut = 0.95f;
    G.target_lo = N->band_lo; G.target_hi = N->band_hi;
    G.playing = 0; G.reel_a = 0; G.reel_b = 1.2f;
    G.line = 0; G.filled = 0; G.solving = 0;
    for (int i = 0; i < APP_COUNT; i++) { G.app_on[i] = 0; G.app_lvl[i] = 0; }
    G.radio_freq = 40.0f;
    G.capacity = N->capacity;
    G.blackout = 0; G.blackout_t = 0; G.over_t = 0; G.bath_open = 0;
    G.interference = 0.12f; G.red_t = 0; G.dead_t = 0;
    G.shadow = 0; G.closet = 0; G.drip_t = 0;
    G.ev_kind = EV_NONE; G.ev_t = 0; G.ev_dur = 0;
    G.attack_active = 0; G.attack_t = 0; G.attack_app = -1;
    G.whisper_t = 0; G.flash = 0;
    build_bank();

    audio_reset();
    AP.clip = NIGHTS[n].clip;
    AP.night = n;
    AP.entity_freq = N->freq;
    set_screen(SCR_BRIEF);
}

/* ------------------------------------------------------------- simulation */
static float app_noise(void) {
    return 0.11f * G.app_lvl[APP_FAN] + 0.17f * G.app_lvl[APP_MICRO] +
           0.28f * G.app_lvl[APP_SHOWER] + 0.10f * G.app_lvl[APP_RADIO];
}

static float suppression(void) {
    float ef = night()->freq;
    if (G.night == 4) ef = night()->freq + 9.0f * sinf(G.t * 0.25f);
    float s = 0.0f;
    s += G.app_lvl[APP_FAN] * gauss(118.0f - ef, 15.0f) * 1.00f;
    s += G.app_lvl[APP_MICRO] * gauss(63.0f - ef, 13.0f) * 1.00f;
    s += G.app_lvl[APP_SHOWER] * 0.42f;
    s += G.app_lvl[APP_RADIO] * gauss(G.radio_freq - ef, 11.0f) * 1.20f;
    return clampf(s, 0.0f, 1.15f);
}

static float compute_load(void) {
    float l = G.playing ? 0.10f : 0.03f;
    l += G.app_lvl[APP_FAN] * 0.13f;
    l += G.app_lvl[APP_MICRO] * 0.33f;
    l += G.app_lvl[APP_SHOWER] * 0.21f;
    l += G.app_lvl[APP_RADIO] * 0.09f;
    return l;
}

static void blow_fuse(void) {
    G.blackout = 1;
    G.blackout_t = 0;
    for (int i = 0; i < APP_COUNT; i++) G.app_on[i] = 0;
    G.playing = 0;
    G.interference = clampf(G.interference + 0.22f, 0, 1.2f);
    G.flash = 1.0f;
    sfx_breaker();
    sfx_thud(0.6f);
}

static void die(void) {
    if (G.scr == SCR_DEAD) return;
    G.dead_t = 0;
    AP.jumpscare = 1.0f;
    sfx_stinger();
    set_screen(SCR_DEAD);
}

static void advance_line(void) {
    G.line++;
    G.flash_ok = 1.0f;
    sfx_chime(0.35f);
    const Night *N = night();
    if (G.line >= N->nlines) {
        G.playing = 0;
        set_screen(SCR_NIGHTEND);
        sfx_creak(0.9f);
        sfx_whisper();
        G.interference = clampf(G.interference + 0.10f, 0, 1.2f);
        return;
    }
    G.target_lo = clampf(N->band_lo + rng_range(&G.rng, -0.07f, 0.07f), 0.05f, 0.9f);
    G.target_hi = clampf(N->band_hi + rng_range(&G.rng, -0.07f, 0.07f), 0.15f, 0.95f);
    build_bank();
    G.interference = clampf(G.interference + 0.05f, 0, 1.2f);
}

static void trigger_event(int kind) {
    G.ev_kind = kind;
    G.ev_t = 0;
    switch (kind) {
    case EV_CREAK:
        G.ev_dur = 2.6f;
        sfx_creak(0.75f);
        G.closet = clampf(G.closet + 0.22f, 0, 1);
        G.interference = clampf(G.interference + 0.03f, 0, 1.2f);
        break;
    case EV_SHADOW:
        G.ev_dur = 4.0f;
        G.shadow = clampf(G.shadow + 0.4f, 0, 1);
        G.interference = clampf(G.interference + 0.05f, 0, 1.2f);
        break;
    case EV_BREATH:
        G.ev_dur = 5.0f;
        sfx_whisper();
        break;
    case EV_MIMIC: {
        int pick = rng_i(&G.rng, APP_COUNT);
        for (int i = 0; i < APP_COUNT; i++) {
            int k = (pick + i) % APP_COUNT;
            if (G.app_on[k] > 0.5f) { G.app_on[k] = 0; G.ev_app = k; break; }
        }
        G.ev_dur = 3.4f;
        sfx_switch(0.7f);
        break;
    }
    case EV_ATTACK: {
        G.attack_active = 1;
        G.attack_app = rng_i(&G.rng, APP_COUNT);
        G.attack_t = 9.0f;
        G.ev_dur = 9.0f;
        break;
    }
    }
}

static void update_entity(float dt) {
    const Night *N = night();
    float supp = suppression();
    float rise = (0.004f + 0.023f * G.clarity) * N->aggression;
    rise *= (1.0f - 0.88f * clampf(supp, 0, 1));
    if (!G.playing) rise *= 0.12f;
    if (G.blackout) rise += 0.020f;
    G.interference += rise * dt;
    G.interference -= (0.075f * clampf(supp, 0, 1) + 0.004f) * dt;
    G.interference = clampf(G.interference, 0.0f, 1.25f);

    /* manifestations */
    G.ev_t += dt;
    float gap = N->event_gap * (1.15f - 0.25f * G.interference);
    if (G.ev_kind == EV_NONE && G.ev_t > gap && G.scr != SCR_BRIEF) {
        int kinds[5], n = 0;
        if (N->event_mask & 0x01) kinds[n++] = EV_CREAK;
        if (N->event_mask & 0x02) kinds[n++] = EV_SHADOW;
        if (N->event_mask & 0x04) kinds[n++] = EV_BREATH;
        if (N->event_mask & 0x08) kinds[n++] = EV_MIMIC;
        if (N->event_mask & 0x10) kinds[n++] = EV_MIMIC;
        if (n > 0) trigger_event(kinds[rng_i(&G.rng, n)]);
        G.ev_t = rng_range(&G.rng, -2.0f, 0.0f);
    }
    if (G.ev_kind != EV_NONE) {
        G.ev_t += 0.0f;
        if (G.ev_kind == EV_BREATH) {
            if (G.ev_t > G.ev_dur) {
                if (supp > 0.35f) { G.interference = clampf(G.interference - 0.05f, 0, 1.2f); }
                else {
                    G.interference = clampf(G.interference + 0.20f, 0, 1.2f);
                    sfx_static(0.5f);
                }
                G.ev_kind = EV_NONE;
            }
        } else if (G.ev_kind == EV_ATTACK) {
            G.attack_t -= dt;
            if (G.attack_t <= 0.0f) {
                int ok = G.app_on[G.attack_app] > 0.5f && !G.blackout;
                if (ok) {
                    G.interference = clampf(G.interference - 0.16f, 0, 1.2f);
                    sfx_chime(0.5f);
                } else {
                    G.interference = clampf(G.interference + 0.34f, 0, 1.2f);
                    sfx_stinger();
                    G.shake = 1.0f;
                }
                G.attack_active = 0;
                G.ev_kind = EV_NONE;
            }
        } else if (G.ev_t > G.ev_dur) {
            G.ev_kind = EV_NONE;
        }
    }

    /* the closet breathes louder the worse things get */
    G.shadow = clampf(G.shadow - 0.05f * dt, 0, 1);
    G.closet = clampf(G.closet - 0.012f * dt, 0, 1);
    if (G.closet > 0.3f && G.ev_kind != EV_CREAK) {
        G.whisper_t -= dt;
        if (G.whisper_t <= 0) { sfx_creak(0.35f); G.whisper_t = rng_range(&G.rng, 12, 25); }
    }

    if (G.interference > 0.97f) G.red_t += dt;
    else G.red_t = clampf(G.red_t - dt * 0.7f, 0, 10);
    if (G.red_t > 3.5f) die();
}

static void update_apps(float dt) {
    for (int i = 0; i < APP_COUNT; i++) {
        float target = G.blackout ? 0.0f : G.app_on[i];
        float rate = (target > G.app_lvl[i]) ? 3.2f : 2.2f;
        G.app_lvl[i] += (target - G.app_lvl[i]) * clampf(rate * dt, 0, 1);
        if (G.app_lvl[i] < 0.002f) G.app_lvl[i] = 0;
    }
    G.load = compute_load();
    if (G.load > G.capacity) {
        G.over_t += dt;
        if (G.over_t > 1.15f) blow_fuse();
    } else {
        G.over_t = clampf(G.over_t - dt, 0, 4);
    }
    if (G.blackout) {
        G.blackout_t += dt;
        if (G.blackout_t > 16.0f) die();
    }
}

static void update_audio_params(float dt) {
    AP.tape_playing = G.playing;
    AP.low_cut = G.low_cut;
    AP.high_cut = G.high_cut;
    AP.clarity = G.clarity;
    AP.tape_level = G.blackout ? 0.0f : (G.playing ? 1.0f : 0.0f);
    for (int i = 0; i < APP_COUNT; i++) AP.app_level[i] = G.app_lvl[i];
    AP.radio_freq = G.radio_freq;
    AP.interference = G.interference;
    AP.blackout = G.blackout ? clampf(G.blackout_t * 2.0f, 0, 1) : 0.0f;
    AP.entity_freq = night()->freq;
    AP.night = G.night;
    if (AP.jumpscare > 0.03f) AP.jumpscare = clampf(AP.jumpscare - 0.55f * dt, 0, 1); else AP.jumpscare = 0;
    AP.static_burst = clampf(G.flash_bad * 0.5f, 0, 1);
}

static void update(float dt) {
    G.t += dt;
    G.scr_t += dt;
    G.flash_ok = clampf(G.flash_ok - dt * 1.4f, 0, 1);
    G.flash_bad = clampf(G.flash_bad - dt * 2.0f, 0, 1);
    G.flash = clampf(G.flash - dt * 2.0f, 0, 1);
    G.shake = clampf(G.shake - dt * 1.5f, 0, 1);

    if (G.scr == SCR_DEAD) {
        G.dead_t += dt;
        AP.jumpscare = clampf(1.0f - G.dead_t * 0.25f, 0, 1);
        if (G.dead_t > 2.6f && G.scr_t > 2.6f) { /* keep screen, waits for input */ }
        return;
    }

    if (G.scr == SCR_DESK || G.scr == SCR_ROOM) {
        /* signal quality */
        float dlo = fabsf(G.low_cut - G.target_lo) / 0.16f;
        float dhi = fabsf(G.high_cut - G.target_hi) / 0.16f;
        float miss = clampf((dlo + dhi) * 0.5f, 0, 1);
        float c = 1.0f - miss * 1.12f - app_noise();
        if (!G.playing) c *= 0.25f;
        if (G.blackout) c = 0;
        G.clarity = clampf(c, 0, 1);
        G.signal = clampf(G.clarity + 0.10f * sinf(G.t * 6.0f) * (1.0f - G.clarity), 0, 1);

        if (G.playing) {
            float sp = (G.blackout ? 0.0f : 1.0f) * (0.6f + 0.4f * G.clarity);
            G.reel_a += dt * sp * 2.4f;
            G.reel_b += dt * sp * 2.4f;
            AP.clip = night()->clip;
        }
        update_apps(dt);
        update_entity(dt);
        update_audio_params(dt);
    } else {
        G.clarity = 0.0f;
        G.playing = 0;
        AP.tape_playing = 0;
        AP.tape_level = 0;
        AP.interference = (G.scr == SCR_TITLE) ? 0.06f : G.interference * 0.6f;
        AP.entity_freq = (G.scr == SCR_TITLE) ? 63.0f : night()->freq;
        for (int i = 0; i < APP_COUNT; i++) AP.app_level[i] = 0;
        AP.blackout = (G.scr == SCR_TITLE) ? 0.0f : AP.blackout;
        AP.jumpscare = (G.scr == SCR_NIGHTEND) ? 0.0f : AP.jumpscare;
    }

    if (G.scr == SCR_HELP || G.scr == SCR_BRIEF || G.scr == SCR_NIGHTEND ||
        G.scr == SCR_ENDING) {
        G.tw += dt * 42.0f;
    }
}

/* ================================================================ RENDER  */
static void screen_fx(FB *fb) {
    /* interference: displaced scan strips */
    float inf = clampf(G.interference, 0, 1);
    if (inf > 0.35f && (G.scr == SCR_DESK || G.scr == SCR_ROOM)) {
        int rows = (int)((inf - 0.35f) * 40.0f);
        u32 s = (u32)(G.t * 913.0f) + 7u;
        for (int i = 0; i < rows; i++) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            int y = (int)((s >> 8) % (u32)fb->h);
            int dx = (int)((s >> 3) % 9u) - 4;
            u32 *row = fb->px + y * fb->w;
            if (dx > 0) {
                memmove(row + dx, row, sizeof(u32) * (size_t)(fb->w - dx));
                for (int k = 0; k < dx; k++) row[k] = row[dx];
            } else if (dx < 0) {
                int d = -dx;
                memmove(row, row + d, sizeof(u32) * (size_t)(fb->w - d));
                for (int k = fb->w - d; k < fb->w; k++) row[k] = row[fb->w - d - 1];
            }
        }
    }
    fb_vignette(fb, 0.85f);
    fb_scanlines(fb, 26);
    G.grain_phase += 0.016f;
    fb_grain(fb, 0.05f + 0.10f * inf, &G.grain_seed);
    if (G.flash > 0.01f) fb_rect_a(fb, 0, 0, fb->w, fb->h, argb((int)(G.flash * 200), 255, 250, 240));
    if (G.flash_bad > 0.01f) fb_rect_a(fb, 0, 0, fb->w, fb->h, argb((int)(G.flash_bad * 60), 180, 20, 20));
}

/* ---------------------------------------------------------------- labels  */
static void sticker(FB *fb, int x, int y, const char *s, u32 col) {
    int w = txt_w(s, 1) + 4;
    fb_rect_a(fb, x - 2, y - 2, w + 1, 13, argb(200, 12, 10, 8));
    fb_rect_a(fb, x - 2, y - 2, w + 1, 1, argb(90, 180, 170, 140));
    txt_draw(fb, x, y, s, col, 1);
}

/* ------------------------------------------------------------------ deck  */
static void draw_knob(FB *fb, int cx, int cy, int r, float val, int id, int highlight) {
    fb_disc(fb, cx, cy + 1, r + 3, argb(140, 6, 6, 6));
    fb_disc(fb, cx, cy, r + 1, rgb_(40, 42, 44));
    for (int i = 0; i <= 10; i++) {
        float a = -2.3f + (float)i * 0.46f;
        fb_line(fb, cx + (int)(cosf(a) * (float)(r + 3)), cy + (int)(sinf(a) * (float)(r + 3)),
                cx + (int)(cosf(a) * (float)(r + 6)), cy + (int)(sinf(a) * (float)(r + 6)),
                argb(150, 195, 195, 180));
    }
    fb_disc(fb, cx, cy, r - 1, rgb_(28, 30, 32));
    fb_disc(fb, cx, cy - 1, r - 3, rgb_(46, 48, 50));
    float a = -2.3f + val * 4.6f;
    fb_line(fb, cx, cy, cx + (int)(cosf(a) * (float)(r - 2)),
            cy + (int)(sinf(a) * (float)(r - 2)), rgb_(240, 225, 185));
    fb_disc(fb, cx, cy, 3, rgb_(80, 80, 80));
    if (highlight || hot(cx - r - 7, cy - r - 7, (r + 7) * 2, (r + 7) * 2))
        fb_ring(fb, cx, cy, r + 8, argb(110, 250, 225, 160));
    hit(cx - r - 7, cy - r - 7, (r + 7) * 2, (r + 7) * 2, id);
}

static void draw_deck(FB *fb) {
    int x = 12, y = 176, w = 252, h = 158;
    bevel(fb, x, y, w, h, rgb_(36, 38, 39));
    fb_rect_a(fb, x + 3, y + 3, w - 6, 14, argb(70, 0, 0, 0));
    txt_draw(fb, x + 8, y + 6, "MR-4  REEL DECK", argb(215, 195, 205, 195), 1);


    /* reels */
    for (int r = 0; r < 2; r++) {
        int cx = x + 52 + r * 68, cy = y + 52;
        fb_disc(fb, cx, cy, 28, argb(160, 4, 4, 4));
        fb_disc(fb, cx, cy, 25, rgb_(52, 45, 38));
        /* wound tape: concentric rings with a little shimmer */
        for (int k = 0; k < 4; k++)
            fb_ring(fb, cx, cy, 24 - k * 5, argb(90, 96, 84, 70));
        float a0 = (r == 0) ? G.reel_a : G.reel_b;
        for (int i = 0; i < 8; i++) {
            float a = a0 + (float)i * 0.7853981f;
            fb_line(fb, cx + (int)(cosf(a) * 8), cy + (int)(sinf(a) * 8),
                    cx + (int)(cosf(a) * 20), cy + (int)(sinf(a) * 20),
                    argb(210, 22, 18, 15));
        }
        fb_disc(fb, cx, cy, 9, rgb_(96, 90, 78));
        fb_disc(fb, cx, cy, 4, rgb_(18, 18, 18));
        fb_ring(fb, cx, cy, 25, argb(150, 14, 12, 10));
        txt_draw(fb, cx - 18, cy + 30, (r == 0) ? "SUPPLY" : "TAKEUP",
                 argb(110, 150, 150, 138), 1);
    }
    /* tape across the heads */
    fb_line(fb, x + 80, y + 40, x + 120, y + 34, argb(220, 120, 110, 88));
    fb_line(fb, x + 80, y + 40, x + 80, y + 34, argb(150, 120, 110, 88));
    fb_rect(fb, x + 96, y + 33, 8, 6, rgb_(78, 76, 70));

    /* knobs on the right, labels to the left of each */
    {
        char vt[16];
        int cx = x + 206;
        int cy1 = y + 46, cy2 = y + 94;
        draw_knob(fb, cx, cy1, 17, G.low_cut, HIT_KNOB_LOW, G.drag == HIT_KNOB_LOW);
        draw_knob(fb, cx, cy2, 17, G.high_cut, HIT_KNOB_HIGH, G.drag == HIT_KNOB_HIGH);
        txt_draw(fb, x + 148, cy1 - 9, "LOW", argb(205, 200, 196, 176), 1);
        txt_draw(fb, x + 148, cy1 + 1, "CUT", argb(205, 200, 196, 176), 1);
        snprintf(vt, sizeof vt, "%02d", (int)(G.low_cut * 100));
        txt_draw(fb, x + 148, cy1 + 12, vt, argb(215, 240, 200, 130), 1);
        txt_draw(fb, x + 148, cy2 - 9, "HIGH", argb(205, 200, 196, 176), 1);
        txt_draw(fb, x + 148, cy2 + 1, "CUT", argb(205, 200, 196, 176), 1);
        snprintf(vt, sizeof vt, "%02d", (int)(G.high_cut * 100));
        txt_draw(fb, x + 148, cy2 + 12, vt, argb(215, 240, 200, 130), 1);
    }

    /* transport */
    int tx = x + 14, ty = y + 118;
    txt_draw(fb, tx, ty - 12, "TRANSPORT", argb(130, 165, 165, 150), 1);
    bevel(fb, tx, ty, 56, 22, G.playing ? rgb_(52, 48, 42) : rgb_(33, 33, 33));
    txt_center(fb, tx + 30, ty + 6, G.playing ? "STOP" : "PLAY", rgb_(225, 225, 205), 1);
    led(fb, tx + 6, ty + 11, 3, rgb_(255, 60, 40), G.playing);
    hover_ring(fb, tx, ty, 56, 22, hot(tx, ty, 56, 22));
    hit(tx, ty, 56, 22, HIT_TRANSPORT);

    /* paper label: the frequency to match */
    {
        char lab[32];
        snprintf(lab, sizeof lab, "%s / %dHZ", night()->tape_label, (int)night()->freq);
        sticker(fb, x + w - txt_w(lab, 1) - 16, y + 2, lab, argb(235, 215, 185, 125));
    }

    /* level ladder */
    int lx = x + 90, ly = ty + 8;
    txt_draw(fb, lx, ly - 12, "LEVEL", argb(125, 165, 165, 150), 1);
    for (int i = 0; i < 8; i++) {
        int on = (G.playing && G.signal * 8.0f > (float)i);
        u32 c = i < 5 ? rgb_(90, 240, 110) : (i < 7 ? rgb_(250, 200, 70) : rgb_(250, 60, 50));
        fb_rect(fb, lx + i * 11, ly, 8, 8, on ? c : rgb_(20, 22, 20));
        fb_frame(fb, lx + i * 11, ly, 8, 8, argb(70, 70, 70, 65));
    }

    /* power/clock LED */
    led(fb, x + w - 12, y + h - 10, 2, rgb_(255, 180, 60), 1);
}

static void draw_spl(FB *fb) {
    int x = 12, y = 148, w = 252, h = 24;
    bevel(fb, x, y, w, h, rgb_(30, 32, 33));
    txt_draw(fb, x + 6, y + 8, "SPL", argb(210, 220, 220, 200), 1);
    int mx0 = x + 30, mx1 = x + w - 30;
    fb_rect(fb, mx0, y + 16, mx1 - mx0, 1, argb(120, 200, 200, 180));
    for (int i = 0; i <= 12; i++) {
        int tx = mx0 + (mx1 - mx0) * i / 12;
        fb_rect(fb, tx, y + 12, 1, 5, argb(120, 180, 180, 170));
    }
    /* red zone */
    fb_rect_a(fb, mx0 + (mx1 - mx0) * 9 / 12, y + 10, (mx1 - mx0) * 3 / 12, 3,
              argb(180, 200, 40, 30));
    float v = clampf(G.interference, 0, 1.15f) / 1.15f;
    int nx = mx0 + (int)((float)(mx1 - mx0) * v);
    fb_rect(fb, nx - 1, y + 5, 2, 13, rgb_(240, 235, 210));
    fb_rect(fb, nx, y + 5, 1, 13, argb(160, 255, 240, 200));

}

static const char *app_name(int i) {
    switch (i) {
        case APP_FAN: return "FAN";
        case APP_MICRO: return "MICROWAVE";
        case APP_SHOWER: return "SHOWER";
        case APP_RADIO: return "RADIO";
    }
    return "?";
}
static const char *app_freq_text(int i) {
    switch (i) {
        case APP_FAN: return "118 HZ";
        case APP_MICRO: return "63 HZ";
        case APP_SHOWER: return "BROADBAND";
        case APP_RADIO: return "TUNED";
    }
    return "";
}

/* ------------------------------------------------------------------- CRT  */
static int crt_x = 280, crt_y = 148, crt_w = 348, crt_h = 192;

static void crt_begin(FB *fb, u32 bg) {
    bevel(fb, crt_x, crt_y, crt_w, crt_h, rgb_(30, 31, 30));
    fb_rect(fb, crt_x + 6, crt_y + 6, crt_w - 12, crt_h - 30, rgb_(10, 12, 10));
    fb_vgrad(fb, crt_x + 8, crt_y + 8, crt_w - 16, crt_h - 34, bg, rgb_(2, 6, 3));
}

static void crt_end(FB *fb) {
    int sx = crt_x + 8, sy = crt_y + 8, sw = crt_w - 16, sh = crt_h - 34;
    for (int y = sy; y < sy + sh; y += 2) fb_rect_a(fb, sx, y, sw, 1, argb(50, 0, 0, 0));
    /* glass reflection */
    for (int i = 0; i < 40; i++)
        fb_rect_a(fb, sx + i, sy + 2, 1, 26, argb(10 + i / 4, 200, 230, 210));
    /* bezel furniture */
    txt_draw(fb, crt_x + 10, crt_y + crt_h - 20, "ARCH-3 TERMINAL", argb(120, 160, 160, 150), 1);
    led(fb, crt_x + crt_w - 22, crt_y + crt_h - 16, 3, rgb_(120, 255, 140), 1);
}

/* draw a transcript line with redactions */
static void draw_transcript(FB *fb, int x, int y, int maxchars, int scale) {
    const TapeLine *ln = &night()->lines[G.line];
    const char *s = ln->text;
    int cx = x, cy = y;
    int blank_i = 0;
    while (*s) {
        char tok[64];
        int n = 0, blank = 0;
        if (*s == '#') { blank = 1; s++; }
        while (*s && *s != ' ' && *s != '#' && n < 63) tok[n++] = *s++;
        tok[n] = 0;
        if (!n && !blank) { if (*s == ' ') s++; continue; }

        const char *disp = tok;
        u32 col = rgb_(120, 225, 140);
        int is_blank = 0;
        if (blank) {
            is_blank = 1;
            if (blank_i < G.filled) {
                disp = G.chosen[blank_i];
                col = rgb_(190, 255, 200);
            } else if (G.clarity > 0.55f) {
                disp = ln->answers[blank_i];
                col = rgb_(72, 132, 84);
            } else {
                disp = "######";
                col = rgb_(42, 86, 54);
            }
        }
        int dw = (int)strlen(disp) * FONT_W * scale;
        int needsp = (cx != x);
        if (cx + dw + (needsp ? FONT_W * scale : 0) > x + maxchars * FONT_W * scale) {
            cx = x; cy += FONT_H * scale + 3;
        }
        if (needsp) cx += FONT_W * scale;
        txt_draw(fb, cx, cy, disp, col, scale);
        if (is_blank && blank_i >= G.filled)
            fb_rect_a(fb, cx, cy + FONT_H * scale - 1, dw, 1, argb(90, 120, 210, 130));
        cx += dw;
        if (blank) blank_i++;
        if (*s == ' ') s++;
    }
}

/* ================================================================= TITLE  */
static void render_title(FB *fb) {
    fb_clear(fb, rgb_(6, 7, 8));
    draw_window(fb, 120, 40, 400, 200, G.t, 0.25f);
    fb_shade(fb, 0, 0, fb->w, fb->h, 0.45f);

    int jitter = (int)(sinf(G.t * 7.0f) * 1.2f);
    txt_draw_shadow(fb, fb->w / 2 - txt_w("DEAD AIR", 5) / 2 + jitter, 214,
                    "DEAD AIR", rgb_(232, 230, 214), 5);
    txt_center(fb, fb->w / 2, 262, "AN AUDITORY HAUNTING  /  1996",
               argb(200, 205, 175, 120), 1);

    const char *items[3] = { "BEGIN CONTRACT", "HOW TO LISTEN", "QUIT" };
    for (int i = 0; i < 3; i++) {
        int y = 288 + i * 15;
        int w = txt_w(items[i], 1);
        txt_draw(fb, fb->w / 2 - w / 2, y, items[i], rgb_(210, 210, 200), 1);
        hit(fb->w / 2 - w / 2 - 8, y - 4, w + 16, 13, HIT_MENU + i);
        int hovered = (G.mx >= fb->w / 2 - w / 2 - 8 && G.mx <= fb->w / 2 + w / 2 + 8 && G.my >= y - 4 && G.my <= y + 9);
        if (hovered) G.menu_sel = i;
        if (G.menu_sel == i)
            txt_draw(fb, fb->w / 2 - w / 2 - 12, y, ">", rgb_(230, 190, 110), 1);
    }
    txt_center(fb, fb->w / 2, 344, "MOUSE + SPACE", argb(120, 160, 160, 150), 1);
    screen_fx(fb);
}

/* ================================================================ HELP    */
static void render_help(FB *fb) {
    fb_clear(fb, rgb_(8, 9, 10));
    fb_rect_a(fb, 40, 24, fb->w - 80, fb->h - 48, argb(220, 14, 15, 16));
    fb_frame(fb, 40, 24, fb->w - 80, fb->h - 48, argb(120, 120, 130, 110));
    txt_draw(fb, 52, 34, "HOW TO LISTEN", rgb_(220, 200, 150), 2);
    const char *lines[] = {
        "1. PLAY THE TAPE. READ THE TRANSCRIPT ON THE CRT.",
        "2. TURN THE LOW CUT AND HIGH CUT KNOBS UNTIL THE",
        "   REDACTED WORDS COME BACK. THE LEVEL LADDER TELLS",
        "   YOU HOW CLEAN THE SIGNAL IS.",
        "3. CLICK THE MISSING WORD IN THE WORD BANK (OR PRESS",
        "   ITS NUMBER). WRONG WORDS MAKE IT ANGRY.",
        "4. WHILE THE VOICE IS CLEAR, THE THING ON THE TAPE",
        "   PUSHES INTO YOUR ROOM. WATCH THE SPL METER.",
        "5. STAND UP WITH SPACE. RUN SOMETHING THAT HUMS AT",
        "   THE SAME FREQUENCY PRINTED ON THE TAPE LABEL.",
        "6. EVERY MACHINE ADDS LOAD. TOO MUCH LOAD BLOWS THE",
        "   FUSE. THE BREAKER IS BY THE FRONT DOOR.",
        "7. IF THE METER STAYS IN THE RED, IT GETS IN.",
        NULL
    };
    for (int i = 0; lines[i]; i++)
        txt_draw(fb, 54, 58 + i * 13, lines[i], rgb_(170, 200, 175), 1);
    txt_draw(fb, 54, 250, "YOU DO NOT WIN BY BEING QUIET.", rgb_(220, 180, 120), 1);
    txt_draw(fb, 54, 262, "YOU WIN BY DROWNING IT OUT AND FINISHING THE WORK.",
               rgb_(220, 180, 120), 1);
    txt_center(fb, fb->w / 2, 320, "[ CLICK OR PRESS SPACE ]", argb(180, 200, 200, 180), 1);
    hit(0, 0, fb->w, fb->h, HIT_NEXT);
    screen_fx(fb);
}

/* ============================================================= BRIEFING   */
static void render_brief(FB *fb) {
    const Night *N = night();
    fb_clear(fb, rgb_(7, 8, 9));
    fb_rect_a(fb, 60, 30, fb->w - 120, fb->h - 60, argb(230, 12, 13, 14));
    fb_frame(fb, 60, 30, fb->w - 120, fb->h - 60, argb(130, 130, 130, 110));
    fb_rect(fb, 60, 30, fb->w - 120, 3, rgb_(90, 80, 50));
    txt_draw(fb, 76, 44, N->code, rgb_(210, 190, 130), 1);
    txt_draw(fb, fb->w - 76 - txt_w(N->tape_label, 2), 40, N->tape_label,
             rgb_(200, 200, 190), 2);
    txt_draw(fb, 76, 60, N->subject, argb(170, 160, 190, 170), 1);
    fb_rect_a(fb, 76, 74, fb->w - 152, 1, argb(120, 140, 120, 100));

    char buf[220];
    int shown = (int)G.tw;
    int used = 0;
    for (int i = 0; N->brief[i]; i++) {
        int len = (int)strlen(N->brief[i]) + 1;
        int take = shown - used;
        if (take <= 0) break;
        if (take > 200) take = 200;
        int n = len < take ? len : take;
        memcpy(buf, N->brief[i], (size_t)(n - 1));
        buf[n - 1] = 0;
        txt_draw(fb, 78, 86 + i * 14, buf, rgb_(175, 205, 180), 1);
        used += len;
        if (take < len) break;
    }
    txt_draw(fb, 76, 210, "TAPE FREQUENCY", argb(150, 160, 150, 140), 1);
    snprintf(buf, sizeof buf, "%d HZ", (int)N->freq);
    txt_draw(fb, 76, 222, buf, rgb_(220, 190, 120), 2);
    txt_draw(fb, 220, 210, "MAINS CAPACITY", argb(150, 160, 150, 140), 1);
    snprintf(buf, sizeof buf, "%d%%", (int)(N->capacity * 100));
    txt_draw(fb, 220, 222, buf, rgb_(150, 200, 220), 2);
    txt_draw(fb, 360, 210, "LINES", argb(150, 160, 150, 140), 1);
    snprintf(buf, sizeof buf, "%d", N->nlines);
    txt_draw(fb, 360, 222, buf, rgb_(200, 180, 210), 2);

    if (G.tw > 200.0f) {
        float a = 0.55f + 0.45f * sinf(G.t * 3.0f);
        txt_center(fb, fb->w / 2, 300, "[ SPACE TO BEGIN ]",
                   argb((int)(200 * a), 220, 210, 170), 1);
    }
    hit(0, 0, fb->w, fb->h, HIT_NEXT);
    screen_fx(fb);
}

/* ================================================================= DESK   */
static void draw_room_wall(FB *fb) {
    fb_vgrad(fb, 0, 0, fb->w, 150, rgb_(27, 30, 36), rgb_(13, 14, 18));
    /* wall texture */
    u32 s = 12345u;
    for (int i = 0; i < 300; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        int x = (int)((s >> 8) % (u32)fb->w);
        int y = (int)((s >> 3) % 150u);
        fb_rect_a(fb, x, y, 1, 1, argb(20, 60, 65, 70));
    }
    /* skirting */
    fb_rect(fb, 0, 146, fb->w, 4, rgb_(9, 10, 11));
    fb_rect_a(fb, 0, 146, fb->w, 1, argb(60, 80, 80, 70));
}

static void draw_desk_surface(FB *fb) {
    fb_vgrad(fb, 0, 150, fb->w, 210, rgb_(44, 34, 24), rgb_(20, 15, 11));
    u32 s = 999u;
    for (int i = 0; i < 900; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        int y = 152 + (int)((s >> 9) % 200u);
        int x = (int)((s >> 3) % (u32)fb->w);
        int w = 8 + (int)((s >> 20) % 60u);
        fb_rect_a(fb, x, y, w, 1, argb(18, 200, 170, 130));
    }
    fb_rect_a(fb, 0, 150, fb->w, 2, argb(120, 10, 8, 6));
    /* desk lamp pool of light */
    for (int r = 0; r < 6; r++)
        fb_rect_a(fb, 300 - r * 6, 150, 40 + r * 12, 210, argb(6, 255, 220, 160));
}

static void draw_desk_scene(FB *fb) {
    draw_room_wall(fb);
    draw_window(fb, 424, 14, 188, 128, G.t, G.interference);
    draw_desk_surface(fb);

    /* desk clutter */
    {
        /* mug */
        int mx2 = 268, my2 = 300;
        fb_disc(fb, mx2, my2, 10, rgb_(44, 40, 36));
        fb_disc(fb, mx2, my2, 8, rgb_(60, 54, 46));
        fb_disc(fb, mx2, my2 - 1, 7, rgb_(28, 18, 12));
        fb_ring(fb, mx2 + 10, my2, 4, argb(200, 70, 62, 54));
        /* papers */
        fb_rect_a(fb, 20, 152, 70, 24, argb(200, 120, 116, 104));
        fb_rect_a(fb, 24, 155, 62, 2, argb(60, 60, 58, 50));
        fb_rect_a(fb, 24, 160, 48, 1, argb(60, 60, 58, 50));
        fb_rect_a(fb, 24, 164, 54, 1, argb(60, 60, 58, 50));
        /* pen */
        fb_line(fb, 150, 352, 190, 344, argb(190, 200, 190, 120));
        /* headphone cable */
        for (int i = 0; i < 22; i++)
            fb_blend_px(fb, 300 + i, 344 + (int)(sinf((float)i * 0.5f) * 3), argb(110, 20, 20, 20));
    }

    /* a dark photograph clipped to the wall */
    fb_rect_a(fb, 300, 40, 54, 40, argb(230, 30, 28, 24));
    fb_rect_a(fb, 303, 43, 48, 34, argb(255, 12, 13, 14));
    txt_draw(fb, 306, 46, "DO NOT", argb(120, 180, 170, 140), 1);
    txt_draw(fb, 306, 56, "ARCHIVE", argb(120, 180, 170, 140), 1);

    draw_spl(fb);
    draw_deck(fb);
    draw_crt(fb);

    /* ---- hints on the desk front ---- */
    txt_draw(fb, 12, 340, "SPACE: STAND UP", argb(120, 190, 180, 150), 1);
    txt_draw(fb, 128, 340, "ESC: PAUSE", argb(90, 150, 145, 120), 1);
    const char *st = G.blackout ? "NO POWER" : (G.playing ? "TAPE IS ROLLING" : "TAPE STOPPED");
    txt_draw(fb, fb->w - 12 - txt_w(st, 1), 340, st, argb(120, 190, 180, 150), 1);
}

static void draw_crt(FB *fb) {
    crt_begin(fb, rgb_(6, 20, 10));
    int sx = crt_x + 12, sy = crt_y + 12;
    char buf[160];
    const Night *N = night();

    /* ---- header ---- */
    snprintf(buf, sizeof buf, "%s // %s", N->tape_label, N->subject);
    txt_draw(fb, sx, sy + 2, buf, rgb_(66, 150, 86), 1);
    snprintf(buf, sizeof buf, "LINE %d OF %d", G.line + 1, N->nlines);
    txt_draw(fb, sx, sy + 15, buf, rgb_(96, 200, 118), 1);
    int bx = sx + crt_w - 24 - 90;
    txt_draw(fb, bx - 30, sy + 15, "SIG",
             G.signal > 0.55f ? rgb_(130, 250, 150) : rgb_(70, 150, 90), 1);
    for (int i = 0; i < 10; i++) {
        int on = G.signal * 10.0f > (float)i;
        fb_rect(fb, bx + i * 9, sy + 16, 7, 7,
                on ? (i > 7 ? rgb_(210, 225, 110) : rgb_(95, 225, 125)) : rgb_(12, 34, 18));
    }
    fb_rect_a(fb, sx, sy + 26, crt_w - 24, 1, argb(120, 50, 130, 70));

    if (G.blackout) {
        txt_draw(fb, sx, sy + 44, "NO SIGNAL", rgb_(80, 130, 88), 2);
        txt_draw(fb, sx, sy + 68, "POWER FAILURE.", rgb_(60, 110, 70), 1);
        txt_draw(fb, sx, sy + 80, "THE BREAKER IS BY THE FRONT DOOR.", rgb_(50, 95, 62), 1);
        crt_end(fb);
        return;
    }

    /* ---- live prediction feed (tape 5) ---- */
    if (N->attack_night && G.attack_active) {
        snprintf(buf, sizeof buf, "LIVE FEED  T-%04.1fS   IMPACT: %s", G.attack_t,
                 app_name(G.attack_app));
        txt_draw(fb, sx, sy + 32, buf, rgb_(225, 195, 95), 1);
    }

    /* ---- transcript ---- */
    draw_transcript(fb, sx, sy + 38, 52, 1);

    /* ---- scope ---- */
    {
        int sc_y = sy + 74, sc_h = 30;
        fb_rect_a(fb, sx, sc_y, crt_w - 24, sc_h, argb(70, 8, 40, 20));
        fb_frame(fb, sx, sc_y, crt_w - 24, sc_h, argb(90, 40, 110, 60));
        int amp = (int)(2.0f + 9.0f * G.signal);
        int noise = (int)(G.interference * 6.0f);
        u32 seed = 991u;
        int py = sc_y + sc_h / 2;
        for (int x = 0; x < crt_w - 26; x++) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            float ph = (float)x * 0.22f + G.t * 6.0f;
            int y = sc_y + sc_h / 2 + (int)(sinf(ph) * (float)amp * (0.4f + 0.6f * G.signal))
                    + (int)((seed % 7) - 3) * noise / 4;
            if (y < sc_y + 1) y = sc_y + 1;
            if (y > sc_y + sc_h - 2) y = sc_y + sc_h - 2;
            fb_rect(fb, sx + 1 + x, y, 1, 1, rgb_(80, 220, 110));
            py = y;
        }
        (void)py;
        if (G.signal < 0.55f && G.scr == SCR_DESK) {
            const char *msg = "SIGNAL WEAK";
            if (fmodf(G.t, 1.2f) < 0.8f)
                txt_draw(fb, sx + crt_w - 26 - txt_w(msg, 1), sc_y + 2, msg,
                         rgb_(120, 160, 90), 1);
        }
    }

    /* ---- word bank ---- */
    int by = sy + 112;
    txt_draw(fb, sx, by, "WORD BANK   CLICK OR PRESS 1-7", rgb_(66, 140, 84), 1);
    for (int i = 0; i < G.nbank; i++) {
        int col = i % 3, row = i / 3;
        int ex = sx + col * 104, ey = by + 12 + row * 15;
        int ew = txt_w(G.bank[i], 1) + 14;
        int done = G.bank_used[i];
        fb_rect_a(fb, ex, ey, ew, 13, done ? argb(50, 30, 70, 40) : argb(80, 26, 52, 30));
        fb_frame(fb, ex, ey, ew, 13, done ? argb(50, 50, 100, 60) : argb(110, 46, 110, 66));
        snprintf(buf, sizeof buf, "%d", i + 1);
        txt_draw(fb, ex + 3, ey + 3, buf, rgb_(90, 175, 100), 1);
        txt_draw(fb, ex + 11, ey + 3, G.bank[i],
                 done ? rgb_(46, 82, 52) : rgb_(160, 250, 175), 1);
        if (!done) {
            hover_ring(fb, ex, ey, ew, 13, hot(ex, ey, ew, 13));
            hit(ex, ey, ew, 13, HIT_BANK + i);
        }
    }

    /* ---- feedback ---- */
    if (G.flash_ok > 0.01f) {
        snprintf(buf, sizeof buf, "LINE ACCEPTED");
        txt_draw(fb, sx + crt_w - 26 - txt_w(buf, 1), sy + 38, buf,
                 argb((int)(255 * G.flash_ok), 200, 255, 190), 1);
    }
    if (G.flash_bad > 0.01f) {
        snprintf(buf, sizeof buf, "WRONG WORD");
        txt_draw(fb, sx + crt_w - 26 - txt_w(buf, 1), sy + 38, buf,
                 argb((int)(255 * G.flash_bad), 255, 120, 110), 1);
    }
    crt_end(fb);
}

static void render_desk(FB *fb) {
    if (G.blackout) {
        fb_clear(fb, rgb_(2, 2, 3));
        draw_desk_scene(fb);
        fb_shade(fb, 0, 0, fb->w, fb->h, 0.22f);
    } else {
        draw_desk_scene(fb);
    }
    /* danger: the room breathes red */
    if (G.interference > 0.72f) {
        float a = (G.interference - 0.72f) / 0.45f;
        float pulse = 0.55f + 0.45f * sinf(G.t * 6.5f);
        fb_rect_a(fb, 0, 0, fb->w, fb->h, argb((int)(a * pulse * 46.0f), 120, 0, 0));
        if (G.interference > 0.9f && fmodf(G.t, 1.0f) < 0.12f) {
            fb_rect_a(fb, 0, 0, fb->w, fb->h, argb((int)(a * 60.0f), 200, 40, 30));
        }
    }
    /* clickable affordances */
    hit(0, 0, 0, 0, HIT_NONE);
    hit(300, 330, 160, 24, HIT_STAND);
    screen_fx(fb);
    if (G.blackout) {
        fb_rect_a(fb, 0, 0, fb->w, fb->h, argb(190, 0, 0, 0));
        txt_center(fb, fb->w / 2, 180, "POWER FAILURE", rgb_(120, 40, 35), 2);
        txt_center(fb, fb->w / 2, 200, "STAND UP. THE BREAKER IS BY THE FRONT DOOR.",
                   rgb_(100, 60, 55), 1);
    }
}

/* ================================================================= ROOM   */
static void draw_appliance_btn(FB *fb, int x, int y, int w, int h, int id, int on,
                               const char *name, const char *sub) {
    bevel(fb, x, y, w, h, on ? rgb_(52, 54, 50) : rgb_(30, 32, 33));
    txt_draw(fb, x + 6, y + 5, name, on ? rgb_(230, 240, 200) : rgb_(130, 140, 135), 1);
    if (sub) txt_draw(fb, x + 6, y + 16, sub, on ? rgb_(180, 220, 140) : rgb_(90, 100, 95), 1);
    led(fb, x + w - 9, y + 9, 3, rgb_(120, 255, 140), on);
    hit(x, y, w, h, id);
}

static void hum(FB *fb, int x, int y, float t, float level, u32 col) {
    if (level < 0.05f) return;
    for (int i = 0; i < 3; i++) {
        float ph = fmodf(t * 1.3f + (float)i * 0.33f, 1.0f);
        int r = 4 + (int)(ph * 22.0f);
        int a = (int)((1.0f - ph) * 90.0f * level);
        if (a < 4) continue;
        fb_ring(fb, x, y, r, argb(a, (col >> 16) & 0xff, (col >> 8) & 0xff, col & 0xff));
    }
}

static void render_room(FB *fb) {
    /* ---- shell ---- */
    fb_clear(fb, rgb_(5, 5, 6));
    fb_vgrad(fb, 0, 0, fb->w, 262, rgb_(30, 29, 32), rgb_(15, 15, 17));
    /* ceiling */
    fb_vgrad(fb, 0, 0, fb->w, 20, rgb_(12, 12, 14), rgb_(20, 20, 23));
    fb_rect_a(fb, 0, 19, fb->w, 2, argb(150, 0, 0, 0));
    /* bare bulb */
    {
        fb_rect_a(fb, 318, 0, 2, 16, argb(200, 30, 30, 30));
        fb_glow(fb, 319, 20, 90, rgb_(150, 120, 70), 150);
        fb_glow(fb, 319, 20, 12, rgb_(255, 240, 200), 210);
        fb_disc(fb, 319, 20, 4, rgb_(255, 246, 214));
    }
    /* floor with perspective */
    fb_vgrad(fb, 0, 258, fb->w, 102, rgb_(38, 32, 26), rgb_(14, 13, 11));
    for (int i = 0; i < 10; i++) {
        int y0 = 258 + i * 11;
        fb_rect_a(fb, 0, y0, fb->w, 1, argb(40, 0, 0, 0));
    }
    for (int i = -5; i <= 5; i++) {
        int x0 = 320 + i * 90, x1 = 320 + i * 190;
        fb_line(fb, x0, 258, x1, 360, argb(28, 0, 0, 0));
    }
    fb_rect_a(fb, 0, 258, fb->w, 2, argb(160, 0, 0, 0));
    /* rug */
    fb_rect_a(fb, 330, 296, 190, 52, argb(90, 60, 34, 30));
    fb_frame(fb, 330, 296, 190, 52, argb(70, 90, 55, 45));

    /* ================= closet ================= */
    fb_rect_a(fb, 20, 62, 100, 196, argb(255, 30, 26, 21));
    fb_frame(fb, 20, 62, 100, 196, argb(210, 10, 9, 7));
    {
        int ajar = (int)(G.closet * 24.0f);
        fb_rect_a(fb, 24, 66, 92 - ajar, 188, argb(255, 44, 38, 30));
        for (int i = 0; i < 4; i++)
            fb_rect_a(fb, 30, 80 + i * 44, 80 - ajar, 1, argb(40, 20, 18, 14));
        if (ajar > 1) {
            fb_rect(fb, 24 + 92 - ajar, 66, ajar + 4, 188, rgb_(3, 3, 4));
            for (int i = 0; i < 3; i++)
                fb_rect_a(fb, 24 + 92 - ajar + 2, 70 + i * 60, ajar, 40, argb(60, 90, 82, 70));
        }
        fb_disc(fb, 24 + 92 - ajar - 6, 150, 2, rgb_(180, 170, 140));
    }
    txt_draw(fb, 26, 68, "CLOSET", argb(120, 170, 165, 150), 1);

    /* ================= bathroom ================= */
    fb_rect_a(fb, 136, 58, 108, 200, argb(255, 34, 30, 24));
    fb_frame(fb, 136, 58, 108, 200, argb(210, 10, 9, 7));
    if (!G.bath_open) {
        fb_rect_a(fb, 142, 64, 96, 188, argb(90, 74, 70, 56));
        fb_rect_a(fb, 152, 78, 76, 60, argb(40, 40, 36, 28));
        fb_disc(fb, 226, 158, 2, rgb_(200, 190, 150));
        {
            int sh = 2 + (int)(G.shadow * 9.0f);
            fb_rect_a(fb, 142, 244 - sh, 96, sh + 8, argb(235, 0, 0, 0));
        }
        txt_draw(fb, 146, 68, "BATH", argb(140, 165, 160, 145), 1);
        if (G.app_lvl[APP_SHOWER] > 0.05f)
            fb_rect_a(fb, 142, 246, 96, 6, argb((int)(120 * G.app_lvl[APP_SHOWER]), 190, 220, 235));
        hover_ring(fb, 136, 58, 108, 200, hot(136, 58, 108, 200));
        hit(136, 58, 108, 200, HIT_BATH);
    } else {
        fb_rect(fb, 136, 58, 108, 200, rgb_(7, 7, 8));
        fb_vgrad(fb, 142, 64, 96, 188, rgb_(26, 30, 33), rgb_(9, 10, 12));
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 12; j++)
                fb_rect_a(fb, 144 + i * 12, 66 + j * 15, 11, 14, argb(26, 130, 150, 160));
        fb_rect(fb, 176, 72, 22, 3, rgb_(130, 135, 140));
        fb_line(fb, 187, 75, 187, 84, rgb_(130, 135, 140));
        if (G.app_lvl[APP_SHOWER] > 0.02f) {
            for (int i = 0; i < 46; i++) {
                float yy = fmodf((float)(i * 37) + G.t * 240.0f, 170.0f);
                fb_line(fb, 180 + (i % 11) * 2, 86 + (int)yy, 179 + (i % 11) * 2,
                        90 + (int)yy, argb(100, 190, 225, 240));
            }
        }
        txt_draw(fb, 146, 68, "SHOWER", argb(150, 195, 205, 195), 1);
        hit(136, 58, 108, 200, HIT_BATH);
        hit(170, 62, 46, 30, HIT_SHOWER);
    }

    /* ================= kitchenette ================= */
    fb_rect_a(fb, 258, 198, 190, 62, argb(255, 36, 31, 25));
    fb_rect_a(fb, 256, 194, 194, 6, argb(255, 66, 58, 47));
    for (int i = 0; i < 2; i++) {
        fb_frame(fb, 266 + i * 88, 206, 82, 48, argb(90, 20, 18, 14));
        fb_disc(fb, 266 + i * 88 + 41, 214, 2, rgb_(140, 130, 100));
    }
    /* microwave */
    fb_rect_a(fb, 266, 160, 86, 36, argb(255, 46, 48, 46));
    fb_rect_a(fb, 270, 164, 56, 20, argb(255, 14, 14, 15));
    if (G.app_lvl[APP_MICRO] > 0.05f) {
        fb_rect_a(fb, 270, 164, 56, 20, argb((int)(70 * G.app_lvl[APP_MICRO]), 90, 225, 130));
        for (int i = 0; i < 3; i++)
            fb_rect_a(fb, 272, 168 + i * 5, 52 - i * 8, 1, argb(60, 200, 255, 210));
    }
    led(fb, 344, 166, 2, rgb_(255, 90, 60), G.app_lvl[APP_MICRO] > 0.3f);
    txt_draw(fb, 270, 186, "MICRO", argb(140, 175, 172, 150), 1);
    txt_draw(fb, 306, 186, "63HZ", argb(125, 205, 195, 135), 1);
    hum(fb, 310, 158, G.t, G.app_lvl[APP_MICRO], rgb_(120, 220, 150));
    hover_ring(fb, 262, 156, 92, 40, hot(262, 156, 92, 44));
    hit(262, 156, 92, 44, HIT_MICRO);

    /* radio */
    fb_rect_a(fb, 358, 164, 78, 32, argb(255, 42, 38, 31));
    fb_rect_a(fb, 362, 168, 26, 12, argb(255, 12, 12, 11));
    txt_draw(fb, 363, 169, "AM", argb(140, 190, 170, 130), 1);
    fb_rect_a(fb, 394, 170, 38, 4, argb(190, 22, 22, 20));
    {
        int dialx = 394 + (int)((G.radio_freq - 30.0f) / 180.0f * 38.0f);
        fb_rect(fb, dialx - 1, 167, 2, 10, rgb_(245, 225, 160));
    }
    {
        char nb[16];
        snprintf(nb, sizeof nb, "%dHZ", (int)G.radio_freq);
        txt_draw(fb, 362, 182, nb, rgb_(225, 205, 140), 1);
    }
    txt_draw(fb, 396, 182, "RADIO", argb(140, 170, 168, 148), 1);
    hum(fb, 396, 162, G.t, G.app_lvl[APP_RADIO], rgb_(230, 200, 130));
    hover_ring(fb, 394, 166, 42, 12, hot(394, 166, 42, 12));
    hit(394, 166, 42, 12, HIT_DIAL);
    hover_ring(fb, 358, 162, 30, 34, hot(358, 162, 30, 34));
    hit(358, 162, 30, 34, HIT_RADIO);

    /* ================= fan ================= */
    {
        int fy = 268;
        fb_rect_a(fb, 288, fy + 46, 40, 6, argb(255, 58, 58, 60));
        fb_rect_a(fb, 306, fy + 12, 5, 36, argb(255, 58, 58, 60));
        fb_ring(fb, 308, fy, 22, argb(240, 110, 110, 112));
        fb_ring(fb, 308, fy, 15, argb(150, 95, 95, 96));
        if (G.app_lvl[APP_FAN] > 0.05f) {
            for (int i = 0; i < 3; i++) {
                float a = G.t * 16.0f + (float)i * 2.094f;
                fb_line(fb, 308, fy, 308 + (int)(cosf(a) * 20), fy + (int)(sinf(a) * 20),
                        argb((int)(150 * G.app_lvl[APP_FAN]), 215, 215, 205));
            }
            fb_disc(fb, 308, fy, 3, argb(200, 180, 180, 175));
        }
        txt_draw(fb, 286, fy + 56, "FAN 118HZ", argb(135, 170, 168, 148), 1);
        hum(fb, 308, fy, G.t, G.app_lvl[APP_FAN], rgb_(180, 200, 210));
        hover_ring(fb, 284, fy - 26, 52, 78, hot(284, fy - 26, 52, 78));
        hit(284, fy - 26, 52, 78, HIT_FAN);
    }

    /* ================= front door ================= */
    fb_rect_a(fb, 478, 46, 128, 212, argb(255, 38, 31, 24));
    fb_frame(fb, 478, 46, 128, 212, argb(225, 12, 10, 8));
    fb_rect_a(fb, 484, 52, 116, 200, argb(80, 84, 64, 45));
    for (int i = 0; i < 3; i++)
        fb_rect_a(fb, 486, 60 + i * 66, 112, 1, argb(40, 0, 0, 0));
    fb_disc(fb, 574, 92, 3, rgb_(20, 18, 16));
    txt_draw(fb, 488, 56, "FRONT DOOR", argb(140, 155, 145, 128), 1);
    for (int i = 0; i < 3; i++) {
        fb_rect_a(fb, 566, 112 + i * 26, 30, 9, argb(255, 76, 72, 62));
        fb_rect_a(fb, 570, 114 + i * 26, 18, 5, argb(255, 130, 128, 118));
    }
    /* chain */
    for (int i = 0; i < 8; i++)
        fb_ring(fb, 566 + (i % 2) * 2, 68 + i * 6, 2, argb(150, 130, 130, 120));

    /* wall clock */
    {
        fb_disc(fb, 180, 34, 12, argb(230, 26, 25, 22));
        fb_ring(fb, 180, 34, 12, argb(200, 120, 120, 105));
        fb_line(fb, 180, 34, 180, 27, argb(220, 210, 200, 180));
        fb_line(fb, 180, 34, 186, 38, argb(220, 210, 200, 180));
    }
    /* little shelf with tapes */
    fb_rect_a(fb, 268, 96, 108, 4, argb(240, 60, 52, 40));
    for (int i = 0; i < 5; i++) {
        int hh = 12 + (i * 7) % 5;
        fb_rect_a(fb, 272 + i * 20, 96 - hh, 14, hh, argb(240, 30 + i * 6, 28, 24));
        fb_rect_a(fb, 273 + i * 20, 96 - hh + 3, 12, 4, argb(120, 190, 180, 150));
    }
    /* door handles */
    fb_disc(fb, 232, 158, 3, rgb_(170, 160, 130));
    fb_rect_a(fb, 112, 156, 4, 16, argb(220, 155, 150, 125));

    /* ================= fuse panel ================= */
    fb_rect_a(fb, 418, 96, 54, 66, argb(255, 44, 44, 42));
    fb_frame(fb, 418, 96, 54, 66, argb(225, 96, 96, 88));
    txt_draw(fb, 422, 100, "MAINS", argb(150, 185, 175, 145), 1);
    for (int i = 0; i < 6; i++) {
        int on = G.load * 12.0f > (float)i;
        u32 c = i < 3 ? rgb_(110, 220, 120) : (i < 5 ? rgb_(240, 200, 80) : rgb_(240, 80, 60));
        fb_rect(fb, 422 + i * 8, 110, 6, 6, on ? c : rgb_(20, 22, 20));
    }
    txt_draw(fb, 422, 118, "LOAD", argb(130, 160, 152, 135), 1);
    {
        char ml[24];
        snprintf(ml, sizeof ml, "MATCH %dHZ", (int)night()->freq);
        sticker(fb, 396, 78, ml, argb(235, 215, 185, 125));
    }
    bevel(fb, 422, 130, 46, 22, G.blackout ? rgb_(78, 32, 26) : rgb_(46, 48, 46));
    txt_center(fb, 445, 136, G.blackout ? "RESET" : "OK", rgb_(225, 225, 215), 1);
    if (G.blackout) {
        int a = (int)(120 + 100 * sinf(G.t * 6.0f));
        led(fb, 441, 141, 2, argb(255, 60, 40, 20), 0);
        fb_rect_a(fb, 420, 98, 50, 62, argb(a > 60 ? 20 : 0, 255, 90, 60));
    }
    hover_ring(fb, 418, 128, 54, 28, hot(418, 128, 54, 28));
    hit(418, 128, 54, 28, HIT_BREAKER);

    /* ================= mini SPL ================= */
    fb_rect_a(fb, 20, 16, 92, 30, argb(230, 28, 27, 24));
    fb_frame(fb, 20, 16, 92, 30, argb(170, 90, 90, 80));
    txt_draw(fb, 24, 20, "SPL", argb(180, 200, 190, 160), 1);
    fb_rect(fb, 24, 36, 84, 2, argb(120, 160, 160, 140));
    fb_rect_a(fb, 24 + 64, 32, 20, 4, argb(170, 200, 50, 40));
    {
        int nx = 24 + (int)(clampf(G.interference, 0, 1.15f) / 1.15f * 84.0f);
        fb_rect(fb, nx - 1, 30, 2, 10, rgb_(245, 235, 205));
        if (G.interference > 0.75f) led(fb, 108, 22, 3, rgb_(255, 60, 40), 1);
    }
    if (night()->attack_night && G.attack_active) {
        char fb2[40];
        snprintf(fb2, sizeof fb2, "FEED T-%.1f  %s", G.attack_t, app_name(G.attack_app));
        txt_draw(fb, 20, 48, fb2, rgb_(230, 190, 90), 1);
    }

    /* ================= entity presence ================= */
    if (G.interference > 0.66f) {
        float a = (G.interference - 0.66f) / 0.5f;
        fb_rect_a(fb, 0, 0, fb->w, fb->h, argb((int)(a * 34.0f), 40, 0, 0));
        if (a > 0.35f) {
            /* something standing in the doorway behind you */
            int sway = (int)(sinf(G.t * 0.7f) * 3.0f);
            fb_disc(fb, 320 + sway, 118, 12, argb((int)(150 * a * a), 2, 2, 3));
            fb_rect_a(fb, 306 + sway, 130, 28, 128, argb((int)(150 * a * a), 2, 2, 3));
            fb_rect_a(fb, 300 + sway, 140, 8, 90, argb((int)(120 * a * a), 2, 2, 3));
            fb_rect_a(fb, 332 + sway, 140, 8, 90, argb((int)(120 * a * a), 2, 2, 3));
        }
    }

    /* ================= hints ================= */
    txt_draw(fb, 12, 344, "SPACE: SIT DOWN", argb(125, 195, 185, 155), 1);
    txt_draw(fb, 138, 344, "CLICK A MACHINE TO SWITCH IT ON", argb(95, 155, 150, 125), 1);
    txt_draw(fb, fb->w - 12 - txt_w("ESC: PAUSE", 1), 344, "ESC: PAUSE",
             argb(95, 155, 150, 125), 1);
    hit(0, 0, 0, 0, HIT_NONE);
    hit(0, 340, 260, 20, HIT_SIT);

    if (G.blackout) {
        fb_rect_a(fb, 0, 0, fb->w, fb->h, argb(215, 0, 0, 0));
        /* faint emergency glow around the mains panel */
        for (int r = 70; r > 0; r -= 4)
            fb_disc(fb, 445, 145, r, argb(2, 120, 40, 30));
        txt_center(fb, fb->w / 2, 200, "POWER FAILURE", rgb_(120, 45, 40), 1);
    }

    screen_fx(fb);
}

/* ================================================================= DEAD   */
/* the thing on the tape lives in face.c */

static void render_dead(FB *fb) {
    float t = G.dead_t;
    if (t < 1.7f) {
        fb_clear(fb, rgb_(1, 1, 2));
        u32 seed = (u32)(t * 5000.0f) | 1u;
        /* the face lunges at the glass */
        float sc = 1.25f + t * 2.0f + 0.05f * sinf(t * 40.0f);
        int shake = (int)(sinf(t * 61.0f) * 6.0f * (1.0f - t / 1.7f));
        draw_entity_face(fb, fb->w / 2 + shake, fb->h / 2 + 10 + shake / 2, sc, t, 255);
        /* vhs slicing over the face */
        fb_noise_block(fb, 0, 0, fb->w, fb->h, 0.35f, &seed);
        for (int i = 0; i < 26; i++) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            int y = (int)((seed >> 7) % (u32)fb->h);
            int dx = (int)((seed >> 3) % 22u) - 11;
            u32 *row = fb->px + y * fb->w;
            if (dx > 0) {
                memmove(row + dx, row, sizeof(u32) * (size_t)(fb->w - dx));
            } else if (dx < 0) {
                int d = -dx;
                memmove(row, row + d, sizeof(u32) * (size_t)(fb->w - d));
            }
        }
        if (fmodf(t * 9.0f, 1.0f) < 0.35f)
            fb_rect_a(fb, 0, 0, fb->w, fb->h, argb(40, 255, 245, 235));
        fb_rect_a(fb, 0, 0, fb->w, fb->h, argb((int)(30 + 60 * (t / 1.7f)), 90, 0, 0));
        fb_vignette(fb, 1.1f);
        return;
    }
    fb_clear(fb, rgb_(3, 3, 4));
    fb_noise_block(fb, 0, 0, fb->w, fb->h, 0.10f, &G.grain_seed);
    txt_center(fb, fb->w / 2, 108, "SIGNAL LOST", rgb_(196, 62, 52), 3);
    char buf[96];
    snprintf(buf, sizeof buf, "%s  /  LINE %d OF %d", night()->tape_label,
             G.line + 1, night()->nlines);
    txt_center(fb, fb->w / 2, 156, buf, argb(190, 170, 160, 150), 1);
    txt_center(fb, fb->w / 2, 176, "IT HEARD YOU BREATHING.", argb(175, 160, 80, 70), 1);
    if (G.scr_t > 1.2f) {
        float a = 0.5f + 0.5f * sinf(G.t * 2.6f);
        txt_center(fb, fb->w / 2, 232, "[ SPACE ]  RE-ROLL THE TAPE",
                   argb((int)(120 + 110 * a), 215, 210, 195), 1);
        hit(0, 0, fb->w, fb->h, HIT_RETRY);
    }
    screen_fx(fb);
}

/* ============================================================= NIGHT END  */
static void render_nightend(FB *fb) {
    fb_clear(fb, rgb_(5, 5, 6));
    const Night *N = night();
    char buf[128];
    snprintf(buf, sizeof buf, "%s  COMPLETE", N->tape_label);
    txt_center(fb, fb->w / 2, 40, buf, rgb_(210, 200, 160), 2);
    snprintf(buf, sizeof buf, "%d OF %d LINES RESTORED", N->nlines, N->nlines);
    txt_center(fb, fb->w / 2, 64, buf, argb(170, 160, 190, 160), 1);

    int shown = (int)G.tw;
    int used = 0;
    for (int i = 0; N->epilogue[i]; i++) {
        int len = (int)strlen(N->epilogue[i]) + 1;
        if (shown - used <= 0) break;
        int take = shown - used;
        int n = len < take ? len : take;
        char line[160];
        memcpy(line, N->epilogue[i], (size_t)(n - 1));
        line[n - 1] = 0;
        txt_draw(fb, 90, 110 + i * 16, line, rgb_(180, 200, 180), 1);
        used += len;
    }
    if (G.tw > 160.0f) {
        if (G.night < 4)
            txt_center(fb, fb->w / 2, 300, "[ SPACE ] NEXT CONTRACT", argb(200, 210, 205, 180), 1);
        else
            txt_center(fb, fb->w / 2, 300, "[ SPACE ] FINISH", argb(200, 210, 205, 180), 1);
        hit(0, 0, fb->w, fb->h, HIT_NEXT);
    }
    screen_fx(fb);
}

/* ================================================================ ENDING */
static void render_ending(FB *fb) {
    fb_clear(fb, rgb_(2, 2, 3));
    const char *lines[] = {
        "THE CONTRACT IS CLOSED.",
        "",
        "YOU PACK THE TAPES INTO THE COURIER BOX AND",
        "WRITE THE ADDRESS. THE ADDRESS WRITES BACK.",
        "",
        "ON THE DESK, THE CRT IS STILL ON. IT IS NOT",
        "CONNECTED TO ANYTHING. IT IS RECORDING.",
        "",
        "YOU HAVE BEEN LISTENING FOR FIVE NIGHTS.",
        "IT HAS BEEN LISTENING FOR LONGER.",
        "",
        "DEAD AIR",
        "",
        "THANK YOU FOR RESTORING THE ARCHIVE.",
        NULL
    };
    int shown = (int)G.tw;
    int used = 0;
    for (int i = 0; lines[i]; i++) {
        int len = (int)strlen(lines[i]) + 1;
        if (shown - used <= 0) break;
        int take = shown - used;
        int n = len < take ? len : take;
        char line[160];
        memcpy(line, lines[i], (size_t)(n - 1));
        line[n - 1] = 0;
        if (i == 12) txt_center(fb, fb->w / 2, 60 + i * 15, line, rgb_(220, 200, 150), 1);
        else txt_draw(fb, 110, 60 + i * 15, line, rgb_(170, 195, 175), 1);
        used += len;
    }
    if (G.tw > 260.0f) txt_center(fb, fb->w / 2, 330, "[ ESC ] EXIT", argb(150, 160, 160, 140), 1);
    screen_fx(fb);
}

/* ================================================================= INPUT  */
static void click_word(int i) {
    if (i < 0 || i >= G.nbank) return;
    if (G.bank_used[i]) return;
    const TapeLine *ln = &night()->lines[G.line];
    const char *ans = ln->answers[G.filled];
    if (!ans) return;
    if (!strcmp(G.bank[i], ans)) {
        G.bank_used[i] = 1;
        strncpy(G.chosen[G.filled], G.bank[i], 23);
        G.chosen[G.filled][23] = 0;
        G.filled++;
        G.flash_ok = 0.8f;
        sfx_click();
        if (G.filled >= night()->blanks) {
            G.solving = 1;
            advance_line();
        }
    } else {
        G.flash_bad = 1.0f;
        G.interference = clampf(G.interference + 0.10f, 0, 1.2f);
        sfx_static(0.7f);
        G.shake = 0.6f;
    }
}

static void toggle_app(int i) {
    if (G.blackout) { sfx_switch(0.4f); return; }
    G.app_on[i] = G.app_on[i] > 0.5f ? 0.0f : 1.0f;
    sfx_switch(0.5f);
    if (G.app_on[i] > 0.5f && G.app_lvl[i] < 0.2f) sfx_thud(0.2f);
}

static void handle_click(int mx, int my) {
    int hi = hit_test(mx, my);
    if (hi < 0) return;
    int id = G.hits[hi].id;
    switch (id) {
    case HIT_MENU + 0: start_night(0); G.scr_t = 0; sfx_click(); break;
    case HIT_MENU + 1: set_screen(SCR_HELP); sfx_click(); break;
    case HIT_MENU + 2: G.quit_requested = 1; break;
    case HIT_NEXT:
        if (G.scr == SCR_HELP) { set_screen(SCR_TITLE); }
        else if (G.scr == SCR_BRIEF) { set_screen(SCR_DESK); }
        else if (G.scr == SCR_NIGHTEND) {
            if (G.night < 4) start_night(G.night + 1);
            else set_screen(SCR_ENDING);
        }
        sfx_click();
        break;
    case HIT_RETRY:
        if (G.scr_t > 1.2f) { start_night(G.night); }
        sfx_click();
        break;
    case HIT_TRANSPORT:
        if (G.blackout) { sfx_static(0.3f); break; }
        G.playing = !G.playing;
        sfx_switch(0.5f);
        if (G.playing) sfx_click();
        break;
    case HIT_STAND: set_screen(SCR_ROOM); break;
    case HIT_SIT: set_screen(SCR_DESK); break;
    case HIT_FAN: toggle_app(APP_FAN); break;
    case HIT_MICRO: toggle_app(APP_MICRO); break;
    case HIT_SHOWER: toggle_app(APP_SHOWER); break;
    case HIT_RADIO: toggle_app(APP_RADIO); break;
    case HIT_BATH: G.bath_open = !G.bath_open; sfx_creak(0.5f); break;
    case HIT_BREAKER:
        if (G.blackout) {
            G.blackout = 0; G.blackout_t = 0; G.over_t = 0;
            G.interference = clampf(G.interference - 0.10f, 0, 1.2f);
            sfx_breaker(); sfx_switch(0.5f);
        } else {
            sfx_switch(0.25f);
        }
        break;
    default:
        if (id >= HIT_BANK && id < HIT_BANK + BANK_MAX) click_word(id - HIT_BANK);
        break;
    }
}

static void handle_input(Input *in) {
    int mx = in->mouse_x, my = in->mouse_y;
    G.mx = mx; G.my = my;

    if (in->mouse_pressed) {
        int hi = hit_test(mx, my);
        if (hi >= 0) {
            int id = G.hits[hi].id;
            if (id == HIT_KNOB_LOW) { G.drag = id; G.drag_val = G.low_cut; G.drag_y = (float)my; }
            else if (id == HIT_KNOB_HIGH) { G.drag = id; G.drag_val = G.high_cut; G.drag_y = (float)my; }
            else if (id == HIT_DIAL) { G.drag = id; G.drag_val = G.radio_freq; G.drag_x = (float)mx; }
            else { }
        }
        handle_click(mx, my);
    }
    if (!in->mouse_down) G.drag = HIT_NONE;
    if (G.drag == HIT_KNOB_LOW) {
        G.low_cut = clampf(G.drag_val + (G.drag_y - (float)my) * 0.005f, 0.0f, 1.0f);
    } else if (G.drag == HIT_KNOB_HIGH) {
        G.high_cut = clampf(G.drag_val + (G.drag_y - (float)my) * 0.005f, 0.0f, 1.0f);
    } else if (G.drag == HIT_DIAL) {
        G.radio_freq = clampf(G.drag_val + ((float)mx - G.drag_x) * 1.25f, 30.0f, 210.0f);
    }

    if (in->key_pressed[' ']) {
        if (G.scr == SCR_BRIEF) set_screen(SCR_DESK);
        else if (G.scr == SCR_HELP) set_screen(SCR_TITLE);
        else if (G.scr == SCR_DESK) set_screen(SCR_ROOM);
        else if (G.scr == SCR_ROOM) set_screen(SCR_DESK);
        else if (G.scr == SCR_DEAD && G.scr_t > 1.2f) start_night(G.night);
        else if (G.scr == SCR_NIGHTEND && G.tw > 160.0f) {
            if (G.night < 4) start_night(G.night + 1); else set_screen(SCR_ENDING);
        } else if (G.scr == SCR_TITLE) { start_night(0); }
    }
    for (int i = 0; i < 7; i++)
        if (in->key_pressed['1' + i] && (G.scr == SCR_DESK)) click_word(i);
    if (in->key_pressed[27] && (G.scr == SCR_DESK || G.scr == SCR_ROOM)) G.paused = !G.paused;
    if (G.scr == SCR_ENDING && in->key_pressed[27]) G.quit_requested = 1;
}

/* ============================================================== PUBLIC    */
static void render_pause(FB *fb) {
    fb_rect_a(fb, 0, 0, fb->w, fb->h, argb(190, 0, 0, 0));
    txt_center(fb, fb->w / 2, 150, "PAUSED", rgb_(220, 210, 180), 2);
    txt_center(fb, fb->w / 2, 180, "ESC TO RESUME", argb(170, 170, 160, 150), 1);
}

void game_init(FB *fb) {
    memset(&G, 0, sizeof G);
    G.fb = *fb;
    G.rng.s = 0xC0FFEE11u ^ (u32)(fb->w * 7919 + fb->h);
    G.scr = SCR_TITLE;
    G.night = 0;
    G.menu_sel = 0;
    G.grain_seed = 12345u;
    G.clarity = 0;
    G.low_cut = 0.15f;
    G.high_cut = 0.9f;
    G.radio_freq = 40.0f;
    audio_reset();
    AP.entity_freq = 63.0f;
}

void game_frame(Input *in) {
    handle_input(in);
    if (!G.paused) update(in->dt);
    else { G.t += in->dt; }

    FB *fb = &G.fb;
    hits_reset();
    switch (G.scr) {
    case SCR_TITLE:    render_title(fb); break;
    case SCR_HELP:     render_help(fb); break;
    case SCR_BRIEF:    render_brief(fb); break;
    case SCR_DESK:     render_desk(fb); break;
    case SCR_ROOM:     render_room(fb); break;
    case SCR_DEAD:     render_dead(fb); break;
    case SCR_NIGHTEND: render_nightend(fb); break;
    case SCR_ENDING:   render_ending(fb); break;
    }
    if (G.paused) render_pause(fb);
}

int  game_should_quit(void) { return G.quit_requested; }

void game_debug_kill(void) { die(); }
void game_debug_force_line(void) {
    if (G.scr == SCR_DESK || G.scr == SCR_ROOM) { G.filled = night()->blanks; advance_line(); }
}
void game_debug_night(int n) { start_night(n < 0 ? 0 : (n > 4 ? 4 : n)); }
void game_debug_interference(float v) { G.interference = v; }

void game_debug(char *buf, int n) {
    snprintf(buf, n,
             "scr=%d night=%d line=%d/%d filled=%d/%dbank clarity=%.2f inf=%.2f load=%.2f/%.2f "
             "lo=%.2f(t%.2f) hi=%.2f(t%.2f) supp=%.2f play=%d black=%d apps=%d%d%d%d rf=%.0f",
             G.scr, G.night, G.line + 1, night()->nlines, G.filled, night()->blanks,
             G.clarity, G.interference, G.load, G.capacity, G.low_cut, G.target_lo,
             G.high_cut, G.target_hi, suppression(), G.playing, G.blackout,
             (int)G.app_on[0], (int)G.app_on[1], (int)G.app_on[2], (int)G.app_on[3],
             G.radio_freq);
}
