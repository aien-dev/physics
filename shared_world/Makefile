# M20 Stage 1 shared-world host qualification build.
# Builds and runs the CPU-only protocol/authority qualification for
# OMEGA_SHARED_WORLD_V1. No GPU required. The GPU cross-processor campaign is a
# separate on-silicon step (see omega_shared_world_worker.*).
CC ?= gcc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -I.
OUT ?= build

.PHONY: all test clean
all: $(OUT)/test_shared_world_host

$(OUT):
	mkdir -p $(OUT)

$(OUT)/omega_shared_world.o: omega_shared_world.c omega_shared_world.h omega_shared_world_abi.h | $(OUT)
	$(CC) $(CFLAGS) -c omega_shared_world.c -o $@

$(OUT)/test_shared_world_host: test_shared_world_host.c $(OUT)/omega_shared_world.o
	$(CC) $(CFLAGS) test_shared_world_host.c $(OUT)/omega_shared_world.o -o $@ -lpthread

# Usage: make test           (default 2,000,000 sustained exchanges)
#        make test N=5000000
N ?= 2000000
test: $(OUT)/test_shared_world_host
	./$(OUT)/test_shared_world_host $(N)

clean:
	rm -rf $(OUT)
