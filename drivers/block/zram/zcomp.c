// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2014 Sergey Senozhatsky.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/cpuhotplug.h>
#include <linux/vmalloc.h>
#include <linux/sysfs.h>

#include "zcomp.h"

#include "backend_lzo.h"
#include "backend_lzorle.h"
#include "backend_lz4.h"
#include "backend_lz4hc.h"
#include "backend_lz4k.h"
#include "backend_lz4kd.h"
#include "backend_zstd.h"
#include "backend_deflate.h"
#include "backend_842.h"

static const struct zcomp_ops * const backends[] = {
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_LZO)
	&backend_lzorle,
	&backend_lzo,
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_LZ4)
	&backend_lz4,
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_LZ4HC)
	&backend_lz4hc,
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_LZ4K)
	&backend_lz4k,
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_LZ4KD)
	&backend_lz4kd,
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_ZSTD)
	&backend_zstd,
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_DEFLATE)
	&backend_deflate,
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_842)
	&backend_842,
#endif
	NULL
};

static void zcomp_strm_free(struct zcomp *comp, struct zcomp_strm *zstrm)
{
	comp->ops->destroy_ctx(&zstrm->ctx);
	vfree(zstrm->local_copy);
	vfree(zstrm->buffer);
	zstrm->ctx.context = NULL;
	zstrm->local_copy = NULL;
	zstrm->buffer = NULL;
}

static int zcomp_strm_init(struct zcomp *comp, struct zcomp_strm *zstrm)
{
	int ret;

	ret = comp->ops->create_ctx(comp->params, &zstrm->ctx);
	if (ret)
		return ret;

	zstrm->local_copy = vzalloc(PAGE_SIZE);
	/* The backend ABI supplies the exact safe output capacity. */
	zstrm->buffer = vzalloc(comp->buffer_size);
	if (!zstrm->buffer || !zstrm->local_copy) {
		zcomp_strm_free(comp, zstrm);
		return -ENOMEM;
	}
	return 0;
}

static const struct zcomp_ops *lookup_backend_ops(const char *comp)
{
	int i = 0;

	while (backends[i]) {
		if (sysfs_streq(comp, backends[i]->name))
			break;
		i++;
	}
	return backends[i];
}

static int zcomp_validate_backend(const struct zcomp_ops *ops)
{
	u32 required = ZCOMP_CAP_COMPRESS | ZCOMP_CAP_DECOMPRESS |
		       ZCOMP_CAP_PERCPU_CONTEXT | ZCOMP_CAP_BOUNDED_OUTPUT;
	u32 supported = required | ZCOMP_CAP_PREPARED_PARAMS |
			ZCOMP_CAP_4K_ONLY;
	int i;

	if (ops->abi_version != ZCOMP_BACKEND_ABI_VERSION ||
	    !ops->name || !ops->name[0] ||
	    ops->backend_id <= ZCOMP_BACKEND_INVALID ||
	    ops->backend_id >= ZCOMP_BACKEND_MAX ||
	    ops->exec_class >= ZCOMP_EXEC_MAX ||
	    (ops->capabilities & required) != required ||
	    ops->capabilities & ~supported ||
	    ((ops->capabilities & ZCOMP_CAP_4K_ONLY) && PAGE_SHIFT != 12) ||
	    ops->param_caps & ~(ZCOMP_PARAM_LEVEL |
				ZCOMP_PARAM_DICTIONARY |
				ZCOMP_PARAM_WINBITS) ||
	    !ops->compress_bound || !ops->setup_params ||
	    !ops->release_params || !ops->compress || !ops->decompress ||
	    !ops->create_ctx || !ops->destroy_ctx)
		return -EINVAL;
	for (i = 0; backends[i]; i++) {
		if (backends[i] == ops)
			continue;
		if (!backends[i]->name)
			return -EINVAL;
		if (backends[i]->backend_id == ops->backend_id ||
		    !strcmp(backends[i]->name, ops->name))
			return -EEXIST;
	}

	return 0;
}

static int zcomp_validate_backend_params(const struct zcomp_ops *ops,
					 const struct zcomp_params *params)
{
	if (!params || params->drv_data ||
	    (!!params->dict != !!params->dict_sz))
		return -EINVAL;
	if (!(ops->param_caps & ZCOMP_PARAM_LEVEL) &&
	    params->level != ZCOMP_PARAM_NOT_SET)
		return -EINVAL;
	if (!(ops->param_caps & ZCOMP_PARAM_DICTIONARY) &&
	    (params->dict || params->dict_sz))
		return -EINVAL;
	if (!(ops->param_caps & ZCOMP_PARAM_WINBITS) &&
	    params->deflate.winbits != ZCOMP_PARAM_NOT_SET)
		return -EINVAL;
	if (ops->validate_params)
		return ops->validate_params(params);
	return 0;
}

const char *zcomp_lookup_backend_name(const char *comp)
{
	const struct zcomp_ops *backend = lookup_backend_ops(comp);

	return backend ? backend->name : NULL;
}

bool zcomp_available_algorithm(const char *comp)
{
	return lookup_backend_ops(comp) != NULL;
}

int zcomp_validate_params(const char *comp,
			  const struct zcomp_params *params)
{
	const struct zcomp_ops *ops = lookup_backend_ops(comp);
	int ret;

	if (!ops)
		return -EINVAL;
	ret = zcomp_validate_backend(ops);
	if (ret)
		return ret;
	return zcomp_validate_backend_params(ops, params);
}

