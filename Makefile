obj-m += bunglefs.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all: module cli

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

cli: cli.c bunglefs.h
	$(CC) -O2 -Wall -Wextra -o bunglefs_cli cli.c

clean:
	-$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f bunglefs_cli

.PHONY: all module cli clean
