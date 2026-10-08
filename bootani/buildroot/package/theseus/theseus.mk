################################################################################
#
# theseus
#
################################################################################

# MrMilenko/Theseus main as of 2026-08-16
THESEUS_VERSION = 5fc7ddddd03b4b5a7e6233769ad6a8fe46009965
THESEUS_SITE = https://github.com/MrMilenko/Theseus.git
THESEUS_SITE_METHOD = git
THESEUS_GIT_SUBMODULES = YES
THESEUS_LICENSE = GPL-3.0+
THESEUS_LICENSE_FILES = LICENSE LICENSE-THIRD-PARTY.md
THESEUS_DEPENDENCIES = host-pkgconf sdl2 sdl2_mixer mpv ffmpeg libcurl opus

THESEUS_BGFX_DIR = $(@D)/theseus/third-party/bgfx
THESEUS_BGFX_LIBS = $(THESEUS_BGFX_DIR)/.build/linux64_gcc/bin
THESEUS_OUT = $(@D)/out/desktop

# bgfx for OpenGL only: no Vulkan (and with it no X11), and GL 3.3 so the
# GLSL 150 shaders Theseus ships load.
define THESEUS_BUILD_BGFX
	cd $(THESEUS_BGFX_DIR) && ../bx/tools/bin/linux/genie --gcc=linux-gcc gmake
	$(TARGET_MAKE_ENV) $(MAKE) -C $(THESEUS_BGFX_DIR)/.build/projects/gmake-linux-gcc \
		bx bimg bgfx config=release64 \
		CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" AR="$(TARGET_AR)" \
		CFLAGS="-DBGFX_CONFIG_RENDERER_OPENGL=33"
endef

define THESEUS_BUILD_CMDS
	$(THESEUS_BUILD_BGFX)
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/build desktop \
		NO_MILKDROP=1 NO_VULKAN=1 \
		BUILDS_ROOT=$(@D)/out \
		BGFX_BUILD_DIR=$(THESEUS_BGFX_LIBS) \
		DESKTOP_CC="$(TARGET_CC)" DESKTOP_CXX="$(TARGET_CXX)" \
		SDL2_CFLAGS="`$(PKG_CONFIG_HOST_BINARY) --cflags sdl2`" \
		SDL2_LDFLAGS="`$(PKG_CONFIG_HOST_BINARY) --libs sdl2`"
endef

# One copy of the payload: the dashboard reads Data/shaders from beside the
# binary and everything else from $XDG_DATA_HOME/theseus. The init script
# points XDG_DATA_HOME at /opt so both are /opt/theseus, and nothing is
# copied into a second tree on first run. Only the GLSL shaders are used.
define THESEUS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(THESEUS_OUT)/theseus $(TARGET_DIR)/opt/theseus/theseus
	for d in Configs Data Library; do \
		rm -rf $(TARGET_DIR)/opt/theseus/$$d; \
		cp -a $(THESEUS_OUT)/$$d $(TARGET_DIR)/opt/theseus/$$d || exit 1; \
	done
	cd $(TARGET_DIR)/opt/theseus/Data/shaders && rm -rf dx11 essl metal spirv
endef

ifeq ($(BR2_PACKAGE_THESEUS_START_AT_BOOT),y)
define THESEUS_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(THESEUS_PKGDIR)/S99theseus $(TARGET_DIR)/etc/init.d/S99theseus
	$(INSTALL) -D -m 0755 $(THESEUS_PKGDIR)/theseus-session $(TARGET_DIR)/usr/libexec/theseus-session
endef
endif

$(eval $(generic-package))
