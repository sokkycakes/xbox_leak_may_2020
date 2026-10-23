# Sion Link for the DS: one app for the live mirror (the DSpico's USB), the Sion's Wi-Fi
# link (Download Play, Sion games with DS players) and the DS as the Sion's pad.
# A recording at nitrofs/mirror.bin is replayed instead (see ../README.md); keep nitrofs/
# empty for the live app.
BLOCKSDS	?= /opt/wonderful/thirdparty/blocksds/core
export BLOCKSDS

NAME		:= mirrorplay
GAME_TITLE	:= Sion Link
GAME_SUBTITLE	:= Link to a Sion
GAME_AUTHOR	:= Sion
NITROFSDIR	:= nitrofs

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9arm7/Makefile

# DS mode only: on a DSi or a 3DS in DS mode the app must run as a plain DS, so the
# programs it receives and boots (all DS-mode) find the DS memory map, and Wi-Fi runs
# on the DS radio our DSWifi patch drives.
NDSTOOL_ARGS	+= -uc 0

# Our own DSWifi build (dswifi/sionlink.patch), used by both CPUs
DSWIFI		:= build/dswifi

$(DSWIFI)/lib/libdswifi7.a: dswifi/BASE dswifi/sionlink.patch dswifi/build.sh
	@echo "  DSWIFI  $(DSWIFI)"
	$(V)sh dswifi/build.sh $(CURDIR)/$(DSWIFI)

arm9 arm7: $(DSWIFI)/lib/libdswifi7.a
