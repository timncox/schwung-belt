/*
 * Belt — Electrosmith Daisy Patch port.
 *
 * Belt is a live vocal processor: pitch correction plus four diatonic
 * harmonies, a doubler and a formant shift. The engine (vendor/belt_core.c)
 * is the same C used by the Move build; this file is the Daisy shim, the
 * counterpart of belt_fx.c on Move.
 *
 * Three things worth knowing before reading further:
 *
 * 1. SAMPLE RATE. belt_core is written for a compile-time BELT_SR and every
 *    period bound derives from it (T_MIN/T_MAX from FMIN/FMAX, YIN_SR, the
 *    instantaneous period). libDaisy's SaiHandle offers only SAI_48KHZ, so
 *    the vendored header is rebased 44100 -> 48000 and the one raw sample
 *    count, BELT_LATENCY, is scaled 1152 -> 1254 to hold the ~26 ms voice
 *    latency the alignment maths assumes. Everything else follows.
 *    UNVERIFIED BY EAR -- this is the first thing to listen for.
 *
 * 2. CONTROLS. The engine already exposes all 20 params over MIDI CC 20..39
 *    with a linear 0-127 scale into each param's own range (see the comment
 *    above param_table in belt_core.c). Rather than duplicate those ranges
 *    here -- formant alone is -100..100 while the rest are 0..100 -- the four
 *    knobs synthesise CC messages and feed belt_on_midi(). The shim therefore
 *    never needs to know a single range, and the knob path is provably
 *    identical to the MIDI path.
 *
 * 3. KNOB PICKUP. Four knobs address sixteen params across four pages, and
 *    the Patch's knobs are absolute. Without pickup, changing page would slam
 *    four params to wherever the pots happen to sit -- the same trap that made
 *    persistence impossible on the Versio panel. A knob here is inert until it
 *    crosses the value it is taking over, and the display says so. That holds
 *    from power-up (g_cc[] is seeded from the engine's defaults) and after an
 *    external MIDI CC (last touched wins: the CC re-arms pickup on its knob).
 */
#include "daisy_patch.h"
#include "patch_alloc.h"
#include "module_picker.h"

extern "C" {
#include "vendor/belt_core.h"
#include "vendor/plugin_api_v1.h"
}

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace daisy;

static DaisyPatch hw;
static belt_t    *B;
static host_api_v1_t HOST;

/* 256 since 2026-09-22, = YIN_HOP. At 128 the pitch analysis (YIN, W 512 x
 * tau 260; 283 since 2026-10-02, the 48k-correct 85 Hz floor) landed in every OTHER block, so half the blocks carried it and
 * overran while the average sat in the 70s: the header meter read c80/99,
 * then c73/99 with the pool in D2 and -O3. One hop per block spreads that
 * evenly. The engine is frame-count agnostic (ingest, analysis and grain
 * firing all loop on `frames`; rings are 4096+), and the one ring a longer
 * block could outrun, MARKS (64), needs ~29 marks for BELT_LATENCY at the
 * highest pitch (T_MIN 44) plus ~6 for the block. Cost: ~2.7 ms more
 * converter-side latency. 128 was the Move host's block and smack-versio's
 * clock-regression figure; Belt has no clock model. */
#define BLOCK_SIZE 256

/* ~128 KB measured from belt_core's five calloc()s (see patch_alloc.h);
 * 192 KB leaves room for belt_t and any future ring.
 *
 * D2 SRAM since 2026-09-22. It was SDRAM, after DTCMRAM (128 KB, ~74 KB
 * free) and RAM_D2_DMA (32 KB) were ruled out; the 480 KB region named SRAM
 * holds .text in BOOT_SRAM. What that survey missed is RAM_D2: 256 KB at
 * 0x30008000, just above the 32 KB non-cacheable DMA window, so it is
 * ordinary cacheable SRAM, clocked by SystemInit, and the stock linker script
 * puts nothing there but an empty NOLOAD .heap (newlib's heap is unusable
 * here anyway: its stack check compares against a DTCM stack pointer).
 * Declaring the pool in section .heap lands it there with no custom script.
 * NOLOAD means it is not zeroed at boot; patch_calloc memsets, so fine.
 *
 * Why: the first rack-powered run read c80/99 on the header meter, i.e. the
 * callback overran, starving the main loop (knobs, encoder, screen). The YIN
 * tracker touches in_ring and yin_ring per sample, and SDRAM was the slow
 * pool. The Alchemy port made the same move (belt-alchemy 7bdba15/ab76eed). */
