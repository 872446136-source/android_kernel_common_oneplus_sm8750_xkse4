// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/lzo.h>
#include <linux/slab.h>

#include "backend_lzorle.h"

static void lzorle_release_params(struct zcomp_params *params)
{
	params->drv_data = NULL;
}

static int lzorle_setup_params(struct zcomp_params *params)
{
	if (params->level != ZCOMP_PARAM_NOT_SET || params->dict ||
	    params->dict_sz ||
	    params->deflate.winbits != ZCOMP_PARAM_NOT_SET)
		return -EINVAL;
	return 0;
}

static int lzorle_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	ctx->context = kzalloc(LZO1X_MEM_COMPRESS, GFP_KERNEL);
	return ctx->context ? 0 : -ENOMEM;
}

static void lzorle_destroy(struct zcomp_ctx *ctx)
{
	kfree(ctx->context);
	ctx->context = NULL;
}

static int lzorle_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			   struct zcomp_req *req)
{
	int ret;

	ret = lzorle1x_1_compress_safe(req->src, req->src_len, req->dst,
				       &req->dst_len, ctx->context);
	if (ret == LZO_E_OK)
		return 0;
	if (ret == LZO_E_NOT_COMPRESSIBLE || ret == LZO_E_OUTPUT_OVERRUN)
		return -ENOSPC;
	return -EINVAL;
}

static int lzorle_decompress(struct zcomp_params *params,
			     struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	size_t expected = req->dst_len;
	int ret;

	ret = lzo1x_decompress_safe(req->src, req->src_len, req->dst,
				    &req->dst_len);
	if (ret != LZO_E_OK || req->dst_len != expected)
		return -EINVAL;
	return 0;
}

const struct zcomp_ops backend_lzorle = {
	.compress	= lzorle_compress,
	.decompress	= lzorle_decompress,
	.create_ctx	= lzorle_create,
	.destroy_ctx	= lzorle_destroy,
	.setup_params	= lzorle_setup_params,
	.release_params	= lzorle_release_params,
	.name		= "lzo-rle",
};
