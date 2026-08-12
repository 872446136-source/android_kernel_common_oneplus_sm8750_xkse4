// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/bio.h>
#include <linux/completion.h>
#include <linux/highmem.h>
#include <linux/ktime.h>
#include <linux/lz4kd.h>
#include <linux/math64.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#include "zram_drv.h"

#define ZRAM_PWB_MAGIC		0x4257505aU
#define ZRAM_PWB_VERSION	1U
#define ZRAM_PWB_MAX_OBJECTS	32U
#define ZRAM_PWB_MAX_BATCH	16U
#define ZRAM_PWB_DEFAULT_GC_DEAD	50U
#define ZRAM_PWB_ID_PROBES	64U

enum zram_pwb_object_kind {
	ZRAM_PWB_OBJECT_COMPRESSED = 1,
	ZRAM_PWB_OBJECT_RAW,
	ZRAM_PWB_OBJECT_ALIAS,
	ZRAM_PWB_OBJECT_DELTA,
	ZRAM_PWB_OBJECT_REF,
};

struct zram_pwb_header {
	__le32 magic;
	__le16 version;
	__le16 header_size;
	__le32 pack_id;
	__le32 pack_generation;
	__le16 object_count;
	__le16 directory_entry_size;
	__le16 data_offset;
	__le16 used_bytes;
	__le32 checksum;
	__le32 reserved;
} __packed;

struct zram_pwb_directory {
	__le32 slot_index;
	__le64 slot_generation;
	__le16 offset;
	__le16 length;
	u8 kind;
	u8 codec_id;
	__le16 codec_generation;
	__le32 reference_id;
	__le32 reference_generation;
	__le64 owner;
	__le32 integrity;
} __packed;

#define ZRAM_PWB_DATA_OFFSET ALIGN(sizeof(struct zram_pwb_header) + \
	ZRAM_PWB_MAX_OBJECTS * sizeof(struct zram_pwb_directory), 8)

struct zram_pwb_pack {
	refcount_t refs;
	struct zram *zram;
	unsigned long block;
	u64 created_ns;
	u32 id;
	u32 generation;
	u32 used_bytes;
	atomic_t live_objects;
	atomic_t readers;
	atomic_t publishers;
	atomic_long_t live_bytes;
	atomic_long_t dead_bytes;
	bool in_table;
	bool xa_reserved;
};

struct zram_pwb_ext {
	struct zram_ext_rep base;
	struct zram_pwb_pack *pack;
	void *reference_token;
	u64 source_generation;
	u64 owner;
	u32 pack_id;
	u32 pack_generation;
	u32 reference_id;
	u32 reference_generation;
	u32 wire_size;
	u32 integrity;
	u16 ordinal;
	u8 kind;
	u8 codec_id;
	u16 codec_generation;
	bool committed;
};

struct zram_pwb_object {
	struct zram_slot_txn txn;
	struct zram_pp_job *job;
	struct zram_sddc_export native;
	struct zram_pwb_ext *ext;
	u32 old_size;
	u32 wire_size;
	u32 integrity;
	u16 offset;
	u8 kind;
	u8 codec_id;
	u16 codec_generation;
	bool native_pinned;
};

struct zram_pwb_builder {
	struct page *page;
	struct zram_pwb_pack *pack;
	struct zram_pwb_object objects[ZRAM_PWB_MAX_OBJECTS];
	u32 object_count;
	u32 payload_bytes;
};

struct zram_pwb_io {
	struct work_struct work;
	struct completion done;
	struct zram *zram;
	struct zram_pwb_builder *builders;
	unsigned long block;
	u32 nr_packs;
	int error;
	bool submitted;
};

struct zram_pwb_cache {
	struct work_struct work;
	struct completion done;
	refcount_t users;
	struct zram_pwb_pack *pack;
	struct page *page;
	int error;
};

struct zram_pwb_publish {
	struct zram_pwb_ext *ext;
	unsigned long block;
	u32 wire_size;
};

struct zram_pwb_gc_request {
	struct work_struct work;
	struct completion done;
	struct zram *zram;
	const char *buf;
	size_t len;
	ssize_t result;
};

static bool zram_pwb_ordinary(u8 type)
{
	return type == ZRAM_REP_RAW || type == ZRAM_REP_COMPRESSED;
}

static u32 zram_pwb_object_bytes(u32 wire_size)
{
	return wire_size + sizeof(struct zram_pwb_directory);
}

static const char *zram_pwb_state_name(int state)
{
	switch (state) {
	case ZRAM_FEATURE_DISABLED:
		return "disabled";
	case ZRAM_FEATURE_ENABLED:
		return "enabled";
	case ZRAM_FEATURE_DRAINING:
		return "draining";
	case ZRAM_FEATURE_QUIESCING:
		return "quiescing";
	default:
		return "invalid";
	}
}

static u64 zram_pwb_next_identity(struct zram *zram)
{
	u64 id = atomic64_inc_return(&zram->pwb.next_ext_identity);

	if (unlikely(!id))
		id = atomic64_inc_return(&zram->pwb.next_ext_identity);
	return id;
}

static bool zram_pwb_pack_get(struct zram_pwb_pack *pack)
{
	return refcount_inc_not_zero(&pack->refs);
}

static void zram_pwb_pack_put(struct zram_pwb_pack *pack)
{
	if (refcount_dec_and_test(&pack->refs)) {
		WARN_ON_ONCE(pack->in_table);
		WARN_ON_ONCE(atomic_read(&pack->live_objects));
		WARN_ON_ONCE(atomic_read(&pack->readers));
		WARN_ON_ONCE(atomic_read(&pack->publishers));
		if (pack->xa_reserved)
			xa_release(&pack->zram->pwb.packs, pack->id);
		if (pack->block)
			zram_backing_free(pack->zram, pack->block, 1);
		kfree(pack);
	}
}

static void zram_pwb_pack_try_reap(struct zram_pwb_pack *pack)
{
	struct zram *zram = pack->zram;
	XA_STATE(xas, &zram->pwb.packs, pack->id);
	bool removed = false;

	if (atomic_read(&pack->live_objects) || atomic_read(&pack->readers) ||
	    atomic_read(&pack->publishers))
		return;
	mutex_lock(&zram->pwb.pack_lock);
	xa_lock(&zram->pwb.packs);
	if (!atomic_read(&pack->live_objects) &&
	    !atomic_read(&pack->readers) &&
	    !atomic_read(&pack->publishers) &&
	    xas_load(&xas) == pack) {
		__xa_erase(&zram->pwb.packs, pack->id);
		WRITE_ONCE(pack->in_table, false);
		removed = true;
	}
	xa_unlock(&zram->pwb.packs);
	mutex_unlock(&zram->pwb.pack_lock);
	if (removed) {
		atomic64_sub(atomic_long_read(&pack->dead_bytes),
			     &zram->pwb.stats.dead_bytes);
		atomic64_dec(&zram->pwb.stats.packs);
		atomic64_sub(PAGE_SIZE, &zram->pwb.stats.physical_bytes);
		zram_pwb_pack_put(pack);
	}
}

static void zram_pwb_publish_done(struct zram_pwb_pack *pack)
{
	/* Keep an explicit publisher reference across the last-slot reap race. */
	WARN_ON_ONCE(atomic_dec_return(&pack->publishers) < 0);
	zram_pwb_pack_try_reap(pack);
	zram_pwb_pack_put(pack);
}

static void zram_pwb_ext_release(struct zram *zram,
				 struct zram_ext_rep *base)
{
	struct zram_pwb_ext *ext = container_of(base,
					       struct zram_pwb_ext, base);
	struct zram_pwb_pack *pack = ext->pack;

	if (ext->committed) {
		u32 physical_size = zram_pwb_object_bytes(ext->wire_size);

		WARN_ON_ONCE(atomic_dec_return(&pack->live_objects) < 0);
		atomic_long_sub(physical_size, &pack->live_bytes);
		atomic64_dec(&zram->pwb.stats.objects);
		atomic64_sub(PAGE_SIZE, &zram->pwb.stats.logical_bytes);
		atomic64_sub(ext->wire_size, &zram->pwb.stats.wire_bytes);
		atomic64_add(physical_size, &zram->pwb.stats.dead_bytes);
		atomic_long_add(physical_size, &pack->dead_bytes);
	}
	if (ext->reference_token)
		zram_sddc_backing_unpin(zram, ext->reference_token);
	zram_pwb_pack_try_reap(pack);
	zram_pwb_pack_put(pack);
	kfree(ext);
}

static struct zram_pwb_ext *
zram_pwb_ext_create(struct zram *zram, struct zram_pwb_pack *pack,
		    struct zram_pwb_object *object, u16 ordinal)
{
	struct zram_pwb_ext *ext;

	ext = kzalloc(sizeof(*ext), GFP_NOIO | __GFP_NOWARN);
	if (!ext || !zram_pwb_pack_get(pack)) {
		kfree(ext);
		return NULL;
	}
	zram_ext_rep_init(&ext->base, zram_pwb_next_identity(zram),
			  zram_pwb_ext_release);
	ext->pack = pack;
	ext->reference_token = object->native.reference_token;
	object->native.reference_token = NULL;
	ext->source_generation = object->txn.snapshot.rep.mutation_seq;
	ext->owner = object->native.owner;
	ext->pack_id = pack->id;
	ext->pack_generation = pack->generation;
	ext->reference_id = object->native.reference_id;
	ext->reference_generation = object->native.reference_generation;
	ext->wire_size = object->wire_size;
	ext->integrity = object->integrity;
	ext->ordinal = ordinal;
	ext->kind = object->kind;
	ext->codec_id = object->codec_id;
	ext->codec_generation = object->codec_generation;
	return ext;
}

static u32 zram_pwb_checksum(const void *data)
{
	const size_t offset = offsetof(struct zram_pwb_header, checksum);
	const u8 *bytes = data;
	u32 hash = 2166136261U;
	u32 i;

	for (i = 0; i < PAGE_SIZE; i++) {
		u8 byte = (i >= offset && i < offset + sizeof(__le32)) ?
			0 : bytes[i];

		hash ^= byte;
		hash *= 16777619U;
	}
	return hash;
}