static uint8_t __attribute__((section(".heap"), aligned(32))) g_pool[192u * 1024u];

/* ---- control surface ---------------------------------------------------- */

/* CC 20..39, in param_table order. Five pages of four. */
#define CC_BASE  20
#define N_PARAMS 20
static const char *const PARAM_NAME[N_PARAMS] = {
    "key",  "scale", "retn", "amnt",
    "flex", "hmnz",  "hrm1", "hrm2",
    "hrm3", "hrm4",  "hlvl", "sprd",
    "dbl",  "form",  "wet",  "hard",
    "midi", "vsns",  "lead", "hmod",
};
/* The engine's own names for the same sixteen, in the same order, for
 * belt_get_param. PARAM_NAME above is only a four-letter screen label: looking
 * a value up by the label fails for eleven of the sixteen, belt_get_param then
 * writes nothing, and the screen printed whatever was on the stack (first
 * rack-powered run, 2026-09-22: "values wouldn't move or sometimes move"). */
static const char *const ENGINE_KEY[N_PARAMS] = {
    "key",  "scale",    "retune", "amount",
    "flex", "humanize", "harm1",  "harm2",
    "harm3", "harm4",   "harm_level", "spread",
    "double_amt", "formant", "wet", "hard",
    "midi_mode", "vel_sens", "lead", "hold_mode",
};
/* Enum params shown by name instead of number. */
static const char *const MIDI_MODE_NAME[3] = { "Off", "Harm", "Trgt" };
static const char *const HOLD_MODE_NAME[2] = { "Frz", "Lock" };
/* Four param pages, then MODS: push-and-turn there opens the module picker,
 * the same gesture as the "mods" menu item on every other Patch module. MODS
 * has no params, so every g_page * 4 index below is guarded against it. */
/* CHORD: the engine's MIDI keyboard harmonizer (schwung-belt PR #8 +
 * feat/chord-only + feat/hold, vendored from feat/hold a86849c). midi Harm
 * (the power-up mode) = each note held on the TRS MIDI input pins a harmony
 * voice to that pitch; voices with no note keep their hrm1-4 interval. Trgt
 * = the newest held note is the correction target. vsns = velocity -> voice
 * level, lead = sung voice level (0 = chord only), hmod = what HOLD does
 * (Freeze / Lock; HOLD itself is a performance param with no control here
 * yet). Hide and Seek: midi Harm, hrm1-4 Off, lead 0. */
#define P_CHORD 4
#define P_MODS  5
#define N_PAGES 6
static const char *const PAGE_NAME[N_PAGES] = { "TUNE", "HARM A", "HARM B", "VOICE", "CHORD", "MODS" };

static int  g_page;                 /* 0..4 params, 5 = MODS */
static int  g_cc[N_PARAMS];         /* engine's value per param on the CC scale, 0..127 */
static bool g_live[4];              /* has this knob picked up on this page? */
static int  g_knob_at[4];           /* last raw knob reading, 0..127 */

/* A knob takes over only once it crosses the value it is replacing. */

static void page_reset(void)
{
    for(int k = 0; k < 4; k++)
    {
        g_live[k] = false;
        g_knob_at[k] = (int)(hw.GetKnobValue((DaisyPatch::Ctrl)k) * 127.0f + 0.5f);
    }
}

