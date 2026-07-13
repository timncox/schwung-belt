/*
 * Belt audio_fx build — loads in Signal Chain slots and Master FX slots
 * (shadow_master_fx_slot_load uses the same audio_fx plugins).
 *
 * NOTE: the chain host loads audio FX as modules/audio_fx/belt/belt.so
 * without consulting module.json, so this .so MUST be named belt.so.
 */
#include <stddef.h>

#include "audio_fx_api_v2.h"
#include "belt_core.h"

static const host_api_v1_t *g_host;

static void *fx_create(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    return belt_create(g_host);
}

static void fx_destroy(void *inst) { belt_destroy((belt_t *)inst); }

static void fx_process(void *inst, int16_t *audio_inout, int frames) {
    belt_process((belt_t *)inst, audio_inout, audio_inout, frames);
}

static void fx_set_param(void *inst, const char *key, const char *val) {
    belt_set_param((belt_t *)inst, key, val);
}

static int fx_get_param(void *inst, const char *key, char *buf, int buf_len) {
    return belt_get_param((belt_t *)inst, key, buf, buf_len);
}

static void fx_on_midi(void *inst, const uint8_t *msg, int len, int source) {
    belt_on_midi((belt_t *)inst, msg, len, source);
}

static audio_fx_api_v2_t api = {
    .api_version      = AUDIO_FX_API_VERSION_2,
    .create_instance  = fx_create,
    .destroy_instance = fx_destroy,
    .process_block    = fx_process,
    .set_param        = fx_set_param,
    .get_param        = fx_get_param,
    /* on_midi deliberately unset: the chain host ignores the struct field
     * and discovers MIDI via dlsym("move_audio_fx_on_midi") below; leaving
     * it NULL avoids ABI issues with older 6-field hosts (ducker pattern). */
};

/* The chain host looks this up via dlsym — Belt doesn't need clock today,
 * but exporting it keeps the MIDI path open (future MIDI-note harmony). */
__attribute__((visibility("default")))
void move_audio_fx_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    fx_on_midi(instance, msg, len, source);
}

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &api;
}