static int zram_pwb_validate_page(struct zram_pwb_pack *pack, void *data)
{
	struct zram_pwb_header *header = data;
	struct zram_pwb_directory *directory;
	u32 previous_end = ZRAM_PWB_DATA_OFFSET;
	u32 count;
	u32 i;

	if (le32_to_cpu(header->magic) != ZRAM_PWB_MAGIC ||
	    le16_to_cpu(header->version) != ZRAM_PWB_VERSION ||
	    le16_to_cpu(header->header_size) != ZRAM_PWB_DATA_OFFSET ||
	    le32_to_cpu(header->pack_id) != pack->id ||
	    le32_to_cpu(header->pack_generation) != pack->generation ||
	    le16_to_cpu(header->directory_entry_size) != sizeof(*directory) ||
	    le16_to_cpu(header->data_offset) != ZRAM_PWB_DATA_OFFSET ||
	    le32_to_cpu(header->reserved) ||
	    le32_to_cpu(header->checksum) != zram_pwb_checksum(data))
		return -EBADMSG;
	count = le16_to_cpu(header->object_count);
	if (!count || count > ZRAM_PWB_MAX_OBJECTS ||
	    le16_to_cpu(header->used_bytes) > PAGE_SIZE - ZRAM_PWB_DATA_OFFSET ||
	    pack->used_bytes != le16_to_cpu(header->used_bytes) +
		count * sizeof(*directory))
		return -EINVAL;
	directory = (void *)header + sizeof(*header);
	for (i = 0; i < count; i++) {
		u32 offset = le16_to_cpu(directory[i].offset);
		u32 length = le16_to_cpu(directory[i].length);
		u32 reference_id = le32_to_cpu(directory[i].reference_id);
		u32 reference_generation =
			le32_to_cpu(directory[i].reference_generation);

		if (directory[i].kind < ZRAM_PWB_OBJECT_COMPRESSED ||
		    directory[i].kind > ZRAM_PWB_OBJECT_REF ||
		    offset != previous_end || offset > PAGE_SIZE ||
		    length > PAGE_SIZE - offset)
			return -EINVAL;
		if (directory[i].kind == ZRAM_PWB_OBJECT_COMPRESSED) {
			if (!length || !directory[i].codec_id ||
			    !le16_to_cpu(directory[i].codec_generation) ||
			    reference_id || reference_generation ||
			    le64_to_cpu(directory[i].owner))
				return -EINVAL;
		} else if (directory[i].kind == ZRAM_PWB_OBJECT_DELTA) {
			if (length < sizeof(struct lz4kd_delta_header) ||
			    directory[i].codec_id ||
			    le16_to_cpu(directory[i].codec_generation) ||
			    !reference_id ||
			    !reference_generation ||
			    !le64_to_cpu(directory[i].owner))
				return -EINVAL;
		} else if (directory[i].kind == ZRAM_PWB_OBJECT_ALIAS ||
			   directory[i].kind == ZRAM_PWB_OBJECT_REF) {
			if (length || directory[i].codec_id ||
			    le16_to_cpu(directory[i].codec_generation) ||
			    !reference_id ||
			    !reference_generation ||
			    !le64_to_cpu(directory[i].owner))
				return -EINVAL;
		} else {
			/* A full raw page cannot share a directory-bearing pack. */
			return -EINVAL;
		}
		previous_end = offset + length;
	}
	if (previous_end != ZRAM_PWB_DATA_OFFSET +
			    le16_to_cpu(header->used_bytes))
		return -EINVAL;
	return 0;
}

static void zram_pwb_fill_header(struct zram_pwb_builder *builder)
{
	struct zram_pwb_header *header;
	struct zram_pwb_directory *directory;
	void *data;
	u32 i;

	data = kmap_local_page(builder->page);
	header = data;
	directory = data + sizeof(*header);
	memset(header, 0, ZRAM_PWB_DATA_OFFSET);
	header->magic = cpu_to_le32(ZRAM_PWB_MAGIC);
	header->version = cpu_to_le16(ZRAM_PWB_VERSION);
	header->header_size = cpu_to_le16(ZRAM_PWB_DATA_OFFSET);
	header->pack_id = cpu_to_le32(builder->pack->id);
	header->pack_generation = cpu_to_le32(builder->pack->generation);
	header->object_count = cpu_to_le16(builder->object_count);
	header->directory_entry_size = cpu_to_le16(sizeof(*directory));
	header->data_offset = cpu_to_le16(ZRAM_PWB_DATA_OFFSET);
	header->used_bytes = cpu_to_le16(builder->payload_bytes);
	for (i = 0; i < builder->object_count; i++) {
		struct zram_pwb_object *object = &builder->objects[i];

		directory[i].slot_index = cpu_to_le32(object->txn.index);
		directory[i].slot_generation = cpu_to_le64(
			object->txn.snapshot.rep.mutation_seq);
		directory[i].offset = cpu_to_le16(object->offset);
		directory[i].length = cpu_to_le16(object->wire_size);
		directory[i].kind = object->kind;
		directory[i].codec_id = object->codec_id;
		directory[i].codec_generation = cpu_to_le16(
			object->codec_generation);
		directory[i].reference_id = cpu_to_le32(
			object->native.reference_id);
		directory[i].reference_generation = cpu_to_le32(
			object->native.reference_generation);
		directory[i].owner = cpu_to_le64(object->native.owner);
		directory[i].integrity = cpu_to_le32(object->integrity);
	}
	header->checksum = cpu_to_le32(zram_pwb_checksum(data));
	kunmap_local(data);
}

static struct zcomp *zram_pwb_fast_comp(struct zram *zram, u8 *codec_id)
{
	u32 prio;

	for (prio = 0; prio < ZRAM_MAX_COMPS; prio++) {
		struct zcomp *comp = zram_comp_at_priority(zram, prio);

		if (comp && zcomp_execution_class(comp) == ZCOMP_EXEC_FAST) {
			*codec_id = zram_policy_codec_id(zram, prio);
			return comp;
		}
	}
	*codec_id = zram_policy_codec_id(zram, ZRAM_PRIMARY_COMP);
	return zram_comp_at_priority(zram, ZRAM_PRIMARY_COMP);
}

static int zram_pwb_flatten_locked(struct zram *zram, u32 index,
				   void *wire, u32 *wire_size, u8 *codec_id)
{
	struct zcomp_strm *zstrm;
	struct zcomp *comp;
	struct page *page;
	void *src;
	unsigned int length;
	int ret;

	page = alloc_page(GFP_NOIO | __GFP_NOWARN);
	if (!page)
		return -ENOMEM;
	ret = zram_read_from_zspool(zram, page, index);
	if (ret)
		goto out;
	comp = zram_pwb_fast_comp(zram, codec_id);
	if (!comp) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	zstrm = zcomp_stream_get(comp);
	src = kmap_local_page(page);
	ret = zcomp_compress(comp, zstrm, src, &length);
	kunmap_local(src);
	if (!ret && length < PAGE_SIZE - ZRAM_PWB_DATA_OFFSET) {
		memcpy(wire, zstrm->buffer, length);
		*wire_size = length;
	} else {
		ret = -EOPNOTSUPP;
	}
	zcomp_stream_put(zstrm);
out:
	__free_page(page);
	return ret;
}

static int zram_pwb_copy_ordinary_locked(struct zram *zram, u32 index,
					 void *wire, u32 *wire_size,
					 u8 *codec_id)
{
	struct zcomp_strm *zstrm;
	struct zcomp *comp;
	void *src;
	u32 size = zram_get_obj_size(zram, index);
	u8 type = zram_rep_type_locked(zram, index);

	if (type == ZRAM_REP_RAW)
		return zram_pwb_flatten_locked(zram, index, wire, wire_size,
					       codec_id);
	if (type != ZRAM_REP_COMPRESSED || !size || size >= PAGE_SIZE)
		return -EOPNOTSUPP;
	*codec_id = zram_rep_codec_id_locked(zram, index);
	if (!*codec_id || zram->slot_state[index].codec_generation !=
			   zram->codec_generation[*codec_id])
		return -ESTALE;
	comp = zram_codec_by_id(zram, *codec_id);
	if (!comp)
		return -EIO;
	zstrm = zcomp_stream_get(comp);
	src = zs_obj_read_begin(zram->mem_pool, zram_get_handle(zram, index),
				size, zstrm->local_copy);
	if (!src) {
		zcomp_stream_put(zstrm);
		return -EIO;
	}
	memcpy(wire, src, size);
	zs_obj_read_end(zram->mem_pool, zram_get_handle(zram, index), size,
			src);
	zcomp_stream_put(zstrm);
	*wire_size = size;
	return 0;
}

static void zram_pwb_release_job(struct zram *zram,
				 struct zram_pwb_object *object)
{
	if (!object->job)
		return;
	zram_slot_lock(zram, object->job->index);
	zram_pp_job_finish_locked(zram, object->job);
	zram_slot_unlock(zram, object->job->index);
	object->job = NULL;
}

static void zram_pwb_abort_object(struct zram *zram,
				  struct zram_pwb_object *object)
{
	if (object->native.reference_token) {
		if (object->native_pinned)
			zram_sddc_backing_unpin(zram,
				object->native.reference_token);
		object->native.reference_token = NULL;
	}
	zram_slot_txn_abort(zram, &object->txn);
	zram_pwb_release_job(zram, object);
}

