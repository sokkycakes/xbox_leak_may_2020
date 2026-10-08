################################################################################
#
# xbcompat
#
################################################################################

# Prebuilt by xbcompat/tools/sion/bundle.sh (a 32-bit tree with its own
# loader and libraries); nothing to download or build here.
XBCOMPAT_VERSION = local
XBCOMPAT_SOURCE =
XBCOMPAT_LICENSE = Proprietary (Microsoft Xbox titles), MIT (XbSymbolDatabase), LGPL (glibc), Zlib (SDL2), MIT (Mesa)
XBCOMPAT_BUNDLE = $(call qstrip,$(BR2_PACKAGE_XBCOMPAT_BUNDLE))

define XBCOMPAT_INSTALL_TARGET_CMDS
	test -r "$(XBCOMPAT_BUNDLE)" || { echo "BR2_PACKAGE_XBCOMPAT_BUNDLE: run xbcompat/tools/sion/bundle.sh and point this at its xbcompat-sion.tar.gz"; exit 1; }
	rm -rf $(TARGET_DIR)/opt/xbcompat
	mkdir -p $(TARGET_DIR)/opt
	tar -C $(TARGET_DIR)/opt -xzf $(XBCOMPAT_BUNDLE)
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/xbox-boot $(TARGET_DIR)/usr/libexec/xbox-boot
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/xbox-session $(TARGET_DIR)/usr/libexec/xbox-session
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/xbox-env $(TARGET_DIR)/usr/lib/xbox/xbox-env
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/dashboard $(TARGET_DIR)/usr/libexec/dashboard
	mkdir -p $(TARGET_DIR)/etc/default
	echo 'DASHBOARD="$(call qstrip,$(BR2_PACKAGE_XBCOMPAT_DASHBOARD))"' > $(TARGET_DIR)/etc/default/dashboard
endef

define XBCOMPAT_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/S98xbox $(TARGET_DIR)/etc/init.d/S98xbox
endef

define XBCOMPAT_LINUX_CONFIG_FIXUPS
	$(call KCONFIG_ENABLE_OPT,CONFIG_IA32_EMULATION)
	$(call KCONFIG_ENABLE_OPT,CONFIG_MODIFY_LDT_SYSCALL)
endef

$(eval $(generic-package))
