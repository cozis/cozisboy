################################################################################
#
# cozisboy-splash: animated boot splash + jingle, built from apps/ in this repo
#
################################################################################

COZISBOY_SPLASH_VERSION = local
COZISBOY_SPLASH_SITE = $(BR2_EXTERNAL_AMBERNIC_PATH)/../apps
COZISBOY_SPLASH_SITE_METHOD = local
# tinyalsa does not install include/tinyalsa/attributes.h to staging; use its
# build dir headers (TINYALSA_DIR) so pcm.h can find it.
COZISBOY_SPLASH_DEPENDENCIES = tinyalsa

define COZISBOY_SPLASH_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -Wall -Wextra -std=gnu11 -I$(@D)/common \
		-I$(TINYALSA_DIR)/include \
		-o $(@D)/cozisboy-splash \
		$(@D)/splash/splash_fb.c $(@D)/splash/splash_anim.c $(@D)/common/sfx.c \
		$(TARGET_LDFLAGS) -ltinyalsa -lpthread -lm
endef

define COZISBOY_SPLASH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/cozisboy-splash $(TARGET_DIR)/usr/bin/cozisboy-splash
endef

$(eval $(generic-package))
