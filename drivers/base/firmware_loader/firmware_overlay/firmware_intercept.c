// SPDX-License-Identifier: GPL-2.0
#include <linux/firmware.h>
#include <linux/ctype.h>
#include <linux/init.h>
#include <linux/security.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/zstd.h>

#include "../firmware.h"
#include "overlay_files.h"

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

bool should_intercept_firmware(const char *name)
{
	/*
	 * Presence in the generated overlay table is the only switch:
	 * matching embedded firmware is replaced; everything else follows
	 * the normal firmware loader path.
	 */
	return find_overlay(name) != NULL;
}

enum intercept_status intercept_firmware_load(struct firmware *fw,
					      const char *name)
{
	const struct overlay_file *ov = find_overlay(name);
	void *decompressed_data, *workspace;
	bool loader_allocated = false;
	size_t decompressed_size, workspace_size;
	struct fw_priv *fw_priv;
	zstd_dctx *dctx;
	int ret;

	if (!ov)
		return INTERCEPT_STATUS_SKIP;
	if (!fw || !fw->priv)
		return INTERCEPT_STATUS_ERROR;

	fw_priv = fw->priv;
	ret = security_kernel_load_data(LOADING_FIRMWARE, true);
	if (ret) {
		pr_err("firmware_overlay: security policy rejected %s: %d\n",
		       name, ret);
		return INTERCEPT_STATUS_ERROR;
	}

	workspace_size = zstd_dctx_workspace_bound();
	workspace = vzalloc(workspace_size);
	if (!workspace)
		return INTERCEPT_STATUS_ERROR;

	dctx = zstd_init_dctx(workspace, workspace_size);
	if (!dctx) {
		vfree(workspace);
		return INTERCEPT_STATUS_ERROR;
	}

	if (fw_priv->data) {
		if (fw_priv->allocated_size < ov->orig_size) {
			pr_err("firmware_overlay: caller buffer for %s is too small: %zu < %zu\n",
			       name, fw_priv->allocated_size, ov->orig_size);
			vfree(workspace);
			return INTERCEPT_STATUS_ERROR;
		}
		decompressed_data = fw_priv->data;
	} else {
		decompressed_data = vmalloc(ov->orig_size);
		if (!decompressed_data) {
			vfree(workspace);
			return INTERCEPT_STATUS_ERROR;
		}
		loader_allocated = true;
	}

	decompressed_size = zstd_decompress_dctx(dctx, decompressed_data,
						 ov->orig_size, ov->data,
						 ov->len);
	vfree(workspace);
	if (zstd_is_error(decompressed_size) ||
	    decompressed_size != ov->orig_size) {
		pr_err("firmware_overlay: failed to decompress %s\n", name);
		if (loader_allocated)
			vfree(decompressed_data);
		return INTERCEPT_STATUS_ERROR;
	}

	ret = security_kernel_post_load_data(decompressed_data,
					     decompressed_size,
					     LOADING_FIRMWARE,
					     "firmware_overlay");
	if (ret) {
		pr_err("firmware_overlay: security appraisal rejected %s: %d\n",
		       name, ret);
		if (loader_allocated)
			vfree(decompressed_data);
		return INTERCEPT_STATUS_ERROR;
	}

	if (loader_allocated) {
		fw_priv->data = decompressed_data;
		/* A zero allocated_size marks data as loader-owned for release. */
		fw_priv->allocated_size = 0;
	}
	fw_priv->size = decompressed_size;
#ifdef CONFIG_FW_LOADER_PAGED_BUF
	fw_priv->is_paged_buf = false;
#endif

	pr_info("firmware_overlay: replaced %s (%zu bytes)\n", name,
		 decompressed_size);
	return INTERCEPT_STATUS_SUCCESS;
}
