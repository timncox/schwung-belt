/*
 * Native host simulation for the Belt core — no hardware needed.
 *
 * Feeds sine tones through the engine block-by-block and asserts on the
 * actual DSP behavior: YIN detection accuracy, correction pull-to-scale,
 * diatonic harmony frequencies (Goertzel probes), doubler stereo spread,
 * formant-shift effect, voicing gate, state round-trip, param clamping.
 *
 * Build & run:  make test
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/belt_core.h"

#define BLK 128
#define CAP (1 << 16)

static float fake_bpm(void) { return 120.0f; }

static belt_t *B;
static double sine_phase = 0.0;

/* capture ring of recent output */
static int16_t cap_l[CAP], cap_r[CAP];
static int cap_n = 0;

static void run_sine(double freq, double amp, int nblocks) {
    int16_t in[BLK * 2], out[BLK * 2];
    for (int b = 0; b < nblocks; b++) {
        for (int i = 0; i < BLK; i++) {
            int16_t v = 0;
            if (freq > 0.0) {
                sine_phase += freq / 44100.0;
                if (sine_phase >= 1.0) sine_phase -= 1.0;
                v = (int16_t)(sin(sine_phase * 2.0 * M_PI) * 32767.0 * amp);
            }
            in[i * 2] = in[i * 2 + 1] = v;
        }
        belt_process(B, in, out, BLK);
        for (int i = 0; i < BLK; i++) {
            cap_l[cap_n & (CAP - 1)] = out[i * 2];
            cap_r[cap_n & (CAP - 1)] = out[i * 2 + 1];
            cap_n++;
        }
    }
}

static void snap(float *l, float *r, int n) {  /* last n frames, floats */
    for (int i = 0; i < n; i++) {
        int idx = (cap_n - n + i) & (CAP - 1);
        l[i] = (float)cap_l[idx] / 32768.0f;
        r[i] = (float)cap_r[idx] / 32768.0f;
    }
}

static double out_rms(int n) {
    double e = 0.0;
    for (int i = 0; i < n; i++) {
        int idx = (cap_n - n + i) & (CAP - 1);
        double m = ((double)cap_l[idx] + (double)cap_r[idx]) * 0.5 / 32768.0;
        e += m * m;
    }
    return sqrt(e / n);
}

/* normalized-autocorrelation period estimate over the last n output frames */
static double detect_freq(int n) {
    static float x[CAP];
    float l[CAP], r[CAP];
    snap(l, r, n);
    for (int i = 0; i < n; i++) x[i] = 0.5f * (l[i] + r[i]);
    int max_lag = 700, min_lag = 40;
    int win = n - max_lag - 1;
    static double rr[701];
    double vmax = 0.0;
    for (int lag = min_lag; lag <= max_lag; lag++) {
        double num = 0.0, e1 = 0.0, e2 = 0.0;
        for (int i = 0; i < win; i++) {
            num += (double)x[i] * x[i + lag];
            e1 += (double)x[i] * x[i];
            e2 += (double)x[i + lag] * x[i + lag];
        }
        double den = sqrt(e1 * e2) + 1e-12;
        rr[lag] = num / den;
        if (rr[lag] > vmax) vmax = rr[lag];
    }
    /* a periodic signal peaks at every period multiple — take the FIRST
     * local peak near the max, not the global max (octave-error guard) */
    int best_lag = 0;
    for (int lag = min_lag + 1; lag < max_lag; lag++) {
        if (rr[lag] >= rr[lag - 1] && rr[lag] >= rr[lag + 1] &&
            rr[lag] > 0.95 * vmax) { best_lag = lag; break; }
    }
    if (best_lag <= min_lag || best_lag >= max_lag) return 0.0;
    /* parabolic refinement using raw autocorr around the peak */
    double y[3];
    for (int k = -1; k <= 1; k++) {
        double num = 0.0;
        int lag = best_lag + k;
        for (int i = 0; i < win; i++) num += (double)x[i] * x[i + lag];
        y[k + 1] = num;
    }
    double den = y[0] - 2.0 * y[1] + y[2];
    double frac = fabs(den) > 1e-12 ? 0.5 * (y[0] - y[2]) / den : 0.0;
    return 44100.0 / ((double)best_lag + frac);
}

