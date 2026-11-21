################################################################################
#
# cozisboy-web: web app server + game library tool, built from apps/ in this repo
#
################################################################################

COZISBOY_WEB_VERSION = local
COZISBOY_WEB_SITE = $(BR2_EXTERNAL_AMBERNIC_PATH)/../apps
COZISBOY_WEB_SITE_METHOD = local

define COZISBOY_WEB_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -Wall -Wextra -std=gnu11 -I$(@D)/common \
		-o $(@D)/cozisboy-web $(@D)/web/web.c $(@D)/common/library.c \
		$(TARGET_LDFLAGS) -lpthread
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -Wall -Wextra -std=gnu11 -I$(@D)/common \
		-o $(@D)/coz-lib $(@D)/common/coz-lib.c $(@D)/common/library.c \
		$(TARGET_LDFLAGS)
endef

define COZISBOY_WEB_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/cozisboy-web $(TARGET_DIR)/usr/sbin/cozisboy-web
	$(INSTALL) -D -m 0755 $(@D)/coz-lib $(TARGET_DIR)/usr/bin/coz-lib
	$(INSTALL) -D -m 0644 $(@D)/web/index.html $(TARGET_DIR)/usr/share/cozisboy-web/index.html
	$(INSTALL) -D -m 0644 $(@D)/web/logo.png $(TARGET_DIR)/usr/share/cozisboy-web/logo.png
	$(INSTALL) -D -m 0644 $(@D)/web/favicon.png $(TARGET_DIR)/usr/share/cozisboy-web/favicon.png
endef

$(eval $(generic-package))
