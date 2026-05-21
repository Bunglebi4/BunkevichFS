.PHONY: all kernel user clean

all: kernel user

kernel:
	$(MAKE) -C kernel

user:
	$(MAKE) -C user

clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C user clean
