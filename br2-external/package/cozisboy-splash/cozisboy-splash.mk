################################################################################
#
# cozisboy-splash: animated boot splash, built from apps/splash in this repo
#
################################################################################

COZISBOY_SPLASH_VERSION = local
COZISBOY_SPLASH_SITE = $(BR2_EXTERNAL_AMBERNIC_PATH)/../apps/splash
COZISBOY_SPLASH_SITE_METHOD = local

define COZISBOY_SPLASH_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -Wall -Wextra -std=gnu11 \
		-o $(@D)/cozisboy-splash $(@D)/splash_fb.c $(@D)/splash_anim.c \
		$(TARGET_LDFLAGS) -lm
endef

define COZISBOY_SPLASH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/cozisboy-splash $(TARGET_DIR)/usr/bin/cozisboy-splash
endef

$(eval $(generic-package))
