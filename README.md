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

Both ship the same chain UI and engine.

## Install

Via schwung-manager → Install Custom Module → GitHub repo:

- `belt`: `timncox/schwung-belt`
- `belt-in`: `timncox/schwung-belt-in`

Or upload the tarballs from the [releases](../../releases).

**Feedback warning:** singing into the Move's mic while its speaker is on
will howl. Belt In auto-mutes in that state (Monitor pad overrides) —
use headphones or line out.

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
