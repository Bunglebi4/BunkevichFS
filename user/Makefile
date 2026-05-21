CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=gnu11
LDFLAGS ?=

BINS := bnkfs_cli bnkfs_test

.PHONY: all clean

all: $(BINS)

bnkfs_cli: bnkfs_cli.c ../kernel/bnkfs.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bnkfs_test: bnkfs_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BINS)