static int zram_pwb_capture(struct zram *zram,
			    struct zram_pp_operation *operation, u32 index,
			    int mode, void *wire, struct zram_pwb_object *object)
{
	u8 type;
	int ret;

	memset(object, 0, sizeof(*object));
	zram_slot_lock(zram, index);
	type = zram_rep_type_locked(zram, index);
	if (type == ZRAM_REP_EMPTY || type == ZRAM_REP_SAME ||
	    type == ZRAM_REP_BACKING || type == ZRAM_REP_PACKED_BACKING ||
	    ((mode & BIT(0)) && !(zram->table[index].attr.flags &
				    BIT(ZRAM_IDLE)))) {
		ret = -ENODATA;
		goto out_unlock;
	}
	if (!zram_pp_operation_charge_candidate(operation)) {
		ret = -EDQUOT;
		goto out_unlock;
	}
	object->job = zram_pp_job_claim_locked(zram, operation, index,
					       GFP_NOWAIT | __GFP_NOWARN);
	if (IS_ERR(object->job)) {
		ret = PTR_ERR(object->job);
		object->job = NULL;
		goto out_unlock;
	}
	if (!zram_pp_job_charge(object->job, PAGE_SIZE)) {
		ret = -EDQUOT;
		goto out_unlock;
	}
	zram_slot_txn_snapshot_locked(zram, index, &object->txn);
	object->old_size = object->txn.snapshot.obj_size;
	if (zram_pwb_ordinary(type)) {
		ret = zram_pwb_copy_ordinary_locked(zram, index, wire,
						    &object->wire_size,
						    &object->codec_id);
		object->kind = ZRAM_PWB_OBJECT_COMPRESSED;
		object->codec_generation = object->codec_id ?
			zram->codec_generation[object->codec_id] : 0;
	} else if (IS_ENABLED(CONFIG_ZRAM_SDDC_NATIVE_WRITEBACK) &&
		   atomic_read(&zram->pwb.native_state) ==
			   ZRAM_FEATURE_ENABLED) {
		ret = zram_sddc_export_locked(zram, index, wire, PAGE_SIZE,
					      &object->native);
		if (!ret && zram_sddc_backing_pin(zram, &object->native)) {
			object->native_pinned = true;
			object->wire_size = object->native.wire_size;
			object->codec_id = ZRAM_CODEC_NONE;
			object->codec_generation = 0;
			if (object->native.kind == ZRAM_SDDC_WIRE_ALIAS)
				object->kind = ZRAM_PWB_OBJECT_ALIAS;
			else if (object->native.kind == ZRAM_SDDC_WIRE_DELTA)
				object->kind = ZRAM_PWB_OBJECT_DELTA;
			else
				object->kind = ZRAM_PWB_OBJECT_REF;
		} else {
			memset(&object->native, 0, sizeof(object->native));
			ret = zram_pwb_flatten_locked(zram, index, wire,
						      &object->wire_size,
						      &object->codec_id);
			object->kind = ZRAM_PWB_OBJECT_COMPRESSED;
			object->codec_generation = object->codec_id ?
				zram->codec_generation[object->codec_id] : 0;
			atomic64_inc(&zram->engine.stats.sddc_flatten_fallbacks);
		}
	} else {
		ret = zram_pwb_flatten_locked(zram, index, wire,
						      &object->wire_size,
						      &object->codec_id);
		object->kind = ZRAM_PWB_OBJECT_COMPRESSED;
		object->codec_generation = object->codec_id ?
			zram->codec_generation[object->codec_id] : 0;
		atomic64_inc(&zram->engine.stats.sddc_flatten_fallbacks);
	}
	if (!ret) {
		if (object->native_pinned && !object->wire_size)
			object->integrity = object->native.integrity;
		else
			object->integrity = lz4kd_delta_hash(wire,
							     object->wire_size);
	}
out_unlock:
	zram_slot_unlock(zram, index);
	if (ret)
		zram_pwb_abort_object(zram, object);
	return ret;
}

static struct zram_pwb_builder *
zram_pwb_best_builder(struct zram_pwb_builder *builders, u32 nr_builders,
		      u32 wire_size)
{
	struct zram_pwb_builder *best = NULL;
	u32 best_remaining = UINT_MAX;
	u32 i;

	for (i = 0; i < nr_builders; i++) {
		u32 remaining = PAGE_SIZE - ZRAM_PWB_DATA_OFFSET -
				builders[i].payload_bytes;

		if (builders[i].object_count >= ZRAM_PWB_MAX_OBJECTS ||
		    wire_size > remaining)
			continue;
		if (remaining - wire_size < best_remaining) {
			best = &builders[i];
			best_remaining = remaining - wire_size;
		}
	}
	return best;
}

static int zram_pwb_add_object(struct zram_pwb_builder *builder,
			       struct zram_pwb_object *object, const void *wire)
{
	struct zram_pwb_object *target;
	void *data;

	if (builder->object_count >= ZRAM_PWB_MAX_OBJECTS ||
	    object->wire_size > PAGE_SIZE - ZRAM_PWB_DATA_OFFSET -
				builder->payload_bytes)
		return -ENOSPC;
	target = &builder->objects[builder->object_count++];
	*target = *object;
	target->offset = ZRAM_PWB_DATA_OFFSET + builder->payload_bytes;
	if (object->wire_size) {
		data = kmap_local_page(builder->page);
		memcpy(data + target->offset, wire, object->wire_size);
		kunmap_local(data);
	}
	builder->payload_bytes += object->wire_size;
	memset(object, 0, sizeof(*object));
	return 0;
}

static struct zram_pwb_pack *zram_pwb_pack_alloc(struct zram *zram,
						  unsigned long block,
						  u32 used_bytes)
{
	struct zram_pwb_pack *pack;
	u32 probes;
	int ret;

	pack = kzalloc(sizeof(*pack), GFP_NOIO | __GFP_NOWARN);
	if (!pack)
		return NULL;
	refcount_set(&pack->refs, 1);
	pack->zram = zram;
	pack->block = block;
	pack->used_bytes = used_bytes;
	pack->created_ns = ktime_get_ns();
	mutex_lock(&zram->pwb.pack_lock);
	for (probes = 0; probes < ZRAM_PWB_ID_PROBES; probes++) {
		pack->id = zram->pwb.next_pack_id++;
		if (unlikely(!pack->id)) {
			zram->pwb.pack_generation++;
			if (!zram->pwb.pack_generation)
				zram->pwb.pack_generation++;
			pack->id = zram->pwb.next_pack_id++;
		}
		ret = xa_insert(&zram->pwb.packs, pack->id, NULL, GFP_NOIO);
		if (ret != -EBUSY)
			break;
	}
	if (probes == ZRAM_PWB_ID_PROBES)
		ret = -ENOSPC;
	pack->generation = zram->pwb.pack_generation;
	mutex_unlock(&zram->pwb.pack_lock);
	if (ret) {
		kfree(pack);
		return NULL;
	}
	pack->xa_reserved = true;
	return pack;
}

static void zram_pwb_write_work(struct work_struct *work)
{
	struct zram_pwb_io *io = container_of(work, struct zram_pwb_io, work);
	struct bio *bio;
	u32 i;

	bio = bio_alloc(io->zram->bdev, io->nr_packs, REQ_OP_WRITE, GFP_NOIO);
	if (!bio) {
		io->error = -ENOMEM;
		goto out;
	}
	bio->bi_iter.bi_sector = io->block * (PAGE_SIZE >> SECTOR_SHIFT);
	for (i = 0; i < io->nr_packs; i++) {
		if (bio_add_page(bio, io->builders[i].page, PAGE_SIZE, 0) !=
		    PAGE_SIZE) {
			io->error = -EIO;
			goto out_bio;
		}
	}
	io->submitted = true;
	io->error = submit_bio_wait(bio);
out_bio:
	bio_put(bio);
out:
	zram_pp_io_put(&io->zram->pp_scheduler);
	complete(&io->done);
}

static int zram_pwb_submit(struct zram *zram,
			   struct zram_pwb_builder *builders, u32 nr_packs,
			   unsigned long block, bool *submitted)
{
	struct zram_pwb_io io = {
		.zram = zram,
		.builders = builders,
		.nr_packs = nr_packs,
		.block = block,
	};

	init_completion(&io.done);
	INIT_WORK_ONSTACK(&io.work, zram_pwb_write_work);
	if (!zram_pp_io_get(&zram->pp_scheduler)) {
		destroy_work_on_stack(&io.work);
		return -ESHUTDOWN;
	}
	if (WARN_ON_ONCE(!queue_work(zram->pp_scheduler.write_wq,
				     &io.work))) {
		zram_pp_io_put(&zram->pp_scheduler);
		destroy_work_on_stack(&io.work);
		return -EBUSY;
	}
	wait_for_completion(&io.done);
	destroy_work_on_stack(&io.work);
	*submitted = io.submitted;
	return io.error;
}

static void zram_pwb_publish_slot(struct zram *zram, u32 index,
		const struct zram_slot_snapshot *old, void *private)
{
	struct zram_pwb_publish *publish = private;
	struct zram_pwb_ext *ext = publish->ext;

	zram_release_slot_data_locked(zram, index, old);
	zram_set_handle(zram, index, publish->block);
	zram_set_obj_size(zram, index, publish->wire_size);
	zram->table[index].attr.flags &= BIT(ZRAM_LOCK) |
					(BIT(ZRAM_FLAG_SHIFT) - 1);
	atomic64_inc(&zram->stats.pages_stored);
	ext->committed = true;
	atomic_inc(&ext->pack->live_objects);
	atomic_long_add(zram_pwb_object_bytes(ext->wire_size),
			&ext->pack->live_bytes);
	atomic64_inc(&zram->pwb.stats.objects);
	atomic64_add(PAGE_SIZE, &zram->pwb.stats.logical_bytes);
	atomic64_add(ext->wire_size, &zram->pwb.stats.wire_bytes);
}

static void zram_pwb_publish_relocation(struct zram *zram, u32 index,
		const struct zram_slot_snapshot *old, void *private)
{
	struct zram_pwb_publish *publish = private;
	struct zram_pwb_ext *ext = publish->ext;
	struct zram_pwb_ext *old_ext = container_of(old->ext,
						    struct zram_pwb_ext, base);

	if (old_ext->reference_token) {
		ext->reference_token = old_ext->reference_token;
		old_ext->reference_token = NULL;
	}
	zram_set_handle(zram, index, publish->block);
	zram_set_obj_size(zram, index, publish->wire_size);
	ext->committed = true;
	atomic_inc(&ext->pack->live_objects);
	atomic_long_add(zram_pwb_object_bytes(ext->wire_size),
			&ext->pack->live_bytes);
	atomic64_inc(&zram->pwb.stats.objects);
	atomic64_add(PAGE_SIZE, &zram->pwb.stats.logical_bytes);
	atomic64_add(ext->wire_size, &zram->pwb.stats.wire_bytes);
}

