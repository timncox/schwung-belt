---
status: active
last_touched: 2026-07-18
---

# Belt

Schwung module for the Ableton Move: live vocal processor. Autotune-style
pitch correction (retune speed 0 = hard-tune robot, higher = transparent),
up to 4 diatonic backing-vocal harmony voices, a stereo doubler, and a
formant knob — inspired by Eventide QVox, TC-Helicon VoiceLive, and
Antares Auto-Tune Artist. Named Belt because it makes you sound like you
can.

Schwung = charlesvestal/schwung, the Shadow-UI sidecar framework for the Move
(LD_PRELOAD shim + QuickJS shadow UI + native ARM DSP modules,
44.1 kHz / 128-frame int16 stereo blocks).

## Two builds, one core

- `src/belt_core.c` — all engine logic (YIN pitch detection, pitch marks,
  TD-PSOLA voices, correction/harmony targeting). Non-allocating render path.
- `src/belt_fx.c` → **belt** (`audio_fx`): works in chain slots AND Master
  FX slots. The .so MUST be named `belt.so` (the chain host loads
  `modules/audio_fx/<id>/<id>.so` without reading module.json). Exports
  `move_audio_fx_on_midi` via dlsym for future MIDI features (ducker
  pattern; struct on_midi field stays NULL for old-host ABI).
- `src/belt_gen.c` → **belt-in** (`sound_generator`, `audio_in: true`):
  standalone live mic/line/USB-C vocal processor reading the host mailbox
  input. Sets `hw_input=1` so the shared chain UI arms the feedback guard.

## DSP architecture

One analysis, many voices — the reason 7 voices are cheap:

1. **YIN** on 2x-decimated input (22.05 kHz), CMNDF + parabolic refine,
   80-1000 Hz, ~172 analyses/s. Tracking mode caps the lag search at 1.35x
   the last period (the long lags are the expensive part); a lost note
   falls back to a full-range pass. 3-point median kills octave blips.
2. **Pitch marks** free-run at the detected period; each mark remembers its
   period. All voices read the same mark train.
3. **TD-PSOLA voices**: causal Hann grains (2 periods long) fired at
   `T/ratio` spacing into a shared stereo accumulator. Grain content
   resampled by the formant factor g — pitch (spacing) and formant
   (content rate) are independent. Latency invariant: G*(1+g) <= L
   (L = BELT_LATENCY = 1152 samples, ~26 ms); fire_grain clamps g so very
   low notes trade formant-up range instead of reading unwritten input.
4. **Correction**: note_slow (~120 ms one-pole with jump-snap) is quantized
   to key+scale; retune speed smooths the offset; humanize re-adds the
   fast residue (vibrato) instead of flattening it; flex fades correction
   for singing far from any scale note. Harmonies walk scale degrees from
   the quantized note (chromatic scale uses fixed semitones), carry half
   the vibrato, and get per-voice static detune + slow wander scaled by
   humanize. Doubler = corrected lead +/- ~10 cents hard-panned.
5. Dry path is latency-matched; `wet` crossfades corrected vs dry lead
   (harmonies/doubler are additive on top). Unvoiced input: harmonies and
   doubler duck via the voicing envelope, correction offset freezes.

## Build / test / deploy

- `make test` — native compile of the core + `test/host_sim.c` (sine-based
  DSP assertions: detection accuracy, correction pull, harmony Goertzel
  probes, doubler decorrelation, formant, gates, clamps, state blob).
  No hardware needed.
- `make arm` (= `scripts/build.sh`) — Docker cross-compile for aarch64
  using debian:bookworm + gcc-aarch64-linux-gnu. Tars INSIDE the container
  (macOS bsdtar embeds AppleDouble ._* entries that break the installer).
- `scripts/deploy.sh` — scp both modules to `ableton@move.local` under
  `/data/UserData/schwung/modules/`, then rescan modules on-device.

`include/*.h` are vendored copies of schwung's stable ABI headers.

## Params

