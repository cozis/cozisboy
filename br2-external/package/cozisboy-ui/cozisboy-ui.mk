################################################################################
#
# cozisboy-ui: the launcher, built from apps/ in this repo
#
################################################################################

COZISBOY_UI_VERSION = local
COZISBOY_UI_SITE = $(BR2_EXTERNAL_AMBERNIC_PATH)/../apps
COZISBOY_UI_SITE_METHOD = local
COZISBOY_UI_DEPENDENCIES = alsa-lib mgba libpng

# apps/ui/Makefile's "device" target knows the sources; build it with
# Buildroot's compiler and flags.
define COZISBOY_UI_BUILD_CMDS
	$(MAKE) -B -C $(@D)/ui cozisboy-ui DEV_CC="$(TARGET_CC)" \
		DEV_EXTRA_CFLAGS="$(TARGET_CFLAGS)" DEV_LDFLAGS="$(TARGET_LDFLAGS)"
endef

define COZISBOY_UI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/ui/cozisboy-ui $(TARGET_DIR)/usr/bin/cozisboy-ui
endef

$(eval $(generic-package))