/* Goertzel power at freq over the last n output frames (mono mix) */
static double tone_power(double freq, int n) {
    float l[CAP], r[CAP];
    snap(l, r, n);
    double k = 2.0 * M_PI * freq / 44100.0;
    double coeff = 2.0 * cos(k);
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        double xm = 0.5 * ((double)l[i] + (double)r[i]);
        s0 = xm + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return (s1 * s1 + s2 * s2 - coeff * s1 * s2) / n;
}

static void sp(const char *k, const char *v) { belt_set_param(B, k, v); }
static const char *gp(const char *k) {
    static char buf[512];
    int r = belt_get_param(B, k, buf, sizeof(buf));
    return r >= 0 ? buf : NULL;
}

static void reset_defaults(void) {
    sp("key", "0"); sp("scale", "1");
    sp("retune", "0"); sp("amount", "100"); sp("flex", "0");
    sp("humanize", "0");
    sp("harm1", "0"); sp("harm2", "0"); sp("harm3", "0"); sp("harm4", "0");
    sp("harm_level", "80"); sp("spread", "70"); sp("double_amt", "0");
    sp("formant", "0"); sp("wet", "100"); sp("hard", "0"); sp("monitor", "1");
}

int main(void) {
    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version = 1;
    host.sample_rate = 44100;
    host.frames_per_block = BLK;
    host.get_bpm = fake_bpm;

    B = belt_create(&host);
    assert(B);
    reset_defaults();
    sp("rui_set", "amount:37");
    assert(!strcmp(gp("amount"), "37"));
    sp("rui_set", "rui_set:amount:0");
    assert(!strcmp(gp("amount"), "37"));
    sp("rui_set", "amount:100");

    int sec = 44100 / BLK;   /* blocks per second */

    /* ---- 1. YIN pitch detection on A3 = 220 Hz ---- */
    run_sine(220.0, 0.35, sec);
    {
        double f = atof(gp("detected_freq"));
        printf("test 1  detect 220 Hz     -> %.2f Hz voiced=%s\n", f, gp("voiced"));
        assert(!strcmp(gp("voiced"), "1"));
        assert(fabs(f - 220.0) < 220.0 * 0.02);
    }

    /* ---- 2. transparent pass: on-scale input stays put, sane level ---- */
    {
        double f = detect_freq(6144);
        double rms = out_rms(6144);
        float l[4096], r[4096];
        snap(l, r, 4096);
        double le = 0.0, re = 0.0;
        for (int i = 0; i < 4096; i++) {
            le += (double)l[i] * l[i];
            re += (double)r[i] * r[i];
        }
        double balance = sqrt(le / (re + 1e-12));
        printf("test 2  wet pass 220      -> %.2f Hz rms=%.3f\n", f, rms);
        assert(fabs(f - 220.0) < 220.0 * 0.02);
        assert(rms > 0.05 && rms < 0.6);   /* COLA sanity: no collapse/blowup */
        assert(balance > 0.95 && balance < 1.05); /* center pan is centered */
    }

    /* ---- 3. correction pulls +40 cents back to A (chromatic) ---- */
    sp("scale", "0");                      /* chromatic */
    run_sine(225.13, 0.35, 2 * sec);       /* 220 * 2^(40/1200) */
    {
        double f = detect_freq(6144);
        printf("test 3  correct 225.1     -> %.2f Hz (want 220)\n", f);
        assert(fabs(f - 220.0) < 220.0 * 0.015);
    }

    /* ---- 4. amount 0 leaves pitch alone ---- */
    sp("amount", "0");
    run_sine(225.13, 0.35, 2 * sec);
    {
        double f = detect_freq(6144);
        printf("test 4  amount 0          -> %.2f Hz (want 225.1)\n", f);
        assert(fabs(f - 225.13) < 225.13 * 0.015);
    }
    sp("amount", "100");

    /* ---- 4b. HARD overrides without destroying the underlying knobs ---- */
    sp("retune", "80"); sp("amount", "0"); sp("hard", "1");
    run_sine(225.13, 0.35, 2 * sec);
    {
        double f = detect_freq(6144);
        printf("test 4b hard override     -> %.2f Hz (want 220)\n", f);
        assert(fabs(f - 220.0) < 220.0 * 0.015);
        assert(!strcmp(gp("retune"), "80"));
        assert(!strcmp(gp("amount"), "0"));
        assert(!strcmp(gp("hard"), "1"));
    }
    sp("hard", "0"); sp("retune", "0"); sp("amount", "100");

    /* ---- 5. diatonic harmony: +3rd on A in A major = C#5 (277.2) ---- */
    sp("key", "9"); sp("scale", "1");      /* A major */
    sp("harm1", "7");                      /* +3rd */
    run_sine(220.0, 0.35, 2 * sec);
    {
        double p_third = tone_power(277.18, 8192);
        double p_base  = tone_power(220.0, 8192);
        sp("harm1", "0");
        run_sine(220.0, 0.35, sec);
        double p_off = tone_power(277.18, 8192);
        printf("test 5  +3rd harmony      -> 277Hz on=%.5f off=%.5f base=%.5f\n",
               p_third, p_off, p_base);
        assert(p_third > 4.0 * p_off);     /* harmony clearly present */
        assert(p_base > 0.0001);           /* lead still there */
    }

    /* ---- 6. octave-up harmony ---- */
    sp("harm2", "11");                     /* +Oct */
    run_sine(220.0, 0.35, 2 * sec);
    {
        double p_oct = tone_power(440.0, 8192);
        sp("harm2", "0");
        run_sine(220.0, 0.35, sec);
        double p_off = tone_power(440.0, 8192);
        printf("test 6  +Oct harmony      -> 440Hz on=%.5f off=%.5f\n", p_oct, p_off);
        assert(p_oct > 4.0 * p_off);
    }

    /* ---- 7. doubler: stereo decorrelation ---- */
    sp("double_amt", "100");
    run_sine(220.0, 0.35, 2 * sec);
    {
        float l[4096], r[4096];
        snap(l, r, 4096);
        double diff = 0.0, tot = 0.0;
        for (int i = 0; i < 4096; i++) {
            diff += fabs((double)l[i] - r[i]);
            tot += fabs((double)l[i]) + fabs((double)r[i]);
        }
        printf("test 7  doubler L/R diff  -> %.3f of total\n", diff / (tot + 1e-9));
        assert(tot > 10.0);
        assert(diff / tot > 0.05);         /* channels meaningfully different */
    }
    sp("double_amt", "0");

    /* ---- 8. formant shift changes the sound, keeps the period ---- */
    run_sine(220.0, 0.35, 2 * sec);
    {
        float l0[4096], r0[4096];
        snap(l0, r0, 4096);
        sp("formant", "70");
        run_sine(220.0, 0.35, 2 * sec);
        float l1[4096], r1[4096];
        snap(l1, r1, 4096);
        double f = detect_freq(6144);
        double d = 0.0, t = 0.0;
        for (int i = 0; i < 4096; i++) {
            d += fabs((double)l1[i] - l0[i]);
            t += fabs((double)l0[i]);
        }
        printf("test 8  formant 70        -> pitch %.2f Hz, waveform delta %.2f\n",
               f, d / (t + 1e-9));
        assert(fabs(f - 220.0) < 220.0 * 0.03);   /* pitch unchanged */
        assert(d / (t + 1e-9) > 0.10);            /* sound changed */
        sp("formant", "0");
    }

    /* ---- 9. silence gates: no stuck output, voiced drops ---- */
    run_sine(0.0, 0.0, 2 * sec);
    {
        double rms = out_rms(4096);
        printf("test 9  silence           -> rms=%.5f voiced=%s\n", rms, gp("voiced"));
        assert(rms < 0.002);
        assert(!strcmp(gp("voiced"), "0"));
    }

    /* ---- 10. monitor mute (feedback guard path) ---- */
    sp("monitor", "0");
    run_sine(220.0, 0.35, sec);
    {
        double rms = out_rms(4096);
        printf("test 10 monitor mute      -> rms=%.5f\n", rms);
        assert(rms < 1e-6);
    }
    sp("monitor", "1");

    /* ---- 11. param clamping ---- */
    sp("retune", "999"); assert(!strcmp(gp("retune"), "100"));
    sp("key", "-5");     assert(!strcmp(gp("key"), "0"));
    sp("formant", "-500"); assert(!strcmp(gp("formant"), "-100"));
    sp("harm1", "42");   assert(!strcmp(gp("harm1"), "11"));
    printf("test 11 param clamps      -> ok\n");

    /* ---- 12. state blob round-trip ---- */
    sp("key", "4"); sp("scale", "3"); sp("retune", "60"); sp("harm1", "9");
    sp("double_amt", "35"); sp("formant", "-40"); sp("hard", "1");
    {
        char blob[512];
        int n = belt_get_param(B, "state", blob, sizeof(blob));
        assert(n > 0 && blob[0] == '{');

        belt_t *B2 = belt_create(&host);
        assert(B2);
        belt_set_param(B2, "state", blob);
        char v[64];
        belt_get_param(B2, "key", v, sizeof(v));      assert(!strcmp(v, "4"));
        belt_get_param(B2, "scale", v, sizeof(v));    assert(!strcmp(v, "3"));
        belt_get_param(B2, "retune", v, sizeof(v));   assert(!strcmp(v, "60"));
        belt_get_param(B2, "harm1", v, sizeof(v));    assert(!strcmp(v, "9"));
        belt_get_param(B2, "double_amt", v, sizeof(v)); assert(!strcmp(v, "35"));
        belt_get_param(B2, "formant", v, sizeof(v));  assert(!strcmp(v, "-40"));
        belt_get_param(B2, "hard", v, sizeof(v));     assert(!strcmp(v, "1"));
        belt_destroy(B2);
        printf("test 12 state round-trip  -> %d bytes ok\n", n);
    }

    /* ---- 13. state blob never overflows a small buffer (nclamp lesson) --- */
    {
        char tiny[24];
        int n = belt_get_param(B, "state", tiny, sizeof(tiny));
        assert(n <= (int)sizeof(tiny) - 1);
        assert(tiny[n < 24 ? n : 23] == '\0' || n < 24);
        printf("test 13 tiny state buffer -> %d bytes, no overflow\n", n);
    }

    /* ---- 14. status combo param parses ---- */
    reset_defaults();
    run_sine(220.0, 0.35, sec);
    {
        const char *s = gp("status");
        int note10, cents, voiced, mask;
        assert(sscanf(s, "%d:%d:%d:%d", &note10, &cents, &voiced, &mask) == 4);
        printf("test 14 status            -> %s\n", s);
        assert(voiced == 1);
        assert(abs(note10 - 570) < 10);   /* A3 = midi 57 */
    }

    /* ---- 15. dense stacks use the soft ceiling, never the hard clamp ---- */
    sp("harm1", "1"); sp("harm2", "7"); sp("harm3", "9"); sp("harm4", "11");
    sp("harm_level", "100"); sp("double_amt", "100");
    run_sine(220.0, 0.95, 2 * sec);
    {
        int peak = 0;
        for (int i = 0; i < 8192; i++) {
            int idx = (cap_n - 8192 + i) & (CAP - 1);
            int al = abs((int)cap_l[idx]), ar = abs((int)cap_r[idx]);
            if (al > peak) peak = al;
            if (ar > peak) peak = ar;
        }
        printf("test 15 vocal-bus ceiling -> peak=%d\n", peak);
        assert(peak <= 32735);
    }

    belt_destroy(B);
    printf("\nall belt sim tests passed\n");
    return 0;
}
