obj-m := bunglefs.o

KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

.PHONY: all module cli clean

all: module cli

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

cli: cli.cpp bunglefs.h
	$(CXX) -O2 -Wall -Wextra -std=c++17 -o bunglefs_cli cli.cpp

clean:
	-$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f bunglefs_cli
