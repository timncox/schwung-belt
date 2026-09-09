# Belt — Daisy Patch firmware build.
#
#   make -C firmware              build belt_patch.bin
#   make -C firmware program-dfu  flash over USB
#
# Requires libDaisy checked out and built:
#   git clone --recurse-submodules https://github.com/electro-smith/libDaisy \
#       ~/tim-os/daisy-sdk/libDaisy && make -C ~/tim-os/daisy-sdk/libDaisy
#
# DaisySP is deliberately NOT used — belt_core is self-contained DSP.

TARGET = belt_patch

LIBDAISY_DIR ?= $(HOME)/tim-os/daisy-sdk/libDaisy

# BOOT_SRAM since 2026-09-09 — for the module picker, not for size.
#
# Belt fits internal flash with room to spare (94816 B of 131072 B, 72%), and
# the first cut was BOOT_NONE for exactly that reason: it flashed with the
# STM32 ROM DFU baked into the silicon — no bootloader, no QSPI write, no
# ~2000 ms window to race.
#
# What changed: all three Patch ports can now switch firmware from the front
# panel (module_picker.cpp). That works by writing the chosen .bin from the SD
# card into the Daisy bootloader's QSPI app slot and resetting into the
# bootloader, which needs the bootloader in internal flash and every app built
# to run under it. BOOT_SRAM runs from the 480 KB AXI SRAM at internal-flash
# speed; Belt uses a fifth of it.
#
# Cost: the bootloader installed once (make program-boot, from ST ROM DFU).
# USB flashing afterwards has to hit the bootloader's ~2000 ms window — but the
# point is that USB is no longer the path: put the .bin in /modules on the card
# and pick it on the screen. See module_picker.h.
APP_TYPE ?= BOOT_SRAM

# FatFS is not in libdaisy.a; this compiles it into the app for the picker.
USE_FATFS = 1

CPP_SOURCES = belt_patch.cpp module_picker.cpp

# belt_core_patch.c #includes vendor/belt_core.c with calloc/free redirected
# to the bump allocator. Never list vendor/belt_core.c here as well — that
# would define every engine symbol twice.
C_SOURCES = patch_alloc.c belt_core_patch.c

# -Os to start; belt_core is float-heavy pitch tracking, so if the YIN pass
# turns out to be the CPU ceiling, -O2 is the first thing to try. Do NOT add
# -ffast-math: the formant and harmony paths depend on float ordering.
OPT = -Os

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# REQUIRED. newlib-nano declares _printf_float as a weak reference that nothing
# defines under --specs=nano.specs; unresolved, vfprintf skips the conversion
# entirely — no error, no warning, no truncation marker, and atoi() then sees 0.
#
# It is easy to talk yourself out of this flag by auditing your OWN printf
# calls, which is what happened here: this shim formats only %s. But the two
# values it displays come from belt_get_param, and the ENGINE formats them:
#
#   belt_core.c:783   snprintf(buf, n, "%.2f", (double)b->note_inst)  detected_note
#   belt_core.c:785   snprintf(buf, n, "%.2f", (double)b->f_inst)     detected_freq
#
# Without the flag the pitch readout renders empty on hardware while every
# host test stays green, because glibc formats %f perfectly. That exact bug
# cost hours on smack-versio's LIVE mode (play_frame, "%.0f" -> "" -> 0).
#
# Applied after the include so it appends to libDaisy's LDFLAGS rather than
# replacing them. Costs ~4.5 KB. Verify on the built ELF:
#   arm-none-eabi-nm build/belt_patch.elf | grep _printf_float
# Empty output means every %f in the image is silently producing nothing.
LDFLAGS += -u _printf_float
