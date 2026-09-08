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

Builds `BOOT_NONE` by default, which fits internal flash with room to spare
(77,808 B of 131,072 B — 59% used). That means it flashes with the **STM32 ROM
DFU baked into the silicon**: hold **BOOT**, tap **RESET**, release BOOT, and
the board sits in DFU indefinitely as `0483:df11`. No Daisy bootloader, no QSPI
write, and none of the ~2000 ms bootloader DFU window smack-versio has to race.

This is the opposite of smack-versio's situation, where a ~137 KB image forced
`BOOT_SRAM` and a bootloader install.

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
| Encoder | Select page: `TUNE` / `HARM A` / `HARM B` / `VOICE` |
| Knobs 1–4 | The four params on the current page (pickup required) |
| Audio In 1/2 | Voice in |
| Audio Out 1/2 | Processed out |
| Audio Out 3/4 | Silent — see below |
| MIDI In | CC 20–35 drive all 16 params directly |

Display shows the page, the detected note (`?` when unvoiced), and each param's
value.

## Known gaps

- **Outs 3/4 are silent.** The interesting thing a 4-out module offers Belt is
  four discrete harmony outputs, one per voice. `belt_process` mixes its seven
  voices to stereo internally, so that is a core change, not a shim change.
- **No persistence.** The Patch has an SD card and the Seed has QSPI; neither is
  wired up. Params reset to `belt_create()` defaults on power-up.
- **Gate inputs unused.**
- **Nothing has been heard on hardware.** The build is clean and the memory map
  is measured; that is all that is known.
