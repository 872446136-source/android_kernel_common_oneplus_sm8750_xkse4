// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/zstd.h>

#include "backend_zstd.h"

struct zstd_ctx {
	zstd_cctx *cctx;
	zstd_dctx *dctx;
	void *cctx_mem;
	void *dctx_mem;
};

struct zstd_params {
	zstd_custom_mem custom_mem;
	zstd_cdict *cdict;
	zstd_ddict *ddict;
	zstd_parameters cprm;
};

static size_t zstd_backend_compress_bound(const struct zcomp_params *params,
					  size_t src_len)
{
	return zstd_compress_bound(src_len);
}

static void *zstd_custom_alloc(void *opaque, size_t size)
{
	return kvzalloc(size, GFP_NOIO | __GFP_NOWARN);
}

static void zstd_custom_free(void *opaque, void *address)
{
	kvfree(address);
}

static void zstd_release_params(struct zcomp_params *params)
{
	struct zstd_params *zp = params->drv_data;

	params->drv_data = NULL;
	if (!zp)
		return;
	zstd_free_cdict(zp->cdict);
	zstd_free_ddict(zp->ddict);
	kfree(zp);
}

static int zstd_validate_params(const struct zcomp_params *params)
{
	if (params->level != ZCOMP_PARAM_NOT_SET &&
	    (params->level < zstd_min_clevel() ||
	     params->level > zstd_max_clevel()))
		return -EINVAL;
	return 0;
}

static int zstd_setup_params(struct zcomp_params *params)
{
	zstd_compression_parameters cparams;
	struct zstd_params *zp;

	if (zstd_validate_params(params))
		return -EINVAL;

	if (params->level == ZCOMP_PARAM_NOT_SET)
		params->level = zstd_default_clevel();

	zp = kzalloc(sizeof(*zp), GFP_KERNEL);
	if (!zp)
		return -ENOMEM;
	params->drv_data = zp;

	zp->cprm = zstd_get_params(params->level, PAGE_SIZE);
	zp->custom_mem.customAlloc = zstd_custom_alloc;
	zp->custom_mem.customFree = zstd_custom_free;

	if (!params->dict)
		return 0;

	cparams = zstd_get_cparams(params->level, PAGE_SIZE, params->dict_sz);
	zp->cdict = zstd_create_cdict_byreference(params->dict,
						  params->dict_sz, cparams,
						  zp->custom_mem);
	if (!zp->cdict)
		goto error;
	zp->ddict = zstd_create_ddict_byreference(params->dict,
						  params->dict_sz,
						  zp->custom_mem);
	if (!zp->ddict)
		goto error;
	return 0;

error:
	zstd_release_params(params);
	return -EINVAL;
}

static void zstd_destroy(struct zcomp_ctx *ctx)
{
	struct zstd_ctx *zctx = ctx->context;

	if (!zctx)
		return;
	if (zctx->cctx_mem)
		vfree(zctx->cctx_mem);
	else if (zctx->cctx)
		zstd_free_cctx(zctx->cctx);
	if (zctx->dctx_mem)
		vfree(zctx->dctx_mem);
	else if (zctx->dctx)
		zstd_free_dctx(zctx->dctx);
	kfree(zctx);
	ctx->context = NULL;
}

static int zstd_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct zstd_params *zp = params->drv_data;
	struct zstd_ctx *zctx;
	zstd_parameters prm;
	size_t sz;

	zctx = kzalloc(sizeof(*zctx), GFP_KERNEL);
	if (!zctx)
		return -ENOMEM;
	ctx->context = zctx;

	if (!params->dict) {
		prm = zstd_get_params(params->level, PAGE_SIZE);
		sz = zstd_cctx_workspace_bound(&prm.cParams);
		zctx->cctx_mem = vzalloc(sz);
		if (!zctx->cctx_mem)
			goto error;
		zctx->cctx = zstd_init_cctx(zctx->cctx_mem, sz);
		if (!zctx->cctx)
			goto error;

		sz = zstd_dctx_workspace_bound();
		zctx->dctx_mem = vzalloc(sz);
		if (!zctx->dctx_mem)
			goto error;
		zctx->dctx = zstd_init_dctx(zctx->dctx_mem, sz);
		if (!zctx->dctx)
			goto error;
	} else {
		zctx->cctx = zstd_create_cctx_advanced(zp->custom_mem);
		if (!zctx->cctx)
			goto error;
		zctx->dctx = zstd_create_dctx_advanced(zp->custom_mem);
		if (!zctx->dctx)
			goto error;
	}
	return 0;

error:
	zstd_destroy(ctx);
	return -ENOMEM;
}

static int zstd_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			 struct zcomp_req *req)
{
	struct zstd_params *zp = params->drv_data;
	struct zstd_ctx *zctx = ctx->context;
	size_t ret;

	if (!params->dict)
		ret = zstd_compress_cctx(zctx->cctx, req->dst, req->dst_len,
					 req->src, req->src_len, &zp->cprm);
	else
		ret = zstd_compress_using_cdict(zctx->cctx, req->dst,
						req->dst_len, req->src,
						req->src_len, zp->cdict);
	if (zstd_is_error(ret)) {
		if (zstd_get_error_code(ret) == ZSTD_error_dstSize_tooSmall)
			return -ENOSPC;
		return -EINVAL;
	}
	req->dst_len = ret;
	return 0;
}

static int zstd_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			   struct zcomp_req *req)
{
	struct zstd_params *zp = params->drv_data;
	struct zstd_ctx *zctx = ctx->context;
	size_t ret;

	if (!params->dict)
		ret = zstd_decompress_dctx(zctx->dctx, req->dst, req->dst_len,
					   req->src, req->src_len);
	else
		ret = zstd_decompress_using_ddict(zctx->dctx, req->dst,
						  req->dst_len, req->src,
						  req->src_len, zp->ddict);
	if (zstd_is_error(ret) || ret != req->dst_len)
		return -EINVAL;
	return 0;
}

const struct zcomp_ops backend_zstd = {
	.abi_version	= ZCOMP_BACKEND_ABI_VERSION,
	.backend_id	= ZCOMP_BACKEND_ZSTD,
	.exec_class	= ZCOMP_EXEC_HIGH_RATIO,
	.capabilities	= ZCOMP_CAP_COMPRESS | ZCOMP_CAP_DECOMPRESS |
			  ZCOMP_CAP_PERCPU_CONTEXT |
			  ZCOMP_CAP_BOUNDED_OUTPUT |
			  ZCOMP_CAP_PREPARED_PARAMS,
	.param_caps	= ZCOMP_PARAM_LEVEL | ZCOMP_PARAM_DICTIONARY,
	.name		= "zstd",
	.compress_bound	= zstd_backend_compress_bound,
	.validate_params = zstd_validate_params,
	.compress	= zstd_compress,
	.decompress	= zstd_decompress,
	.create_ctx	= zstd_create,
	.destroy_ctx	= zstd_destroy,
	.setup_params	= zstd_setup_params,
	.release_params	= zstd_release_params,
};
