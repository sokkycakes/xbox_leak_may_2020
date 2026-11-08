# Out-of-tree build: make KDIR=/path/to/linux-6.18.7 (configured, modules_prepare done)
obj-m += trigger6.o
KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
