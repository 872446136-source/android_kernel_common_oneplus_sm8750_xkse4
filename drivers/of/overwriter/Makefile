# SPDX-License-Identifier: GPL-2.0
obj-y += overwrite_configs.o overwrite_config_loader.o
ccflags-y += -I$(srctree)/drivers/of/overwriter

extra-y += overwrite_configs.c

quiet_cmd_gencfg = GEN     $@
      cmd_gencfg = $(CONFIG_SHELL) $(srctree)/drivers/of/overwriter/overwrite_configs/convert_configs.sh $@ $(srctree)/drivers/of/overwriter

PHONY += FORCE
FORCE:

$(obj)/overwrite_configs.c: $(src)/overwrite_configs/convert_configs.sh \
			    $(wildcard $(src)/overwrite_configs/common/*.conf) FORCE
	$(Q)mkdir -p $(dir $@)
	$(call if_changed,gencfg)
