################################################################################
#
# cozisboy-splash: animated boot splash + jingle, built from apps/ in this repo
#
################################################################################

COZISBOY_SPLASH_VERSION = local
COZISBOY_SPLASH_SITE = $(BR2_EXTERNAL_AMBERNIC_PATH)/../apps
COZISBOY_SPLASH_SITE_METHOD = local
COZISBOY_SPLASH_DEPENDENCIES = alsa-lib

# The splash is statically linked: as PID 1 it starts from a cold page cache
# and one file loads much faster than ld.so + libraries.
# The jingle player uses alsa-lib (tinyalsa never started the stream on this
# codec) and runs as a separate process, so the splash can stay static.
define COZISBOY_SPLASH_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -Wall -Wextra -std=gnu11 -static \
		-o $(@D)/cozisboy-splash \
		$(@D)/splash/splash_fb.c $(@D)/splash/splash_anim.c \
		$(TARGET_LDFLAGS) -lm
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -Wall -Wextra -std=gnu11 -I$(@D)/common \
		-o $(@D)/cozisboy-chime \
		$(@D)/splash/chime.c $(@D)/common/sfx.c \
		$(TARGET_LDFLAGS) -lasound -lm
endef

define COZISBOY_SPLASH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/cozisboy-splash $(TARGET_DIR)/usr/bin/cozisboy-splash
	$(INSTALL) -D -m 0755 $(@D)/cozisboy-chime $(TARGET_DIR)/usr/bin/cozisboy-chime
	ln -sf ../usr/bin/cozisboy-splash $(TARGET_DIR)/sbin/preinit
endef

$(eval $(generic-package))
