// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/lz4k.h>
#include <linux/slab.h>

#include "backend_lz4k.h"

static size_t lz4k_compress_bound(const struct zcomp_params *params,
				  size_t src_len)
{
	return src_len;
}

static int lz4k_validate_params(const struct zcomp_params *params)
{
	return PAGE_SHIFT != 12 ? -EINVAL : 0;
}

static void lz4k_release_params(struct zcomp_params *params)
{
	params->drv_data = NULL;
}

static int lz4k_setup_params(struct zcomp_params *params)
{
	return lz4k_validate_params(params);
}

static int lz4k_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	ctx->context = kvzalloc(lz4k_state_bytes_min(),
				GFP_KERNEL | __GFP_NOWARN);
	return ctx->context ? 0 : -ENOMEM;
}

static void lz4k_destroy(struct zcomp_ctx *ctx)
{
	kvfree(ctx->context);
	ctx->context = NULL;
}

static int lz4k_compress_backend(struct zcomp_params *params,
				 struct zcomp_ctx *ctx,
				 struct zcomp_req *req)
{
	int ret;

	ret = lz4k_compress(ctx->context, req->src, req->dst,
			    req->src_len, req->dst_len);
	if (ret < 0)
		return -ENOSPC;
	if (!ret)
		return -EINVAL;
	req->dst_len = ret;
	return 0;
}

static int lz4k_decompress_backend(struct zcomp_params *params,
				   struct zcomp_ctx *ctx,
				   struct zcomp_req *req)
{
	int ret;

	ret = lz4k_decompress(req->src, req->dst, req->src_len,
			      req->dst_len);
	return ret == req->dst_len ? 0 : -EINVAL;
}

const struct zcomp_ops backend_lz4k = {
	.abi_version	= ZCOMP_BACKEND_ABI_VERSION,
	.backend_id	= ZCOMP_BACKEND_LZ4K,
	.exec_class	= ZCOMP_EXEC_FAST,
	.capabilities	= ZCOMP_CAP_COMPRESS | ZCOMP_CAP_DECOMPRESS |
			  ZCOMP_CAP_PERCPU_CONTEXT |
			  ZCOMP_CAP_BOUNDED_OUTPUT | ZCOMP_CAP_4K_ONLY,
	.param_caps	= 0,
	.name		= "lz4k",
	.compress_bound	= lz4k_compress_bound,
	.validate_params = lz4k_validate_params,
	.compress	= lz4k_compress_backend,
	.decompress	= lz4k_decompress_backend,
	.create_ctx	= lz4k_create,
	.destroy_ctx	= lz4k_destroy,
	.setup_params	= lz4k_setup_params,
	.release_params	= lz4k_release_params,
};
