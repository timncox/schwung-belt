CC ?= cc
CFLAGS = -O2 -g -Wall -Wextra -Iinclude

.PHONY: test arm clean

test: build/host_sim
	./build/host_sim

build/host_sim: src/belt_core.c src/belt_core.h test/host_sim.c
	@mkdir -p build
	$(CC) $(CFLAGS) src/belt_core.c test/host_sim.c -o $@ -lm

arm:
	./scripts/build.sh

clean:
	rm -rf build
