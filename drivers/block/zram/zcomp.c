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
	/*
	 * Keep one extra page so backends which add framing or padding can
	 * report an oversized result and let ZRAM store the original page.
	 */
	zstrm->buffer = vzalloc(2 * PAGE_SIZE);
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

const char *zcomp_lookup_backend_name(const char *comp)
{
	const struct zcomp_ops *backend = lookup_backend_ops(comp);

	return backend ? backend->name : NULL;
}

bool zcomp_available_algorithm(const char *comp)
{
	return lookup_backend_ops(comp) != NULL;
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
		.dst_len = 2 * PAGE_SIZE,
	};
	int ret;

	might_sleep();
	ret = comp->ops->compress(comp->params, &zstrm->ctx, &req);
	if (!ret)
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
	int ret, cpu;

	comp->stream = alloc_percpu(struct zcomp_strm);
	if (!comp->stream)
		return -ENOMEM;

	comp->params = params;
	ret = comp->ops->setup_params(params);
	if (ret)
		goto release_params;

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
