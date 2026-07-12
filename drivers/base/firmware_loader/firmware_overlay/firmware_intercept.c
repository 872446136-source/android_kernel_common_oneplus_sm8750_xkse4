// SPDX-License-Identifier: GPL-2.0
#include <linux/firmware.h>
#include <linux/ctype.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/zstd.h>

#include "../firmware.h"
#include "overlay_files.h"

#define REGDB_OVERLAY_CMDLINE "regdb_overlay.enable"

static const char *firmware_basename(const char *name)
{
	const char *basename = strrchr(name, '/');

	return basename ? basename + 1 : name;
}

static const struct overlay_file *find_overlay(const char *name)
{
	const char *basename = firmware_basename(name);
	int i;

	for (i = 0; i < firmware_file_list_count; i++) {
		if (!strcmp(firmware_file_list[i].name, basename))
			return &firmware_file_list[i];
	}
	return NULL;
}

static bool regdb_overlay_enabled(void)
{
	const char *cmdline = saved_command_line;
	const size_t option_len = strlen(REGDB_OVERLAY_CMDLINE);
	const char *value;

	while ((value = strstr(cmdline, REGDB_OVERLAY_CMDLINE))) {
		if ((value == saved_command_line || isspace(value[-1])) &&
		    value[option_len] == '=' && value[option_len + 1] == '1' &&
		    (!value[option_len + 2] || isspace(value[option_len + 2])))
			return true;
		cmdline = value + option_len;
	}
	return false;
}

bool should_intercept_firmware(const char *name)
{
	return regdb_overlay_enabled() && find_overlay(name);
}

enum intercept_status intercept_firmware_load(struct firmware *fw,
					      const char *name)
{
	const struct overlay_file *ov = find_overlay(name);
	void *decompressed_data, *workspace;
	size_t decompressed_size, workspace_size;
	struct fw_priv *fw_priv;
	zstd_dctx *dctx;

	if (!ov || !regdb_overlay_enabled())
		return INTERCEPT_STATUS_SKIP;
	if (!fw || !fw->priv)
		return INTERCEPT_STATUS_ERROR;

	fw_priv = fw->priv;
	if (fw_priv->data)
		return INTERCEPT_STATUS_ERROR;

	workspace_size = zstd_dctx_workspace_bound();
	workspace = vzalloc(workspace_size);
	if (!workspace)
		return INTERCEPT_STATUS_ERROR;

	dctx = zstd_init_dctx(workspace, workspace_size);
	if (!dctx) {
		vfree(workspace);
		return INTERCEPT_STATUS_ERROR;
	}

	decompressed_data = vmalloc(ov->orig_size);
	if (!decompressed_data) {
		vfree(workspace);
		return INTERCEPT_STATUS_ERROR;
	}

	decompressed_size = zstd_decompress_dctx(dctx, decompressed_data,
						 ov->orig_size, ov->data,
						 ov->len);
	vfree(workspace);
	if (zstd_is_error(decompressed_size) ||
	    decompressed_size != ov->orig_size) {
		pr_err("firmware_overlay: failed to decompress %s\n", name);
		vfree(decompressed_data);
		return INTERCEPT_STATUS_ERROR;
	}

	fw_priv->data = decompressed_data;
	fw_priv->size = decompressed_size;
	/* A zero allocated_size marks data as loader-owned for release. */
	fw_priv->allocated_size = 0;
#ifdef CONFIG_FW_LOADER_PAGED_BUF
	fw_priv->is_paged_buf = false;
#endif

	pr_info("firmware_overlay: replaced %s (%zu bytes)\n", name,
		 decompressed_size);
	return INTERCEPT_STATUS_SUCCESS;
}
