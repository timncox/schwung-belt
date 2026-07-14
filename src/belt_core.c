/*
 * Belt core — see belt_core.h for the architecture overview.
 *
 * Timeline bookkeeping: `w` is the absolute input-sample counter (write
 * head). Output time == input time; every voice fires Hann grains into a
 * shared stereo accumulator ring at absolute positions, and the block read
 * clears behind itself. Grains are causal (window starts at the fire time),
 * so a grain fired at t < block_end never writes before block_end of the
 * previous read — future firings can't touch already-read samples.
 *
 * Latency invariant: a grain fired at time t reads input around mark
 * m ≈ t + G - L (G = one period, L = BELT_LATENCY). The highest sample it
 * touches is m + G*g (g = formant resample factor), which must stay behind
 * the write head: G*(1+g) <= L. fire_grain() clamps g per-grain to honor
 * that, so very low notes trade a little formant-up range instead of
 * reading unwritten input.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "belt_core.h"

#define IN_RING   16384              /* mono float input history */
#define IN_MASK   (IN_RING - 1)
#define DRY_RING  4096               /* stereo int16 latency-matched dry */
#define DRY_MASK  (DRY_RING - 1)
#define ACC_RING  4096               /* stereo float output accumulator */
#define ACC_MASK  (ACC_RING - 1)
#define MARKS     64                 /* recent pitch marks (time + period) */

#define YIN_DEC    2                 /* analyze at 22050 Hz */
#define YIN_SR     (BELT_SR / YIN_DEC)
#define YIN_RING   4096
#define YIN_MASK   (YIN_RING - 1)
#define YIN_W      512               /* ~23 ms integration window */
#define YIN_TAUMAX 260               /* 22050/85 Hz */
#define YIN_TAUMIN 22                /* 22050/1000 Hz */
#define YIN_HOP    256               /* input samples between analyses */

#define T_MAX ((float)BELT_SR / BELT_FMIN)   /* ~519 samples */
#define T_MIN ((float)BELT_SR / BELT_FMAX)   /* ~44 samples */

#define RMS_GATE 0.0015f             /* ~-56 dBFS voicing gate */

/* Harmony interval enum (module.json order). Value 6 = Unison. */
#define ITV_OFF  0
#define ITV_UNIS 6

/* degree steps per interval for 7-note-style scales; octave handled as
 * +/- scale length. index matches the enum. */
static const int itv_steps[12]  = { 0, 0, -5, -4, -3, -2, 0, 2, 3, 4, 5, 0 };
/* chromatic fallback: fixed semitones */
static const int itv_chroma[12] = { 0, -12, -9, -7, -5, -4, 0, 4, 5, 7, 9, 12 };
static const int itv_is_oct[12] = { 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

/* Scale tables: semitone offsets from the key, ascending within one octave.
 * Order matches the module.json "scale" enum. */
static const int scale_len[9]      = { 12, 7, 7, 7, 7, 7, 5, 5, 6 };
static const int scale_deg[9][12]  = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 },   /* Chromatic  */
    { 0, 2, 4, 5, 7, 9, 11 },                   /* Major      */
    { 0, 2, 3, 5, 7, 8, 10 },                   /* Minor      */
    { 0, 2, 3, 5, 7, 8, 11 },                   /* Harm Minor */
    { 0, 2, 3, 5, 7, 9, 10 },                   /* Dorian     */
    { 0, 2, 4, 5, 7, 9, 10 },                   /* Mixolydian */
    { 0, 2, 4, 7, 9 },                          /* Maj Pent   */
    { 0, 3, 5, 7, 10 },                         /* Min Pent   */
    { 0, 3, 5, 6, 7, 10 },                      /* Blues      */
};

typedef struct {
    double next_out;      /* absolute output time of the next grain */
    float  ratio_log;     /* smoothed log2 pitch ratio */
    float  ratio_tgt;     /* target log2 pitch ratio */
    float  gain;          /* smoothed linear gain */
    float  gain_tgt;
    float  pan;           /* -1..1 */
    float  det_cents;     /* static humanize detune */
    float  wander;        /* slow random detune walk, cents */
    float  formant_mul;   /* per-voice formant flavor on top of global */
} belt_voice_t;

