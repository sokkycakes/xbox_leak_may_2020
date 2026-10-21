include $(sort $(wildcard $(BR2_EXTERNAL_BOOTANI_PATH)/package/*/*.mk))

# Mesa's iris driver pulls in libclc (through BR2_PACKAGE_MESA3D_OPENCL), but
# Mesa 26 only links libclc for OpenCL through rusticl, which this image
# leaves out. Buildroot's libclc package does not configure against LLVM 23
# (libclc now builds one target per build, taken from the default triple),
# so skip it instead of building something nothing uses.
define LIBCLC_CONFIGURE_CMDS
endef
define LIBCLC_BUILD_CMDS
endef
define LIBCLC_INSTALL_STAGING_CMDS
endef
define LIBCLC_INSTALL_TARGET_CMDS
endef
define HOST_LIBCLC_CONFIGURE_CMDS
endef
define HOST_LIBCLC_BUILD_CMDS
endef
define HOST_LIBCLC_INSTALL_CMDS
endef

# Theseus plays MP3 soundtracks through SDL_mixer. Buildroot builds SDL2_mixer
# without MP3; its bundled dr_mp3 decoder needs no other library.
ifeq ($(BR2_PACKAGE_THESEUS),y)
SDL2_MIXER_CONF_OPTS += --enable-music-mp3 --enable-music-mp3-drmp3
endif

# Raspberry Pi: SDL's off-screen driver, so xbcompat still runs (and takes
# screenshots) where there is no display device, such as in QEMU.
ifeq ($(BR2_arm),y)
SDL2_CONF_OPTS += --enable-video-offscreen
# Desktop GL over EGL (through glvnd's libOpenGL), as on the Sion: buildroot
# only offers SDL's OpenGL with X11, and without it SDL hands out GLES
# contexts, which xbcompat's fixed-function GL can't use.
SDL2_CONF_OPTS += --enable-video-opengl
SDL2_DEPENDENCIES += libglvnd
endif
