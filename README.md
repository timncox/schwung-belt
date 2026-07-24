# Belt

**Live vocal processor for the Ableton Move** (a [schwung](https://github.com/charlesvestal/schwung) module).

**📖 Operation manual: [timncox.github.io/schwung-belt](https://timncox.github.io/schwung-belt/)**

Autotune-style pitch correction, up to four diatonic backing-vocal
harmonies, a stereo doubler, and a formant knob — on the Move's own mic,
line-in, or USB-C input. Named Belt because it makes you sound like you can.

- **Retune** 0 = instant hard-tune robot, higher = transparent correction
- **Harmony voices 1-4** — scale-aware intervals (3rds, 5ths, octaves…),
  humanized with per-voice detune and stereo spread
- **Doubler** — instant thickness, hard-panned corrected doubles
- **Formant** — deeper or brighter without changing pitch
- **HARD pad** — momentary or latched T-Pain punch
- **Tuner strip** — steps 1-12 show your note (green = in tune); press to set the key
- **Flex / Humanize** — leave expressive slides alone, keep your vibrato

## Two modules

| id | type | where it runs |
|----|------|----------------|
| `belt` | `audio_fx` | Signal Chain slots + Master FX (processes whatever is upstream) |
| `belt-in` | `sound_generator` (audio_in) | standalone slot synth on the hardware input — the live-vocals build |

Both ship the same chain UI and engine. They are built from this canonical
repository and published together under one release tag.

## Install

In Schwung Manager, search the Module Store for Belt or Belt In. The store
resolves both entries from this repository's multi-module release manifest.
Schwung's Custom GitHub installer does not yet offer a module picker, so use
the dedicated compatibility repository for the form you want:

- `belt` Audio FX: `timncox/schwung-belt-fx`
- `belt-in` Voice: `timncox/schwung-belt-voice`

Those compatibility repositories mirror the archives built here; they do not
carry separate Belt source.

All source and issue tracking remain in this repository.

**Feedback warning:** singing into the Move's mic while its speaker is on
will howl. Belt In auto-mutes in that state (Monitor pad overrides) —
use headphones or line out.

## MIDI CC control

Every Belt parameter responds to MIDI CC from a controller plugged into the
Move's USB-A port (Launch Control, Faderfox, keyboard knobs — anything that
sends CC). The 0–127 CC value scales linearly across each parameter's range.

| CC | param | range | CC | param | range |
|----|-------|-------|----|-------|-------|
| 20 | key | C..B | 28 | harm 3 | off..interval |
| 21 | scale | 9 scales | 29 | harm 4 | off..interval |
| 22 | retune | 0–100 | 30 | harm level | 0–100 |
| 23 | amount | 0–100 | 31 | spread | 0–100 |
| 24 | flex | 0–100 | 32 | double | 0–100 |
| 25 | humanize | 0–100 | 33 | formant | -100–100 |
| 26 | harm 1 | off..interval | 34 | wet | 0–100 |
| 27 | harm 2 | off..interval | 35 | hard | ≥64 = on |

Channel notes:

- **`belt` in a chain or Master FX slot:** any MIDI channel works — the host
  broadcasts external CCs to audio FX regardless of the slot's receive
  channel.
- **`belt-in` (sound generator):** CCs follow note routing, so set your
  controller to the same MIDI channel the slot receives on (don't rely on
  auto channel mapping — it remaps notes, not CCs).

Move's own encoders are internal MIDI and never collide with this map.

## Build from source

```
make test    # native sim tests, no hardware needed
make arm     # Docker cross-compile + module tarballs (see scripts/build.sh)
```

## How it works

One YIN pitch analysis (decimated to 22.05 kHz) drives a shared train of
pitch marks; every voice — corrected lead, four harmonies, two doubler
streams — is a TD-PSOLA grain stream reading those same marks, so seven
voices cost barely more than one. Formant control resamples grain content
independently of the pitch ratio. ~26 ms fixed latency, dry path
delay-matched. Details in [CLAUDE.md](CLAUDE.md).

MIT licensed.
