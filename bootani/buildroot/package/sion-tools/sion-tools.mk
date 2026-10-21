################################################################################
#
# sion-tools
#
################################################################################

SION_TOOLS_DEPENDENCIES = openssh avahi wpa_supplicant efibootmgr e2fsprogs util-linux

define SION_TOOLS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(SION_TOOLS_PKGDIR)/sion $(TARGET_DIR)/usr/sbin/sion
	$(INSTALL) -D -m 0644 $(SION_TOOLS_PKGDIR)/sion-lib.sh $(TARGET_DIR)/usr/lib/sion/sion-lib.sh
	$(INSTALL) -D -m 0755 $(SION_TOOLS_PKGDIR)/sion-netd $(TARGET_DIR)/usr/libexec/sion-netd
	$(INSTALL) -D -m 0600 $(SION_TOOLS_PKGDIR)/sshd_config $(TARGET_DIR)/etc/ssh/sshd_config
	$(INSTALL) -D -m 0644 $(SION_TOOLS_PKGDIR)/sion.conf $(TARGET_DIR)/usr/share/sion/sion.conf
	$(INSTALL) -D -m 0644 $(SION_TOOLS_PKGDIR)/README.txt $(TARGET_DIR)/usr/share/sion/README.txt
	echo "$(SION_TOOLS_IMAGE_VERSION)" > $(TARGET_DIR)/etc/sion-version
endef

# Shown by "sion status": when and from what the image was built.
SION_TOOLS_IMAGE_VERSION = $(shell date -u +%Y-%m-%dT%H:%MZ) $(shell git -C $(BR2_EXTERNAL_BOOTANI_PATH) rev-parse --short HEAD 2>/dev/null)

define SION_TOOLS_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(SION_TOOLS_PKGDIR)/S03sion $(TARGET_DIR)/etc/init.d/S03sion
	$(INSTALL) -D -m 0755 $(SION_TOOLS_PKGDIR)/S41sion-net $(TARGET_DIR)/etc/init.d/S41sion-net
endef

$(eval $(generic-package))
