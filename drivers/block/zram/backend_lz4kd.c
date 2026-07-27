// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/lz4kd.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "backend_lz4kd.h"

static void lz4kd_release_params(struct zcomp_params *params)
{
	params->drv_data = NULL;
}

static int lz4kd_setup_params(struct zcomp_params *params)
{
	if (PAGE_SIZE != SZ_4K ||
	    params->level != ZCOMP_PARAM_NOT_SET || params->dict ||
	    params->dict_sz ||
	    params->deflate.winbits != ZCOMP_PARAM_NOT_SET)
		return -EINVAL;
	return 0;
}

static int lz4kd_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	ctx->context = kvzalloc(lz4kd_encode_state_bytes_min(),
				GFP_KERNEL | __GFP_NOWARN);
	return ctx->context ? 0 : -ENOMEM;
}

static void lz4kd_destroy(struct zcomp_ctx *ctx)
{
	kvfree(ctx->context);
	ctx->context = NULL;
}

static int lz4kd_compress_backend(struct zcomp_params *params,
				  struct zcomp_ctx *ctx,
				  struct zcomp_req *req)
{
	int ret;

	ret = lz4kd_encode(ctx->context, req->src, req->dst, req->src_len,
			   req->dst_len, req->dst_len);
	if (ret == LZ4K_STATUS_INCOMPRESSIBLE)
		return -ENOSPC;
	if (ret < 0)
		return -EINVAL;
	req->dst_len = ret;
	return 0;
}

static int lz4kd_decompress_backend(struct zcomp_params *params,
				    struct zcomp_ctx *ctx,
				    struct zcomp_req *req)
{
	int ret;

	ret = lz4kd_decode(req->src, req->dst, req->src_len, req->dst_len);
	return ret == req->dst_len ? 0 : -EINVAL;
}

const struct zcomp_ops backend_lz4kd = {
	.compress	= lz4kd_compress_backend,
	.decompress	= lz4kd_decompress_backend,
	.create_ctx	= lz4kd_create,
	.destroy_ctx	= lz4kd_destroy,
	.setup_params	= lz4kd_setup_params,
	.release_params	= lz4kd_release_params,
	.name		= "lz4kd",
};