/* Feed the engine through its own MIDI path so ranges live in one place. */
static void send_cc(int idx, int val127)
{
    uint8_t msg[3] = { 0xB0, (uint8_t)(CC_BASE + idx), (uint8_t)val127 };
    belt_on_midi(B, msg, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    g_cc[idx] = val127;
}

/* Seed g_cc[] with the engine's power-up values, so pickup applies from boot
 * like everywhere else (it used to start at -1, "unknown", and the first knob
 * move > 2 took over without crossing anything). Ranges still live only in
 * the engine: each param's lo/hi is read back by sending CC 0 and CC 127, and
 * the power-up value is then restored -- all before audio starts. Rounding to
 * nearest inverts belt_on_midi's lo + (cc * (hi - lo) + 63) / 127 exactly for
 * ranges of <= 127 steps. formant (-100..100) has more steps than CC values,
 * so its 0 is not reachable by CC at all: it seeds to 64 (= +1), the nearest. */
static void seed_cc_from_engine(void)
{
    char was[12], buf[12];
    for(int i = 0; i < N_PARAMS; i++)
    {
        if(belt_get_param(B, ENGINE_KEY[i], was, sizeof(was)) < 0) continue;
        send_cc(i, 0);
        belt_get_param(B, ENGINE_KEY[i], buf, sizeof(buf));
        int lo = atoi(buf);
        send_cc(i, 127);
        belt_get_param(B, ENGINE_KEY[i], buf, sizeof(buf));
        int hi = atoi(buf);
        belt_set_param(B, ENGINE_KEY[i], was);
        int v = atoi(was);
        g_cc[i] = hi > lo ? ((v - lo) * 127 + (hi - lo) / 2) / (hi - lo) : 0;
    }
}

/* ---- audio -------------------------------------------------------------- */

static int16_t bufi[BLOCK_SIZE * 2];

/* Audio-callback load, shown in the header as avg/peak percent over the last
 * second. Added 2026-09-22 when the first rack-powered run showed knobs and
 * pages misbehaving: the same engine overran the M7 on the Alchemy Lab even
 * with its pool in D2 SRAM, and this port keeps the pool in SDRAM. A callback
 * near 100 % starves this main loop, which is where the knobs, encoder and
 * screen live. */
static CpuLoadMeter g_cpu;
static int          g_cpu_avg, g_cpu_peak;   /* percent, last full second */

/* DaisyPatch::StartAudio takes the NON-interleaving callback -- unlike the
 * Versio path, which is interleaving. That suits the Patch: the hardware is
 * 4-in/4-out and belt_process is stereo, so the channel split has to be
 * explicit anyway. Audio In 1/2 carry the voice, Audio Out 1/2 carry the
 * processed result.
 *
 * Outs 3/4 carry the DRY input, latency-matched by nothing -- it is a straight
 * thru, so it is early by BELT_LATENCY relative to the wet pair. That is the
 * useful arrangement for parallel processing: send the dry to a compressor or
 * a delay and recombine downstream, where a fixed offset is a design choice
 * rather than a defect.
 *
 * What outs 3/4 are NOT is one harmony each. That is the thing a 4-out module
 * ought to offer Belt, but belt_process mixes its seven voices to stereo
 * internally, so exposing them is a core change and not part of this port. */
static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    /* size is FRAMES in the non-interleaving callback. */
    g_cpu.OnBlockStart();
    const size_t n = size < BLOCK_SIZE ? size : BLOCK_SIZE;

    for(size_t i = 0; i < n; i++)
    {
        float l = in[0][i], r = in[1][i];
        if(l > 1.0f) l = 1.0f; else if(l < -1.0f) l = -1.0f;
        if(r > 1.0f) r = 1.0f; else if(r < -1.0f) r = -1.0f;
        bufi[i * 2]     = (int16_t)(l * 32767.0f);
        bufi[i * 2 + 1] = (int16_t)(r * 32767.0f);
    }

    belt_process(B, bufi, bufi, (int)n);

    for(size_t i = 0; i < n; i++)
    {
        out[0][i] = (float)bufi[i * 2]     / 32767.0f;
        out[1][i] = (float)bufi[i * 2 + 1] / 32767.0f;
        out[2][i] = in[0][i];    /* dry thru */
        out[3][i] = in[1][i];
    }
    g_cpu.OnBlockEnd();
}