struct belt {
    const host_api_v1_t *host;

    /* ---- params (raw UI integers) ---- */
    int key;              /* 0-11, C..B */
    int scale;            /* index into scale_deg */
    int retune;           /* 0-100: 0 = instant (hard-tune) */
    int amount;           /* 0-100 correction depth */
    int flex;             /* 0-100: leave far-from-note singing alone */
    int humanize;         /* 0-100: vibrato preservation + voice wander */
    int harm[BELT_HARMONIES];   /* interval enum, 0 = off */
    int harm_level;       /* 0-100 */
    int spread;           /* 0-100 stereo spread of harmonies */
    int double_amt;       /* 0-100 doubler */
    int formant;          /* -100..100 -> +/- half octave */
    int wet;              /* 0-100 corrected vs dry lead */
    int hard;             /* performance override: instant, full correction */
    int monitor;          /* 0 = output muted (feedback guard, never saved) */
    int hw_input;         /* set by the gen wrapper: mic guard applies */

    /* ---- input / analysis state ---- */
    uint64_t w;           /* absolute input write head */
    float   *in_ring;     /* mono float */
    int16_t *dry_ring;    /* stereo, latency-matched dry path */
    float   *acc;         /* stereo output accumulator */

    float   *yin_ring;    /* decimated mono */
    uint64_t yw;
    int      dec_phase;
    float    dec_acc;
    int      since_analysis;
    float    yin_buf[YIN_W + YIN_TAUMAX + 4];
    float    d[YIN_TAUMAX + 1];
    float    cmnd[YIN_TAUMAX + 1];

    float f_hist[3];      /* recent voiced f0s for median filtering */
    int   f_hist_n;
    float f_inst;         /* median-filtered instantaneous f0 (held) */
    float note_inst;      /* midi note of f_inst */
    float note_slow;      /* vibrato-smoothed pitch line */
    int   jump_frames;    /* consecutive far-from-slow frames (note change) */
    int   voiced;         /* last analysis verdict */
    float voiced_sm;      /* smoothed 0..1 */
    float rms;            /* analysis-window rms */

    /* ---- pitch marks ---- */
    double mark_t[MARKS];
    float  mark_T[MARKS];
    int    mark_head;     /* newest mark index */
    int    mark_n;
    double next_mark;
    float  cur_T;

    /* ---- correction ---- */
    float lead_off;       /* smoothed correction offset, semitones */
    float q_note;         /* current full quantized target note */
    float cents_err;      /* display: cents from target */

