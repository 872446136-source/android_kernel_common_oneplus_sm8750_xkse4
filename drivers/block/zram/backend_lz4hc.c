// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/lz4.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "backend_lz4hc.h"

struct lz4hc_ctx {
	void *mem;
	LZ4_streamDecode_t *dstrm;
	LZ4_streamHC_t *cstrm;
};

static size_t lz4hc_compress_bound(const struct zcomp_params *params,
				   size_t src_len)
{
	return LZ4_compressBound(src_len);
}

static void lz4hc_release_params(struct zcomp_params *params)
{
	params->drv_data = NULL;
}

static int lz4hc_validate_params(const struct zcomp_params *params)
{
	if (params->dict_sz > INT_MAX)
		return -EINVAL;

	if (params->level != ZCOMP_PARAM_NOT_SET &&
	    (params->level < LZ4HC_MIN_CLEVEL ||
	     params->level > LZ4HC_MAX_CLEVEL))
		return -EINVAL;
	return 0;
}

static int lz4hc_setup_params(struct zcomp_params *params)
{
	int ret = lz4hc_validate_params(params);

	if (ret)
		return ret;
	if (params->level == ZCOMP_PARAM_NOT_SET)
		params->level = LZ4HC_DEFAULT_CLEVEL;
	return 0;
}

static void lz4hc_destroy(struct zcomp_ctx *ctx)
{
	struct lz4hc_ctx *zctx = ctx->context;

	if (!zctx)
		return;
	kfree(zctx->dstrm);
	kfree(zctx->cstrm);
	vfree(zctx->mem);
	kfree(zctx);
	ctx->context = NULL;
}

static int lz4hc_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct lz4hc_ctx *zctx;

	zctx = kzalloc(sizeof(*zctx), GFP_KERNEL);
	if (!zctx)
		return -ENOMEM;
	ctx->context = zctx;

	if (!params->dict) {
		zctx->mem = vmalloc(LZ4HC_MEM_COMPRESS);
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
	lz4hc_destroy(ctx);
	return -ENOMEM;
}

static int lz4hc_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			  struct zcomp_req *req)
{
	struct lz4hc_ctx *zctx = ctx->context;
	int ret;

	if (!zctx->cstrm) {
		ret = LZ4_compress_HC((const char *)req->src,
				      (char *)req->dst, (int)req->src_len,
				      (int)req->dst_len, params->level,
				      zctx->mem);
	} else {
		LZ4_resetStreamHC(zctx->cstrm, params->level);
		ret = LZ4_loadDictHC(zctx->cstrm,
				     (const char *)params->dict,
				     (int)params->dict_sz);
		if (ret != (int)params->dict_sz)
			return -EINVAL;
		ret = LZ4_compress_HC_continue(zctx->cstrm,
					       (const char *)req->src,
					       (char *)req->dst,
					       (int)req->src_len,
					       (int)req->dst_len);
	}
	if (!ret)
		return -ENOSPC;
	req->dst_len = ret;
	return 0;
}

static int lz4hc_decompress(struct zcomp_params *params,
			    struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	struct lz4hc_ctx *zctx = ctx->context;
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

const struct zcomp_ops backend_lz4hc = {
	.abi_version	= ZCOMP_BACKEND_ABI_VERSION,
	.backend_id	= ZCOMP_BACKEND_LZ4HC,
	.exec_class	= ZCOMP_EXEC_HIGH_RATIO,
	.capabilities	= ZCOMP_CAP_COMPRESS | ZCOMP_CAP_DECOMPRESS |
			  ZCOMP_CAP_PERCPU_CONTEXT |
			  ZCOMP_CAP_BOUNDED_OUTPUT,
	.param_caps	= ZCOMP_PARAM_LEVEL | ZCOMP_PARAM_DICTIONARY,
	.name		= "lz4hc",
	.compress_bound	= lz4hc_compress_bound,
	.validate_params = lz4hc_validate_params,
	.compress	= lz4hc_compress,
	.decompress	= lz4hc_decompress,
	.create_ctx	= lz4hc_create,
	.destroy_ctx	= lz4hc_destroy,
	.setup_params	= lz4hc_setup_params,
	.release_params	= lz4hc_release_params,
};