static int zram_pwb_insert_pack(struct zram *zram,
				struct zram_pwb_pack *pack)
{
	XA_STATE(xas, &zram->pwb.packs, pack->id);
	void *stored;
	int ret = 0;

	mutex_lock(&zram->pwb.pack_lock);
	atomic_inc(&pack->publishers);
	/* The builder owns one reference here, so this cannot fail. */
	refcount_inc(&pack->refs);
	xa_lock(&zram->pwb.packs);
	stored = xas_load(&xas);
	if (stored != XA_ZERO_ENTRY) {
		ret = -EEXIST;
	} else {
		xas_store(&xas, pack);
		ret = xas_error(&xas);
	}
	xa_unlock(&zram->pwb.packs);
	if (!ret)
		WRITE_ONCE(pack->in_table, true);
	if (pack->in_table) {
		pack->xa_reserved = false;
	} else {
		WARN_ON_ONCE(atomic_dec_return(&pack->publishers) < 0);
		zram_pwb_pack_put(pack);
	}
	mutex_unlock(&zram->pwb.pack_lock);
	if (ret)
		return ret;
	atomic64_inc(&zram->pwb.stats.packs);
	atomic64_add(PAGE_SIZE, &zram->pwb.stats.physical_bytes);
	return 0;
}

static int zram_pwb_publish_pack(struct zram *zram,
				 struct zram_pwb_builder *builder)
{
	struct zram_pwb_pack *pack = builder->pack;
	u32 i;
	int ret;

	ret = zram_pwb_insert_pack(zram, pack);
	if (ret)
		return ret;

	for (i = 0; i < builder->object_count; i++) {
		struct zram_pwb_object *object = &builder->objects[i];
		struct zram_pwb_publish publish;

		object->ext = zram_pwb_ext_create(zram, pack, object, i);
		if (!object->ext)
			goto stale;
		ret = zram_slot_txn_prepare(zram, &object->txn,
			ZRAM_REP_PACKED_BACKING, object->codec_id,
			&object->ext->base, GFP_NOIO);
		if (ret) {
			zram_ext_rep_put(zram, &object->ext->base);
			object->ext = NULL;
			goto stale;
		}
		if (object->txn.target_codec_generation !=
		    object->codec_generation)
			goto stale;
		publish.ext = object->ext;
		publish.block = pack->block;
		publish.wire_size = object->wire_size;
		zram_slot_lock(zram, object->txn.index);
		if (atomic_read(&zram->pwb.state) != ZRAM_FEATURE_ENABLED ||
		    ((object->kind == ZRAM_PWB_OBJECT_ALIAS ||
		      object->kind == ZRAM_PWB_OBJECT_DELTA ||
		      object->kind == ZRAM_PWB_OBJECT_REF) &&
		     atomic_read(&zram->pwb.native_state) !=
			ZRAM_FEATURE_ENABLED) ||
		    !object->job ||
		    !zram_pp_job_is_current_locked(zram, object->job))
			ret = -ESTALE;
		else
			ret = zram_slot_txn_commit_if_current_locked(zram,
				&object->txn, zram_pwb_publish_slot, &publish);
		if (object->job) {
			zram_pp_job_finish_locked(zram, object->job);
			object->job = NULL;
		}
		zram_slot_unlock(zram, object->txn.index);
		zram_slot_txn_abort(zram, &object->txn);
		if (!ret)
			continue;
stale:
		atomic64_inc(&zram->pwb.stats.snapshot_mismatches);
		atomic64_add(zram_pwb_object_bytes(object->wire_size),
			     &zram->pwb.stats.dead_bytes);
		atomic_long_add(zram_pwb_object_bytes(object->wire_size),
				&pack->dead_bytes);
		zram_pwb_abort_object(zram, object);
	}
	zram_pwb_publish_done(pack);
	return 0;
}

static int zram_pwb_parse_mode(const char *buf)
{
	if (sysfs_streq(buf, "idle") || sysfs_streq(buf, "type=idle"))
		return BIT(0);
	return -EOPNOTSUPP;
}

ssize_t zram_pwb_writeback(struct zram *zram, const char *buf, size_t len)
{
	struct zram_pwb_builder *builders = NULL;
	struct zram_pp_operation *operation = NULL;
	struct zram_pwb_object candidate;
	unsigned long block = 0;
	unsigned long nr_slots;
	void *wire = NULL;
	u32 max_packs;
	u32 nr_packs = 0;
	u32 index;
	u32 i;
	int mode;
	int ret = -EOPNOTSUPP;
	bool fallback_needed = false;
	bool write_submitted = false;

	if (PAGE_SIZE != 4096 || !zram_pwb_enabled(zram) ||
	    !zram->backing_dev || !zram->compressed_wb)
		return -EOPNOTSUPP;
	mode = zram_pwb_parse_mode(buf);
	if (mode < 0)
		return mode;
	operation = zram_pp_operation_begin(&zram->pp_scheduler,
		ZRAM_PP_WRITEBACK, ZRAM_PP_PRIO_NORMAL, 0, 0);
	if (IS_ERR(operation))
		return PTR_ERR(operation);
	max_packs = clamp_t(u32, zram->wb_bio_pages, 1,
			   ZRAM_PWB_MAX_BATCH);
	builders = kcalloc(max_packs, sizeof(*builders), GFP_KERNEL);
	wire = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!builders || !wire) {
		atomic64_inc(&zram->pwb.stats.fallback);
		ret = -EOPNOTSUPP;
		goto out;
	}
	for (i = 0; i < max_packs; i++) {
		builders[i].page = alloc_page(GFP_KERNEL | __GFP_NOWARN);
		if (!builders[i].page) {
			atomic64_inc(&zram->pwb.stats.fallback);
			ret = -EOPNOTSUPP;
			goto out;
		}
		clear_highpage(builders[i].page);
	}

	nr_slots = zram->disksize >> PAGE_SHIFT;
	for (index = 0; index < nr_slots; index++) {
		struct zram_pwb_builder *builder;

		if (zram_pp_operation_cancelled(operation) ||
		    !zram_pp_operation_charge_scan(operation, 1))
			break;
		ret = zram_pwb_capture(zram, operation, index, mode, wire,
				       &candidate);
		if (ret) {
			if (ret == -ENODATA || ret == -ESTALE || ret == -EBUSY)
				continue;
			if (ret == -EAGAIN) {
				fallback_needed = true;
				atomic64_inc(&zram->pwb.stats.fallback);
				break;
			}
			if (ret == -EOPNOTSUPP || ret == -ENOSPC ||
			    ret == -ENOMEM || ret == -EDQUOT) {
				fallback_needed = true;
				atomic64_inc(&zram->pwb.stats.fallback);
				if (ret == -ENOMEM || ret == -EDQUOT)
					break;
				continue;
			}
			break;
		}
		builder = zram_pwb_best_builder(builders, max_packs,
						candidate.wire_size);
		if (!builder) {
			zram_pwb_abort_object(zram, &candidate);
			atomic64_inc(&zram->pwb.stats.fallback);
			fallback_needed = true;
			break;
		}
		zram_pwb_add_object(builder, &candidate, wire);
	}
	if (ret && ret != -ENODATA && ret != -EAGAIN && ret != -ESTALE &&
	    ret != -EBUSY &&
	    ret != -EOPNOTSUPP && ret != -ENOSPC && ret != -ENOMEM &&
	    ret != -EDQUOT)
		goto out;
	for (i = 0; i < max_packs; i++)
		if (builders[i].object_count)
			nr_packs++;
	if (!nr_packs) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (zram_pp_operation_cancelled(operation)) {
		ret = -ECANCELED;
		goto out;
	}
	if (!zram_pp_budget_reserve_physical(&operation->budget, nr_packs)) {
		ret = -EDQUOT;
		goto out;
	}
	if (!zram_backing_write_reserve(zram, nr_packs)) {
		zram_pp_budget_rollback_physical(&operation->budget, nr_packs);
		atomic64_inc(&zram->pwb.stats.fallback);
		ret = -EOPNOTSUPP;
		goto out;
	}
	block = zram_backing_alloc(zram, nr_packs);
	if (!block) {
		zram_pp_budget_rollback_physical(&operation->budget, nr_packs);
		zram_backing_write_rollback(zram, nr_packs);
		atomic64_inc(&zram->pwb.stats.enospc);
		atomic64_inc(&zram->pwb.stats.fallback);
		ret = -EOPNOTSUPP;
		goto out;
	}
	for (i = 0; i < nr_packs; i++) {
		builders[i].pack = zram_pwb_pack_alloc(zram, block + i,
			builders[i].payload_bytes + builders[i].object_count *
				sizeof(struct zram_pwb_directory));
		if (!builders[i].pack) {
			atomic64_inc(&zram->pwb.stats.fallback);
			ret = -EOPNOTSUPP;
			goto rollback_blocks;
		}
		zram_pwb_fill_header(&builders[i]);
	}
	if (zram_pp_operation_cancelled(operation) ||
	    atomic_read(&zram->pwb.state) != ZRAM_FEATURE_ENABLED) {
		ret = -ECANCELED;
		goto rollback_blocks;
	}
	ret = zram_pwb_submit(zram, builders, nr_packs, block,
			      &write_submitted);
	if (write_submitted) {
		zram_pp_budget_commit_physical(&operation->budget, nr_packs,
					       false);
		atomic64_add(nr_packs, &zram->stats.bd_writes);
		atomic64_add(nr_packs, &zram->pwb.stats.physical_writes);
	}
	if (ret) {
		if (!write_submitted && ret != -ESHUTDOWN && ret != -ECANCELED) {
			atomic64_inc(&zram->pwb.stats.fallback);
			ret = -EOPNOTSUPP;
		}
		goto rollback_blocks;
	}
	if (WARN_ON_ONCE(!write_submitted)) {
		ret = -EIO;
		goto rollback_blocks;
	}
	for (i = 0; i < nr_packs; i++) {
		ret = zram_pwb_publish_pack(zram, &builders[i]);
		if (ret)
			goto publish_error;
	}
	block = 0;
	ret = fallback_needed ? -EOPNOTSUPP : len;
	goto out;

