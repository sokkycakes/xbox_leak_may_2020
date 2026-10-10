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

ifeq ($(BR2_arm),y)
# 32-bit ARM: build xbcompat itself (TARGET=armhf: guest code in Unicorn)
# and unpack the titles beside it. Same /opt/xbcompat layout as the Sion's.
XBCOMPAT_SITE = $(BR2_EXTERNAL_BOOTANI_PATH)/../../xbcompat
XBCOMPAT_SITE_METHOD = local
XBCOMPAT_DEPENDENCIES = unicorn sdl2 mesa3d libglvnd host-python3 host-pkgconf
XBCOMPAT_TITLES = $(call qstrip,$(BR2_PACKAGE_XBCOMPAT_TITLES))
XBCOMPAT_LEAK = $(BR2_EXTERNAL_BOOTANI_PATH)/../../xbox_leak_may_2020/xbox trunk/xbox

# Mesa drops its desktop GL headers when GLX is off, but libglvnd's
# libOpenGL (desktop GL over EGL, no X11) is what xbcompat links.
define XBCOMPAT_GL_HEADERS
	$(INSTALL) -D -m 0644 $(MESA3D_DIR)/include/GL/gl.h $(STAGING_DIR)/usr/include/GL/gl.h
	$(INSTALL) -D -m 0644 $(MESA3D_DIR)/include/GL/glext.h $(STAGING_DIR)/usr/include/GL/glext.h
endef
XBCOMPAT_PRE_BUILD_HOOKS += XBCOMPAT_GL_HEADERS

define XBCOMPAT_BUILD_CMDS
	$(TARGET_MAKE_ENV) PATH=$(HOST_DIR)/bin:$$PATH $(MAKE) -C $(@D) TARGET=armhf \
		CROSS=$(TARGET_CROSS) CFLAGS="$(TARGET_CFLAGS) -O2 -g" GL=opengl \
		PKGCFG="$(PKG_CONFIG_HOST_BINARY)" UNICORN=$(STAGING_DIR)/usr \
		LEAK="$(XBCOMPAT_LEAK)" BUILD=build-buildroot build-buildroot/xbcompat
endef

ifeq ($(BR2_PACKAGE_BOX86),y)
# box86 runs the i386 build, compiled by the build machine's own 32-bit x86
# gcc, SDL2 and GL (gcc-multilib, libsdl2-dev:i386, libgl-dev:i386): box86
# hands its libc, SDL2 and GL calls to the Pi's native libraries.
XBCOMPAT_DEPENDENCIES += box86
define XBCOMPAT_BUILD_X86
	$(MAKE) -C $(@D) TARGET=i386 CC=/usr/bin/gcc CFLAGS="-O2 -g -fno-stack-protector" \
		PKGCFG="PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig /usr/bin/pkg-config" \
		LEAK="$(XBCOMPAT_LEAK)" BUILD=build-x86 build-x86/xbcompat
endef
XBCOMPAT_POST_BUILD_HOOKS += XBCOMPAT_BUILD_X86
define XBCOMPAT_INSTALL_X86
	$(INSTALL) -D -m 0755 $(@D)/build-x86/xbcompat $(TARGET_DIR)/opt/xbcompat/bin/xbcompat.x86
	mv $(TARGET_DIR)/opt/xbcompat/bin/xbcompat $(TARGET_DIR)/opt/xbcompat/bin/xbcompat.arm
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/xbcompat-launch $(TARGET_DIR)/opt/xbcompat/bin/xbcompat
endef
XBCOMPAT_POST_INSTALL_TARGET_HOOKS += XBCOMPAT_INSTALL_X86
endif

define XBCOMPAT_INSTALL_TARGET_CMDS
	test -r "$(XBCOMPAT_TITLES)" || { echo "BR2_PACKAGE_XBCOMPAT_TITLES: run xbcompat/tools/pi/titles.sh and point this at its xbcompat-titles.tar.gz"; exit 1; }
	rm -rf $(TARGET_DIR)/opt/xbcompat
	mkdir -p $(TARGET_DIR)/opt/xbcompat
	tar -C $(TARGET_DIR)/opt/xbcompat -xzf $(XBCOMPAT_TITLES)
	$(INSTALL) -D -m 0755 $(@D)/build-buildroot/xbcompat $(TARGET_DIR)/opt/xbcompat/bin/xbcompat
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/xbox-boot $(TARGET_DIR)/usr/libexec/xbox-boot
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/xbox-session $(TARGET_DIR)/usr/libexec/xbox-session
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/xbox-title $(TARGET_DIR)/usr/libexec/xbox-title
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/xbox-env $(TARGET_DIR)/usr/lib/xbox/xbox-env
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/dashboard $(TARGET_DIR)/usr/libexec/dashboard
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/xbox-sample $(TARGET_DIR)/usr/bin/xbox-sample
	mkdir -p $(TARGET_DIR)/etc/default
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/../../../../xbcompat/tools/sion/cards/sion-cards $(TARGET_DIR)/usr/sbin/sion-cards
	echo 'DASHBOARD="xbox"' > $(TARGET_DIR)/etc/default/dashboard
endef

define XBCOMPAT_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/../../../../xbcompat/tools/sion/cards/S35cards $(TARGET_DIR)/etc/init.d/S35cards
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/S11xbox-boot $(TARGET_DIR)/etc/init.d/S11xbox-boot
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/S98xbox $(TARGET_DIR)/etc/init.d/S98xbox
endef

else

define XBCOMPAT_INSTALL_TARGET_CMDS
	test -r "$(XBCOMPAT_BUNDLE)" || { echo "BR2_PACKAGE_XBCOMPAT_BUNDLE: run xbcompat/tools/sion/bundle.sh and point this at its xbcompat-sion.tar.gz"; exit 1; }
	rm -rf $(TARGET_DIR)/opt/xbcompat
	mkdir -p $(TARGET_DIR)/opt
	tar -C $(TARGET_DIR)/opt -xzf $(XBCOMPAT_BUNDLE)
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/xbox-boot $(TARGET_DIR)/usr/libexec/xbox-boot
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/xbox-session $(TARGET_DIR)/usr/libexec/xbox-session
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/xbox-title $(TARGET_DIR)/usr/libexec/xbox-title
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/xbox-env $(TARGET_DIR)/usr/lib/xbox/xbox-env
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/dashboard $(TARGET_DIR)/usr/libexec/dashboard
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/../../../../xbcompat/tools/sion/cards/sion-cards $(TARGET_DIR)/usr/sbin/sion-cards
	mkdir -p $(TARGET_DIR)/etc/default
	echo 'DASHBOARD="$(call qstrip,$(BR2_PACKAGE_XBCOMPAT_DASHBOARD))"' > $(TARGET_DIR)/etc/default/dashboard
endef

define XBCOMPAT_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/S98xbox $(TARGET_DIR)/etc/init.d/S98xbox
	$(INSTALL) -D -m 0755 $(XBCOMPAT_PKGDIR)/../../../../xbcompat/tools/sion/cards/S35cards $(TARGET_DIR)/etc/init.d/S35cards
endef

endif

define XBCOMPAT_LINUX_CONFIG_FIXUPS
	$(call KCONFIG_ENABLE_OPT,CONFIG_IA32_EMULATION)
	$(call KCONFIG_ENABLE_OPT,CONFIG_MODIFY_LDT_SYSCALL)
endef

$(eval $(generic-package))
