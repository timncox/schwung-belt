#!/usr/bin/env bash
# Cross-compile Belt for the Ableton Move (aarch64 Linux) via Docker,
# mirroring schwung's own toolchain (debian:bookworm + gcc-aarch64-linux-gnu)
# so the .so links against the same glibc as the rest of the ecosystem.
#
# Outputs:
#   build/modules/audio_fx/belt/belt.so + module.json      (chain + master FX)
#   build/modules/sound_generators/belt-in/dsp.so + module.json
#   build/belt-module.tar.gz, build/belt-in-module.tar.gz
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=belt-build
CFLAGS="-O3 -g -shared -fPIC -Wall -Wextra -Iinclude"

if ! docker image inspect "$IMAGE" &>/dev/null; then
    docker build -t "$IMAGE" - <<'EOF'
FROM debian:bookworm
RUN apt-get update && apt-get install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu file && rm -rf /var/lib/apt/lists/*
EOF
fi

mkdir -p build/modules/audio_fx/belt build/modules/sound_generators/belt-in

cp modules/audio_fx/belt/module.json src/ui_chain.js build/modules/audio_fx/belt/
cp src/help.json build/modules/audio_fx/belt/help.json
cp modules/sound_generators/belt-in/module.json src/ui_chain.js build/modules/sound_generators/belt-in/
cp src/help_belt_in.json build/modules/sound_generators/belt-in/help.json

# Compile AND tar inside the container: macOS bsdtar embeds AppleDouble
# (._*) xattr entries that Linux tar extracts as real files — the schwung
# installer then reads entries[0] = "._belt" and fails with "No module.json
# found in tarball". GNU tar in the container produces clean archives.
docker run --rm -v "$PWD":/w -w /w "$IMAGE" bash -c "
    set -e
    aarch64-linux-gnu-gcc $CFLAGS src/belt_core.c src/belt_fx.c \
        -o build/modules/audio_fx/belt/belt.so -lm
    aarch64-linux-gnu-gcc $CFLAGS src/belt_core.c src/belt_gen.c \
        -o build/modules/sound_generators/belt-in/dsp.so -lm
    file build/modules/audio_fx/belt/belt.so build/modules/sound_generators/belt-in/dsp.so
    tar --owner=0 --group=0 -czf build/belt-module.tar.gz -C build/modules/audio_fx belt
    tar --owner=0 --group=0 -czf build/belt-in-module.tar.gz -C build/modules/sound_generators belt-in
    echo 'tarball contents:'
    tar -tzf build/belt-module.tar.gz
    tar -tzf build/belt-in-module.tar.gz
"

echo "Built: build/belt-module.tar.gz, build/belt-in-module.tar.gz"
