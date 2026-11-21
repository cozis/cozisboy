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

# Statically linked (tinyalsa core compiled in, no plugins): as PID 1 it starts
# from a cold page cache, and one file is much faster to load than ld.so + libs.
COZISBOY_SPLASH_TINYALSA_SRCS = pcm.c pcm_hw.c mixer.c mixer_hw.c limits.c snd_card_plugin.c

define COZISBOY_SPLASH_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -Wall -Wextra -std=gnu11 -static \
		-I$(@D)/common -I$(TINYALSA_DIR)/include \
		-o $(@D)/cozisboy-splash \
		$(@D)/splash/splash_fb.c $(@D)/splash/splash_anim.c $(@D)/common/sfx.c \
		$(addprefix $(TINYALSA_DIR)/src/,$(COZISBOY_SPLASH_TINYALSA_SRCS)) \
		$(TARGET_LDFLAGS) -lpthread -lm
endef

define COZISBOY_SPLASH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/cozisboy-splash $(TARGET_DIR)/usr/bin/cozisboy-splash
	ln -sf ../usr/bin/cozisboy-splash $(TARGET_DIR)/sbin/preinit
endef

$(eval $(generic-package))