    belt_voice_t v[BELT_VOICES];
    uint32_t rng;
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */

static float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static int clampi(int x, int lo, int hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static float med3(float a, float b, float c) {
    if (a > b) { float t = a; a = b; b = t; }
    if (b > c) { float t = b; b = c; c = t; }
    if (a > b) { float t = a; a = b; b = t; }
    return b;
}
static uint32_t rng_next(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}
static float rng_bipolar(uint32_t *s) {           /* -1..1 */
    return ((float)(rng_next(s) & 0xFFFF) / 32768.0f) - 1.0f;
}

/* bounded snprintf append (the smack OOB-write lesson: snprintf returns
 * would-have-written length; never let n walk past the buffer) */
static void app(char *buf, int cap, int *n, const char *fmt, ...) {
    if (*n >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf + *n, (size_t)(cap - *n), fmt, ap);
    va_end(ap);
    if (r > 0) {
        *n += r;
        if (*n > cap - 1) *n = cap - 1;
    }
}

static float note_of_freq(float f) {
    return 69.0f + 12.0f * log2f(f / 440.0f);
}

/* nearest allowed note (absolute, fractional input) in key+scale */
static float quantize_note(float note, int key, int scale) {
    const int *deg = scale_deg[scale];
    int n = scale_len[scale];
    int base = (int)floorf(note);
    float best = (float)base, bestd = 1e9f;
    for (int cand = base - 9; cand <= base + 9; cand++) {
        int pc = ((cand - key) % 12 + 12) % 12;
        int ok = 0;
        for (int i = 0; i < n; i++) if (deg[i] == pc) { ok = 1; break; }
        if (!ok) continue;
        float d = fabsf((float)cand - note);
        if (d < bestd) { bestd = d; best = (float)cand; }
    }
    return best;
}

/* walk `steps` scale degrees from an on-scale note (nearest degree if the
 * note is off-scale, e.g. partially corrected) */
static float degree_shift(float note, int key, int scale, int steps) {
    const int *deg = scale_deg[scale];
    int n = scale_len[scale];
    int rn = (int)lrintf(note);
    int rel = rn - key;
    int oct = (int)floorf((float)rel / 12.0f);
    int pc = ((rel % 12) + 12) % 12;
    int idx = 0, bestd = 99;
    for (int i = 0; i < n; i++) {
        int d = abs(deg[i] - pc);
        if (d < bestd) { bestd = d; idx = i; }
    }
    int j = idx + steps;
    while (j < 0)  { j += n; oct--; }
    while (j >= n) { j -= n; oct++; }
    return (float)(key + oct * 12 + deg[j]) + (note - (float)rn);
}

static float harm_target(const belt_t *b, float q, int itv) {
    if (itv == ITV_OFF) return q;
    if (itv_is_oct[itv]) return q + 12.0f * (float)itv_is_oct[itv];
    if (itv == ITV_UNIS) return q;
    if (b->scale == 0)   return q + (float)itv_chroma[itv];
    return degree_shift(q, b->key, b->scale, itv_steps[itv]);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */

belt_t *belt_create(const host_api_v1_t *host) {
    belt_t *b = (belt_t *)calloc(1, sizeof(belt_t));
    if (!b) return NULL;
    b->in_ring  = (float *)calloc(IN_RING, sizeof(float));
    b->dry_ring = (int16_t *)calloc((size_t)DRY_RING * 2, sizeof(int16_t));
    b->acc      = (float *)calloc((size_t)ACC_RING * 2, sizeof(float));
    b->yin_ring = (float *)calloc(YIN_RING, sizeof(float));
    if (!b->in_ring || !b->dry_ring || !b->acc || !b->yin_ring) {
        belt_destroy(b);
        return NULL;
    }
    b->host = host;

    b->key = 0; b->scale = 1;            /* C Major */
    b->retune = 25; b->amount = 100; b->flex = 0; b->humanize = 30;
    b->harm_level = 80; b->spread = 70; b->double_amt = 0;
    b->formant = 0; b->wet = 100; b->monitor = 1;

    /* start the clock late enough that (w - LATENCY - grain reach) never
     * underflows the unsigned math */
    b->w = IN_RING;
    b->yw = YIN_RING;
    b->f_inst = 150.0f;
    b->note_inst = b->note_slow = note_of_freq(150.0f);
    b->cur_T = (float)BELT_SR / 150.0f;
    b->next_mark = (double)b->w;
    b->rng = 0xBE17u;

    static const float pans[BELT_VOICES]  = { 0, -0.8f, 0.8f, -0.45f, 0.45f, -0.95f, 0.95f };
    static const float cents[BELT_VOICES] = { 0, -9, 7, -5, 11, -10, 10 };
    static const float fmul[BELT_VOICES]  = { 1.0f, 0.97f, 1.03f, 0.95f, 1.05f, 0.98f, 1.02f };
    for (int i = 0; i < BELT_VOICES; i++) {
        b->v[i].next_out = (double)b->w;
        b->v[i].pan = pans[i];
        b->v[i].det_cents = cents[i];
        b->v[i].formant_mul = fmul[i];
        b->v[i].ratio_log = b->v[i].ratio_tgt = 0.0f;
    }
    return b;
}

void belt_destroy(belt_t *b) {
    if (!b) return;
    free(b->in_ring); free(b->dry_ring); free(b->acc); free(b->yin_ring);
    free(b);
}

void belt_on_midi(belt_t *b, const uint8_t *msg, int len, int source) {
    /* v1 ignores MIDI (no clock dependence). Kept for ABI + future
     * MIDI-controlled harmony. */
    (void)b; (void)msg; (void)len; (void)source;
}

/* ------------------------------------------------------------------ */
/* analysis                                                           */

static void belt_analyze(belt_t *b) {
    const int n = YIN_W + YIN_TAUMAX;
    uint64_t start = b->yw - (uint64_t)n;
    for (int i = 0; i < n; i++)
        b->yin_buf[i] = b->yin_ring[(start + (uint64_t)i) & YIN_MASK];

    float acc = 0.0f;
    for (int i = n - YIN_W; i < n; i++) acc += b->yin_buf[i] * b->yin_buf[i];
    b->rms = sqrtf(acc / (float)YIN_W);

    if (b->rms < RMS_GATE) {
        b->voiced = 0;
        return;
    }

    /* tracking optimization: while locked onto a voice, only search lags up
     * to 1.35x the previous period — the long-lag region is the expensive
     * part of YIN. CMNDF stays valid because the running sum always starts
     * at tau=1. If the capped pass loses the note, redo a full pass. */
    int tau_hi = YIN_TAUMAX;
    if (b->voiced && b->f_inst > BELT_FMIN) {
        int last_tau = (int)((float)YIN_SR / b->f_inst);
        tau_hi = last_tau + last_tau / 3 + 4;
        if (tau_hi > YIN_TAUMAX) tau_hi = YIN_TAUMAX;
    }

    int best = -1, voiced = 0;
    for (int pass = 0; pass < 2; pass++) {
        float running = 0.0f;
        float *d = b->d, *cm = b->cmnd;
        d[0] = 0.0f; cm[0] = 1.0f;
        for (int tau = 1; tau <= tau_hi; tau++) {
            float s = 0.0f;
            const float *x = b->yin_buf;
            const float *y = b->yin_buf + tau;
            for (int j = 0; j < YIN_W; j++) {
                float diff = x[j] - y[j];
                s += diff * diff;
            }
            d[tau] = s;
            running += s;
            cm[tau] = running > 1e-12f ? s * (float)tau / running : 1.0f;
        }

        best = -1;
        for (int tau = YIN_TAUMIN; tau <= tau_hi; tau++) {
            if (cm[tau] < 0.15f) {
                while (tau + 1 <= tau_hi && cm[tau + 1] < cm[tau]) tau++;
                best = tau;
                break;
            }
        }
        if (best < 0) {
            float mn = 1e9f;
            for (int tau = YIN_TAUMIN; tau <= tau_hi; tau++)
                if (cm[tau] < mn) { mn = cm[tau]; best = tau; }
        }
        voiced = best > 0 && b->cmnd[best] < 0.30f;
        if (voiced || tau_hi == YIN_TAUMAX) break;
        tau_hi = YIN_TAUMAX;   /* tracking lost the note: full search */
    }
    float *cm = b->cmnd;
    b->voiced = voiced;
    if (!voiced) return;

    float tf = (float)best;
    if (best > YIN_TAUMIN && best < tau_hi) {   /* cm above tau_hi is stale */
        float a = cm[best - 1], c0 = cm[best], c = cm[best + 1];
        float den = a - 2.0f * c0 + c;
        if (fabsf(den) > 1e-9f) tf += 0.5f * (a - c) / den;
    }
    float f = (float)YIN_SR / tf;
    f = clampf(f, BELT_FMIN, BELT_FMAX);

    b->f_hist[0] = b->f_hist[1];
    b->f_hist[1] = b->f_hist[2];
    b->f_hist[2] = f;
    if (b->f_hist_n < 3) { b->f_hist_n++; b->f_inst = f; }
    else b->f_inst = med3(b->f_hist[0], b->f_hist[1], b->f_hist[2]);

    b->note_inst = note_of_freq(b->f_inst);

    /* vibrato-smoothing line with note-jump snap: a leap > 0.8 st that
     * persists 3 analyses is a new note, not vibrato — snap instead of
     * gliding through it */
    if (fabsf(b->note_inst - b->note_slow) > 0.8f) {
        if (++b->jump_frames >= 3) {
            b->note_slow = b->note_inst;
            b->jump_frames = 0;
        }
    } else {
        b->jump_frames = 0;
    }
    /* ~120 ms one-pole at the 172 Hz analysis rate */
    b->note_slow += (b->note_inst - b->note_slow) * 0.045f;
}

/* ------------------------------------------------------------------ */
/* per-block control update                                           */

static void belt_update_targets(belt_t *b, int frames) {
    float dt = (float)frames / (float)BELT_SR;

    /* voicing envelope: fast attack, ~60 ms release */
    float vt = b->voiced ? 1.0f : 0.0f;
    float va = vt > b->voiced_sm ? 0.5f : dt / (dt + 0.060f);
    b->voiced_sm += (vt - b->voiced_sm) * va;

    float amount01 = (float)(b->hard ? 100 : b->amount) / 100.0f;
    float flex01 = (float)b->flex / 100.0f;
    float hum01 = (float)b->humanize / 100.0f;

    /* quantize the slow line */
    float q = quantize_note(b->note_slow, b->key, b->scale);
    b->q_note = q;

    float wgt = 1.0f;
    if (b->flex > 0) {
        float dist = fabsf(q - b->note_slow);
        float margin = 0.5f * (1.0f - flex01 * 0.85f);
        if (dist > margin)
            wgt = clampf(1.0f - (dist - margin) / 0.35f, 0.0f, 1.0f);
    }
    float off_target = (q - b->note_slow) * amount01 * wgt;

    /* retune-speed smoothing of the correction offset; freeze during
     * unvoiced so each phrase doesn't re-glide from stale values */
    if (b->voiced) {
        float tau = ((float)(b->hard ? 0 : b->retune) / 100.0f);
        tau = tau * tau * 0.5f;                       /* 0..500 ms */
        float alpha = tau < 1e-4f ? 1.0f : dt / (dt + tau);
        b->lead_off += (off_target - b->lead_off) * alpha;
    }
    b->cents_err = (b->note_inst - q) * 100.0f;

    float residue = b->note_inst - b->note_slow;      /* vibrato etc. */

    /* lead: output = note_slow + off + humanize*residue */
    float lead_out = b->note_slow + b->lead_off + hum01 * residue;
    float wet01 = (float)b->wet / 100.0f;
    b->v[0].ratio_tgt = (lead_out - b->note_inst) / 12.0f;
    b->v[0].gain_tgt = wet01;
    b->v[0].pan = 0.0f;

    /* harmonies: track the fully-quantized note, carry half the vibrato */
    float hlvl = (float)b->harm_level / 100.0f * 0.9f;
    float spread01 = (float)b->spread / 100.0f;
    static const float hpan[BELT_HARMONIES] = { -0.8f, 0.8f, -0.45f, 0.45f };
    for (int i = 0; i < BELT_HARMONIES; i++) {
        belt_voice_t *v = &b->v[1 + i];
        int itv = b->harm[i];
        if (itv == ITV_OFF) {
            v->gain_tgt = 0.0f;
        } else {
            float hn = harm_target(b, q, itv);
            float cents = (v->det_cents + v->wander) * hum01;
            float out = hn + cents / 100.0f + 0.5f * hum01 * residue;
            v->ratio_tgt = (out - b->note_inst) / 12.0f;
            v->gain_tgt = hlvl * b->voiced_sm;
        }
        v->pan = hpan[i] * spread01;
        /* slow detune wander, humanize-scaled */
        v->wander = clampf(v->wander * 0.999f + rng_bipolar(&b->rng) * 0.25f,
                           -8.0f, 8.0f);
    }

    /* doubler: corrected lead +/- detune, hard-panned */
    float dbl01 = (float)b->double_amt / 100.0f;
    float dcents = 8.0f + dbl01 * 8.0f;
    for (int i = 0; i < 2; i++) {
        belt_voice_t *v = &b->v[5 + i];
        float sgn = i ? 1.0f : -1.0f;
        v->ratio_tgt = b->v[0].ratio_tgt +
                       sgn * (dcents + v->wander * 0.4f) / 1200.0f;
        v->gain_tgt = dbl01 * 0.65f * b->voiced_sm;
        v->wander = clampf(v->wander * 0.999f + rng_bipolar(&b->rng) * 0.25f,
                           -6.0f, 6.0f);
    }

    /* smooth ratios & gains (zipper control) */
    float ra = dt / (dt + 0.012f);
    float ga = dt / (dt + 0.010f);
    for (int i = 0; i < BELT_VOICES; i++) {
        belt_voice_t *v = &b->v[i];
        v->ratio_log += (v->ratio_tgt - v->ratio_log) * ra;
        v->gain += (v->gain_tgt - v->gain) * ga;
    }
}

/* ------------------------------------------------------------------ */
/* synthesis                                                          */

static float ring_lerp(const belt_t *b, double pos) {
    double fl = floor(pos);
    float frac = (float)(pos - fl);
    uint64_t i0 = (uint64_t)fl;
    float a = b->in_ring[i0 & IN_MASK];
    float c = b->in_ring[(i0 + 1) & IN_MASK];
    return a + (c - a) * frac;
}

/* nearest recorded pitch mark to `want`; returns its period in *T */
static double nearest_mark(const belt_t *b, double want, float *T) {
    if (b->mark_n == 0) { *T = b->cur_T; return want; }
    double best = 0.0; float bestT = b->cur_T; double bestd = 1e18;
    for (int k = 0; k < b->mark_n; k++) {
        int idx = (b->mark_head - k + MARKS) % MARKS;
        double d = fabs(b->mark_t[idx] - want);
        if (d < bestd) { bestd = d; best = b->mark_t[idx]; bestT = b->mark_T[idx]; }
        if (b->mark_t[idx] < want - (double)T_MAX * 2.0) break;
    }
    *T = bestT;
    return best;
}

static void fire_grain(belt_t *b, const belt_voice_t *v, double t_o, float g) {
    float T; /* period at the source mark */
    float Gtmp = clampf(b->cur_T, T_MIN, T_MAX);
    double want = t_o + (double)Gtmp - (double)BELT_LATENCY;
    double m = nearest_mark(b, want, &T);
    float G = clampf(T, T_MIN, T_MAX);

    /* keep the grain's content reach behind the write head: G*(1+g) <= L */
    float gmax = ((float)BELT_LATENCY - 64.0f) / G - 1.0f;
    g = clampf(g * v->formant_mul, 0.55f, gmax < 0.55f ? 0.55f : gmax);

    int len = (int)(2.0f * G);
    if (len < 8) len = 8;
    float inv = 1.0f / (float)len;
    /* equal-power pan */
    float th = (v->pan + 1.0f) * 0.785398163f;
    float gl = cosf(th) * v->gain, gr = sinf(th) * v->gain;

    uint64_t t0 = (uint64_t)t_o;
    for (int k = 0; k < len; k++) {
        float ph = ((float)k + 0.5f) * inv;
        float win = 0.5f - 0.5f * cosf(6.2831853f * ph);
        double src = m + (double)((((float)k + 0.5f) - G) * g);
        float s = ring_lerp(b, src) * win;
        uint64_t oi = (t0 + (uint64_t)k) & ACC_MASK;
        b->acc[oi * 2]     += s * gl;
        b->acc[oi * 2 + 1] += s * gr;
    }
}

/* Leave normal levels untouched, then bend the final 10% smoothly toward
 * full scale. Additive harmony stacks can otherwise sit on the hard int16
 * clamp for long stretches, which sounds much harsher than a vocal-bus
 * limiter and makes the exact result depend on voice correlation. */
static float output_limit(float x) {
    float a = fabsf(x);
    if (a <= 0.9f) return x;
    float y = 0.9f + 0.1f * tanhf((a - 0.9f) * 10.0f);
    if (y > 0.999f) y = 0.999f;
    return x < 0.0f ? -y : y;
}

void belt_process(belt_t *b, const int16_t *in, int16_t *out, int frames) {
    /* 1. ingest */
    for (int i = 0; i < frames; i++) {
        float l = (float)in[i * 2] / 32768.0f;
        float r = (float)in[i * 2 + 1] / 32768.0f;
        float mono = 0.5f * (l + r);
        uint64_t wi = b->w + (uint64_t)i;
        b->in_ring[wi & IN_MASK] = mono;
        b->dry_ring[(wi & DRY_MASK) * 2]     = in[i * 2];
        b->dry_ring[(wi & DRY_MASK) * 2 + 1] = in[i * 2 + 1];
        b->dec_acc += mono;
        if (++b->dec_phase == YIN_DEC) {
            b->yin_ring[b->yw & YIN_MASK] = b->dec_acc / (float)YIN_DEC;
            b->yw++;
            b->dec_phase = 0;
            b->dec_acc = 0.0f;
        }
    }
    b->w += (uint64_t)frames;

    /* 2. periodic pitch analysis */
    b->since_analysis += frames;
    while (b->since_analysis >= YIN_HOP) {
        b->since_analysis -= YIN_HOP;
        belt_analyze(b);
    }

    /* 3. advance the pitch-mark train */
    b->cur_T = clampf((float)BELT_SR / b->f_inst, T_MIN, T_MAX);
    while (b->next_mark <= (double)b->w) {
        b->mark_head = (b->mark_head + 1) % MARKS;
        b->mark_t[b->mark_head] = b->next_mark;
        b->mark_T[b->mark_head] = b->cur_T;
        if (b->mark_n < MARKS) b->mark_n++;
        b->next_mark += (double)b->cur_T;
    }

    /* 4. control targets */
    belt_update_targets(b, frames);

    /* 5. fire grains up to the end of this block */
    double e = (double)b->w;
    float g_global = exp2f((float)b->formant / 100.0f * 0.5f);
    for (int vi = 0; vi < BELT_VOICES; vi++) {
        belt_voice_t *v = &b->v[vi];
        if (v->gain < 0.001f && v->gain_tgt <= 0.0f) {
            if (v->next_out < e) v->next_out = e;   /* stay in sync while off */
            continue;
        }
        int fired = 0;
        while (v->next_out < e && fired < 64) {
            fire_grain(b, v, v->next_out, g_global);
            float ratio = exp2f(v->ratio_log);
            float T_out = b->cur_T / clampf(ratio, 0.25f, 4.0f);
            v->next_out += (double)clampf(T_out, 16.0f, 4096.0f);
            fired++;
        }
        if (v->next_out < e) v->next_out = e;       /* runaway guard */
    }

    /* 6. mix accumulator + latency-matched dry, clear behind ourselves */
    float dry01 = 1.0f - (float)b->wet / 100.0f;
    uint64_t rd = b->w - (uint64_t)frames;
    int mute = !b->monitor;
    int limit_on = b->wet > 0 || b->double_amt > 0;
    for (int i = 0; i < BELT_HARMONIES; i++)
        if (b->harm[i] != ITV_OFF) limit_on = 1;
    for (int i = 0; i < frames; i++) {
        uint64_t t = rd + (uint64_t)i;
        uint64_t ai = t & ACC_MASK;
        float l = b->acc[ai * 2];
        float r = b->acc[ai * 2 + 1];
        b->acc[ai * 2] = b->acc[ai * 2 + 1] = 0.0f;

        uint64_t di = (t - (uint64_t)BELT_LATENCY) & DRY_MASK;
        l += dry01 * (float)b->dry_ring[di * 2] / 32768.0f;
        r += dry01 * (float)b->dry_ring[di * 2 + 1] / 32768.0f;

        if (limit_on) {
            l = output_limit(l);
            r = output_limit(r);
        }

        int32_t li = (int32_t)(l * 32767.0f);
        int32_t ri = (int32_t)(r * 32767.0f);
        out[i * 2]     = mute ? 0 : (int16_t)li;
        out[i * 2 + 1] = mute ? 0 : (int16_t)ri;
    }
}

/* ------------------------------------------------------------------ */
/* params                                                             */

typedef struct { const char *key; int *field; int lo, hi; } param_map_t;

static int param_table(belt_t *b, param_map_t *t) {
    int n = 0;
    t[n++] = (param_map_t){ "key",        &b->key,        0, 11 };
    t[n++] = (param_map_t){ "scale",      &b->scale,      0, 8 };
    t[n++] = (param_map_t){ "retune",     &b->retune,     0, 100 };
    t[n++] = (param_map_t){ "amount",     &b->amount,     0, 100 };
    t[n++] = (param_map_t){ "flex",       &b->flex,       0, 100 };
    t[n++] = (param_map_t){ "humanize",   &b->humanize,   0, 100 };
    t[n++] = (param_map_t){ "harm1",      &b->harm[0],    0, 11 };
    t[n++] = (param_map_t){ "harm2",      &b->harm[1],    0, 11 };
    t[n++] = (param_map_t){ "harm3",      &b->harm[2],    0, 11 };
    t[n++] = (param_map_t){ "harm4",      &b->harm[3],    0, 11 };
    t[n++] = (param_map_t){ "harm_level", &b->harm_level, 0, 100 };
    t[n++] = (param_map_t){ "spread",     &b->spread,     0, 100 };
    t[n++] = (param_map_t){ "double_amt", &b->double_amt, 0, 100 };
    t[n++] = (param_map_t){ "formant",    &b->formant,    -100, 100 };
    t[n++] = (param_map_t){ "wet",        &b->wet,        0, 100 };
    t[n++] = (param_map_t){ "hard",       &b->hard,       0, 1 };
    return n;
}

/* minimal JSON number scan for the state blob: finds "key": <num> */
static int json_int(const char *src, const char *key, int *out) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(src, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    *out = (int)strtol(p, NULL, 10);
    return 1;
}

void belt_set_param(belt_t *b, const char *key, const char *val) {
    if (!b || !key || !val) return;
    if (!strcmp(key, "rui_set")) {
        const char *separator = strchr(val, ':');
        size_t key_len = separator ? (size_t)(separator - val) : 0;
        char routed_key[32];
        if (!separator || key_len == 0 || key_len >= sizeof(routed_key)) return;
        memcpy(routed_key, val, key_len);
        routed_key[key_len] = '\0';
        if (!strcmp(routed_key, "rui_set")) return;
        belt_set_param(b, routed_key, separator + 1);
        return;
    }
    param_map_t t[24];
    int n = param_table(b, t);
    for (int i = 0; i < n; i++) {
        if (!strcmp(key, t[i].key)) {
            *t[i].field = clampi((int)strtol(val, NULL, 10), t[i].lo, t[i].hi);
            return;
        }
    }
    if (!strcmp(key, "monitor")) {          /* never preset-saved */
        b->monitor = strtol(val, NULL, 10) != 0;
        return;
    }
    if (!strcmp(key, "hw_input")) {
        b->hw_input = strtol(val, NULL, 10) != 0;
        return;
    }
    if (!strcmp(key, "state")) {
        for (int i = 0; i < n; i++) {
            int v;
            if (json_int(val, t[i].key, &v))
                *t[i].field = clampi(v, t[i].lo, t[i].hi);
        }
        return;
    }
}

int belt_get_param(belt_t *b, const char *key, char *buf, int buf_len) {
    if (!b || !key || !buf || buf_len < 2) return -1;
    param_map_t t[24];
    int n = param_table(b, t);
    for (int i = 0; i < n; i++)
        if (!strcmp(key, t[i].key))
            return snprintf(buf, (size_t)buf_len, "%d", *t[i].field);

    if (!strcmp(key, "monitor"))
        return snprintf(buf, (size_t)buf_len, "%d", b->monitor);
    if (!strcmp(key, "hw_input"))
        return snprintf(buf, (size_t)buf_len, "%d", b->hw_input);
    if (!strcmp(key, "module_id"))
        return snprintf(buf, (size_t)buf_len, "belt");

    /* combined UI status: one poll per tick (mark lesson).
     * "<midi_note*10>:<cents_to_target>:<voiced>:<harm_active_mask>" */
    if (!strcmp(key, "status")) {
        int mask = 0;
        for (int i = 0; i < BELT_HARMONIES; i++)
            if (b->harm[i] != ITV_OFF) mask |= 1 << i;
        return snprintf(buf, (size_t)buf_len, "%d:%d:%d:%d",
                        (int)lrintf(b->note_inst * 10.0f),
                        (int)lrintf(b->cents_err),
                        b->voiced,
                        mask);
    }
    if (!strcmp(key, "voiced"))
        return snprintf(buf, (size_t)buf_len, "%d", b->voiced);
    if (!strcmp(key, "detected_note"))
        return snprintf(buf, (size_t)buf_len, "%.2f", (double)b->note_inst);
    if (!strcmp(key, "detected_freq"))
        return snprintf(buf, (size_t)buf_len, "%.2f", (double)b->f_inst);

    if (!strcmp(key, "state")) {
        int w = 0;
        app(buf, buf_len, &w, "{");
        for (int i = 0; i < n; i++)
            app(buf, buf_len, &w, "%s\"%s\":%d", i ? "," : "", t[i].key, *t[i].field);
        app(buf, buf_len, &w, "}");
        return w;
    }
    return -1;
}
