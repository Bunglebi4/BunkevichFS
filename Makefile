obj-m := bunglefs.o

KDIR ?= /home/1nflutorm/Downloads/Linux/linux-6.12.90
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
