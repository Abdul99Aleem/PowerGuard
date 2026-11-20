# Standalone QNX SDP 8 testbench: does not modify root Momentics project.
QCC ?= qcc
ARCH ?= aarch64le
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -pthread
TARGETS = pgbench pgbench_ham
all: $(TARGETS)

pgbench: pgbench.c pgbench_protocol.h
	$(QCC) -Vgcc_nto$(ARCH) $(CFLAGS) -o $@ pgbench.c -pthread

pgbench_ham: pgbench_ham.c
	$(QCC) -Vgcc_nto$(ARCH) $(CFLAGS) -o $@ pgbench_ham.c -lham

clean:
	rm -f $(TARGETS)

.PHONY: all clean
