################################################################################
#
# sion-tools
#
################################################################################

SION_TOOLS_DEPENDENCIES = openssh avahi wpa_supplicant efibootmgr e2fsprogs util-linux

define SION_TOOLS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(SION_TOOLS_PKGDIR)/sion $(TARGET_DIR)/usr/sbin/sion
	$(INSTALL) -D -m 0755 $(SION_TOOLS_PKGDIR)/sion-install-internal $(TARGET_DIR)/usr/sbin/sion-install-internal
	$(INSTALL) -D -m 0644 $(SION_TOOLS_PKGDIR)/sion-lib.sh $(TARGET_DIR)/usr/lib/sion/sion-lib.sh
	$(INSTALL) -D -m 0755 $(SION_TOOLS_PKGDIR)/sion-early-init $(TARGET_DIR)/usr/lib/sion/sion-early-init
	$(INSTALL) -D -m 0755 $(SION_TOOLS_PKGDIR)/sion-netd $(TARGET_DIR)/usr/libexec/sion-netd
	$(INSTALL) -D -m 0755 $(SION_TOOLS_PKGDIR)/sion-video $(TARGET_DIR)/usr/libexec/sion-video
	$(INSTALL) -D -m 0644 $(SION_TOOLS_PKGDIR)/crimson-480i.edid $(TARGET_DIR)/usr/share/sion/crimson-480i.edid
	mkdir -p $(TARGET_DIR)/lib/firmware/edid
	ln -sf ../../../usr/share/sion/crimson-480i.edid $(TARGET_DIR)/lib/firmware/edid/crimson-480i.edid
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

ifeq ($(BR2_PACKAGE_SION_TOOLS_SPLIT_ROOT),y)
# A new ID at every build, in the system (rootfs.squashfs) and the early
# root filesystem (rootfs.cpio, inside the kernel) alike. The kernel's
# version string carries it too (KBUILD_BUILD_HOST, see external.mk), so
# "sion update" can tell from a BOOTX64.EFI which system it needs.
define SION_TOOLS_ROOT_ID
	head -c 8 /dev/urandom | od -An -tx1 | tr -d ' \n' > $(TARGET_DIR)/etc/sion-root-id
	echo >> $(TARGET_DIR)/etc/sion-root-id
endef
SION_TOOLS_TARGET_FINALIZE_HOOKS += SION_TOOLS_ROOT_ID

# rootfs.cpio becomes the early root filesystem.
define SION_TOOLS_EARLY_ROOTFS
	$(SION_TOOLS_PKGDIR)/sion-mkearly $(TARGET_DIR) $(TARGET_READELF)
endef
ROOTFS_CPIO_PRE_GEN_HOOKS += SION_TOOLS_EARLY_ROOTFS
endif

$(eval $(generic-package))
