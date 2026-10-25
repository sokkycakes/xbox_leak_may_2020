# The Nintendo Wi-Fi USB Connector (0411:008b, a Ralink RT2570) runs on
# rt2500usb, which needs no firmware. The Sion's kernel leaves RT2X00 out,
# so the three modules are built out of tree from the kernel's own sources:
#   make KDIR=/path/to/linux-6.18.7 (configured like the Sion, modules_prepare done)
# rt2500usb-hwbeacon.patch keeps the chip from beaconing (it wedges the
# connector's USB sends; sionlinkd --beacon-if sends the beacons instead)
# and sets the receiver gain to 0x60, which stops false channel-busy alarms.
# Prebuilt copies for the Sion's 6.18.7 kernel are in prebuilt/.
KDIR ?= /lib/modules/$(shell uname -r)/build
SRC = $(KDIR)/drivers/net/wireless/ralink/rt2x00
BUILD = $(CURDIR)/build

all:
	mkdir -p $(BUILD)
	cp $(SRC)/*.c $(SRC)/*.h $(BUILD)/
	cd $(BUILD) && patch -p1 < $(CURDIR)/rt2500usb-hwbeacon.patch
	cp $(CURDIR)/Kbuild.in $(BUILD)/Kbuild
	$(MAKE) -C $(KDIR) M=$(BUILD) modules
	strip --strip-debug $(BUILD)/*.ko

clean:
	rm -rf $(BUILD)
