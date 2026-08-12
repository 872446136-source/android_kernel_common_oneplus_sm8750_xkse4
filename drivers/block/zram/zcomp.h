/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2014 Sergey Senozhatsky.
 */

#ifndef _ZCOMP_H_
#define _ZCOMP_H_

#include <linux/bits.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define ZCOMP_PARAM_NOT_SET	INT_MIN
#define ZCOMP_BACKEND_ABI_VERSION	2

/* Stable backend identities. Never derive an on-disk decoder from policy. */
enum zcomp_backend_id {
	ZCOMP_BACKEND_INVALID = 0,
	ZCOMP_BACKEND_LZO,
	ZCOMP_BACKEND_LZO_RLE,
	ZCOMP_BACKEND_LZ4,
	ZCOMP_BACKEND_LZ4HC,
	ZCOMP_BACKEND_LZ4K,
	ZCOMP_BACKEND_LZ4KD,
	ZCOMP_BACKEND_ZSTD,
	ZCOMP_BACKEND_DEFLATE,
	ZCOMP_BACKEND_842,
	ZCOMP_BACKEND_MAX,
};

enum zcomp_backend_capability {
	ZCOMP_CAP_COMPRESS		= BIT(0),
	ZCOMP_CAP_DECOMPRESS		= BIT(1),
	ZCOMP_CAP_PERCPU_CONTEXT	= BIT(2),
	ZCOMP_CAP_BOUNDED_OUTPUT	= BIT(3),
	ZCOMP_CAP_PREPARED_PARAMS	= BIT(4),
	ZCOMP_CAP_4K_ONLY		= BIT(5),
};

enum zcomp_param_capability {
	ZCOMP_PARAM_LEVEL	= BIT(0),
	ZCOMP_PARAM_DICTIONARY	= BIT(1),
	ZCOMP_PARAM_WINBITS	= BIT(2),
};

enum zcomp_exec_class {
	ZCOMP_EXEC_FAST = 0,
	ZCOMP_EXEC_BALANCED,
	ZCOMP_EXEC_HIGH_RATIO,
	ZCOMP_EXEC_MAX,
};

struct deflate_params {
	s32 winbits;
};

/*
 * Immutable backend parameters shared by all per-CPU execution contexts.
 * A backend may attach its own prepared representation to ->drv_data.
 */
struct zcomp_params {
	void *dict;
	size_t dict_sz;
	s32 level;
	union {
		struct deflate_params deflate;
	};
	void *drv_data;
};

/* Mutable per-CPU backend context. */
struct zcomp_ctx {
	void *context;
};

struct zcomp_strm {
	struct mutex lock;
	/* compression output buffer */
	void *buffer;
	/* local copy for zsmalloc objects crossing pages */
	void *local_copy;
	struct zcomp_ctx ctx;
};

struct zcomp_req {
	const unsigned char *src;
	const size_t src_len;
	unsigned char *dst;
	size_t dst_len;
};

struct zcomp_ops {
	u16 abi_version;
	u8 backend_id;
	u8 exec_class;
	u32 capabilities;
	u32 param_caps;
	const char *name;

	size_t (*compress_bound)(const struct zcomp_params *params,
				 size_t src_len);
	int (*validate_params)(const struct zcomp_params *params);
	int (*setup_params)(struct zcomp_params *params);
	void (*release_params)(struct zcomp_params *params);

	int (*compress)(struct zcomp_params *params, struct zcomp_ctx *ctx,
			struct zcomp_req *req);
	int (*decompress)(struct zcomp_params *params, struct zcomp_ctx *ctx,
			  struct zcomp_req *req);
	int (*create_ctx)(struct zcomp_params *params, struct zcomp_ctx *ctx);
	void (*destroy_ctx)(struct zcomp_ctx *ctx);
};

struct zcomp {
	struct zcomp_strm __percpu *stream;
	const struct zcomp_ops *ops;
	struct zcomp_params *params;
	size_t buffer_size;
	struct hlist_node node;
};

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node);
int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node);
ssize_t zcomp_available_show(const char *comp, char *buf, ssize_t at);
bool zcomp_available_algorithm(const char *comp);
const char *zcomp_lookup_backend_name(const char *comp);
int zcomp_validate_params(const char *comp,
			  const struct zcomp_params *params);

struct zcomp *zcomp_create(const char *alg, struct zcomp_params *params);
void zcomp_destroy(struct zcomp *comp);

u8 zcomp_backend_id(const struct zcomp *comp);
u32 zcomp_capabilities(const struct zcomp *comp);
u32 zcomp_param_capabilities(const struct zcomp *comp);
enum zcomp_exec_class zcomp_execution_class(const struct zcomp *comp);
size_t zcomp_compress_bound(const struct zcomp *comp);

struct zcomp_strm *zcomp_stream_get(struct zcomp *comp);
void zcomp_stream_put(struct zcomp_strm *zstrm);

int zcomp_compress(struct zcomp *comp, struct zcomp_strm *zstrm,
		   const void *src, unsigned int *dst_len);
int zcomp_decompress(struct zcomp *comp, struct zcomp_strm *zstrm,
		     const void *src, unsigned int src_len, void *dst);

#endif /* _ZCOMP_H_ */
