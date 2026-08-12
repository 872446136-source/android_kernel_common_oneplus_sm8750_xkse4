// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/lz4kd.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>

struct lz4kd_delta_run {
	__le16 offset;
	__le16 length;
} __packed;

#define LZ4KD_DELTA_HASH_INIT	2166136261U
#define LZ4KD_DELTA_HASH_PRIME	16777619U

static u32 lz4kd_delta_hash_update(u32 hash, const u8 *data, size_t len)
{
	while (len--) {
		hash ^= *data++;
		hash *= LZ4KD_DELTA_HASH_PRIME;
	}
	return hash;
}

u32 lz4kd_delta_hash(const void *data, size_t len)
{
	if (!data && len)
		return 0;
	return lz4kd_delta_hash_update(LZ4KD_DELTA_HASH_INIT, data, len);
}
EXPORT_SYMBOL_GPL(lz4kd_delta_hash);

static u32 lz4kd_delta_wire_hash(const void *wire, size_t wire_len)
{
	const size_t hash_offset = offsetof(struct lz4kd_delta_header,
					    wire_hash);
	const u8 *bytes = wire;
	u32 hash = LZ4KD_DELTA_HASH_INIT;

	hash = lz4kd_delta_hash_update(hash, bytes, hash_offset);
	if (wire_len > hash_offset + sizeof(__le32))
		hash = lz4kd_delta_hash_update(hash,
			bytes + hash_offset + sizeof(__le32),
			wire_len - hash_offset - sizeof(__le32));
	return hash;
}

size_t lz4kd_delta_bound(size_t logical_size)
{
	size_t records;
	size_t bound;

	if (logical_size != LZ4KD_DELTA_PAGE_SIZE)
		return 0;
	records = DIV_ROUND_UP(logical_size, 2);
	if (check_mul_overflow(records, sizeof(struct lz4kd_delta_run),
			       &bound) ||
	    check_add_overflow(bound, logical_size, &bound) ||
	    check_add_overflow(bound, sizeof(struct lz4kd_delta_header),
			       &bound))
		return 0;
	return bound;
}
EXPORT_SYMBOL_GPL(lz4kd_delta_bound);

int lz4kd_delta_ctx_init(struct lz4kd_delta_ctx *ctx, gfp_t gfp)
{
	if (!ctx || PAGE_SIZE != LZ4KD_DELTA_PAGE_SIZE)
		return -EINVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->workspace = kmalloc(LZ4KD_DELTA_PAGE_SIZE, gfp);
	if (!ctx->workspace)
		return -ENOMEM;
	ctx->workspace_size = LZ4KD_DELTA_PAGE_SIZE;
	return 0;
}
EXPORT_SYMBOL_GPL(lz4kd_delta_ctx_init);

void lz4kd_delta_ctx_release(struct lz4kd_delta_ctx *ctx)
{
	if (!ctx)
		return;
	kfree(ctx->workspace);
	ctx->workspace = NULL;
	ctx->workspace_size = 0;
}
EXPORT_SYMBOL_GPL(lz4kd_delta_ctx_release);

int lz4kd_delta_encode(struct lz4kd_delta_ctx *ctx,
			const void *reference, size_t reference_len,
			const void *target, size_t target_len,
			u32 reference_id, u32 reference_generation,
			void *wire, size_t wire_capacity,
			size_t *wire_len)
{
	struct lz4kd_delta_header *header = wire;
	const u8 *ref = reference;
	const u8 *src = target;
	u8 *cursor;
	size_t payload_capacity;
	size_t pos = 0;

	if (!ctx || !ctx->workspace ||
	    ctx->workspace_size != LZ4KD_DELTA_PAGE_SIZE ||
	    !reference || !target || !wire || !wire_len ||
	    reference_len != LZ4KD_DELTA_PAGE_SIZE ||
	    target_len != LZ4KD_DELTA_PAGE_SIZE ||
	    !reference_id || !reference_generation ||
	    wire_capacity < sizeof(*header))
		return -EINVAL;

	memset(header, 0, sizeof(*header));
	cursor = (u8 *)wire + sizeof(*header);
	payload_capacity = wire_capacity - sizeof(*header);
	while (pos < target_len) {
		struct lz4kd_delta_run run;
		size_t start;
		size_t length;
		size_t record_size;

		while (pos < target_len && ref[pos] == src[pos])
			pos++;
		if (pos == target_len)
			break;
		start = pos;
		while (pos < target_len && ref[pos] != src[pos])
			pos++;
		length = pos - start;
		if (!length || start > U16_MAX || length > U16_MAX ||
		    check_add_overflow(sizeof(run), length, &record_size) ||
		    record_size > payload_capacity)
			return -ENOSPC;
		run.offset = cpu_to_le16(start);
		run.length = cpu_to_le16(length);
		memcpy(cursor, &run, sizeof(run));
		memcpy(cursor + sizeof(run), src + start, length);
		cursor += record_size;
		payload_capacity -= record_size;
	}

	*wire_len = cursor - (u8 *)wire;
	header->magic = cpu_to_le32(LZ4KD_DELTA_MAGIC);
	header->version = cpu_to_le16(LZ4KD_DELTA_VERSION);
	header->header_size = cpu_to_le16(sizeof(*header));
	header->kind = LZ4KD_DELTA_KIND;
	header->reference_id = cpu_to_le32(reference_id);
	header->reference_generation = cpu_to_le32(reference_generation);
	header->reference_logical_size = cpu_to_le32(reference_len);
	header->target_logical_size = cpu_to_le32(target_len);
	header->payload_size = cpu_to_le32(*wire_len - sizeof(*header));
	header->reference_hash = cpu_to_le32(
		lz4kd_delta_hash(reference, reference_len));
	header->target_hash = cpu_to_le32(
		lz4kd_delta_hash(target, target_len));
	header->wire_hash = cpu_to_le32(lz4kd_delta_wire_hash(wire, *wire_len));
	return 0;
}
EXPORT_SYMBOL_GPL(lz4kd_delta_encode);

