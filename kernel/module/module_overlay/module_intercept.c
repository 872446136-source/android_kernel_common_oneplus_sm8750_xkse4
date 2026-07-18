// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/security.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/zstd.h>

#include "../internal.h"
#include "overlay_files.h"

static const struct module_overlay_file *
module_overlay_find(const char *name)
{
	size_t i;

	if (!name)
		return NULL;

	for (i = 0; i < module_overlay_file_count; i++) {
		if (!strcmp(module_overlay_files[i].name, name))
			return &module_overlay_files[i];
	}

	return NULL;
}

bool module_overlay_has(const char *name)
{
	return module_overlay_find(name) != NULL;
}

enum module_overlay_result
module_overlay_replace(struct load_info *info, const char *name)
{
	const struct module_overlay_file *overlay;
	Elf_Ehdr *old_hdr;
	void *new_hdr = NULL;
	void *workspace = NULL;
	zstd_dctx *dctx;
	int ret;
	size_t workspace_size;
	size_t decompressed_size;

	if (!info || !name)
		return MODULE_OVERLAY_ERROR;

	overlay = module_overlay_find(name);
	if (!overlay)
		return MODULE_OVERLAY_SKIP;

	workspace_size = zstd_dctx_workspace_bound();
	workspace = vzalloc(workspace_size);
	if (!workspace)
		goto error;

	dctx = zstd_init_dctx(workspace, workspace_size);
	if (!dctx)
		goto error;

	new_hdr = vmalloc(overlay->original_size);
	if (!new_hdr)
		goto error;

	decompressed_size = zstd_decompress_dctx(
		dctx, new_hdr, overlay->original_size,
		overlay->data, overlay->compressed_size);
	if (zstd_is_error(decompressed_size) ||
	    decompressed_size != overlay->original_size) {
		pr_err("module_overlay: decompression failed for %s: %zu/%zu\n",
		       name, decompressed_size, overlay->original_size);
		goto error;
	}

	/* Appraise the bytes that will actually be loaded, not only the stub. */
	ret = security_kernel_post_load_data(new_hdr, decompressed_size,
					     LOADING_MODULE, "module_overlay");
	if (ret) {
		pr_err("module_overlay: security appraisal rejected %s: %d\n",
		       name, ret);
		goto error;
	}

	/*
	 * Keep the original userspace-provided image until the complete
	 * embedded image has been decompressed and size-checked.
	 */
	old_hdr = info->hdr;
	info->hdr = new_hdr;
	info->len = decompressed_size;

	/* elf_validity_cache_copy() rebuilds these from the replacement ELF. */
	info->name = NULL;
	info->mod = NULL;
	info->sechdrs = NULL;
	info->secstrings = NULL;
	info->strtab = NULL;
	info->symoffs = 0;
	info->stroffs = 0;
	info->init_typeoffs = 0;
	info->core_typeoffs = 0;
	info->sig_ok = false;
	memset(&info->index, 0, sizeof(info->index));

	vfree(old_hdr);
	vfree(workspace);

	pr_info("module_overlay: replaced %s (%zu compressed, %zu original)\n",
		name, overlay->compressed_size, decompressed_size);
	return MODULE_OVERLAY_REPLACED;

error:
	vfree(new_hdr);
	vfree(workspace);
	return MODULE_OVERLAY_ERROR;
}