ssize_t zcomp_available_show(const char *comp, char *buf, ssize_t at)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(backends) - 1; i++) {
		if (!strcmp(comp, backends[i]->name))
			at += sysfs_emit_at(buf, at, "[%s] ",
					    backends[i]->name);
		else
			at += sysfs_emit_at(buf, at, "%s ", backends[i]->name);
	}

	at += sysfs_emit_at(buf, at, "\n");
	return at;
}

struct zcomp_strm *zcomp_stream_get(struct zcomp *comp)
{
	for (;;) {
		struct zcomp_strm *zstrm = raw_cpu_ptr(comp->stream);

		/*
		 * The mutex prevents cpu_dead() from releasing a selected stream.
		 * Migration can happen before the lock is acquired, so retry if
		 * that CPU's context has already been torn down.
		 */
		mutex_lock(&zstrm->lock);
		if (likely(zstrm->buffer))
			return zstrm;
		mutex_unlock(&zstrm->lock);
	}
}

void zcomp_stream_put(struct zcomp_strm *zstrm)
{
	mutex_unlock(&zstrm->lock);
}

int zcomp_compress(struct zcomp *comp, struct zcomp_strm *zstrm,
		   const void *src, unsigned int *dst_len)
{
	struct zcomp_req req = {
		.src = src,
		.src_len = PAGE_SIZE,
		.dst = zstrm->buffer,
		.dst_len = comp->buffer_size,
	};
	int ret;

	might_sleep();
	ret = comp->ops->compress(comp->params, &zstrm->ctx, &req);
	if (ret)
		return ret;
	if (WARN_ON_ONCE(!req.dst_len || req.dst_len > comp->buffer_size))
		return -EOVERFLOW;
	*dst_len = req.dst_len;
	return ret;
}

int zcomp_decompress(struct zcomp *comp, struct zcomp_strm *zstrm,
		     const void *src, unsigned int src_len, void *dst)
{
	struct zcomp_req req = {
		.src = src,
		.src_len = src_len,
		.dst = dst,
		.dst_len = PAGE_SIZE,
	};

	might_sleep();
	return comp->ops->decompress(comp->params, &zstrm->ctx, &req);
}

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp, node);
	struct zcomp_strm *zstrm = per_cpu_ptr(comp->stream, cpu);
	int ret;

	ret = zcomp_strm_init(comp, zstrm);
	if (ret)
		pr_err("Can't allocate a compression stream\n");
	return ret;
}

int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp, node);
	struct zcomp_strm *zstrm = per_cpu_ptr(comp->stream, cpu);

	mutex_lock(&zstrm->lock);
	zcomp_strm_free(comp, zstrm);
	mutex_unlock(&zstrm->lock);
	return 0;
}

static int zcomp_init(struct zcomp *comp, struct zcomp_params *params)
{
	size_t bound;
	int ret, cpu;

	ret = zcomp_validate_backend(comp->ops);
	if (ret)
		return ret;
	ret = zcomp_validate_backend_params(comp->ops, params);
	if (ret)
		return ret;

	comp->stream = alloc_percpu(struct zcomp_strm);
	if (!comp->stream)
		return -ENOMEM;

	comp->params = params;
	ret = comp->ops->setup_params(params);
	if (ret)
		goto release_params;

	bound = comp->ops->compress_bound(params, PAGE_SIZE);
	if (bound < PAGE_SIZE || bound > 2 * PAGE_SIZE) {
		ret = -EINVAL;
		goto release_params;
	}
	comp->buffer_size = bound;

	for_each_possible_cpu(cpu)
		mutex_init(&per_cpu_ptr(comp->stream, cpu)->lock);

	ret = cpuhp_state_add_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	if (ret < 0)
		goto release_params;

	return 0;

release_params:
	comp->ops->release_params(params);
	free_percpu(comp->stream);
	comp->stream = NULL;
	return ret;
}

void zcomp_destroy(struct zcomp *comp)
{
	cpuhp_state_remove_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	comp->ops->release_params(comp->params);
	free_percpu(comp->stream);
	kfree(comp);
}

struct zcomp *zcomp_create(const char *alg, struct zcomp_params *params)
{
	struct zcomp *comp;
	int ret;

	BUILD_BUG_ON(ARRAY_SIZE(backends) <= 1);

	comp = kzalloc(sizeof(*comp), GFP_KERNEL);
	if (!comp)
		return ERR_PTR(-ENOMEM);

	comp->ops = lookup_backend_ops(alg);
	if (!comp->ops) {
		kfree(comp);
		return ERR_PTR(-EINVAL);
	}

	ret = zcomp_init(comp, params);
	if (ret) {
		kfree(comp);
		return ERR_PTR(ret);
	}
	return comp;
}

u8 zcomp_backend_id(const struct zcomp *comp)
{
	return comp->ops->backend_id;
}

u32 zcomp_capabilities(const struct zcomp *comp)
{
	return comp->ops->capabilities;
}

u32 zcomp_param_capabilities(const struct zcomp *comp)
{
	return comp->ops->param_caps;
}

enum zcomp_exec_class zcomp_execution_class(const struct zcomp *comp)
{
	return comp->ops->exec_class;
}

size_t zcomp_compress_bound(const struct zcomp *comp)
{
	return comp->buffer_size;
}
