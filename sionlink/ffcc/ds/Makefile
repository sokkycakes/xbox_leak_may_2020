# FFCC's GBA client on the DS. Run ../setup.py first (it puts the client's
# source and graphics in ../build/cli).
BLOCKSDS	?= /opt/wonderful/thirdparty/blocksds/core
export BLOCKSDS

NAME		:= ffcc-gba
GAME_TITLE	:= FFCC GBA link
GAME_SUBTITLE	:= Sion Link
GAME_AUTHOR	:= Sion

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9arm7/Makefile

# DS mode only, like the launcher: the launcher boots only the DS-mode
# binaries, so a DSi or 3DS must never start this in DSi mode, where it
# would look for DSi-only code that was never loaded.
NDSTOOL_ARGS	+= -uc 0

DSWIFI		:= ../../ds/build/dswifi

$(DSWIFI)/lib/libdswifi7.a:
	$(V)$(MAKE) -C ../../ds build/dswifi/lib/libdswifi7.a

../build/cli/global.h:
	@echo "Run ../setup.py first (--disc FFCC.iso, or --stub)"; false

arm9 arm7: $(DSWIFI)/lib/libdswifi7.a ../build/cli/global.h
