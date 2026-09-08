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

# BOOT_NONE, and unlike smack-versio that is not a close call.
#
# Measured 2026-09-08 on the first working build:
#   BOOT_NONE   FLASH 77808 B / 131072 B = 59.36%  (53264 B free)
#   BOOT_SRAM   .bin  77656 B
#
# smack-versio had to take BOOT_SRAM because its image was ~137 KB against the
# STM32H750's 128 KB of internal flash. Belt is far under: belt_core is 796
# lines of self-contained DSP, and this shim deliberately pulls in no USB
# serial logger — which was 7.1 KB of smack-versio's overrun on its own.
#
# The practical consequence is the whole reason to prefer it: BOOT_NONE flashes
# with the STM32 ROM DFU that is baked into the silicon. No Daisy bootloader in
# internal flash, no QSPI write (ST ROM DFU cannot write QSPI anyway), and no
# ~2000 ms bootloader DFU window to race — hold BOOT, tap RESET, and the device
# sits in DFU indefinitely.
#
# If a future feature pushes this over 131072 B, switch to BOOT_SRAM and follow
# smack-versio's FLASHING.md.
APP_TYPE ?= BOOT_NONE

CPP_SOURCES = belt_patch.cpp

# belt_core_patch.c #includes vendor/belt_core.c with calloc/free redirected
# to the bump allocator. Never list vendor/belt_core.c here as well — that
# would define every engine symbol twice.
C_SOURCES = patch_alloc.c belt_core_patch.c

# -Os to start; belt_core is float-heavy pitch tracking, so if the YIN pass
# turns out to be the CPU ceiling, -O2 is the first thing to try. Do NOT add
# -ffast-math: the formant and harmony paths depend on float ordering.
OPT = -Os

# NOTE: no `-u _printf_float` here, deliberately. That flag is needed whenever
# newlib-nano has to format a %f (it silently prints nothing otherwise — see
# reference_newlib_nano_printf_float). This firmware only ever prints strings
# returned by belt_get_param, so it is not needed. If you add a %f to the
# display code, you must add the flag or the field will render empty.

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