publish_error:
	/* Successfully written packs that could not be published are dead. */
	for (; i < nr_packs; i++) {
		if (builders[i].pack) {
			zram_pwb_pack_put(builders[i].pack);
			builders[i].pack = NULL;
		}
	}
	block = 0;
	ret = -EIO;
	goto out;

rollback_blocks:
	if (!write_submitted) {
		zram_pp_budget_rollback_physical(&operation->budget, nr_packs);
		zram_backing_write_rollback(zram, nr_packs);
	}
	if (block)
		zram_backing_free(zram, block, nr_packs);
	block = 0;
	for (i = 0; i < nr_packs; i++) {
		if (builders[i].pack) {
			builders[i].pack->block = 0;
			zram_pwb_pack_put(builders[i].pack);
			builders[i].pack = NULL;
		}
	}
out:
	if (builders) {
		for (i = 0; i < max_packs; i++) {
			u32 j;

			for (j = 0; j < builders[i].object_count; j++)
				zram_pwb_abort_object(zram,
					&builders[i].objects[j]);
			if (builders[i].page)
				__free_page(builders[i].page);
		}
	}
	kfree(wire);
	kfree(builders);
	zram_pp_operation_end(operation);
	return ret;
}

static void zram_pwb_cache_put(struct zram_pwb_cache *cache)
{
	struct zram_pwb_pack *pack = cache->pack;
	struct zram *zram = pack->zram;

	mutex_lock(&zram->pwb.read_lock);
	if (!refcount_dec_and_test(&cache->users)) {
		mutex_unlock(&zram->pwb.read_lock);
		return;
	}
	if (xa_load(&zram->pwb.read_cache, pack->id) == cache)
		xa_erase(&zram->pwb.read_cache, pack->id);
	mutex_unlock(&zram->pwb.read_lock);
	__free_page(cache->page);
	WARN_ON_ONCE(atomic_dec_return(&pack->readers) < 0);
	zram_pwb_pack_try_reap(pack);
	zram_pwb_pack_put(pack);
	kfree(cache);
}

static void zram_pwb_read_work(struct work_struct *work)
{
	struct zram_pwb_cache *cache = container_of(work,
						    struct zram_pwb_cache, work);
	struct zram_pwb_pack *pack = cache->pack;
	struct zram *zram = pack->zram;
	struct bio bio;
	struct bio_vec bvec;
	void *data;

	bio_init(&bio, zram->bdev, &bvec, 1, REQ_OP_READ);
	bio.bi_iter.bi_sector = pack->block * (PAGE_SIZE >> SECTOR_SHIFT);
	__bio_add_page(&bio, cache->page, PAGE_SIZE, 0);
	cache->error = submit_bio_wait(&bio);
	if (!cache->error) {
		data = kmap_local_page(cache->page);
		cache->error = zram_pwb_validate_page(pack, data);
		kunmap_local(data);
		if (cache->error)
			atomic64_inc(&zram->pwb.stats.checksum_errors);
	}
	atomic64_inc(&zram->stats.bd_reads);
	atomic64_inc(&zram->pwb.stats.physical_reads);
	zram_pp_io_put(&zram->pp_scheduler);
	complete_all(&cache->done);
	zram_pwb_cache_put(cache);
}

static struct zram_pwb_cache *
zram_pwb_cache_get(struct zram_pwb_pack *pack)
{
	struct zram *zram = pack->zram;
	struct zram_pwb_cache *cache;
	void *stored;

	mutex_lock(&zram->pwb.read_lock);
	cache = xa_load(&zram->pwb.read_cache, pack->id);
	if (cache && cache->pack->generation == pack->generation &&
	    refcount_inc_not_zero(&cache->users)) {
		atomic64_inc(&zram->pwb.stats.coalesced_reads);
		mutex_unlock(&zram->pwb.read_lock);
		return cache;
	}
	if (cache)
		goto fail_unlock;
	cache = kzalloc(sizeof(*cache), GFP_NOIO | __GFP_NOWARN);
	if (!cache)
		goto fail_unlock;
	cache->page = alloc_page(GFP_NOIO | __GFP_NOWARN);
	if (!cache->page)
		goto fail_cache;
	if (!zram_pwb_pack_get(pack))
		goto fail_cache;
	if (!zram_pp_io_get(&zram->pp_scheduler))
		goto fail_pack;
	cache->pack = pack;
	atomic_inc(&pack->readers);
	refcount_set(&cache->users, 2); /* caller and read worker */
	init_completion(&cache->done);
	INIT_WORK(&cache->work, zram_pwb_read_work);
	stored = xa_cmpxchg(&zram->pwb.read_cache, pack->id, NULL, cache,
			    GFP_NOIO);
	if (xa_is_err(stored) || stored)
		goto fail_token;
	mutex_unlock(&zram->pwb.read_lock);
	if (WARN_ON_ONCE(!queue_work(zram->pp_scheduler.fault_wq,
				     &cache->work))) {
		mutex_lock(&zram->pwb.read_lock);
		xa_cmpxchg(&zram->pwb.read_cache, pack->id, cache, NULL, 0);
		mutex_unlock(&zram->pwb.read_lock);
		cache->error = -EIO;
		zram_pp_io_put(&zram->pp_scheduler);
		complete_all(&cache->done);
		zram_pwb_cache_put(cache); /* worker reference */
	}
	return cache;

fail_token:
	zram_pp_io_put(&zram->pp_scheduler);
	WARN_ON_ONCE(atomic_dec_return(&pack->readers) < 0);
	zram_pwb_pack_try_reap(pack);
fail_pack:
	zram_pwb_pack_put(pack);
fail_cache:
	if (cache->page)
		__free_page(cache->page);
	kfree(cache);
fail_unlock:
	mutex_unlock(&zram->pwb.read_lock);
	return NULL;
}

int zram_pwb_read(struct zram *zram, struct page *page,
		  const struct zram_slot_txn *txn)
{
	struct zram_pwb_ext *ext;
	struct zram_pwb_cache *cache;
	struct zram_pwb_directory *directory;
	struct zram_pwb_directory entry;
	struct zcomp_strm *zstrm;
	struct zcomp *comp;
	struct page *reference_page = NULL;
	struct lz4kd_delta_ctx ctx;
	void *data;
	void *wire = NULL;
	void *dst;
	u32 length;
	int ret = -EIO;

	memzero_page(page, 0, PAGE_SIZE);
	if (!txn->snapshot.ext)
		return -EIO;
	ext = container_of(txn->snapshot.ext, struct zram_pwb_ext, base);
	if (!ext->committed || !ext->pack || ext->pack_id != ext->pack->id ||
	    ext->pack_generation != ext->pack->generation ||
	    ((ext->kind == ZRAM_PWB_OBJECT_ALIAS ||
	      ext->kind == ZRAM_PWB_OBJECT_DELTA ||
	      ext->kind == ZRAM_PWB_OBJECT_REF) && !ext->reference_token)) {
		atomic64_inc(&zram->pwb.stats.snapshot_mismatches);
		return -ESTALE;
	}
	cache = zram_pwb_cache_get(ext->pack);
	if (!cache)
		return -ENOMEM;
	wait_for_completion(&cache->done);
	if (cache->error) {
		ret = cache->error;
		goto out_cache;
	}
	length = ext->wire_size;
	if (length) {
		wire = kmalloc(length, GFP_NOIO | __GFP_NOWARN);
		if (!wire) {
			ret = -ENOMEM;
			goto out_cache;
		}
	}
	data = kmap_local_page(cache->page);
	directory = data + sizeof(struct zram_pwb_header);
	if (ext->ordinal >= le16_to_cpu(
		((struct zram_pwb_header *)data)->object_count) ||
	    le32_to_cpu(directory[ext->ordinal].slot_index) != txn->index ||
	    le64_to_cpu(directory[ext->ordinal].slot_generation) !=
		ext->source_generation ||
	    le16_to_cpu(directory[ext->ordinal].length) != ext->wire_size ||
	    directory[ext->ordinal].kind != ext->kind ||
	    directory[ext->ordinal].codec_id != ext->codec_id ||
	    le16_to_cpu(directory[ext->ordinal].codec_generation) !=
		ext->codec_generation ||
	    le32_to_cpu(directory[ext->ordinal].reference_id) !=
		ext->reference_id ||
	    le32_to_cpu(directory[ext->ordinal].reference_generation) !=
		ext->reference_generation ||
	    le64_to_cpu(directory[ext->ordinal].owner) != ext->owner ||
	    le32_to_cpu(directory[ext->ordinal].integrity) != ext->integrity) {
		kunmap_local(data);
		ret = -ESTALE;
		goto mismatch;
	}
	entry = directory[ext->ordinal];
	if (length) {
		u32 offset = le16_to_cpu(directory[ext->ordinal].offset);

		memcpy(wire, data + offset, length);
		if (lz4kd_delta_hash(wire, length) != ext->integrity) {
			kunmap_local(data);
			ret = -EBADMSG;
			goto checksum;
		}
	}
	kunmap_local(data);

	if (ext->kind == ZRAM_PWB_OBJECT_COMPRESSED) {
		if (!ext->codec_id ||
		    zram->codec_generation[ext->codec_id] !=
			ext->codec_generation)
			goto mismatch;
		comp = zram_codec_by_id(zram, ext->codec_id);
		if (!comp || !wire || !length)
			goto out_cache;
		zstrm = zcomp_stream_get(comp);
		dst = kmap_local_page(page);
		ret = zcomp_decompress(comp, zstrm, wire, length, dst);
		if (ret)
			memset(dst, 0, PAGE_SIZE);
		kunmap_local(dst);
		zcomp_stream_put(zstrm);
	} else if (ext->kind == ZRAM_PWB_OBJECT_ALIAS ||
		   ext->kind == ZRAM_PWB_OBJECT_REF) {
		ret = zram_sddc_reference_read(zram,
			le32_to_cpu(entry.reference_id),
			le32_to_cpu(entry.reference_generation),
			le64_to_cpu(entry.owner),
			page);
		if (!ret) {
			data = kmap_local_page(page);
			if (lz4kd_delta_hash(data, PAGE_SIZE) != ext->integrity)
				ret = -EBADMSG;
			kunmap_local(data);
			if (ret) {
				atomic64_inc(&zram->pwb.stats.checksum_errors);
				atomic64_inc(&zram->engine.stats.sddc_integrity_failures);
				atomic64_inc(&zram->engine.stats.sddc_decode_failures);
			}
		}
	} else if (ext->kind == ZRAM_PWB_OBJECT_DELTA) {
		reference_page = alloc_page(GFP_NOIO | __GFP_NOWARN);
		if (!reference_page || !wire)
			goto out_cache;
		ret = zram_sddc_reference_read(zram,
			le32_to_cpu(entry.reference_id),
			le32_to_cpu(entry.reference_generation),
			le64_to_cpu(entry.owner),
			reference_page);
		if (ret)
			goto out_cache;
		ret = lz4kd_delta_ctx_init(&ctx, GFP_NOIO | __GFP_NOWARN);
		if (ret)
			goto out_cache;
		data = kmap_local_page(reference_page);
		dst = kmap_local_page(page);
		ret = lz4kd_delta_decode(&ctx, wire, length, data, PAGE_SIZE,
					 dst, PAGE_SIZE);
		if (ret)
			memset(dst, 0, PAGE_SIZE);
		kunmap_local(dst);
		kunmap_local(data);
		lz4kd_delta_ctx_release(&ctx);
		if (ret) {
			atomic64_inc(&zram->engine.stats.sddc_decode_failures);
			atomic64_inc(&zram->engine.stats.sddc_integrity_failures);
		}
	}
	if (ret)
		goto out_cache;
	zram_slot_lock(zram, txn->index);
	if (!zram_slot_txn_revalidate_locked(zram, txn) ||
	    zram_rep_type_locked(zram, txn->index) != ZRAM_REP_PACKED_BACKING)
		ret = -ESTALE;
	zram_slot_unlock(zram, txn->index);
	if (ret)
		goto mismatch;
	goto out_cache;

checksum:
	atomic64_inc(&zram->pwb.stats.checksum_errors);
	if (ext->kind == ZRAM_PWB_OBJECT_DELTA) {
		atomic64_inc(&zram->engine.stats.sddc_decode_failures);
		atomic64_inc(&zram->engine.stats.sddc_integrity_failures);
	}
	goto out_cache;
mismatch:
	atomic64_inc(&zram->pwb.stats.snapshot_mismatches);
out_cache:
	if (ret)
		memzero_page(page, 0, PAGE_SIZE);
	if (reference_page)
		__free_page(reference_page);
	kfree(wire);
	zram_pwb_cache_put(cache);
	return ret;
}

