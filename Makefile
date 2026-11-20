# Thin wrapper around Buildroot. Everything builds into ./output.
BR       := $(CURDIR)/src/buildroot
OUT      := $(CURDIR)/output
EXTERNAL := $(CURDIR)/br2-external
BRMAKE   := $(MAKE) -C $(BR) O=$(OUT) BR2_EXTERNAL=$(EXTERNAL)

.PHONY: all config menuconfig savedefconfig linux-menuconfig clean
all: $(OUT)/.config
	$(BRMAKE)

$(OUT)/.config:
	$(BRMAKE) rg35xx_sp_defconfig

config:
	$(BRMAKE) rg35xx_sp_defconfig

menuconfig savedefconfig linux-menuconfig uboot-menuconfig:
	$(BRMAKE) $@

# Pass through any other Buildroot target, e.g. `make linux-rebuild`
%:
	$(BRMAKE) $@
