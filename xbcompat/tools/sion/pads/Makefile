# Controller drivers the Sion's kernel lacks, built out of tree from the
# kernel's own sources:
#   make KDIR=/path/to/linux-6.18.7 (configured like the Sion, modules_prepare done)
# xpad_leds.ko is xpad with CONFIG_JOYSTICK_XPAD_LEDS on and the driver
# renamed, so it can sit beside the built-in xpad (which has no LEDs). Xbox
# 360-style pads get their player number from it; 8BitDo pads send nothing
# until they do. hid-nintendo.ko drives Switch Pro-mode pads and adapters.
# Prebuilt copies for the Sion's 6.18.7 kernel are in prebuilt/.
KDIR ?= /lib/modules/$(shell uname -r)/build
BUILD = $(CURDIR)/build

all:
	mkdir -p $(BUILD)
	sed 's/\.name\t\t= "xpad",/.name\t\t= "xpad_leds",/' \
		$(KDIR)/drivers/input/joystick/xpad.c > $(BUILD)/xpad_leds.c
	grep -q '"xpad_leds"' $(BUILD)/xpad_leds.c
	cp $(KDIR)/drivers/hid/hid-nintendo.c $(KDIR)/drivers/hid/hid-ids.h $(BUILD)/
	printf 'obj-m := xpad_leds.o hid-nintendo.o\nCFLAGS_xpad_leds.o := -DCONFIG_JOYSTICK_XPAD_LEDS=1\nCFLAGS_hid-nintendo.o := -DCONFIG_NINTENDO_FF=1\n' > $(BUILD)/Kbuild
	$(MAKE) -C $(KDIR) M=$(BUILD) KBUILD_MODPOST_WARN=1 modules
	strip --strip-debug $(BUILD)/*.ko

clean:
	rm -rf $(BUILD)
