/* ===========================================================================
   DEAD AIR -- procedural audio engine
   Everything you hear is synthesised here at runtime, except the six recorded
   tapes which are embedded PCM (see audio_data.h).
   =========================================================================== */
#include "da.h"
#include "audio_data.h"

AudioParams AP;

/* ------------------------------------------------------------------ noise */
static u32 S = 0x9e3779b9u;
static inline float frand(void) {
    S ^= S << 13; S ^= S >> 17; S ^= S << 5;
    return (float)(S & 0x00ffffffu) * (1.0f / 8388608.0f) - 1.0f;
}

/* ------------------------------------------------------- state variable fx */
typedef struct { float low, band; } SVF;
static inline float svf_lp(SVF *f, float in, float freq, float q) {
    if (freq < 20.0f) freq = 20.0f;
    if (freq > MIX_SR * 0.45f) freq = MIX_SR * 0.45f;
    float F = 2.0f * sinf(3.14159265f * freq / (float)MIX_SR);
    f->low += F * f->band;
    float high = in - f->low - q * f->band;
    f->band += F * high;
    return f->low;
}
static inline float svf_hp(SVF *f, float in, float freq, float q) {
    if (freq < 20.0f) freq = 20.0f;
    if (freq > MIX_SR * 0.45f) freq = MIX_SR * 0.45f;
    float F = 2.0f * sinf(3.14159265f * freq / (float)MIX_SR);
    f->low += F * f->band;
    float high = in - f->low - q * f->band;
    f->band += F * high;
    return high;
}
static inline float svf_bp(SVF *f, float in, float freq, float q) {
    if (freq < 20.0f) freq = 20.0f;
    if (freq > MIX_SR * 0.45f) freq = MIX_SR * 0.45f;
    float F = 2.0f * sinf(3.14159265f * freq / (float)MIX_SR);
    f->low += F * f->band;
    float high = in - f->low - q * f->band;
    f->band += F * high;
    return f->band;
}

/* ------------------------------------------------------------------ state */
static double g_t;
static double tape_pos;          /* in source samples */
static int    last_clip = -1;
static int    last_playing = 0;
static SVF    tape_hp, tape_lp, hiss_bp, hiss_lp2;
static SVF    fan_bp, micro_lp, shower_hp, radio_bp;
static SVF    room_lp, rain_hp, rain_lp;
static SVF    drone_bp;
static float  heartbeat_phase, heartbeat_env;
static float  drop_env = 1.0f;
static float  master_dc = 0.0f;
static float  prev_lp = 0.0f;   /* master DC blocker */

/* one shot effects ------------------------------------------------------- */
enum { SFX_NONE = 0, SFX_CREAK, SFX_THUD, SFX_CLICK, SFX_SWITCH, SFX_STATIC,
       SFX_STINGER, SFX_CHIME, SFX_DRIP, SFX_WHISPER, SFX_BREAKER };

typedef struct {
    int   type;
    float t, dur, vol;
    float p1, p2;
    SVF   f1;
    float ph, ph2;
    double pos;                  /* for whisper clip */
} Sfx;
static Sfx sfx[10];

static Sfx *sfx_alloc(void) {
    Sfx *best = &sfx[0];
    for (int i = 0; i < 10; i++)
        if (sfx[i].type == SFX_NONE) return &sfx[i];
    for (int i = 1; i < 10; i++)
        if (sfx[i].t > best->t) best = &sfx[i];
    return best;
}

static void sfx_start(int type, float vol) {
    Sfx *s = sfx_alloc();
    memset(s, 0, sizeof(*s));
    s->type = type; s->vol = vol; s->t = 0;
    switch (type) {
        case SFX_CREAK:   s->dur = 1.4f;  s->p1 = 220.0f + frand() * 200.0f; break;
        case SFX_THUD:    s->dur = 0.55f; break;
        case SFX_CLICK:   s->dur = 0.05f; break;
        case SFX_SWITCH:  s->dur = 0.12f; break;
        case SFX_STATIC:  s->dur = 0.45f; break;
        case SFX_STINGER: s->dur = 2.6f;  break;
        case SFX_CHIME:   s->dur = 1.4f;  break;
        case SFX_DRIP:    s->dur = 0.30f; s->p1 = 900.0f + frand() * 500.0f; break;
        case SFX_WHISPER: s->dur = 6.2f;  s->pos = 0; break;
        case SFX_BREAKER: s->dur = 0.6f;  break;
    }
}

