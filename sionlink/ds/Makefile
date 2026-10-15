BLOCKSDS	?= /opt/wonderful/thirdparty/blocksds/core

NAME		:= sionlink
GAME_TITLE	:= Sion Link
GAME_SUBTITLE	:= Link to a Sion
GAME_AUTHOR	:= Sion

INCLUDEDIRS	:= ../common

ARM7ELF		:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi.elf
LIBS		:= -ldswifi9_noip -lnds9
LIBDIRS		:= $(BLOCKSDS)/libs/dswifi \
		   $(BLOCKSDS)/libs/libnds

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
