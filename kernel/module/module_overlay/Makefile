obj-y += module_intercept.o overlay_files.o

targets += overlay_files.c

quiet_cmd_gen_module_overlay = GEN     $@
      cmd_gen_module_overlay = $(srctree)/kernel/module/module_overlay/gen_overlay.sh $@ $(srctree)/kernel/module/module_overlay/modules

module_overlay_payloads := $(sort $(wildcard $(srctree)/kernel/module/module_overlay/modules/*.payload))

PHONY += FORCE
FORCE:

$(obj)/overlay_files.c: $(srctree)/kernel/module/module_overlay/gen_overlay.sh $(module_overlay_payloads) FORCE
	$(call if_changed,gen_module_overlay)
