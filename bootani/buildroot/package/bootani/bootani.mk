################################################################################
#
# bootani
#
################################################################################

# The sources are the bootani/ directory this external tree lives in.
BOOTANI_VERSION = local
BOOTANI_SITE = $(BR2_EXTERNAL_BOOTANI_PATH)/..
BOOTANI_SITE_METHOD = local
BOOTANI_LICENSE = Proprietary (Microsoft/Pipeworks source)
BOOTANI_DEPENDENCIES = host-pkgconf libdrm libegl libgbm alsa-lib

BOOTANI_CONF_OPTS = \
	-DBOOTANI_WITH_SDL2=OFF \
	-DBOOTANI_WITH_EGL=ON \
	-DBOOTANI_WITH_KMS=ON \
	-DBOOTANI_WITH_ALSA=ON \
	-DBOOTSOUND_BUILD_WAV_TOOL=OFF

define BOOTANI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(BOOTANI_BUILDDIR)/bootani $(TARGET_DIR)/usr/bin/bootani
endef

ifeq ($(BR2_PACKAGE_BOOTANI_START_AT_BOOT),y)
define BOOTANI_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(BOOTANI_PKGDIR)/S00bootani $(TARGET_DIR)/etc/init.d/S00bootani
	echo 'BOOTANI_ARGS="$(call qstrip,$(BR2_PACKAGE_BOOTANI_ARGS))"' > $(TARGET_DIR)/etc/default/bootani
endef
endif

$(eval $(cmake-package))
