// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/lz4.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "backend_lz4.h"

struct lz4_ctx {
	void *mem;
	LZ4_streamDecode_t *dstrm;
	LZ4_stream_t *cstrm;
};

static void lz4_release_params(struct zcomp_params *params)
{
	kfree(params->drv_data);
	params->drv_data = NULL;
}

static int lz4_setup_params(struct zcomp_params *params)
{
	LZ4_stream_t *dict_stream;
	int ret;

	if (params->deflate.winbits != ZCOMP_PARAM_NOT_SET ||
	    params->dict_sz > INT_MAX || (!!params->dict != !!params->dict_sz))
		return -EINVAL;

	if (params->level == ZCOMP_PARAM_NOT_SET)
		params->level = LZ4_ACCELERATION_DEFAULT;
	else if (params->level <= 0 ||
		 params->level > LZ4_ACCELERATION_MAX)
		return -EINVAL;

	if (!params->dict)
		return 0;

	dict_stream = kzalloc(sizeof(*dict_stream), GFP_KERNEL);
	if (!dict_stream)
		return -ENOMEM;

	ret = LZ4_loadDict(dict_stream, (const char *)params->dict,
			   (int)params->dict_sz);
	if (ret != (int)params->dict_sz) {
		kfree(dict_stream);
		return -EINVAL;
	}
	params->drv_data = dict_stream;
	return 0;
}

static void lz4_destroy(struct zcomp_ctx *ctx)
{
	struct lz4_ctx *zctx = ctx->context;

	if (!zctx)
		return;
	vfree(zctx->mem);
	kfree(zctx->dstrm);
	kfree(zctx->cstrm);
	kfree(zctx);
	ctx->context = NULL;
}

static int lz4_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct lz4_ctx *zctx;

	zctx = kzalloc(sizeof(*zctx), GFP_KERNEL);
	if (!zctx)
		return -ENOMEM;
	ctx->context = zctx;

	if (!params->dict) {
		zctx->mem = vmalloc(LZ4_MEM_COMPRESS);
		if (!zctx->mem)
			goto error;
	} else {
		zctx->dstrm = kzalloc(sizeof(*zctx->dstrm), GFP_KERNEL);
		zctx->cstrm = kzalloc(sizeof(*zctx->cstrm), GFP_KERNEL);
		if (!zctx->dstrm || !zctx->cstrm)
			goto error;
	}
	return 0;

error:
	lz4_destroy(ctx);
	return -ENOMEM;
}

static int lz4_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			struct zcomp_req *req)
{
	struct lz4_ctx *zctx = ctx->context;
	int ret;

	if (!zctx->cstrm) {
		ret = LZ4_compress_fast((const char *)req->src,
					(char *)req->dst, (int)req->src_len,
					(int)req->dst_len, params->level,
					zctx->mem);
	} else {
		memcpy(zctx->cstrm, params->drv_data, sizeof(*zctx->cstrm));
		ret = LZ4_compress_fast_continue(zctx->cstrm,
						 (const char *)req->src,
						 (char *)req->dst,
						 (int)req->src_len,
						 (int)req->dst_len,
						 params->level);
	}
	if (!ret)
		return -ENOSPC;
	req->dst_len = ret;
	return 0;
}

static int lz4_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			  struct zcomp_req *req)
{
	struct lz4_ctx *zctx = ctx->context;
	int ret;

	if (!zctx->dstrm) {
		ret = LZ4_decompress_safe((const char *)req->src,
					  (char *)req->dst,
					  (int)req->src_len,
					  (int)req->dst_len);
	} else {
		ret = LZ4_setStreamDecode(zctx->dstrm,
					  (const char *)params->dict,
					  (int)params->dict_sz);
		if (!ret)
			return -EINVAL;
		ret = LZ4_decompress_safe_continue(zctx->dstrm,
						   (const char *)req->src,
						   (char *)req->dst,
						   (int)req->src_len,
						   (int)req->dst_len);
	}
	return ret == (int)req->dst_len ? 0 : -EINVAL;
}

const struct zcomp_ops backend_lz4 = {
	.compress	= lz4_compress,
	.decompress	= lz4_decompress,
	.create_ctx	= lz4_create,
	.destroy_ctx	= lz4_destroy,
	.setup_params	= lz4_setup_params,
	.release_params	= lz4_release_params,
	.name		= "lz4",
};