void sfx_creak(float v)   { sfx_start(SFX_CREAK, v); }
void sfx_thud(float v)    { sfx_start(SFX_THUD, v); }
void sfx_click(void)      { sfx_start(SFX_CLICK, 0.35f); }
void sfx_switch(float v)  { sfx_start(SFX_SWITCH, v); }
void sfx_static(float v)  { sfx_start(SFX_STATIC, v); }
void sfx_stinger(void)    { sfx_start(SFX_STINGER, 1.0f); }
void sfx_chime(float v)   { sfx_start(SFX_CHIME, v); }
void sfx_drip(float v)    { sfx_start(SFX_DRIP, v); }
void sfx_whisper(void)    { sfx_start(SFX_WHISPER, 0.55f); }
void sfx_breaker(void)    { sfx_start(SFX_BREAKER, 0.8f); }

void audio_reset(void) {
    memset(&AP, 0, sizeof(AP));
    AP.tape_rate = 1.0f;
    AP.radio_freq = 63.0f;
    tape_pos = 0; last_clip = -1;
    for (int i = 0; i < 10; i++) sfx[i].type = SFX_NONE;
}

/* ------------------------------------------------------------- render sfx */
static float sfx_sample(Sfx *s) {
    if (s->type == SFX_NONE) return 0.0f;
    float dt = 1.0f / (float)MIX_SR;
    s->t += dt;
    if (s->t >= s->dur) { s->type = SFX_NONE; return 0.0f; }
    float u = s->t / s->dur;
    float out = 0.0f;

    switch (s->type) {
    case SFX_CREAK: {
        float env = sinf(3.14159265f * u) * s->vol;
        float f = s->p1 * (0.75f + 0.5f * sinf(s->t * 2.1f));
        float n = frand();
        out = svf_bp(&s->f1, n, f, 0.12f) * env * 3.0f;
        out += svf_bp(&s->f1, n, f * 2.02f, 0.2f) * env * 1.2f;
        break;
    }
    case SFX_THUD: {
        float env = expf(-u * 7.0f) * s->vol;
        float f = 62.0f * (1.0f - u * 0.5f);
        s->ph += f / (float)MIX_SR;
        out = sinf(s->ph * 6.2831853f) * env;
        out += svf_lp(&s->f1, frand(), 400.0f, 0.9f) * env * 0.6f;
        break;
    }
    case SFX_CLICK: {
        float env = expf(-s->t * 180.0f) * s->vol;
        out = (frand() * 0.6f + sinf(s->t * 6.2831853f * 2400.0f) * 0.4f) * env;
        break;
    }
    case SFX_SWITCH: {
        float env = expf(-s->t * 40.0f) * s->vol;
        out = (frand() * 0.5f + sinf(s->t * 6.2831853f * 140.0f) * 0.5f) * env;
        break;
    }
    case SFX_STATIC: {
        float env = (1.0f - u) * s->vol;
        out = svf_bp(&s->f1, frand(), 2400.0f, 0.4f) * env * 1.6f;
        break;
    }
    case SFX_STINGER: {
        float env = expf(-u * 2.2f);
        float rise = 90.0f + 900.0f * u * u;
        s->ph += rise / (float)MIX_SR;
        float v = sinf(s->ph * 6.2831853f) * 0.55f + frand() * 0.45f;
        v = (float)tanh((double)(v * 3.0f));
        out = v * env * s->vol * 0.8f;
        break;
    }
    case SFX_CHIME: {
        float env = expf(-u * 4.0f) * s->vol;
        s->ph += 1180.0f / (float)MIX_SR;
        s->ph2 += 1760.0f / (float)MIX_SR;
        out = (sinf(s->ph * 6.2831853f) * 0.6f + sinf(s->ph2 * 6.2831853f) * 0.3f) * env;
        break;
    }
    case SFX_DRIP: {
        float env = expf(-u * 12.0f) * s->vol;
        s->ph += (s->p1 * (1.0f - u * 0.6f)) / (float)MIX_SR;
        out = sinf(s->ph * 6.2831853f) * env * 0.7f;
        break;
    }
    case SFX_BREAKER: {
        float env = expf(-s->t * 18.0f) * s->vol;
        out = (frand() * 0.7f + sinf(s->t * 6.2831853f * 70.0f) * 0.3f) * env;
        break;
    }
    case SFX_WHISPER: {
        const AudioClip *c = &audio_clips[CLIP_ENTITY];
        if (s->pos >= (double)c->samples - 2) { s->type = SFX_NONE; return 0.0f; }
        int i0 = (int)s->pos;
        float f = (float)(s->pos - (double)i0);
        float smp = ((float)c->pcm[i0] * (1.0f - f) + (float)c->pcm[i0 + 1] * f) / 32768.0f;
        s->pos += 0.72;                       /* pitched down */
        float v = svf_bp(&s->f1, smp, 700.0f, 0.5f) * 2.2f;
        float env = 1.0f;
        if (u < 0.05f) env = u / 0.05f;
        if (u > 0.85f) env = (1.0f - u) / 0.15f;
        out = v * env * s->vol;
        break;
    }
    }
    return out;
}

