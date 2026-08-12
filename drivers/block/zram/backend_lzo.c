// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/lzo.h>
#include <linux/slab.h>

#include "backend_lzo.h"

static size_t lzo_compress_bound(const struct zcomp_params *params,
				 size_t src_len)
{
	return lzo1x_worst_compress(src_len);
}

static void lzo_release_params(struct zcomp_params *params)
{
	params->drv_data = NULL;
}

static int lzo_setup_params(struct zcomp_params *params)
{
	if (params->level != ZCOMP_PARAM_NOT_SET || params->dict ||
	    params->dict_sz ||
	    params->deflate.winbits != ZCOMP_PARAM_NOT_SET)
		return -EINVAL;
	return 0;
}

static int lzo_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	ctx->context = kzalloc(LZO1X_MEM_COMPRESS, GFP_KERNEL);
	return ctx->context ? 0 : -ENOMEM;
}

static void lzo_destroy(struct zcomp_ctx *ctx)
{
	kfree(ctx->context);
	ctx->context = NULL;
}

static int lzo_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			struct zcomp_req *req)
{
	int ret;

	ret = lzo1x_1_compress_safe(req->src, req->src_len, req->dst,
				    &req->dst_len, ctx->context);
	if (ret == LZO_E_OK)
		return 0;
	if (ret == LZO_E_NOT_COMPRESSIBLE || ret == LZO_E_OUTPUT_OVERRUN)
		return -ENOSPC;
	return -EINVAL;
}

static int lzo_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			  struct zcomp_req *req)
{
	size_t expected = req->dst_len;
	int ret;

	ret = lzo1x_decompress_safe(req->src, req->src_len, req->dst,
				    &req->dst_len);
	if (ret != LZO_E_OK || req->dst_len != expected)
		return -EINVAL;
	return 0;
}

const struct zcomp_ops backend_lzo = {
	.abi_version	= ZCOMP_BACKEND_ABI_VERSION,
	.backend_id	= ZCOMP_BACKEND_LZO,
	.exec_class	= ZCOMP_EXEC_FAST,
	.capabilities	= ZCOMP_CAP_COMPRESS | ZCOMP_CAP_DECOMPRESS |
			  ZCOMP_CAP_PERCPU_CONTEXT |
			  ZCOMP_CAP_BOUNDED_OUTPUT,
	.param_caps	= 0,
	.name		= "lzo",
	.compress_bound	= lzo_compress_bound,
	.compress	= lzo_compress,
	.decompress	= lzo_decompress,
	.create_ctx	= lzo_create,
	.destroy_ctx	= lzo_destroy,
	.setup_params	= lzo_setup_params,
	.release_params	= lzo_release_params,
};