void zram_pwb_release_slot_locked(
	struct zram *zram, u32 index, const struct zram_slot_snapshot *snapshot)
{
	WARN_ON_ONCE(snapshot->rep.type != ZRAM_REP_PACKED_BACKING);
	zram->table[index].attr.flags &= BIT(ZRAM_LOCK);
	zram_set_handle(zram, index, 0);
	zram_set_obj_size(zram, index, 0);
	atomic64_dec(&zram->stats.pages_stored);
}

bool zram_pwb_enabled(struct zram *zram)
{
	return atomic_read(&zram->pwb.state) == ZRAM_FEATURE_ENABLED;
}

ssize_t zram_pwb_state_show(struct zram *zram, char *buf)
{
	return sysfs_emit(buf, "%s\n",
		zram_pwb_state_name(atomic_read(&zram->pwb.state)));
}

ssize_t zram_pwb_state_store(struct zram *zram, const char *buf, size_t len)
{
	struct zram_pp_operation *operation = NULL;
	struct page *page;
	unsigned long nr_slots;
	u32 index;
	bool remaining = false;
	ssize_t result = len;

	mutex_lock(&zram->pwb.state_lock);
	if (atomic_read(&zram->pwb.state) == ZRAM_FEATURE_QUIESCING ||
	    READ_ONCE(zram->pp_scheduler.stopping)) {
		result = -ESHUTDOWN;
		goto out;
	}
	if (sysfs_streq(buf, "1") || sysfs_streq(buf, "enabled")) {
		if (PAGE_SIZE != LZ4KD_DELTA_PAGE_SIZE)
			result = -EOPNOTSUPP;
		else
			atomic_set(&zram->pwb.state, ZRAM_FEATURE_ENABLED);
	} else if (sysfs_streq(buf, "0") || sysfs_streq(buf, "disabled"))
		atomic_set(&zram->pwb.state, ZRAM_FEATURE_DRAINING);
	else if (sysfs_streq(buf, "drain")) {
		atomic_set(&zram->pwb.state, ZRAM_FEATURE_DRAINING);
		operation = zram_pp_operation_begin(&zram->pp_scheduler,
			ZRAM_PP_WRITEBACK, ZRAM_PP_PRIO_NORMAL, 0, 0);
		if (IS_ERR(operation)) {
			result = PTR_ERR(operation);
			operation = NULL;
			goto out;
		}
		flush_workqueue(zram->pp_scheduler.write_wq);
		page = alloc_page(GFP_KERNEL | __GFP_NOWARN);
		if (!page) {
			zram_pp_operation_end(operation);
			operation = NULL;
			result = -ENOMEM;
			goto out;
		}
		nr_slots = zram->disksize >> PAGE_SHIFT;
		for (index = 0; index < nr_slots; index++) {
			struct zram_slot_txn txn;
			int ret;

			if (zram_pp_operation_cancelled(operation)) {
				remaining = true;
				break;
			}
			zram_slot_lock(zram, index);
			if (zram_rep_type_locked(zram, index) !=
			    ZRAM_REP_PACKED_BACKING) {
				zram_slot_unlock(zram, index);
				continue;
			}
			zram_slot_txn_snapshot_locked(zram, index, &txn);
			zram_slot_unlock(zram, index);
			ret = zram_pwb_read(zram, page, &txn);
			if (!ret)
				ret = zram_store_page_if_current(zram, page, &txn);
			zram_slot_txn_abort(zram, &txn);
			if (ret)
				remaining = true;
			cond_resched();
		}
		__free_page(page);
		zram_pp_operation_end(operation);
		if (!remaining)
			atomic_set(&zram->pwb.state, ZRAM_FEATURE_DISABLED);
		else
			result = -EBUSY;
	} else
		result = -EINVAL;
out:
	mutex_unlock(&zram->pwb.state_lock);
	return result;
}

ssize_t zram_pwb_native_state_show(struct zram *zram, char *buf)
{
	return sysfs_emit(buf, "%s\n",
		zram_pwb_state_name(atomic_read(&zram->pwb.native_state)));
}

