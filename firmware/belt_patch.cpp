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
 * 2. CONTROLS. The engine already exposes all 16 params over MIDI CC 20..35
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
 *    crosses the value it is taking over, and the display says so.
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

/* 128 was measured on smack-versio as the block size its clock regression
 * needs. Belt has no clock model, but 128 is also what the Move host uses,
 * so the engine has only ever run at this block size. Keep it. */
#define BLOCK_SIZE 128

/* ~128 KB measured from belt_core's five calloc()s (see patch_alloc.h);
 * 192 KB leaves room for belt_t and any future ring.
 *
 * SDRAM, after the memory map ruled out everything faster. Under
 * STM32H750IB_sram.lds the uninitialised regions are:
 *
 *     .bss / .dtcmram_bss  -> DTCMRAM     128 KB  (~74 KB free) -- too small
 *     .sram1_bss           -> RAM_D2_DMA   32 KB                -- too small
 *     .sdram_bss           -> SDRAM        64 MB                -- fits
 *
 * The 480 KB region named SRAM is not available for data here: in BOOT_SRAM
 * the app's .text lives there. Both faster placements were tried and both
 * overflowed (DTCMRAM by 119252 B, RAM_D2_DMA by 180800 B).
 *
 * SDRAM is the slow pool and the YIN tracker touches in_ring and yin_ring
 * per sample, so this is the thing to watch if CPU becomes the ceiling. It is
 * not a guess that it works -- smack-versio runs a 16 MB SDRAM ring inside its
 * audio callback on the same silicon -- but Belt's access pattern is different
 * and unmeasured. If it stalls, the fix is to move in_ring/yin_ring alone into
 * DTCMRAM (they are 64 KB + 16 KB, which does fit) and leave the rest here. */
static uint8_t DSY_SDRAM_BSS g_pool[192u * 1024u];

/* ---- control surface ---------------------------------------------------- */

/* CC 20..35, in param_table order. Four pages of four. */
#define CC_BASE 20
static const char *const PARAM_NAME[16] = {
    "key",  "scale", "retn", "amnt",
    "flex", "hmnz",  "hrm1", "hrm2",
    "hrm3", "hrm4",  "hlvl", "sprd",
    "dbl",  "form",  "wet",  "hard",
};
static const char *const PAGE_NAME[4] = { "TUNE", "HARM A", "HARM B", "VOICE" };

static int  g_page;                 /* 0..3 */
static int  g_cc[16];               /* last CC value sent per param, 0..127 */
static bool g_live[4];              /* has this knob picked up on this page? */
static int  g_knob_at[4];           /* last raw knob reading, 0..127 */

/* A knob takes over only once it crosses the value it is replacing. */
#define PICKUP_SLOP 2

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

/* ---- audio -------------------------------------------------------------- */

static int16_t bufi[BLOCK_SIZE * 2];

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
}

/* ---- CV and gate outputs ------------------------------------------------ */

/*
 * This is what makes Belt worth running on a Eurorack module rather than a
 * laptop: the pitch tracker is already there, so expose it.
 *
 *   CV Out 1   detected pitch as 1V/oct, 0 V = C2 (65.406 Hz)
 *   CV Out 2   the first enabled harmony voice's note, 1V/oct on the same
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

    snprintf(line, sizeof(line), "%-7s %s%s",
             PAGE_NAME[g_page], note, voiced[0] == '1' ? "" : "?");
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);

    /* Four params, value plus a marker when the knob has not picked up. */
    for(int k = 0; k < 4; k++)
    {
        int   idx = g_page * 4 + k;
        char  val[12];
        belt_get_param(B, PARAM_NAME[idx], val, sizeof(val));
        snprintf(line, sizeof(line), "%-4s %-5s%s",
                 PARAM_NAME[idx], val, g_live[k] ? "" : " *");
        hw.display.SetCursor(0, 16 + k * 10);
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

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    hw.midi.StartReceive();

    page_reset();
    for(int i = 0; i < 16; i++) g_cc[i] = -1;

    uint32_t last_draw = System::GetNow();

    for(;;)
    {
        hw.ProcessAllControls();

        /* Hold the encoder for a second: the module picker (module_picker.h).
         * Belt has no other press gesture, so a hold is free here; Smack and
         * Mark reach the same screen from a menu item. It returns only when
         * the user backs out, after the encoder is released. */
        if(hw.encoder.Pressed() && hw.encoder.TimeHeldMs() > 1000.0f)
        {
            picker::run(hw);
            page_reset();   /* the knobs may have moved meanwhile */
        }

        /* Encoder turns pages; every page change re-arms pickup. */
        int inc = hw.encoder.Increment();
        if(inc)
        {
            g_page = (g_page + inc) & 3;
            page_reset();
        }

        /* Knobs -> CC, with pickup. */
        for(int k = 0; k < 4; k++)
        {
            int idx = g_page * 4 + k;
            int raw = (int)(hw.GetKnobValue((DaisyPatch::Ctrl)k) * 127.0f + 0.5f);

            if(!g_live[k])
            {
                int cur = g_cc[idx];
                /* Unknown current value (nothing sent yet on this param):
                 * wait for the knob to move before taking over. */
                if(cur < 0)
                {
                    if(abs(raw - g_knob_at[k]) > PICKUP_SLOP) g_live[k] = true;
                }
                else if((g_knob_at[k] <= cur && raw >= cur)
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

        /* External MIDI CC drives the same 16 params directly. */
        hw.midi.Listen();
        while(hw.midi.HasEvents())
        {
            MidiEvent ev = hw.midi.PopEvent();
            if(ev.type == ControlChange)
            {
                uint8_t msg[3] = { (uint8_t)(0xB0 | (ev.channel & 0x0F)),
                                   (uint8_t)ev.data[0],
                                   (uint8_t)ev.data[1] };
                belt_on_midi(B, msg, 3, MOVE_MIDI_SOURCE_EXTERNAL);
                int idx = (int)ev.data[0] - CC_BASE;
                if(idx >= 0 && idx < 16) g_cc[idx] = ev.data[1];
            }
        }

        /* CV and gate track the voice, so they run every loop pass rather
         * than at the display's 20 Hz — a gate that lagged 50 ms would be
         * useless for triggering an envelope. */
        update_cv_outs();

        /* ~20 Hz is plenty for a readout and keeps SPI off the audio ISR's back. */
        uint32_t now = System::GetNow();
        if(now - last_draw >= 50u)
        {
            draw();
            last_draw = now;
        }
    }
}
