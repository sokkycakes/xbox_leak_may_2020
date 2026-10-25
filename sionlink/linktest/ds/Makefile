# Link Test DS client: the Sion's linktest game sends it to every DS that
# links (Download Play). Uses the Sion Link app's DSWifi build.
BLOCKSDS	?= /opt/wonderful/thirdparty/blocksds/core
export BLOCKSDS

NAME		:= linktest
GAME_TITLE	:= Link Test
GAME_SUBTITLE	:= Sion Link
GAME_AUTHOR	:= Sion

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9arm7/Makefile

DSWIFI		:= ../../ds/build/dswifi

$(DSWIFI)/lib/libdswifi7.a:
	$(V)$(MAKE) -C ../../ds build/dswifi/lib/libdswifi7.a

arm9 arm7: $(DSWIFI)/lib/libdswifi7.a
