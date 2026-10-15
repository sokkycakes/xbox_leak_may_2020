BLOCKSDS	?= /opt/wonderful/thirdparty/blocksds/core
export BLOCKSDS

NAME		:= sionlink
GAME_TITLE	:= Sion Link
GAME_SUBTITLE	:= Link to a Sion
GAME_AUTHOR	:= Sion

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9arm7/Makefile

# Our own DSWifi build (dswifi/sionlink.patch), used by both CPUs
DSWIFI		:= build/dswifi

$(DSWIFI)/lib/libdswifi7.a: dswifi/BASE dswifi/sionlink.patch dswifi/build.sh
	@echo "  DSWIFI  $(DSWIFI)"
	$(V)sh dswifi/build.sh $(CURDIR)/$(DSWIFI)

arm9 arm7: $(DSWIFI)/lib/libdswifi7.a