ssize_t zram_pwb_native_state_store(struct zram *zram, const char *buf,
				    size_t len)
{
	ssize_t ret = len;

	if (!IS_ENABLED(CONFIG_ZRAM_SDDC_NATIVE_WRITEBACK))
		return -EOPNOTSUPP;
	mutex_lock(&zram->pwb.state_lock);
	if (atomic_read(&zram->pwb.native_state) ==
	    ZRAM_FEATURE_QUIESCING ||
	    READ_ONCE(zram->pp_scheduler.stopping)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (sysfs_streq(buf, "1") || sysfs_streq(buf, "enabled")) {
		if (PAGE_SIZE != LZ4KD_DELTA_PAGE_SIZE)
			ret = -EOPNOTSUPP;
		else
			atomic_set(&zram->pwb.native_state,
				   ZRAM_FEATURE_ENABLED);
	} else if (sysfs_streq(buf, "0") || sysfs_streq(buf, "disabled"))
		atomic_set(&zram->pwb.native_state, ZRAM_FEATURE_DRAINING);
	else
		ret = -EINVAL;
out:
	mutex_unlock(&zram->pwb.state_lock);
	return ret;
}

ssize_t zram_pwb_gc_state_show(struct zram *zram, char *buf)
{
	return sysfs_emit(buf, "%s\n",
		zram_pwb_state_name(atomic_read(&zram->pwb.gc_state)));
}

ssize_t zram_pwb_gc_state_store(struct zram *zram, const char *buf,
				size_t len)
{
	ssize_t ret = len;

	if (!IS_ENABLED(CONFIG_ZRAM_PWB_GC))
		return -EOPNOTSUPP;
	mutex_lock(&zram->pwb.state_lock);
	if (atomic_read(&zram->pwb.gc_state) == ZRAM_FEATURE_QUIESCING ||
	    READ_ONCE(zram->pp_scheduler.stopping)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (sysfs_streq(buf, "1") || sysfs_streq(buf, "enabled")) {
		if (PAGE_SIZE != LZ4KD_DELTA_PAGE_SIZE)
			ret = -EOPNOTSUPP;
		else
			atomic_set(&zram->pwb.gc_state, ZRAM_FEATURE_ENABLED);
	} else if (sysfs_streq(buf, "0") || sysfs_streq(buf, "disabled"))
		atomic_set(&zram->pwb.gc_state, ZRAM_FEATURE_DRAINING);
	else
		ret = -EINVAL;
out:
	mutex_unlock(&zram->pwb.state_lock);
	return ret;
}

ssize_t zram_pwb_stats_show(struct zram *zram, char *buf)
{
	struct zram_pwb_stats *s = &zram->pwb.stats;
	u64 physical = atomic64_read(&s->physical_bytes);
	u64 wire = atomic64_read(&s->wire_bytes);
	u64 utilization = physical ?
		mul_u64_u64_div_u64(wire, 10000, physical) : 0;

	return sysfs_emit(buf,
		"packs=%lld objects=%lld logical_bytes=%lld wire_bytes=%lld physical_bytes=%lld packing_utilization_bp=%llu dead_bytes=%lld physical_reads=%lld physical_writes=%lld coalesced_reads=%lld snapshot_mismatches=%lld enospc=%lld fallback=%lld gc_input_packs=%lld gc_output_packs=%lld gc_physical_writes=%lld checksum_errors=%lld\n",
		atomic64_read(&s->packs), atomic64_read(&s->objects),
		atomic64_read(&s->logical_bytes), atomic64_read(&s->wire_bytes),
		atomic64_read(&s->physical_bytes), utilization,
		atomic64_read(&s->dead_bytes),
		atomic64_read(&s->physical_reads),
		atomic64_read(&s->physical_writes),
		atomic64_read(&s->coalesced_reads),
		atomic64_read(&s->snapshot_mismatches),
		atomic64_read(&s->enospc), atomic64_read(&s->fallback),
		atomic64_read(&s->gc_input_packs),
		atomic64_read(&s->gc_output_packs),
		atomic64_read(&s->gc_physical_writes),
		atomic64_read(&s->checksum_errors));
}

static int zram_pwb_gc_capture(struct zram *zram,
			       struct zram_pp_operation *operation,
			       struct zram_pwb_pack *old_pack,
			       const struct zram_pwb_directory *directory,
			       u16 ordinal, const void *wire,
			       struct zram_pwb_builder *builder)
{
	struct zram_pwb_object object = { };
	struct zram_pwb_ext *old_ext;
	struct zram_ext_rep *base;
	u32 index = le32_to_cpu(directory->slot_index);
	u32 length = le16_to_cpu(directory->length);
	int ret = -ESTALE;

	if (((directory->kind == ZRAM_PWB_OBJECT_COMPRESSED ||
	      directory->kind == ZRAM_PWB_OBJECT_DELTA) &&
	     lz4kd_delta_hash(wire, length) !=
		le32_to_cpu(directory->integrity)) ||
	    (directory->kind == ZRAM_PWB_OBJECT_DELTA &&
	     lz4kd_delta_validate(wire, length,
		le32_to_cpu(directory->reference_id),
		le32_to_cpu(directory->reference_generation)))) {
		atomic64_inc(&zram->pwb.stats.checksum_errors);
		if (directory->kind == ZRAM_PWB_OBJECT_DELTA) {
			atomic64_inc(&zram->engine.stats.sddc_integrity_failures);
			atomic64_inc(&zram->engine.stats.sddc_decode_failures);
		}
		return -EBADMSG;
	}
	if (index >= zram->disksize >> PAGE_SHIFT ||
	    !zram_pp_operation_charge_scan(operation, 1) ||
	    !zram_pp_operation_charge_candidate(operation))
		return -EAGAIN;
	zram_slot_lock(zram, index);
	if (zram_rep_type_locked(zram, index) != ZRAM_REP_PACKED_BACKING)
		goto out_unlock;
	base = zram_rep_extended_locked(zram, index);
	if (!base)
		goto out_unlock;
	old_ext = container_of(base, struct zram_pwb_ext, base);
	if (!old_ext->committed || old_ext->pack != old_pack ||
	    old_ext->ordinal != ordinal || old_ext->kind != directory->kind ||
	    old_ext->source_generation !=
		le64_to_cpu(directory->slot_generation) ||
	    old_ext->wire_size != le16_to_cpu(directory->length) ||
	    old_ext->integrity != le32_to_cpu(directory->integrity) ||
	    old_ext->codec_id != directory->codec_id ||
	    old_ext->codec_generation !=
		le16_to_cpu(directory->codec_generation) ||
	    old_ext->reference_id != le32_to_cpu(directory->reference_id) ||
	    old_ext->reference_generation !=
		le32_to_cpu(directory->reference_generation) ||
	    old_ext->owner != le64_to_cpu(directory->owner))
		goto out_unlock;
	if ((old_ext->kind == ZRAM_PWB_OBJECT_ALIAS ||
	     old_ext->kind == ZRAM_PWB_OBJECT_DELTA ||
	     old_ext->kind == ZRAM_PWB_OBJECT_REF) &&
	    !old_ext->reference_token)
		goto out_unlock;
	object.job = zram_pp_job_claim_locked(zram, operation, index,
					      GFP_NOWAIT | __GFP_NOWARN);
	if (IS_ERR(object.job)) {
		ret = PTR_ERR(object.job);
		object.job = NULL;
		goto out_unlock;
	}
	if (!zram_pp_job_charge(object.job, PAGE_SIZE)) {
		ret = -EDQUOT;
		goto out_unlock;
	}
	zram_slot_txn_snapshot_locked(zram, index, &object.txn);
	object.old_size = object.txn.snapshot.obj_size;
	object.wire_size = old_ext->wire_size;
	object.integrity = old_ext->integrity;
	object.kind = old_ext->kind;
	object.codec_id = old_ext->codec_id;
	object.codec_generation = old_ext->codec_generation;
	object.native.reference_id = le32_to_cpu(directory->reference_id);
	object.native.reference_generation =
		le32_to_cpu(directory->reference_generation);
	object.native.owner = le64_to_cpu(directory->owner);
	ret = zram_pwb_add_object(builder, &object, wire);
out_unlock:
	zram_slot_unlock(zram, index);
	if (ret)
		zram_pwb_abort_object(zram, &object);
	return ret;
}

static int zram_pwb_publish_gc_pack(struct zram *zram,
				    struct zram_pwb_builder *builder)
{
	struct zram_pwb_pack *pack = builder->pack;
	u32 i;
	int ret;

	ret = zram_pwb_insert_pack(zram, pack);
	if (ret)
		return ret;
	for (i = 0; i < builder->object_count; i++) {
		struct zram_pwb_object *object = &builder->objects[i];
		struct zram_pwb_publish publish;

		object->ext = zram_pwb_ext_create(zram, pack, object, i);
		if (!object->ext)
			goto stale;
		ret = zram_slot_txn_prepare(zram, &object->txn,
			ZRAM_REP_PACKED_BACKING, object->codec_id,
			&object->ext->base, GFP_NOIO);
		if (ret) {
			zram_ext_rep_put(zram, &object->ext->base);
			object->ext = NULL;
			goto stale;
		}
		if (object->txn.target_codec_generation !=
		    object->codec_generation)
			goto stale;
		publish.ext = object->ext;
		publish.block = pack->block;
		publish.wire_size = object->wire_size;
		zram_slot_lock(zram, object->txn.index);
		if (atomic_read(&zram->pwb.state) != ZRAM_FEATURE_ENABLED ||
		    atomic_read(&zram->pwb.gc_state) != ZRAM_FEATURE_ENABLED ||
		    !object->job ||
		    !zram_pp_job_is_current_locked(zram, object->job))
			ret = -ESTALE;
		else
			ret = zram_slot_txn_commit_if_current_locked(zram,
				&object->txn, zram_pwb_publish_relocation,
				&publish);
		if (object->job) {
			zram_pp_job_finish_locked(zram, object->job);
			object->job = NULL;
		}
		zram_slot_unlock(zram, object->txn.index);
		zram_slot_txn_abort(zram, &object->txn);
		if (!ret)
			continue;
stale:
		atomic64_inc(&zram->pwb.stats.snapshot_mismatches);
		atomic64_add(zram_pwb_object_bytes(object->wire_size),
			     &zram->pwb.stats.dead_bytes);
		atomic_long_add(zram_pwb_object_bytes(object->wire_size),
				&pack->dead_bytes);
		zram_pwb_abort_object(zram, object);
	}
	zram_pwb_publish_done(pack);
	return 0;
}

static ssize_t zram_pwb_gc_execute(struct zram *zram, const char *buf,
				    size_t len)
{
	XA_STATE(xas, &zram->pwb.packs, 0);
	struct zram_pwb_pack **inputs = NULL;
	struct zram_pwb_pack *iter_pack;
	struct zram_pwb_builder *builder = NULL;
	struct zram_pp_operation *operation = NULL;
	unsigned long block = 0;
	void *copy = NULL;
	u32 max_packs = 1;
	u32 selected = 0;
	u32 i;
	int ret = -ENODATA;
	bool write_submitted = false;

	if (PAGE_SIZE != 4096 ||
	    !IS_ENABLED(CONFIG_ZRAM_PWB_GC) ||
	    atomic_read(&zram->pwb.gc_state) != ZRAM_FEATURE_ENABLED)
		return -EACCES;
	if (*skip_spaces(buf) && kstrtou32(buf, 10, &max_packs))
		return -EINVAL;
	max_packs = clamp_t(u32, max_packs, 1, ZRAM_PWB_MAX_BATCH);
	inputs = kcalloc(max_packs, sizeof(*inputs), GFP_KERNEL);
	copy = kmalloc(PAGE_SIZE, GFP_KERNEL);
	builder = kzalloc(sizeof(*builder), GFP_KERNEL);
	if (!inputs || !copy || !builder) {
		ret = -ENOMEM;
		goto out;
	}
	builder->page = alloc_page(GFP_KERNEL | __GFP_NOWARN);
	if (!builder->page) {
		ret = -ENOMEM;
		goto out;
	}
	clear_highpage(builder->page);
	operation = zram_pp_operation_begin(&zram->pp_scheduler, ZRAM_PP_GC,
					    ZRAM_PP_PRIO_LOW, 0, 0);
	if (IS_ERR(operation)) {
		ret = PTR_ERR(operation);
		operation = NULL;
		goto out;
	}
	mutex_lock(&zram->pwb.pack_lock);
	xa_lock(&zram->pwb.packs);
	xas_for_each(&xas, iter_pack, ULONG_MAX) {
		struct zram_pwb_pack *pack;
		u64 live;
		u64 live_limit;

		if (xa_is_zero(iter_pack))
			continue;
		pack = iter_pack;
		live = atomic_long_read(&pack->live_bytes);
		live_limit = (u64)pack->used_bytes *
			(100 - zram->pwb.gc_dead_percent);
		if (selected >= max_packs)
			break;
		if (!pack->used_bytes || atomic_read(&pack->readers) ||
		    atomic_read(&pack->publishers) ||
		    ktime_get_ns() - pack->created_ns < NSEC_PER_SEC ||
		    live * 100 > live_limit || !zram_pwb_pack_get(pack))
			continue;
		inputs[selected++] = pack;
	}
	xa_unlock(&zram->pwb.packs);
	mutex_unlock(&zram->pwb.pack_lock);
	for (i = 0; i < selected; i++) {
		struct zram_pwb_cache *cache;
		struct zram_pwb_header *header;
		struct zram_pwb_directory *directory;
		void *data;
		u32 count;
		u32 ordinal;

		if (zram_pp_operation_cancelled(operation)) {
			ret = -ECANCELED;
			break;
		}
		cache = zram_pwb_cache_get(inputs[i]);
		if (!cache)
			continue;
		wait_for_completion(&cache->done);
		if (cache->error) {
			zram_pwb_cache_put(cache);
			continue;
		}
		data = kmap_local_page(cache->page);
		memcpy(copy, data, PAGE_SIZE);
		kunmap_local(data);
		zram_pwb_cache_put(cache);
		header = copy;
		directory = copy + sizeof(*header);
		count = le16_to_cpu(header->object_count);
		for (ordinal = 0; ordinal < count; ordinal++) {
			u32 offset = le16_to_cpu(directory[ordinal].offset);
			u32 length = le16_to_cpu(directory[ordinal].length);

			if (builder->object_count >= ZRAM_PWB_MAX_OBJECTS ||
			    length > PAGE_SIZE - ZRAM_PWB_DATA_OFFSET -
					 builder->payload_bytes)
				break;
			zram_pwb_gc_capture(zram, operation, inputs[i],
				&directory[ordinal], ordinal, copy + offset,
				builder);
		}
		atomic64_inc(&zram->pwb.stats.gc_input_packs);
		if (builder->object_count >= ZRAM_PWB_MAX_OBJECTS)
			break;
	}
	if (!builder->object_count)
		goto out;
	if (zram_pp_operation_cancelled(operation)) {
		ret = -ECANCELED;
		goto out;
	}
	if (!zram_pp_budget_reserve_physical(&operation->budget, 1)) {
		ret = -EDQUOT;
		goto out;
	}
	if (!zram_backing_write_reserve(zram, 1)) {
		zram_pp_budget_rollback_physical(&operation->budget, 1);
		ret = -EDQUOT;
		goto out;
	}
	block = zram_backing_alloc(zram, 1);
	if (!block) {
		zram_pp_budget_rollback_physical(&operation->budget, 1);
		zram_backing_write_rollback(zram, 1);
		atomic64_inc(&zram->pwb.stats.enospc);
		ret = -ENOSPC;
		goto out;
	}
	builder->pack = zram_pwb_pack_alloc(zram, block,
					    builder->payload_bytes +
					    builder->object_count *
					    sizeof(struct zram_pwb_directory));
	if (!builder->pack) {
		ret = -ENOMEM;
		goto rollback;
	}
	zram_pwb_fill_header(builder);
	if (zram_pp_operation_cancelled(operation) ||
	    atomic_read(&zram->pwb.state) != ZRAM_FEATURE_ENABLED ||
	    atomic_read(&zram->pwb.gc_state) != ZRAM_FEATURE_ENABLED) {
		ret = -ECANCELED;
		goto rollback;
	}
	ret = zram_pwb_submit(zram, builder, 1, block, &write_submitted);
	if (write_submitted) {
		zram_pp_budget_commit_physical(&operation->budget, 1, true);
		atomic64_inc(&zram->stats.bd_writes);
		atomic64_inc(&zram->pwb.stats.physical_writes);
		atomic64_inc(&zram->pwb.stats.gc_physical_writes);
	}
	if (ret)
		goto rollback;
	if (WARN_ON_ONCE(!write_submitted)) {
		ret = -EIO;
		goto rollback;
	}
	ret = zram_pwb_publish_gc_pack(zram, builder);
	if (ret)
		goto written_unpublished;
	atomic64_inc(&zram->pwb.stats.gc_output_packs);
	block = 0;
	ret = len;
	goto out;

rollback:
	if (!write_submitted) {
		zram_pp_budget_rollback_physical(&operation->budget, 1);
		zram_backing_write_rollback(zram, 1);
	}
written_unpublished:
	if (builder->pack) {
		zram_pwb_pack_put(builder->pack);
		builder->pack = NULL;
		block = 0;
	} else if (block) {
		zram_backing_free(zram, block, 1);
		block = 0;
	}
out:
	if (builder) {
		for (i = 0; i < builder->object_count; i++)
			zram_pwb_abort_object(zram, &builder->objects[i]);
		if (builder->page)
			__free_page(builder->page);
	}
	if (inputs) {
		for (i = 0; i < selected; i++)
			zram_pwb_pack_put(inputs[i]);
	}
	if (operation)
		zram_pp_operation_end(operation);
	kfree(builder);
	kfree(copy);
	kfree(inputs);
	return ret;
}

static void zram_pwb_gc_work(struct work_struct *work)
{
	struct zram_pwb_gc_request *request = container_of(work,
				struct zram_pwb_gc_request, work);

	request->result = zram_pwb_gc_execute(request->zram, request->buf,
					       request->len);
	zram_pp_io_put(&request->zram->pp_scheduler);
	complete(&request->done);
}

ssize_t zram_pwb_gc_run(struct zram *zram, const char *buf, size_t len)
{
	struct zram_pwb_gc_request request = {
		.zram = zram,
		.buf = buf,
		.len = len,
	};

	init_completion(&request.done);
	INIT_WORK_ONSTACK(&request.work, zram_pwb_gc_work);
	if (!zram_pp_io_get(&zram->pp_scheduler)) {
		destroy_work_on_stack(&request.work);
		return -ESHUTDOWN;
	}
	if (WARN_ON_ONCE(!queue_work(zram->pp_scheduler.cpu_wq,
				    &request.work))) {
		zram_pp_io_put(&zram->pp_scheduler);
		destroy_work_on_stack(&request.work);
		return -EBUSY;
	}
	wait_for_completion(&request.done);
	destroy_work_on_stack(&request.work);
	return request.result;
}

int zram_pwb_init(struct zram *zram)
{
	BUILD_BUG_ON(ZRAM_PWB_DATA_OFFSET >= LZ4KD_DELTA_PAGE_SIZE);
	BUILD_BUG_ON(ZRAM_PWB_MAX_OBJECTS > U16_MAX);
	memset(&zram->pwb, 0, sizeof(zram->pwb));
	atomic_set(&zram->pwb.state, ZRAM_FEATURE_DISABLED);
	atomic_set(&zram->pwb.native_state, ZRAM_FEATURE_DISABLED);
	atomic_set(&zram->pwb.gc_state, ZRAM_FEATURE_DISABLED);
	xa_init(&zram->pwb.packs);
	xa_init(&zram->pwb.read_cache);
	mutex_init(&zram->pwb.state_lock);
	mutex_init(&zram->pwb.pack_lock);
	mutex_init(&zram->pwb.read_lock);
	zram->pwb.next_pack_id = 1;
	zram->pwb.pack_generation = 1;
	zram->pwb.gc_dead_percent = ZRAM_PWB_DEFAULT_GC_DEAD;
	return 0;
}

void zram_pwb_quiesce(struct zram *zram)
{
	mutex_lock(&zram->pwb.state_lock);
	if (atomic_read(&zram->pwb.state) != ZRAM_FEATURE_QUIESCING)
		zram->pwb.resume_state = atomic_read(&zram->pwb.state);
	if (atomic_read(&zram->pwb.native_state) != ZRAM_FEATURE_QUIESCING)
		zram->pwb.native_resume_state =
			atomic_read(&zram->pwb.native_state);
	if (atomic_read(&zram->pwb.gc_state) != ZRAM_FEATURE_QUIESCING)
		zram->pwb.gc_resume_state = atomic_read(&zram->pwb.gc_state);
	atomic_set(&zram->pwb.state, ZRAM_FEATURE_QUIESCING);
	atomic_set(&zram->pwb.native_state, ZRAM_FEATURE_QUIESCING);
	atomic_set(&zram->pwb.gc_state, ZRAM_FEATURE_QUIESCING);
	mutex_unlock(&zram->pwb.state_lock);
}

void zram_pwb_resume(struct zram *zram)
{
	mutex_lock(&zram->pwb.state_lock);
	if (!zram->disksize) {
		atomic_set(&zram->pwb.state, ZRAM_FEATURE_DISABLED);
		atomic_set(&zram->pwb.native_state, ZRAM_FEATURE_DISABLED);
		atomic_set(&zram->pwb.gc_state, ZRAM_FEATURE_DISABLED);
	} else {
		atomic_set(&zram->pwb.state, zram->pwb.resume_state);
		atomic_set(&zram->pwb.native_state,
			   zram->pwb.native_resume_state);
		atomic_set(&zram->pwb.gc_state, zram->pwb.gc_resume_state);
	}
	mutex_unlock(&zram->pwb.state_lock);
}

void zram_pwb_reset(struct zram *zram)
{
	struct zram_pwb_pack *pack;

	mutex_lock(&zram->pwb.pack_lock);
	for (;;) {
		XA_STATE(xas, &zram->pwb.packs, 0);
		void *entry;

		pack = NULL;
		xa_lock(&zram->pwb.packs);
		xas_for_each(&xas, entry, ULONG_MAX) {
			xas_store(&xas, NULL);
			if (xa_is_zero(entry))
				continue;
			pack = entry;
			break;
		}
		xa_unlock(&zram->pwb.packs);
		if (!pack)
			break;
		pack->in_table = false;
		WARN_ON_ONCE(atomic_read(&pack->live_objects) ||
				     atomic_read(&pack->readers) ||
				     atomic_read(&pack->publishers));
		zram_pwb_pack_put(pack);
	}
	mutex_unlock(&zram->pwb.pack_lock);
	WARN_ON_ONCE(!xa_empty(&zram->pwb.read_cache));
	zram->pwb.next_pack_id = 1;
	zram->pwb.pack_generation++;
	if (!zram->pwb.pack_generation)
		zram->pwb.pack_generation++;
	memset(&zram->pwb.stats, 0, sizeof(zram->pwb.stats));
	atomic_set(&zram->pwb.state, ZRAM_FEATURE_DISABLED);
	atomic_set(&zram->pwb.native_state, ZRAM_FEATURE_DISABLED);
	atomic_set(&zram->pwb.gc_state, ZRAM_FEATURE_DISABLED);
}

void zram_pwb_fini(struct zram *zram)
{
	zram_pwb_reset(zram);
	xa_destroy(&zram->pwb.read_cache);
	xa_destroy(&zram->pwb.packs);
}