/* ---------------------------------------------------------------- helpers */
static float tape_read(const AudioClip *c) {
    if (c->samples < 4) return 0.0f;
    int i0 = (int)tape_pos;
    float f = (float)(tape_pos - (double)i0);
    if (i0 < 0) i0 = 0;
    if (i0 >= c->samples - 1) i0 = c->samples - 2;
    return ((float)c->pcm[i0] * (1.0f - f) + (float)c->pcm[i0 + 1] * f) / 32768.0f;
}

void audio_mix(short *out, int frames) {
    const AudioClip *clip = &audio_clips[(AP.clip >= 0 && AP.clip < AUDIO_CLIP_COUNT) ? AP.clip : 0];
    if (AP.clip != last_clip) { tape_pos = 0; last_clip = AP.clip; }

    float dt = 1.0f / (float)MIX_SR;

    /* knob -> filter cutoffs */
    float hp_hz = 80.0f * powf(25.0f, AP.low_cut);            /* 80 .. 2000 */
    float lp_hz = 7000.0f * powf(0.09f, AP.high_cut);         /* 7000 .. 630 */
    float clarity = clampf(AP.clarity, 0.0f, 1.0f);
    float warn = clampf((AP.interference - 0.62f) / 0.38f, 0.0f, 1.0f);

    for (int i = 0; i < frames; i++) {
        g_t += dt;
        float t = (float)g_t;

        /* ---------------- tape voice ------------------------------- */
        float voice = 0.0f, hiss = 0.0f;
        if (AP.tape_playing && AP.tape_level > 0.001f) {
            /* wow & flutter + slow tape deterioration */
            float wow = 1.0f + 0.0022f * sinf(t * 3.9f) + 0.0009f * sinf(t * 11.3f);
            float rate = AP.tape_rate * wow;
            tape_pos += (double)rate * (double)AUDIO_SR / (double)MIX_SR;
            if (tape_pos >= (double)clip->samples - 2) tape_pos = 0;

            float raw = tape_read(clip);
            /* dropouts */
            if (frand() > 0.99965f) drop_env = 0.0f;
            drop_env += (1.0f - drop_env) * 0.0009f;
            raw *= drop_env;

            float v = svf_hp(&tape_hp, raw, hp_hz, 0.85f);
            v = svf_lp(&tape_lp, v, lp_hz, 0.85f);
            voice = v * AP.tape_level * (0.85f + 0.55f * clarity);

            /* tape hiss follows the knobs (so tuning is audible) */
            float hn = frand();
            float h = svf_bp(&hiss_bp, hn, clampf(hp_hz * 2.2f, 200.0f, 6000.0f), 0.6f);
            h += svf_lp(&hiss_lp2, hn, clampf(lp_hz * 0.9f, 300.0f, 8000.0f), 0.9f) * 0.5f;
            hiss = h * (0.05f + 0.10f * (1.0f - clarity)) * AP.tape_level;
        } else {
            voice = 0.0f;
        }

        /* ---------------- appliances ------------------------------- */
        float app = 0.0f;
        float al;
        al = AP.app_level[APP_FAN];
        if (al > 0.001f) {
            float n = frand();
            float b = svf_bp(&fan_bp, n, 620.0f + 180.0f * sinf(t * 0.7f), 0.9f);
            float am = 0.6f + 0.4f * sinf(t * 6.2831853f * 22.0f);
            app += b * al * 0.42f * am;
        }
        al = AP.app_level[APP_MICRO];
        if (al > 0.001f) {
            float hum = sinf(t * 6.2831853f * 120.0f) * 0.35f
                      + sinf(t * 6.2831853f * 240.0f) * 0.12f
                      + sinf(t * 6.2831853f * 60.0f) * 0.2f;
            float air = svf_lp(&micro_lp, frand(), 1800.0f, 0.9f) * 0.35f;
            app += (hum + air) * al * 0.30f;
        }
        al = AP.app_level[APP_SHOWER];
        if (al > 0.001f) {
            float n = frand();
            float s1 = svf_hp(&shower_hp, n, 1200.0f, 0.7f);
            float am = 0.85f + 0.15f * sinf(t * 1.7f);
            app += s1 * al * 0.26f * am;
        }
        al = AP.app_level[APP_RADIO];
        if (al > 0.001f) {
            float f = AP.radio_freq;
            float tone = sinf(t * 6.2831853f * f) * 0.5f
                       + sinf(t * 6.2831853f * f * 2.0f) * 0.18f;
            float det = fabsf(f - AP.entity_freq);
            float stat = svf_bp(&radio_bp, frand(), 900.0f + 2500.0f * clampf(det / 60.0f, 0, 1), 0.5f);
            app += (tone * 0.4f + stat * 0.18f) * al * 0.35f;
        }

        /* ---------------- background room -------------------------- */
        float room = svf_lp(&room_lp, frand(), 280.0f, 0.9f) * 0.10f;
        float rain = svf_hp(&rain_hp, frand(), 700.0f, 0.9f);
        rain = svf_lp(&rain_lp, rain, 4200.0f, 0.9f) * 0.055f
             * (0.8f + 0.2f * sinf(t * 0.23f));
        float amb = (room + rain) * (1.0f - 0.85f * AP.blackout);

        /* ---------------- entity ----------------------------------- */
        float ent = 0.0f;
        float inf = clampf(AP.interference, 0.0f, 1.2f);
        if (inf > 0.01f) {
            float ef = AP.entity_freq;
            float d = sinf(t * 6.2831853f * ef) * 0.5f
                    + sinf(t * 6.2831853f * ef * 3.01f + 1.2f) * 0.25f
                    + sinf(t * 6.2831853f * ef * 5.03f + 2.7f) * 0.12f;
            d += svf_bp(&drone_bp, frand(), ef * 6.0f, 0.3f) * 0.25f * inf;
            ent = d * inf * inf * 0.30f;
            ent *= 0.75f + 0.25f * sinf(t * 0.4f);
        }
        /* heartbeat */
        float bpm = 52.0f + 70.0f * clampf(inf, 0, 1);
        heartbeat_phase += dt * bpm / 60.0f;
        if (heartbeat_phase >= 1.0f) { heartbeat_phase -= 1.0f; heartbeat_env = 1.0f; }
        float hb = 0.0f;
        if (heartbeat_env > 0.001f) {
            heartbeat_env *= 0.9993f;
            hb = sinf(t * 6.2831853f * 44.0f) * heartbeat_env * inf * inf * 0.5f;
        }

        /* ---------------- one shots -------------------------------- */
        float fx = 0.0f;
        for (int k = 0; k < 10; k++) fx += sfx_sample(&sfx[k]);

        /* ---------------- jumpscare -------------------------------- */
        float js = 0.0f;
        if (AP.jumpscare > 0.001f) {
            js = (frand() * 0.55f + sinf(t * 6.2831853f * (110.0f + 60.0f * sinf(t * 3.0f))) * 0.45f)
                 * AP.jumpscare;
            js = (float)tanh((double)(js * 2.4f));
        }

        float mix = voice * 0.85f + hiss + app + amb + ent + hb + fx * 0.85f + js * 0.9f;
        mix += AP.static_burst * frand() * 0.5f;

        /* master: DC block + soft clip */
        float dc = mix - prev_lp;
        prev_lp += (mix - prev_lp) * 0.0008f;
        mix = dc;
        mix = (float)tanh((double)(mix * 1.25f)) * 0.82f;
        float ms = clampf(mix * (1.0f + warn * 0.05f), -1.0f, 1.0f);
        short s = (short)(ms * 32000.0f);
        out[i * 2 + 0] = s;
        out[i * 2 + 1] = s;
    }
}