key 0-11, scale 0-8 (Chromatic/Major/Minor/HarmMin/Dorian/Mixo/MajPent/
MinPent/Blues), retune 0-100 (0 = instant), amount, flex, humanize,
harm1-4 (interval enum: Off/-Oct/-6th/-5th/-4th/-3rd/Unis/+3rd/+4th/+5th/
+6th/+Oct), harm_level, spread, double_amt, formant -100..100, wet.
`monitor` (0 mutes output; feedback guard; never preset-saved),
`hw_input` (set by gen wrapper), `status` = "note10:cents:voiced:mask"
(ONE UI poll per tick), `state` = JSON preset blob (bounded appends —
the smack snprintf OOB lesson is baked in + regression-tested).

## Chain UI (src/ui_chain.js, shipped in both tarballs)

Pads 68-71 = harmony voices (tap toggle, Shift+tap cycles interval),
76 = HARD punch (tap latch / hold momentary: retune 0 + amount 100),
73 = Monitor (belt-in only, mirrors the smack-in feedback guard).
Steps 1-12 = chromatic tuner strip (in-scale dim, detected note green when
within 25 cents, orange otherwise; press = set Key), steps 13-16 = voice
indicators. Knobs: Key, Scale, Retune, Amount, Harm, Dbl, Formant, Wet;
Shift page: Humanize, Flex, Spread, Monitor. QuickJS modules are strict
mode — audit for assigned-but-undeclared identifiers before shipping
(the smack punchPad lesson).

## Not yet verified on hardware

1. End-to-end smoke test: mic capture, pitch tracking on real voice,
   correction quality, harmony sound.
2. CPU headroom: bench says 65x realtime on an M-series Mac with all
   7 voices on (~10-15% of a Move A53 core, extrapolated — MEASURE).
3. Latency feel while monitoring (~26 ms engine latency).
4. Feedback guard behavior with Move speaker + built-in mic.
5. Chain UI: LED colors, tuner strip chase rate, HARD hold timing.

## Operation-manual site

https://timncox.github.io/schwung-belt/ — single-file `docs/index.html` on
main:/docs (GitHub Pages via gh api). Design: soul-revue-poster / 70s pedal
manual (cream paper, ink, burnt orange; Alfa Slab One + Karla + IBM Plex
Mono) — deliberately NOT the mark/smack dark family. Interactive pad map
(rows numbered from the TOP per [[feedback-move-pad-rows-top-down]]) +
tuner-strip explainer; pad numbers/colors/behaviors mirror ui_chain.js —
**update the page when the surface changes**. Accessibility: axe-core scan
clean (contrast fixes: small accent text uses --rust not --acc; pad labels
cream-on-darkened voice colors); skip link, landmarks, aria-live readout,
visible focus, reduced-motion support. Re-run an axe pass after edits.

## Release process (dedicated distributions, one source)

Source: this repo (timncox/schwung-belt). Dedicated THIN distribution repos:
`belt` = timncox/schwung-belt-fx and `belt-in` =
timncox/schwung-belt-voice. Each holds README + release.json + release
tarballs (the installer resolves one release.json per repo, so each catalog
ID needs its own repo).

To ship version X: bump both module.json versions + root release.json here,
`make arm`, commit/push, then publish `belt-module.tar.gz` to schwung-belt-fx
and `belt-in-module.tar.gz` to schwung-belt-voice under the same version tag.
Catalog PR to charlesvestal/schwung: only after hardware test (house rule).

## v1 limitations (deliberate, revisit)

- No MIDI-note harmony mode (sing + play target notes) — the wrapper
  already forwards MIDI; core ignores it. Top v2 candidate.
- No chord-follow from Move's clips (TC NaturalPlay style).
- Formant shift is grain-resampling (envelope+content together per grain),
  not LPC envelope warping — the classic pedal sound, not studio-grade.
- Harmony voices duck on unvoiced input rather than passing shifted
  consonants (backing singers drop their t's — sounds natural in practice).
- Analysis is mono (L+R downmix) — fine for vocals.
