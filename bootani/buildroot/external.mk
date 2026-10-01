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
