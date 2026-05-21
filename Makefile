obj-m += bnkfs.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

CXX ?= g++
CXXFLAGS ?= -O2 -std=c++20 -Wall -Wextra -pedantic

.PHONY: all module user clean

all: module user

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

user: bnkfs_tool

bnkfs_tool: user_tool.cpp bnkfs_ioctl.h
	$(CXX) $(CXXFLAGS) user_tool.cpp -o bnkfs_tool

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f bnkfs_tool
