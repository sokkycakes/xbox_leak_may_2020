################################################################################
#
# box86
#
################################################################################

BOX86_VERSION = 7dec0819b5f212910cc7ff030f5161742add121c
BOX86_SITE = https://github.com/ptitSeb/box86.git
BOX86_SITE_METHOD = git
BOX86_LICENSE = MIT
BOX86_LICENSE_FILES = LICENSE
BOX86_DEPENDENCIES = host-python3
# RPI3 turns on the ARM dynarec tuned for the Pi 3's Cortex-A53.
BOX86_CONF_OPTS = -DRPI3=1 -DNOGIT=1

define BOX86_NATIVE_RENDERER_WRAPPER
	$(HOST_DIR)/bin/python3 $(BR2_EXTERNAL_BOOTANI_PATH)/../../xbcompat/tools/box86-renderer/install.py $(@D)
endef
BOX86_PRE_CONFIGURE_HOOKS += BOX86_NATIVE_RENDERER_WRAPPER

define BOX86_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/box86 $(TARGET_DIR)/usr/bin/box86
endef

$(eval $(cmake-package))
