# Сборка ядерного модуля BunkevichFS.
# По умолчанию строится против /lib/modules/$(uname -r)/build.
# Для других ядер задавайте KDIR=<путь до build-дерева>.

obj-m       += bnkfs.o
bnkfs-y     := super.o inode.o file.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

ccflags-y := -Wall

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
