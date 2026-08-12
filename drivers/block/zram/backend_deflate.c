// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/zlib.h>

#include "backend_deflate.h"

#define DEFLATE_DEF_WINBITS	(-11)
#define DEFLATE_DEF_MEMLEVEL	MAX_MEM_LEVEL

struct deflate_ctx {
	struct z_stream_s cctx;
	struct z_stream_s dctx;
};

static size_t deflate_compress_bound(const struct zcomp_params *params,
				     size_t src_len)
{
	return deflateBound(src_len);
}

static void deflate_release_params(struct zcomp_params *params)
{
	params->drv_data = NULL;
}

static int deflate_validate_params(const struct zcomp_params *params)
{
	s32 winbits = params->deflate.winbits;

	if (params->level != ZCOMP_PARAM_NOT_SET &&
	    (params->level < Z_DEFAULT_COMPRESSION ||
	     params->level > Z_BEST_COMPRESSION))
		return -EINVAL;

	if (winbits == ZCOMP_PARAM_NOT_SET)
		return 0;
	if ((winbits < 8 || winbits > MAX_WBITS) &&
	    (winbits < -MAX_WBITS || winbits > -8))
		return -EINVAL;
	return 0;
}

static int deflate_setup_params(struct zcomp_params *params)
{
	int ret = deflate_validate_params(params);

	if (ret)
		return ret;
	if (params->level == ZCOMP_PARAM_NOT_SET)
		params->level = Z_DEFAULT_COMPRESSION;
	if (params->deflate.winbits == ZCOMP_PARAM_NOT_SET)
		params->deflate.winbits = DEFLATE_DEF_WINBITS;
	return 0;
}

static void deflate_destroy(struct zcomp_ctx *ctx)
{
	struct deflate_ctx *zctx = ctx->context;

	if (!zctx)
		return;
	if (zctx->cctx.workspace) {
		zlib_deflateEnd(&zctx->cctx);
		vfree(zctx->cctx.workspace);
	}
	if (zctx->dctx.workspace) {
		zlib_inflateEnd(&zctx->dctx);
		vfree(zctx->dctx.workspace);
	}
	kfree(zctx);
	ctx->context = NULL;
}

static int deflate_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct deflate_ctx *zctx;
	size_t sz;
	int ret;

	zctx = kzalloc(sizeof(*zctx), GFP_KERNEL);
	if (!zctx)
		return -ENOMEM;
	ctx->context = zctx;

	sz = zlib_deflate_workspacesize(params->deflate.winbits,
					DEFLATE_DEF_MEMLEVEL);
	zctx->cctx.workspace = vzalloc(sz);
	if (!zctx->cctx.workspace)
		goto nomem;
	ret = zlib_deflateInit2(&zctx->cctx, params->level, Z_DEFLATED,
				params->deflate.winbits, DEFLATE_DEF_MEMLEVEL,
				Z_DEFAULT_STRATEGY);
	if (ret != Z_OK)
		goto invalid;

	sz = zlib_inflate_workspacesize();
	zctx->dctx.workspace = vzalloc(sz);
	if (!zctx->dctx.workspace)
		goto nomem;
	ret = zlib_inflateInit2(&zctx->dctx, params->deflate.winbits);
	if (ret != Z_OK)
		goto invalid;
	return 0;

nomem:
	deflate_destroy(ctx);
	return -ENOMEM;
invalid:
	deflate_destroy(ctx);
	return -EINVAL;
}

static int deflate_compress(struct zcomp_params *params,
			    struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	struct z_stream_s *stream = &((struct deflate_ctx *)ctx->context)->cctx;
	int ret;

	ret = zlib_deflateReset(stream);
	if (ret != Z_OK)
		return -EINVAL;
	stream->next_in = (u8 *)req->src;
	stream->avail_in = req->src_len;
	stream->next_out = req->dst;
	stream->avail_out = req->dst_len;

	ret = zlib_deflate(stream, Z_FINISH);
	if (ret == Z_BUF_ERROR)
		return -ENOSPC;
	if (ret != Z_STREAM_END)
		return -EINVAL;
	req->dst_len = stream->total_out;
	return 0;
}

static int deflate_decompress(struct zcomp_params *params,
			      struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	struct z_stream_s *stream = &((struct deflate_ctx *)ctx->context)->dctx;
	size_t expected = req->dst_len;
	int ret;

	ret = zlib_inflateReset(stream);
	if (ret != Z_OK)
		return -EINVAL;
	stream->next_in = (u8 *)req->src;
	stream->avail_in = req->src_len;
	stream->next_out = req->dst;
	stream->avail_out = req->dst_len;

	ret = zlib_inflate(stream, Z_SYNC_FLUSH);
	if (ret != Z_STREAM_END || stream->total_out != expected)
		return -EINVAL;
	return 0;
}

const struct zcomp_ops backend_deflate = {
	.abi_version	= ZCOMP_BACKEND_ABI_VERSION,
	.backend_id	= ZCOMP_BACKEND_DEFLATE,
	.exec_class	= ZCOMP_EXEC_BALANCED,
	.capabilities	= ZCOMP_CAP_COMPRESS | ZCOMP_CAP_DECOMPRESS |
			  ZCOMP_CAP_PERCPU_CONTEXT |
			  ZCOMP_CAP_BOUNDED_OUTPUT,
	.param_caps	= ZCOMP_PARAM_LEVEL | ZCOMP_PARAM_WINBITS,
	.name		= "deflate",
	.compress_bound	= deflate_compress_bound,
	.validate_params = deflate_validate_params,
	.compress	= deflate_compress,
	.decompress	= deflate_decompress,
	.create_ctx	= deflate_create,
	.destroy_ctx	= deflate_destroy,
	.setup_params	= deflate_setup_params,
	.release_params	= deflate_release_params,
};