int lz4kd_delta_validate(const void *wire, size_t wire_len,
			 u32 reference_id, u32 reference_generation)
{
	const struct lz4kd_delta_header *header = wire;
	const u8 *cursor;
	const u8 *end;
	size_t previous_end = 0;
	size_t expected;

	if (!wire || wire_len < sizeof(*header) ||
	    wire_len > lz4kd_delta_bound(LZ4KD_DELTA_PAGE_SIZE))
		return -EINVAL;
	if (le32_to_cpu(header->magic) != LZ4KD_DELTA_MAGIC ||
	    le16_to_cpu(header->version) != LZ4KD_DELTA_VERSION ||
	    le16_to_cpu(header->header_size) != sizeof(*header) ||
	    header->kind != LZ4KD_DELTA_KIND || header->flags ||
	    le16_to_cpu(header->reserved) ||
	    !le32_to_cpu(header->reference_id) ||
	    !le32_to_cpu(header->reference_generation) ||
	    le32_to_cpu(header->reference_logical_size) !=
		LZ4KD_DELTA_PAGE_SIZE ||
	    le32_to_cpu(header->target_logical_size) !=
		LZ4KD_DELTA_PAGE_SIZE)
		return -EINVAL;
	if (reference_id && le32_to_cpu(header->reference_id) != reference_id)
		return -ESTALE;
	if (reference_generation &&
	    le32_to_cpu(header->reference_generation) != reference_generation)
		return -ESTALE;
	if (check_add_overflow(sizeof(*header),
			       (size_t)le32_to_cpu(header->payload_size),
			       &expected) || expected != wire_len)
		return -EINVAL;
	if (le32_to_cpu(header->wire_hash) !=
	    lz4kd_delta_wire_hash(wire, wire_len))
		return -EBADMSG;
	cursor = (const u8 *)wire + sizeof(*header);
	end = (const u8 *)wire + wire_len;
	while (cursor < end) {
		struct lz4kd_delta_run run;
		size_t offset;
		size_t length;

		if ((size_t)(end - cursor) < sizeof(run))
			return -EINVAL;
		memcpy(&run, cursor, sizeof(run));
		cursor += sizeof(run);
		offset = le16_to_cpu(run.offset);
		length = le16_to_cpu(run.length);
		if (!length || offset < previous_end ||
		    offset >= LZ4KD_DELTA_PAGE_SIZE ||
		    length > LZ4KD_DELTA_PAGE_SIZE - offset ||
		    length > (size_t)(end - cursor))
			return -EINVAL;
		cursor += length;
		previous_end = offset + length;
	}
	if (cursor != end)
		return -EINVAL;
	return 0;
}
EXPORT_SYMBOL_GPL(lz4kd_delta_validate);

int lz4kd_delta_decode(struct lz4kd_delta_ctx *ctx,
			const void *wire, size_t wire_len,
			const void *reference, size_t reference_len,
			void *target, size_t target_len)
{
	const struct lz4kd_delta_header *header = wire;
	const u8 *cursor;
	const u8 *end;
	u8 *scratch;
	size_t previous_end = 0;
	int ret;

	if (!ctx || !ctx->workspace || !reference || !target ||
	    reference_len != LZ4KD_DELTA_PAGE_SIZE ||
	    target_len != LZ4KD_DELTA_PAGE_SIZE ||
	    ctx->workspace_size != LZ4KD_DELTA_PAGE_SIZE)
		return -EINVAL;
	ret = lz4kd_delta_validate(wire, wire_len, 0, 0);
	if (ret)
		return ret;
	if (le32_to_cpu(header->reference_hash) !=
	    lz4kd_delta_hash(reference, reference_len))
		return -EBADMSG;

	scratch = ctx->workspace;
	memcpy(scratch, reference, reference_len);
	cursor = (const u8 *)wire + sizeof(*header);
	end = (const u8 *)wire + wire_len;
	while (cursor < end) {
		struct lz4kd_delta_run run;
		size_t offset;
		size_t length;

		if ((size_t)(end - cursor) < sizeof(run))
			return -EINVAL;
		memcpy(&run, cursor, sizeof(run));
		cursor += sizeof(run);
		offset = le16_to_cpu(run.offset);
		length = le16_to_cpu(run.length);
		if (!length || offset < previous_end || offset >= target_len ||
		    length > target_len - offset ||
		    length > (size_t)(end - cursor))
			return -EINVAL;
		memcpy(scratch + offset, cursor, length);
		cursor += length;
		previous_end = offset + length;
	}
	if (cursor != end || le32_to_cpu(header->target_hash) !=
	    lz4kd_delta_hash(scratch, target_len))
		return -EBADMSG;
	memcpy(target, scratch, target_len);
	return 0;
}
EXPORT_SYMBOL_GPL(lz4kd_delta_decode);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Private LZ4KD page delta codec");
