// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/lz4kd.h>
#include <linux/slab.h>

#include "backend_lz4kd.h"

static size_t lz4kd_compress_bound(const struct zcomp_params *params,
				   size_t src_len)
{
	if (src_len > SIZE_MAX / 2)
		return SIZE_MAX;
	return 2 * src_len;
}

static int lz4kd_validate_params(const struct zcomp_params *params)
{
	return PAGE_SHIFT != 12 ? -EINVAL : 0;
}

static void lz4kd_release_params(struct zcomp_params *params)
{
	params->drv_data = NULL;
}

static int lz4kd_setup_params(struct zcomp_params *params)
{
	return lz4kd_validate_params(params);
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
	.abi_version	= ZCOMP_BACKEND_ABI_VERSION,
	.backend_id	= ZCOMP_BACKEND_LZ4KD,
	.exec_class	= ZCOMP_EXEC_FAST,
	.capabilities	= ZCOMP_CAP_COMPRESS | ZCOMP_CAP_DECOMPRESS |
			  ZCOMP_CAP_PERCPU_CONTEXT |
			  ZCOMP_CAP_BOUNDED_OUTPUT | ZCOMP_CAP_4K_ONLY,
	.param_caps	= 0,
	.name		= "lz4kd",
	.compress_bound	= lz4kd_compress_bound,
	.validate_params = lz4kd_validate_params,
	.compress	= lz4kd_compress_backend,
	.decompress	= lz4kd_decompress_backend,
	.create_ctx	= lz4kd_create,
	.destroy_ctx	= lz4kd_destroy,
	.setup_params	= lz4kd_setup_params,
	.release_params	= lz4kd_release_params,
};
