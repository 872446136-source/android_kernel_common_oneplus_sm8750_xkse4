obj-y += module_intercept.o overlay_files.o

targets += overlay_files.c

quiet_cmd_gen_module_overlay = GEN     $@
      cmd_gen_module_overlay = $(srctree)/kernel/module/module_overlay/gen_overlay.sh $@ $(srctree)/kernel/module/module_overlay/modules

PHONY += FORCE
FORCE:

$(obj)/overlay_files.c: $(srctree)/kernel/module/module_overlay/gen_overlay.sh FORCE
	$(call if_changed,gen_module_overlay)
