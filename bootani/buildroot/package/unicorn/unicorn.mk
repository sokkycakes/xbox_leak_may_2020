################################################################################
#
# unicorn
#
################################################################################

UNICORN_VERSION = 2.1.4
# A git checkout: plain HTTPS to GitHub archives is not always reachable
# from the build machines this tree is built on.
UNICORN_SITE = https://github.com/unicorn-engine/unicorn.git
UNICORN_SITE_METHOD = git
UNICORN_LICENSE = GPL-2.0
UNICORN_LICENSE_FILES = COPYING
UNICORN_INSTALL_STAGING = YES
UNICORN_INSTALL_TARGET = NO
UNICORN_CONF_OPTS = \
	-DUNICORN_ARCH=x86 \
	-DUNICORN_BUILD_TESTS=OFF \
	-DBUILD_SHARED_LIBS=OFF

$(eval $(cmake-package))
