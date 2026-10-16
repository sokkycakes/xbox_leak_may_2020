# FFCC's GBA client on the 3DS. Run ../setup.py first (it puts the client's
# source and graphics in ../build/cli). Needs devkitARM and libctru
# (DEVKITPRO, as devkitPro's installer sets it).
#
#   make          ffcc-gba.3dsx (Homebrew Launcher)
#   make elf      ffcc-gba.elf only

DEVKITPRO	?= /opt/devkitpro
DEVKITARM	?= $(DEVKITPRO)/devkitARM
CTRULIB		?= $(DEVKITPRO)/libctru
PREFIX		:= $(DEVKITARM)/bin/arm-none-eabi-
CC		:= $(PREFIX)gcc
TOOLS		:= $(DEVKITPRO)/tools/bin

NAME		:= ffcc-gba
APP_TITLE	:= FFCC GBA link
APP_DESC	:= Sion Link
APP_AUTHOR	:= Sion

CLI		:= ../build/cli
REGION		:= $(shell cat ../build/region 2>/dev/null || echo GCCE01)
BUILD		:= build

ARCH		:= -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
INCLUDES	:= -Isource -I.. -I../../common -I../../dslib -I$(CLI) -I$(CTRULIB)/include
# The client is 2006 GBA code: quiet its warnings, and it reads one type
# through another
CLIENT_FLAGS	:= -DPORT_CTR -DVERSION_$(REGION) -fno-strict-aliasing -w
CFLAGS		:= -O2 -g -mword-relocations -ffunction-sections $(ARCH) -D__3DS__ $(INCLUDES) $(EXTRA_CFLAGS)
LDFLAGS		:= -specs=3dsx.specs -g $(ARCH) -L$(CTRULIB)/lib -Wl,-Map,$(BUILD)/$(NAME).map

PORT_SRC	:= source/port.c source/gbappu.c ../../ctr/sl3ds.c
CLI_SRC		:= $(wildcard $(CLI)/*.c)
CLI_ASM		:= $(wildcard $(CLI)/*.s)
OBJS		:= $(addprefix $(BUILD)/,$(notdir $(PORT_SRC:.c=.o))) \
		   $(addprefix $(BUILD)/cli_,$(notdir $(CLI_SRC:.c=.o) $(CLI_ASM:.s=.o)))

.PHONY: cia all elf clean
all: $(NAME).3dsx
elf: $(NAME).elf

$(CLI)/global.h:
	@echo "Run ../setup.py first (--disc FFCC.iso, --bin ffcc_cli.bin or --stub)"; false

$(BUILD):
	@mkdir -p $@

$(BUILD)/%.o: source/%.c $(CLI)/global.h | $(BUILD)
	$(CC) $(CFLAGS) -Wall -c $< -o $@

$(BUILD)/sl3ds.o: ../../ctr/sl3ds.c | $(BUILD)
	$(CC) $(CFLAGS) -Wall -c $< -o $@

$(BUILD)/cli_%.o: $(CLI)/%.c $(CLI)/global.h | $(BUILD)
	$(CC) $(CFLAGS) $(CLIENT_FLAGS) -c $< -o $@

$(BUILD)/cli_%.o: $(CLI)/%.s | $(BUILD)
	$(CC) $(ARCH) -x assembler-with-cpp -Wa,-I$(CLI) -c $< -o $@

$(NAME).elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ -lctru -lm -o $@

$(BUILD)/$(NAME).smdh: | $(BUILD)
	$(TOOLS)/smdhtool --create "$(APP_TITLE)" "$(APP_DESC)" "$(APP_AUTHOR)" $(CTRULIB)/default_icon.png $@

$(NAME).3dsx: $(NAME).elf $(BUILD)/$(NAME).smdh
	$(TOOLS)/3dsxtool $< $@ --smdh=$(BUILD)/$(NAME).smdh

# The Download Play child (cia/ffcc-gba.rsf). makerom comes from
# github.com/3DSGuy/Project_CTR; it isn't part of devkitPro.
MAKEROM	?= makerom

cia: $(NAME).cia

$(NAME).cia: $(NAME).elf $(BUILD)/$(NAME).smdh cia/$(NAME).rsf
	$(MAKEROM) -f cia -o $@ -elf $< -rsf cia/$(NAME).rsf -icon $(BUILD)/$(NAME).smdh -exefslogo -target t

clean:
	rm -rf $(BUILD) $(NAME).elf $(NAME).3dsx $(NAME).cia
