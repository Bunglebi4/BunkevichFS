# Сборка ядерного модуля и userspace-утилиты для BunkevichFS.
#
# Использование:
#   make            — собрать модуль (bnkfs.ko) и userspace-утилиту (user_tool)
#   make module     — только модуль
#   make user       — только userspace-программу
#   make clean      — почистить артефакты

obj-m := bnkfs.o

KDIR ?= /home/1nflutorm/Downloads/Linux/linux-6.12.90
PWD  := $(shell pwd)

CXX      ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17

.PHONY: all module user clean

all: module user

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

user: user_tool

user_tool: user_tool.cpp bnkfs_ioctl.h
	$(CXX) $(CXXFLAGS) user_tool.cpp -o user_tool

clean:
	-$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f user_tool