/* ---- CV and gate outputs ------------------------------------------------ */

/*
 * This is what makes Belt worth running on a Eurorack module rather than a
 * laptop: the pitch tracker is already there, so expose it.
 *
 *   CV Out 1   detected pitch as 1V/oct, 0 V = C2 (65.406 Hz)
 *   CV Out 2   the first enabled harmony voice's note (see the engine's
 *              harm_note for MIDI mode), 1V/oct on the same
 *              scale -- or the corrected lead's quantized target when no
 *              harmony is on
 *   Gate Out   high while the tracker says the input is voiced
 *
 * Sing into it and the rack gets a pitch CV and a gate. Belt's tracking range
 * is BELT_FMIN..BELT_FMAX, 85-1000 Hz, which is 0.38 V to 3.93 V on this
 * scaling -- comfortably inside the Patch's 0-5 V output span, with no
 * clipping at either end of the vocal range.
 *
 * CV Out 2 used to echo harm_level, a knob position. The rack already has
 * the knob; what it does not have is the note Belt is singing that the input
 * is not. Two 1V/oct outs make a two-voice pitch-to-CV: an oscillator on CV 1
 * follows the singer, one on CV 2 follows the harmony Belt chose for them,
 * both gated by the voice.
 *
 * Pitch is HELD when unvoiced rather than dropped to zero, because a pitch CV
 * that collapses between phrases would slam whatever it is driving. The gate
 * is what says "this is a note"; the CV just stays where it was.
 */
#define CV_FULL_SCALE_V 5.0f
#define CV_REF_HZ       65.406f   /* C2 -> 0 V */
#define CV_REF_NOTE     36.0f     /* the same C2, as a MIDI note */

static uint16_t g_cv_pitch_last;
static uint16_t g_cv_harm_last;

static uint16_t note_to_dac(float note)
{
    float v = (note - CV_REF_NOTE) / 12.0f;
    if(v < 0.0f) v = 0.0f;
    if(v > CV_FULL_SCALE_V) v = CV_FULL_SCALE_V;
    return (uint16_t)(v / CV_FULL_SCALE_V * 4095.0f);
}

static void update_cv_outs(void)
{
    char buf[24];

    /* Voiced -> gate. */
    int voiced = 0;
    if(belt_get_param(B, "voiced", buf, sizeof(buf)) >= 0) voiced = atoi(buf);
    hw.gate_output.Write(voiced != 0);

    /* Pitch -> 1V/oct. belt_get_param formats this with %.2f, which is why
     * the Makefile links -u _printf_float; without it this reads as "". */
    if(voiced && belt_get_param(B, "detected_freq", buf, sizeof(buf)) >= 0)
    {
        float f = (float)atof(buf);
        if(f >= BELT_FMIN && f <= BELT_FMAX)
        {
            float v = log2f(f / CV_REF_HZ);
            if(v < 0.0f) v = 0.0f;
            if(v > CV_FULL_SCALE_V) v = CV_FULL_SCALE_V;
            g_cv_pitch_last = (uint16_t)(v / CV_FULL_SCALE_V * 4095.0f);
        }
    }
    hw.seed.dac.WriteValue(DacHandle::Channel::ONE, g_cv_pitch_last);

    /* Harmony -> CV out 2, 1V/oct on the same scale. harm_note is a MIDI note
     * formatted "%.2f" -- the second reason this build needs _printf_float.
     * Held while unvoiced for the same reason as the pitch CV. */
    if(voiced && belt_get_param(B, "harm_note", buf, sizeof(buf)) >= 0)
        g_cv_harm_last = note_to_dac((float)atof(buf));
    hw.seed.dac.WriteValue(DacHandle::Channel::TWO, g_cv_harm_last);
}

