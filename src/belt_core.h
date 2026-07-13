/*
 * Belt — live vocal processor: pitch correction (autotune) + diatonic
 * backing-vocal harmonies + doubler + formant shift.
 *
 * Shared engine used by both builds:
 *   - belt_fx.c   audio_fx_api_v2 wrapper (chain slots + Master FX slots)
 *   - belt_gen.c  plugin_api_v2 wrapper (sound_generator reading hardware input)
 *
 * DSP model: one YIN pitch analysis (decimated to 22.05 kHz) drives a train
 * of pitch marks; every output voice is a TD-PSOLA grain stream reading the
 * same marks — 1 corrected lead, 4 diatonic harmony voices, 2 doubler
 * streams — so N voices cost barely more than one analysis. Formant control
 * = grain-content resampling, independent of the pitch ratio (mark spacing).
 * Fixed latency BELT_LATENCY samples; the dry path is delay-matched so the
 * wet/dry correction blend stays phase-coherent.
 *
 * The render path is non-allocating and non-blocking per schwung's realtime
 * rules; all buffers live inside belt_t, allocated in belt_create().
 */
#ifndef BELT_CORE_H
#define BELT_CORE_H

#include <stdint.h>
#include "plugin_api_v1.h"

#define BELT_SR        44100
#define BELT_LATENCY   1152          /* fixed voice latency, samples (~26 ms) */
#define BELT_FMIN      85.0f         /* lowest tracked vocal pitch (F2-ish) */
#define BELT_FMAX      1000.0f
#define BELT_HARMONIES 4
#define BELT_VOICES    7             /* lead + 4 harmonies + 2 doubler */

typedef struct belt belt_t;

belt_t *belt_create(const host_api_v1_t *host);
void    belt_destroy(belt_t *b);

/* Process one block, stereo interleaved int16. in and out may alias. */
void belt_process(belt_t *b, const int16_t *in, int16_t *out, int frames);

void belt_on_midi(belt_t *b, const uint8_t *msg, int len, int source);
void belt_set_param(belt_t *b, const char *key, const char *val);
int  belt_get_param(belt_t *b, const char *key, char *buf, int buf_len);

#endif /* BELT_CORE_H */
