# Belt — Daisy Patch firmware

Belt's engine (`src/belt_core.c`) ported to the Electrosmith **Daisy Patch**,
a 20 HP Eurorack module built on the Daisy Seed. This directory is the Daisy
shim; it is the counterpart of `src/belt_fx.c`, which is the Move shim. The
engine is vendored, not forked.

## Build

```sh
make -C firmware                 # -> firmware/build/belt_patch.bin
make -C firmware program-dfu     # flash over USB
```

Needs libDaisy built at `~/tim-os/daisy-sdk/libDaisy` (override with
`LIBDAISY_DIR`). DaisySP is not used.

## Flashing

Builds `BOOT_SRAM`. Belt fits internal flash with room to spare (94,816 B of
131,072 B), and the first cut was `BOOT_NONE` for exactly that reason: it
flashed with the STM32 ROM DFU baked into the silicon, no bootloader, no
~2000 ms window to race. What changed is the module picker below, which needs
every app to run under the Daisy bootloader.

So, once: board in ST ROM DFU (hold **BOOT**, tap **RESET**, release BOOT),
then `make -C firmware program-boot` installs the bootloader. After that
`make -C firmware program-dfu` has to hit the bootloader's ~2 s DFU window on
power-up — or, simpler, the `.bin` goes on the card.

## Switching modules from the panel

All three Patch ports share `module_picker.cpp`. The SD card is a module
library: put `belt.bin`, `smack.bin` and `mark.bin` in a `modules` folder on a
FAT32 card, leave the card in the slot, and switch from the screen:

1. Hold the encoder for a second. The list appears: `back`, then every `.bin`
   in `/modules`. (Belt has no other press gesture; Smack and Mark reach the
   same screen from a `mods` menu item.)
2. Turn to choose, press to load. The file is written into the Daisy
   bootloader's QSPI app slot (the same place a USB flash writes), verified
   byte for byte, and the module resets into the bootloader, which boots it.
   A few seconds; the card never leaves the slot.

Keep `.bin` files **out of the card's root**: the bootloader itself flashes
the first root `.bin` it finds at every power-up, a cruder mechanism that would
fight the picker.

If a load fails the screen says why (`no card`, `no folder`, `flash build` for
an image built for internal flash, `verify failed`) and the running module
carries on. A failed write does leave the QSPI slot invalid until the next
successful load, so a power cycle in that state lands in the bootloader
waiting for USB — nothing is lost, it just needs a `make program-dfu`.

A switch is a reset: whatever is playing stops.

## Port decisions

**48 kHz.** `belt_core` is written for a compile-time `BELT_SR`, and every
period bound derives from it — `T_MIN`/`T_MAX` from `BELT_FMIN`/`BELT_FMAX`,
`YIN_SR`, the instantaneous period. libDaisy's `SaiHandle` offers only
`SAI_48KHZ`, so `vendor/belt_core.h` is rebased 44100 → 48000. The one raw
sample count, `BELT_LATENCY`, is scaled 1152 → 1254 to preserve the ~26 ms
voice latency the alignment maths assumes.

**Not verified by ear.** This is the first thing to listen for.

**Controls via synthesised CC.** The engine already maps CC 20..35 onto all 16
params with a linear 0–127 scale into each param's own range. The four knobs
synthesise CC and call `belt_on_midi()`, so the shim never duplicates a range —
`formant` alone is −100..100 while the rest are 0..100 — and the knob path is
provably identical to the MIDI path. External MIDI CC works simultaneously.

**Knob pickup.** Four knobs address sixteen params across four pages, and the
Patch's knobs are absolute. A knob is inert until it crosses the value it is
taking over; the display marks un-picked-up knobs with `*`. Without this,
changing page would slam four params to wherever the pots sit — the trap that
made persistence impossible on the Versio panel.

**Memory.** Belt's working set is ~128 KB (rings measured in `patch_alloc.h`),
allocated once from a 192 KB bump pool. The pool is in SDRAM because nothing
faster fits: `.bss`/`.dtcmram_bss` → DTCMRAM (128 KB, ~74 KB free) and
`.sram1_bss` → RAM_D2_DMA (32 KB) both overflowed, and the 480 KB `SRAM` region
holds `.text`. If the YIN tracker turns out to be CPU-bound, move `in_ring`
(64 KB) and `yin_ring` (16 KB) alone into DTCMRAM.

## Panel

| Control | Function |
|---|---|
| Encoder turn | Select page: `TUNE` / `HARM A` / `HARM B` / `VOICE` |
| Encoder hold > 1 s | Module picker — switch firmware from the card |
| Knobs 1–4 | The four params on the current page (pickup required) |
| Audio In 1/2 | Voice in |
| Audio Out 1/2 | Processed out |
| Audio Out 3/4 | **Dry thru** — straight from the input, early by `BELT_LATENCY` |
| **CV Out 1** | **Detected pitch, 1V/oct, 0 V = C2 (65.406 Hz)** |
| **CV Out 2** | **First enabled harmony voice, 1V/oct on the same scale** (the corrected lead's target note when no harmony is on) |
| **Gate Out** | **High while the input is voiced** |
| MIDI In | CC 20–35 drive all 16 params directly |

Display shows the page, the detected note (`?` when unvoiced), and each param's
value.

## Pitch to CV

Belt already runs a YIN tracker, so the Patch's CV outputs turn it into
something no other module in the rack does: **sing into it and get 1V/oct plus
a gate.**

Belt's tracking range is `BELT_FMIN`–`BELT_FMAX`, 85–1000 Hz, which on this
scaling is 0.38 V to 3.93 V — comfortably inside the 0–5 V output span with no
clipping at either end of the vocal range.

**CV Out 2 is the harmony, not a knob.** It carries the first enabled harmony
voice's note as 1V/oct on the same scale, so an oscillator on CV 1 follows the
singer and one on CV 2 follows the harmony Belt chose for them — a two-voice
pitch-to-CV, both gated by the voice. With no harmony on it carries the
corrected lead's target note instead, which is the quantized version of CV 1.
(It used to echo `harm_level`, a knob position; the rack already has the knob.)

Both pitches are **held** when unvoiced rather than dropped to zero; a pitch CV
that collapsed between phrases would slam whatever it drives. The gate says
"this is a note"; the CVs just stay where they were.

This needs `-u _printf_float` in the link line, because `belt_get_param`
formats `detected_freq` and `harm_note` with `%.2f`. See the Makefile — it is
not optional and it is not obvious.

## Known gaps

- **Outs 3/4 are dry thru, not per-harmony outputs.** Four discrete harmony
  outs is the thing a 4-out module ought to offer Belt, but `belt_process`
  mixes its seven voices to stereo internally, so that is a core change, not a
  shim change.
- **No persistence.** The Patch has an SD card and the Seed has QSPI; neither is
  wired up. Params reset to `belt_create()` defaults on power-up.
- **Gate inputs unused.**
- **CV inputs unused — and with paged knobs they cannot be used.** libDaisy's
  `DaisyPatch` exposes four analog controls, not eight: each CV jack is summed
  with its knob before the ADC, so a cable in CV 1 would steer whatever param
  is on the current page and defeat pickup. Giving the CV inputs a job means
  fixing four params to the knobs and moving the other twelve to an encoder
  menu, as the Smack and Mark ports do. Which four is a design decision, not
  a port decision, so it is left open here.
- **Nothing has been heard on hardware.** The build is clean and the memory map
  is measured; that is all that is known.