/* ---- display ------------------------------------------------------------ */

static void draw(void)
{
    char line[32];
    hw.display.Fill(false);

    /* Header: page name, and what the tracker currently hears. */
    char note[16] = "--";
    belt_get_param(B, "detected_note", note, sizeof(note));
    char voiced[8] = "0";
    belt_get_param(B, "voiced", voiced, sizeof(voiced));

    snprintf(line, sizeof(line), "%-7s %s%s", PAGE_NAME[g_page], note,
             voiced[0] == '1' ? "" : "?");
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);
    snprintf(line, sizeof(line), "c%02d/%02d",
             g_cpu_avg > 99 ? 99 : g_cpu_avg, g_cpu_peak > 99 ? 99 : g_cpu_peak);
    hw.display.SetCursor(128 - 6 * 6, 0); /* right edge, clear of the page name */
    hw.display.WriteString(line, Font_6x8, true);

    if(g_page == P_MODS)
    {
        hw.display.SetCursor(0, 26);
        hw.display.WriteString("push + turn:", Font_6x8, true);
        hw.display.SetCursor(0, 36);
        hw.display.WriteString("  module picker", Font_6x8, true);
        hw.display.Update();
        return;
    }

    /* Four params, value plus a marker when the knob has not picked up. */
    for(int k = 0; k < 4; k++)
    {
        int   idx = g_page * 4 + k;
        char  val[12];
        if(belt_get_param(B, ENGINE_KEY[idx], val, sizeof(val)) < 0)
            snprintf(val, sizeof(val), "?");
        else if(idx == 16 && atoi(val) >= 0 && atoi(val) < 3)
            snprintf(val, sizeof(val), "%s", MIDI_MODE_NAME[atoi(val)]);
        else if(idx == 19 && atoi(val) >= 0 && atoi(val) < 2)
            snprintf(val, sizeof(val), "%s", HOLD_MODE_NAME[atoi(val)]);
        snprintf(line, sizeof(line), "%-4s %-5s%s",
                 PARAM_NAME[idx], val, g_live[k] ? "" : " *");
        hw.display.SetCursor(0, 16 + k * 10);
        hw.display.WriteString(line, Font_6x8, true);
    }

    /* CHORD: how many MIDI notes are held (status field 5). y 56 is the
     * last row a Font_6x8 line fits (56 + 8 = 64). */
    char st[48];
    if(g_page == P_CHORD && belt_get_param(B, "status", st, sizeof(st)) >= 0)
    {
        int f[7] = {0};
        sscanf(st, "%d:%d:%d:%d:%d:%d:%d", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6]);
        snprintf(line, sizeof(line), "held %d", f[4]);
        hw.display.SetCursor(0, 56);
        hw.display.WriteString(line, Font_6x8, true);
    }

    hw.display.Update();
}

