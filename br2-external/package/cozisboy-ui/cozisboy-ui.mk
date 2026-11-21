################################################################################
#
# cozisboy-ui: the launcher, built from apps/ in this repo
#
################################################################################

COZISBOY_UI_VERSION = local
COZISBOY_UI_SITE = $(BR2_EXTERNAL_AMBERNIC_PATH)/../apps
COZISBOY_UI_SITE_METHOD = local
COZISBOY_UI_DEPENDENCIES = alsa-lib mgba

# Same sources as the PC preview (apps/ui/Makefile), with the device front end
# (host_dev.c) and system backend (sys_dev.c) instead of SDL and the mock.
COZISBOY_UI_SRCS = host_dev.c sys_dev.c ui.c games.c net.c emu.c gfx.c ../common/sfx.c

define COZISBOY_UI_BUILD_CMDS
	cd $(@D)/ui && $(TARGET_CC) $(TARGET_CFLAGS) -O2 -Wall -Wextra -std=c11 \
		-D_DEFAULT_SOURCE -I../common -o cozisboy-ui $(COZISBOY_UI_SRCS) \
		$(TARGET_LDFLAGS) -lmgba -lasound -lpthread -lm
endef

define COZISBOY_UI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/ui/cozisboy-ui $(TARGET_DIR)/usr/bin/cozisboy-ui
endef

$(eval $(generic-package))
