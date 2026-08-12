/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_LZ4KD_H
#define _LINUX_LZ4KD_H

#include <linux/gfp_types.h>
#include <linux/types.h>

enum lz4kd_status {
	LZ4K_STATUS_INCOMPRESSIBLE = 0,
	LZ4K_STATUS_FAILED = -1,
	LZ4K_STATUS_READ_ERROR = -2,
	LZ4K_STATUS_WRITE_ERROR = -3,
};

const char *lz4kd_version(void);
unsigned int lz4kd_encode_state_bytes_min(void);
int lz4kd_encode(void *state, const void *in, void *out,
		 unsigned int in_max, unsigned int out_max,
		 unsigned int out_limit);
int lz4kd_decode(const void *in, void *out,
		 unsigned int in_max, unsigned int out_max);

#define LZ4KD_DELTA_MAGIC	0x444b344cU
#define LZ4KD_DELTA_VERSION	1U
#define LZ4KD_DELTA_KIND		1U
#define LZ4KD_DELTA_PAGE_SIZE	4096U

struct lz4kd_delta_header {
	__le32 magic;
	__le16 version;
	__le16 header_size;
	u8 kind;
	u8 flags;
	__le16 reserved;
	__le32 reference_id;
	__le32 reference_generation;
	__le32 reference_logical_size;
	__le32 target_logical_size;
	__le32 payload_size;
	__le32 reference_hash;
	__le32 target_hash;
	__le32 wire_hash;
} __packed;

struct lz4kd_delta_ctx {
	void *workspace;
	size_t workspace_size;
};

size_t lz4kd_delta_bound(size_t logical_size);
u32 lz4kd_delta_hash(const void *data, size_t len);
int lz4kd_delta_ctx_init(struct lz4kd_delta_ctx *ctx, gfp_t gfp);
void lz4kd_delta_ctx_release(struct lz4kd_delta_ctx *ctx);
int lz4kd_delta_encode(struct lz4kd_delta_ctx *ctx,
			const void *reference, size_t reference_len,
			const void *target, size_t target_len,
			u32 reference_id, u32 reference_generation,
			void *wire, size_t wire_capacity,
			size_t *wire_len);
int lz4kd_delta_validate(const void *wire, size_t wire_len,
			 u32 reference_id, u32 reference_generation);
int lz4kd_delta_decode(struct lz4kd_delta_ctx *ctx,
			const void *wire, size_t wire_len,
			const void *reference, size_t reference_len,
			void *target, size_t target_len);

#endif