/* ---- main --------------------------------------------------------------- */

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(BLOCK_SIZE);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    patch_alloc_init(g_pool, sizeof(g_pool));

    HOST.api_version     = 1;
    HOST.sample_rate     = BELT_SR;
    HOST.frames_per_block = BLOCK_SIZE;
    /* belt_core stores the host pointer but never dereferences it -- verified
     * by grepping for "host->" in belt_core.c. Everything below stays null so
     * a future engine change that does reach for the host faults loudly here
     * rather than reading garbage. */

    B = belt_create(&HOST);

    if(!B || patch_alloc_failed())
    {
        /* Say so rather than run half-initialised. */
        hw.display.Fill(false);
        hw.display.SetCursor(0, 0);
        hw.display.WriteString("BELT: alloc fail", Font_6x8, true);
        hw.display.Update();
        for(;;) {}
    }

    seed_cc_from_engine();   /* before audio: it briefly moves each param */

    g_cpu.Init(hw.AudioSampleRate(), hw.AudioBlockSize());
    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    hw.midi.StartReceive();

    page_reset();

    uint32_t last_draw = System::GetNow();
    uint32_t last_cpu  = last_draw;

    for(;;)
    {
        hw.ProcessAllControls();

        /* Encoder turns pages; every page change re-arms pickup. On MODS,
         * push-and-turn opens the module picker (module_picker.h), the same
         * gesture as the "mods" item on every other Patch module. It returns
         * only when the user backs out, after the encoder is released. */
        int inc = hw.encoder.Increment();
        if(inc && g_page == P_MODS && hw.encoder.Pressed())
        {
            picker::run(hw);
            page_reset();   /* the knobs may have moved meanwhile */
        }
        else if(inc)
        {
            g_page = ((g_page + inc) % N_PAGES + N_PAGES) % N_PAGES;
            page_reset();
        }

        /* Knobs -> CC, with pickup. MODS has no params. */
        for(int k = 0; k < 4 && g_page != P_MODS; k++)
        {
            int idx = g_page * 4 + k;
            int raw = (int)(hw.GetKnobValue((DaisyPatch::Ctrl)k) * 127.0f + 0.5f);

            if(!g_live[k])
            {
                int cur = g_cc[idx];
                if((g_knob_at[k] <= cur && raw >= cur)
                   || (g_knob_at[k] >= cur && raw <= cur))
                {
                    g_live[k] = true;
                }
                g_knob_at[k] = raw;
                if(!g_live[k]) continue;
            }

            if(raw != g_cc[idx]) send_cc(idx, raw);
            g_knob_at[k] = raw;
        }

        /* External MIDI: CCs drive the same params directly; notes go to the
         * engine's harmonizer (it tracks them in every midi mode, so nothing
         * is stranded by a mode change). */
        hw.midi.Listen();
        while(hw.midi.HasEvents())
        {
            MidiEvent ev = hw.midi.PopEvent();
            if(ev.type == NoteOn || ev.type == NoteOff)
            {
                uint8_t msg[3] = { (uint8_t)((ev.type == NoteOn ? 0x90 : 0x80) | (ev.channel & 0x0F)),
                                   (uint8_t)ev.data[0],
                                   (uint8_t)ev.data[1] };
                belt_on_midi(B, msg, 3, MOVE_MIDI_SOURCE_EXTERNAL);
            }
            else if(ev.type == ControlChange)
            {
                uint8_t msg[3] = { (uint8_t)(0xB0 | (ev.channel & 0x0F)),
                                   (uint8_t)ev.data[0],
                                   (uint8_t)ev.data[1] };
                belt_on_midi(B, msg, 3, MOVE_MIDI_SOURCE_EXTERNAL);
                int idx = (int)ev.data[0] - CC_BASE;
                if(idx >= 0 && idx < N_PARAMS)
                {
                    g_cc[idx] = ev.data[1];
                    /* Last touched wins. A live knob on this param would
                     * resend its own position on the next pass and undo the
                     * CC at once; re-arm pickup so it has to cross the new
                     * value before taking over again. */
                    if(idx / 4 == g_page) g_live[idx % 4] = false;
                }
            }
        }

        /* CV and gate track the voice, so they run every loop pass rather
         * than at the display's 20 Hz — a gate that lagged 50 ms would be
         * useless for triggering an envelope. */
        update_cv_outs();

        /* ~20 Hz is plenty for a readout and keeps SPI off the audio ISR's back. */
        uint32_t now = System::GetNow();
        if(now - last_cpu >= 1000u)
        {
            g_cpu_avg  = (int)(g_cpu.GetAvgCpuLoad() * 100.0f + 0.5f);
            g_cpu_peak = (int)(g_cpu.GetMaxCpuLoad() * 100.0f + 0.5f);
            g_cpu.Reset();
            last_cpu = now;
        }
        if(now - last_draw >= 50u)
        {
            draw();
            last_draw = now;
        }
    }
}
