# Out-of-tree module, built against the Sion's kernel tree:
#   make KDIR=/path/to/linux-6.18.7 (configured like the Sion, modules_prepare done)
obj-m := iwl_ratefix.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
