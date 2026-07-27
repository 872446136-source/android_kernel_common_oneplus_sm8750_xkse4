// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sw842.h>

#include "backend_842.h"

static void release_params_842(struct zcomp_params *params)
{
	params->drv_data = NULL;
}

static int setup_params_842(struct zcomp_params *params)
{
	if (params->level != ZCOMP_PARAM_NOT_SET || params->dict ||
	    params->dict_sz ||
	    params->deflate.winbits != ZCOMP_PARAM_NOT_SET)
		return -EINVAL;
	return 0;
}

static void destroy_842(struct zcomp_ctx *ctx)
{
	kfree(ctx->context);
	ctx->context = NULL;
}

static int create_842(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	ctx->context = kmalloc(SW842_MEM_COMPRESS, GFP_KERNEL);
	return ctx->context ? 0 : -ENOMEM;
}

static int compress_842(struct zcomp_params *params, struct zcomp_ctx *ctx,
			struct zcomp_req *req)
{
	unsigned int dlen = req->dst_len;
	int ret;

	ret = sw842_compress(req->src, req->src_len, req->dst, &dlen,
			     ctx->context);
	if (!ret) {
		req->dst_len = dlen;
		return 0;
	}
	return ret == -ENOSPC ? -ENOSPC : -EINVAL;
}

static int decompress_842(struct zcomp_params *params, struct zcomp_ctx *ctx,
			  struct zcomp_req *req)
{
	unsigned int dlen = req->dst_len;
	int ret;

	ret = sw842_decompress(req->src, req->src_len, req->dst, &dlen);
	if (ret || dlen != req->dst_len)
		return -EINVAL;
	return 0;
}

const struct zcomp_ops backend_842 = {
	.compress	= compress_842,
	.decompress	= decompress_842,
	.create_ctx	= create_842,
	.destroy_ctx	= destroy_842,
	.setup_params	= setup_params_842,
	.release_params	= release_params_842,
	.name		= "842",
};
