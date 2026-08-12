// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/blk-cgroup.h>
#include <linux/cgroup-defs.h>
#include <linux/highmem.h>
#include <linux/ktime.h>
#include <linux/lz4kd.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/vmalloc.h>

#include "zram_drv.h"

#define ZRAM_ENGINE_DEFAULT_CPU_NS	(2ULL * NSEC_PER_MSEC)
#define ZRAM_ENGINE_DEFAULT_PIN_BYTES	(16ULL * 1024 * 1024)
#define ZRAM_ENGINE_DELTA_RESERVE	4U
#define ZRAM_ENGINE_ID_PROBES		64U

struct zram_delta_scratch {
	u8 wire[PAGE_SIZE];
	u8 workspace[PAGE_SIZE];
};

struct zram_sddc_ref {
	refcount_t refs;
	struct zram *zram;
	unsigned long handle;
	u64 owner;
	u32 id;
	u32 generation;
	u32 hash;
	u32 size;
	u32 logical_size;
	u8 type;
	u8 codec_id;
	u16 codec_generation;
	atomic_t resident_refs;
	atomic_t backing_refs;
	atomic_t reader_refs;
	bool owns_payload;
	bool codec_held;
	bool in_table;
};

struct zram_sddc_ext {
	struct zram_ext_rep base;
	struct zram_sddc_ref *reference;
	unsigned long wire_handle;
	u64 owner;
	u32 reference_id;
	u32 reference_generation;
	u32 integrity;
	u32 wire_size;
	u8 kind;
};

struct zram_sddc_candidate {
	struct zram_slot_txn txn;
	struct zram_sddc_ref *reference;
	u32 index;
	bool ordinary;
};

static bool zram_engine_ordinary(u8 type)
{
	return type == ZRAM_REP_RAW || type == ZRAM_REP_COMPRESSED;
}

static void __maybe_unused zram_engine_ema_update(atomic64_t *ema, u64 sample)
{
	u64 old;
	u64 next;

	do {
		old = atomic64_read(ema);
		next = old ? old - (old >> 3) + (sample >> 3) : sample;
	} while (atomic64_cmpxchg(ema, old, next) != old);
}

bool zram_engine_rep_is_managed(u8 type)
{
	return type == ZRAM_REP_REF || type == ZRAM_REP_ALIAS ||
	       type == ZRAM_REP_DELTA || type == ZRAM_REP_PACKED_BACKING;
}

static u64 zram_engine_next_identity(struct zram *zram)
{
	u64 identity = atomic64_inc_return(&zram->engine.next_ext_identity);

	if (unlikely(!identity))
		identity = atomic64_inc_return(&zram->engine.next_ext_identity);
	return identity;
}

static void zram_sddc_ref_put(struct zram_sddc_ref *reference);

static void zram_sddc_ref_destroy(struct zram_sddc_ref *reference)
{
	struct zram *zram = reference->zram;

	WARN_ON_ONCE(reference->in_table);
	WARN_ON_ONCE(atomic_read(&reference->resident_refs));
	WARN_ON_ONCE(atomic_read(&reference->backing_refs));
	WARN_ON_ONCE(atomic_read(&reference->reader_refs));
	if (reference->owns_payload) {
		zs_free(zram->mem_pool, reference->handle);
		atomic64_sub(reference->size, &zram->stats.compr_data_size);
		atomic64_dec(&zram->engine.stats.sddc_refs);
		atomic64_sub(reference->size,
			     &zram->engine.stats.sddc_ref_bytes);
	}
	if (reference->codec_held)
		zram_codec_rep_put(zram, reference->codec_id);
	kfree(reference);
}

static void zram_sddc_ref_put(struct zram_sddc_ref *reference)
{
	if (refcount_dec_and_test(&reference->refs))
		zram_sddc_ref_destroy(reference);
}

static bool zram_sddc_ref_get(struct zram_sddc_ref *reference)
{
	return refcount_inc_not_zero(&reference->refs);
}

static void zram_sddc_ref_try_reap(struct zram_sddc_ref *reference)
{
	struct zram *zram = reference->zram;
	XA_STATE(xas, &zram->engine.references, reference->id);
	bool removed = false;

	if (atomic_read(&reference->resident_refs) ||
	    atomic_read(&reference->backing_refs) ||
	    atomic_read(&reference->reader_refs))
		return;
	xa_lock(&zram->engine.references);
	if (!atomic_read(&reference->resident_refs) &&
	    !atomic_read(&reference->backing_refs) &&
	    !atomic_read(&reference->reader_refs) &&
	    xas_load(&xas) == reference) {
		__xa_erase(&zram->engine.references, reference->id);
		WRITE_ONCE(reference->in_table, false);
		removed = true;
	}
	xa_unlock(&zram->engine.references);
	if (removed)
		zram_sddc_ref_put(reference);
}

static bool zram_sddc_reader_get(struct zram_sddc_ref *reference)
{
	if (!zram_sddc_ref_get(reference))
		return false;
	atomic_inc(&reference->reader_refs);
	return true;
}

static void zram_sddc_reader_put(struct zram_sddc_ref *reference)
{
	WARN_ON_ONCE(atomic_dec_return(&reference->reader_refs) < 0);
	zram_sddc_ref_try_reap(reference);
	zram_sddc_ref_put(reference);
}

static struct zram_sddc_ref *
zram_sddc_reference_lookup(struct zram *zram, u32 id, u32 generation)
{
	XA_STATE(xas, &zram->engine.references, id);
	struct zram_sddc_ref *reference = NULL;

	xa_lock(&zram->engine.references);
	reference = xas_load(&xas);
	if (!reference || reference->generation != generation ||
	    !zram_sddc_reader_get(reference))
		reference = NULL;
	xa_unlock(&zram->engine.references);
	if (!reference)
		atomic64_inc(&zram->engine.stats.sddc_cookie_mismatches);
	return reference;
}

static struct zram_sddc_ref *
zram_sddc_reference_create(struct zram *zram,
			   const struct zram_slot_snapshot *source,
			   u64 owner, u32 hash)
{
	struct zram_sddc_ref *reference;
	u32 probes;
	u32 id;
	int ret;

	reference = kzalloc(sizeof(*reference), GFP_NOIO | __GFP_NOWARN);
	if (!reference)
		return ERR_PTR(-ENOMEM);
	refcount_set(&reference->refs, 2); /* creator and reference table */
	reference->zram = zram;
	reference->handle = source->handle;
	reference->owner = owner;
	reference->hash = hash;
	reference->size = source->obj_size;
	reference->logical_size = PAGE_SIZE;
	reference->type = source->rep.type;
	reference->codec_id = source->rep.codec_id;
	reference->codec_generation = source->rep.codec_generation;

	ret = zram_codec_rep_get(zram, reference->codec_id);
	if (ret) {
		kfree(reference);
		return ERR_PTR(ret);
	}
	reference->codec_held = true;

	mutex_lock(&zram->engine.reference_lock);
	for (probes = 0; probes < ZRAM_ENGINE_ID_PROBES; probes++) {
		id = zram->engine.next_reference_id++;
		if (unlikely(!id)) {
			zram->engine.reference_generation++;
			if (!zram->engine.reference_generation)
				zram->engine.reference_generation++;
			id = zram->engine.next_reference_id++;
		}
		reference->id = id;
		reference->generation = zram->engine.reference_generation;
		WRITE_ONCE(reference->in_table, true);
		ret = xa_insert(&zram->engine.references, id, reference,
				GFP_NOIO);
		if (ret)
			WRITE_ONCE(reference->in_table, false);
		if (ret != -EBUSY)
			break;
	}
	if (probes == ZRAM_ENGINE_ID_PROBES)
		ret = -ENOSPC;
	if (ret) {
		mutex_unlock(&zram->engine.reference_lock);
		zram_sddc_ref_put(reference);
		zram_sddc_ref_put(reference);
		return ERR_PTR(ret);
	}
	mutex_unlock(&zram->engine.reference_lock);
	return reference;
}

static void zram_sddc_ext_release(struct zram *zram,
				  struct zram_ext_rep *base)
{
	struct zram_sddc_ext *ext = container_of(base,
						 struct zram_sddc_ext, base);
	struct zram_sddc_ref *reference = ext->reference;

	if (ext->wire_handle)
		zs_free(zram->mem_pool, ext->wire_handle);
	WARN_ON_ONCE(atomic_dec_return(&reference->resident_refs) < 0);
	zram_sddc_ref_try_reap(reference);
	zram_sddc_ref_put(reference);
	kfree(ext);
}

static struct zram_sddc_ext *
zram_sddc_ext_create(struct zram *zram, struct zram_sddc_ref *reference,
		     u8 kind, unsigned long wire_handle, u32 wire_size,
		     u32 integrity, u64 owner)
{
	struct zram_sddc_ext *ext;

	ext = kzalloc(sizeof(*ext), GFP_NOIO | __GFP_NOWARN);
	if (!ext)
		return NULL;
	if (!zram_sddc_ref_get(reference)) {
		kfree(ext);
		return NULL;
	}
	atomic_inc(&reference->resident_refs);
	zram_ext_rep_init(&ext->base, zram_engine_next_identity(zram),
			  zram_sddc_ext_release);
	ext->reference = reference;
	ext->wire_handle = wire_handle;
	ext->wire_size = wire_size;
	ext->kind = kind;
	ext->integrity = integrity;
	ext->owner = owner;
	ext->reference_id = reference->id;
	ext->reference_generation = reference->generation;
	return ext;
}

static int zram_sddc_ref_read(struct zram_sddc_ref *reference,
			      struct page *page)
{
	struct zram *zram = reference->zram;
	struct zcomp_strm *zstrm;
	struct zcomp *comp;
	void *src;
	void *dst;
	int ret = 0;

	if (!reference->owns_payload || !reference->handle ||
	    reference->logical_size != PAGE_SIZE)
		return -EIO;
	if (reference->type == ZRAM_REP_RAW) {
		if (reference->size != PAGE_SIZE)
			return -EIO;
		src = zs_obj_read_begin(zram->mem_pool, reference->handle,
					reference->size, NULL);
		if (!src)
			return -EIO;
		dst = kmap_local_page(page);
		memcpy(dst, src, PAGE_SIZE);
		if (lz4kd_delta_hash(dst, PAGE_SIZE) != reference->hash) {
			memset(dst, 0, PAGE_SIZE);
			ret = -EBADMSG;
		}
		kunmap_local(dst);
		zs_obj_read_end(zram->mem_pool, reference->handle,
				reference->size, src);
		return ret;
	}
	if (reference->type != ZRAM_REP_COMPRESSED ||
	    !reference->codec_id || !reference->size ||
	    reference->size >= PAGE_SIZE)
		return -EIO;
	if (zram->codec_generation[reference->codec_id] !=
	    reference->codec_generation)
		return -ESTALE;
	comp = zram_codec_by_id(zram, reference->codec_id);
	if (!comp)
		return -EIO;
	zstrm = zcomp_stream_get(comp);
	src = zs_obj_read_begin(zram->mem_pool, reference->handle,
				reference->size, zstrm->local_copy);
	if (!src) {
		zcomp_stream_put(zstrm);
		return -EIO;
	}
	dst = kmap_local_page(page);
	ret = zcomp_decompress(comp, zstrm, src, reference->size, dst);
	if (!ret && lz4kd_delta_hash(dst, PAGE_SIZE) != reference->hash)
		ret = -EBADMSG;
	if (ret)
		memset(dst, 0, PAGE_SIZE);
	kunmap_local(dst);
	zs_obj_read_end(zram->mem_pool, reference->handle,
			reference->size, src);
	zcomp_stream_put(zstrm);
	return ret;
}

int zram_engine_read_managed_locked(struct zram *zram, struct page *page,
				    u32 index)
{
	struct zram_sddc_ext *ext;
	struct zram_ext_rep *base;
	struct zram_delta_scratch *scratch = NULL;
	struct page *reference_page = NULL;
	struct lz4kd_delta_ctx ctx;
	void *wire;
	void *src;
	void *dst;
	int ret;

	memzero_page(page, 0, PAGE_SIZE);
	base = zram_rep_extended_locked(zram, index);
	if (!base)
		return -EIO;
	ext = container_of(base, struct zram_sddc_ext, base);
	if (!ext->reference ||
	    (zram_rep_type_locked(zram, index) == ZRAM_REP_REF &&
	     ext->kind != ZRAM_SDDC_WIRE_REF) ||
	    (zram_rep_type_locked(zram, index) == ZRAM_REP_ALIAS &&
	     ext->kind != ZRAM_SDDC_WIRE_ALIAS) ||
	    (zram_rep_type_locked(zram, index) == ZRAM_REP_DELTA &&
	     ext->kind != ZRAM_SDDC_WIRE_DELTA))
		return -EIO;
	if (ext->reference_id != ext->reference->id ||
	    ext->reference_generation != ext->reference->generation ||
	    ext->owner != ext->reference->owner) {
		atomic64_inc(&zram->engine.stats.sddc_cookie_mismatches);
		return -ESTALE;
	}
	if (ext->kind != ZRAM_SDDC_WIRE_DELTA &&
	    ext->integrity != ext->reference->hash) {
		atomic64_inc(&zram->engine.stats.sddc_integrity_failures);
		atomic64_inc(&zram->engine.stats.sddc_decode_failures);
		return -EBADMSG;
	}
	if (ext->kind == ZRAM_SDDC_WIRE_REF ||
	    ext->kind == ZRAM_SDDC_WIRE_ALIAS) {
		ret = zram_sddc_ref_read(ext->reference, page);
		if (ret) {
			atomic64_inc(&zram->engine.stats.sddc_decode_failures);
			if (ret == -EBADMSG)
				atomic64_inc(&zram->engine.stats.sddc_integrity_failures);
		}
		return ret;
	}
	if (ext->kind != ZRAM_SDDC_WIRE_DELTA || !ext->wire_handle ||
	    !ext->wire_size || ext->wire_size >= PAGE_SIZE)
		return -EIO;
	if (!zram->engine.delta_scratch_pool || !zram->engine.delta_page_pool)
		return -EOPNOTSUPP;

	scratch = mempool_alloc(zram->engine.delta_scratch_pool, GFP_NOIO);
	reference_page = mempool_alloc(zram->engine.delta_page_pool, GFP_NOIO);
	if (!scratch || !reference_page) {
		ret = -ENOMEM;
		goto out;
	}
	wire = scratch->wire;
	ret = zram_sddc_ref_read(ext->reference, reference_page);
	if (ret) {
		atomic64_inc(&zram->engine.stats.sddc_decode_failures);
		if (ret == -EBADMSG)
			atomic64_inc(&zram->engine.stats.sddc_integrity_failures);
		goto out;
	}
	src = zs_obj_read_begin(zram->mem_pool, ext->wire_handle,
				ext->wire_size, wire);
	if (!src) {
		ret = -EIO;
		goto out;
	}
	if (src != wire)
		memcpy(wire, src, ext->wire_size);
	zs_obj_read_end(zram->mem_pool, ext->wire_handle, ext->wire_size, src);
	if (lz4kd_delta_hash(wire, ext->wire_size) != ext->integrity) {
		ret = -EBADMSG;
		goto integrity;
	}
	ret = lz4kd_delta_validate(wire, ext->wire_size,
				   ext->reference_id,
				   ext->reference_generation);
	if (ret)
		goto integrity;
	ctx.workspace = scratch->workspace;
	ctx.workspace_size = PAGE_SIZE;
	src = kmap_local_page(reference_page);
	dst = kmap_local_page(page);
	ret = lz4kd_delta_decode(&ctx, wire, ext->wire_size, src, PAGE_SIZE,
				 dst, PAGE_SIZE);
	if (ret)
		memset(dst, 0, PAGE_SIZE);
	kunmap_local(dst);
	kunmap_local(src);
	if (ret)
		goto integrity;
	goto out;

integrity:
	atomic64_inc(&zram->engine.stats.sddc_integrity_failures);
	atomic64_inc(&zram->engine.stats.sddc_decode_failures);
out:
	if (ret)
		memzero_page(page, 0, PAGE_SIZE);
	if (scratch)
		mempool_free(scratch, zram->engine.delta_scratch_pool);
	if (reference_page)
		mempool_free(reference_page, zram->engine.delta_page_pool);
	return ret;
}

void zram_engine_release_managed_locked(
	struct zram *zram, u32 index, const struct zram_slot_snapshot *snapshot)
{
	switch (snapshot->rep.type) {
	case ZRAM_REP_REF:
		break;
	case ZRAM_REP_ALIAS:
		atomic64_dec(&zram->engine.stats.sddc_aliases);
		break;
	case ZRAM_REP_DELTA:
		atomic64_dec(&zram->engine.stats.sddc_deltas);
		atomic64_sub(snapshot->obj_size,
			     &zram->engine.stats.sddc_delta_wire_bytes);
		atomic64_sub(snapshot->obj_size, &zram->stats.compr_data_size);
		break;
	default:
		return;
	}
	zram->table[index].attr.flags &= BIT(ZRAM_LOCK);
	zram_set_handle(zram, index, 0);
	zram_set_obj_size(zram, index, 0);
	atomic64_dec(&zram->stats.pages_stored);
}

static u32 zram_sddc_sample_hash(const void *data)
{
	const u8 *bytes = data;
	u32 hash = 2166136261U;
	u32 offset;

	for (offset = 0; offset < PAGE_SIZE; offset += 64) {
		hash ^= bytes[offset];
		hash *= 16777619U;
	}
	return hash;
}

static void zram_sddc_index_insert(struct zram *zram, u32 index,
				   u64 generation, u64 owner, u32 full_hash,
				   u32 sample_hash)
{
	struct zram_engine *engine = &zram->engine;
	struct zram_sddc_cell *cell;
	unsigned long flags;
	u32 bucket;
	u32 way;

	spin_lock_irqsave(&engine->index_lock, flags);
	bucket = full_hash % ZRAM_SDDC_BUCKETS;
	way = engine->exact_hand[bucket]++ % ZRAM_SDDC_WAYS;
	cell = &engine->exact_index[bucket * ZRAM_SDDC_WAYS + way];
	*cell = (struct zram_sddc_cell) {
		.hash = full_hash,
		.generation = generation,
		.owner = owner,
		.index = index,
	};
	bucket = sample_hash % ZRAM_SDDC_BUCKETS;
	way = engine->sample_hand[bucket]++ % ZRAM_SDDC_WAYS;
	cell = &engine->sample_index[bucket * ZRAM_SDDC_WAYS + way];
	*cell = (struct zram_sddc_cell) {
		.hash = sample_hash,
		.generation = generation,
		.owner = owner,
		.index = index,
	};
	spin_unlock_irqrestore(&engine->index_lock, flags);
}

static u32 __maybe_unused
zram_sddc_index_candidates(struct zram *zram, u64 owner, u32 full_hash,
			   u32 sample_hash, struct zram_sddc_cell *candidates)
{
	struct zram_engine *engine = &zram->engine;
	unsigned long flags;
	u32 count = 0;
	u32 bucket;
	u32 way;

	spin_lock_irqsave(&engine->index_lock, flags);
	bucket = full_hash % ZRAM_SDDC_BUCKETS;
	for (way = 0; way < ZRAM_SDDC_WAYS &&
	     count < ZRAM_SDDC_CANDIDATES; way++) {
		struct zram_sddc_cell *cell =
			&engine->exact_index[bucket * ZRAM_SDDC_WAYS + way];

		if (cell->hash == full_hash && cell->owner == owner)
			candidates[count++] = *cell;
	}
	bucket = sample_hash % ZRAM_SDDC_BUCKETS;
	for (way = 0; way < ZRAM_SDDC_WAYS &&
	     count < ZRAM_SDDC_CANDIDATES; way++) {
		struct zram_sddc_cell *cell =
			&engine->sample_index[bucket * ZRAM_SDDC_WAYS + way];

		if (cell->hash == sample_hash && cell->owner == owner)
			candidates[count++] = *cell;
	}
	spin_unlock_irqrestore(&engine->index_lock, flags);
	return count;
}

static int zram_sddc_candidate_read(struct zram *zram,
				    const struct zram_sddc_cell *cell,
				    struct page *page,
				    struct zram_sddc_candidate *candidate)
{
	struct zram_sddc_ext *ext;
	u8 type;
	int ret = -ESTALE;

	memset(candidate, 0, sizeof(*candidate));
	candidate->index = cell->index;
	if (cell->index >= zram->disksize >> PAGE_SHIFT)
		goto stale;
	zram_slot_lock(zram, cell->index);
	type = zram_rep_type_locked(zram, cell->index);
	if (zram_rep_mutation_seq_locked(zram, cell->index) !=
	    cell->generation || zram->engine.slot_owner[cell->index] !=
	    cell->owner)
		goto out;
	if (zram_engine_ordinary(type)) {
		zram_slot_txn_snapshot_locked(zram, cell->index,
					      &candidate->txn);
		ret = zram_read_from_zspool(zram, page, cell->index);
		candidate->ordinary = !ret;
		goto out;
	}
	if (type != ZRAM_REP_REF)
		goto out;
	{
		struct zram_ext_rep *base =
			zram_rep_extended_locked(zram, cell->index);

		if (!base)
			goto out;
		ext = container_of(base, struct zram_sddc_ext, base);
	}
	if (!ext || !ext->reference || ext->kind != ZRAM_SDDC_WIRE_REF ||
	    ext->wire_handle || ext->wire_size)
		goto out;
	if (ext->reference_id != ext->reference->id ||
	    ext->reference_generation != ext->reference->generation ||
	    ext->owner != cell->owner ||
	    ext->owner != ext->reference->owner ||
	    !READ_ONCE(ext->reference->in_table)) {
		atomic64_inc(&zram->engine.stats.sddc_cookie_mismatches);
		goto out;
	}
	if (ext->integrity != ext->reference->hash ||
	    !ext->reference->owns_payload || !ext->reference->handle ||
	    ext->reference->logical_size != PAGE_SIZE) {
		atomic64_inc(&zram->engine.stats.sddc_integrity_failures);
		goto out;
	}
	if (!zram_sddc_reader_get(ext->reference))
		goto out;
	candidate->reference = ext->reference;
	ret = zram_sddc_ref_read(ext->reference, page);
	if (ret) {
		zram_sddc_reader_put(ext->reference);
		candidate->reference = NULL;
	}
out:
	zram_slot_unlock(zram, cell->index);
stale:
	if (ret)
		atomic64_inc(&zram->engine.stats.sddc_stale_cells);
	return ret;
}

static void zram_sddc_candidate_put(struct zram *zram,
				    struct zram_sddc_candidate *candidate)
{
	if (candidate->reference)
		zram_sddc_reader_put(candidate->reference);
	zram_slot_txn_abort(zram, &candidate->txn);
}

static void zram_sddc_publish_reference(struct zram *zram, u32 index,
		const struct zram_slot_snapshot *old, void *private)
{
	struct zram_sddc_ref *reference = private;

	reference->owns_payload = true;
	if (old->rep.type == ZRAM_REP_RAW &&
	    (old->identity_flags & BIT(ZRAM_HUGE)))
		atomic64_dec(&zram->stats.huge_pages);
	zram->table[index].attr.flags &= BIT(ZRAM_LOCK);
	zram_set_handle(zram, index, 0);
	zram_set_obj_size(zram, index, 0);
	atomic64_inc(&zram->engine.stats.sddc_refs);
	atomic64_add(reference->size, &zram->engine.stats.sddc_ref_bytes);
}

struct zram_sddc_publish_target {
	unsigned long wire_handle;
	u32 wire_size;
	u32 saved;
	u8 kind;
};

static void zram_sddc_publish_target(struct zram *zram, u32 index,
		const struct zram_slot_snapshot *old, void *private)
{
	struct zram_sddc_publish_target *target = private;

	zram_release_slot_data_locked(zram, index, old);
	zram_set_handle(zram, index, target->wire_handle);
	zram_set_obj_size(zram, index, target->wire_size);
	atomic64_inc(&zram->stats.pages_stored);
	atomic64_add(target->saved, &zram->engine.stats.sddc_saved_bytes);
	if (target->kind == ZRAM_SDDC_WIRE_ALIAS)
		atomic64_inc(&zram->engine.stats.sddc_aliases);
	else {
		atomic64_inc(&zram->engine.stats.sddc_deltas);
		atomic64_add(target->wire_size,
			     &zram->engine.stats.sddc_delta_wire_bytes);
		atomic64_add(target->wire_size, &zram->stats.compr_data_size);
	}
}

static int zram_sddc_commit_existing_ref(
	struct zram *zram, struct zram_pp_job *job,
	struct zram_slot_txn *target_txn, struct zram_sddc_ref *reference,
	u64 owner, u8 kind, unsigned long wire_handle, u32 wire_size,
	u32 integrity)
{
	struct zram_sddc_publish_target publish = {
		.wire_handle = wire_handle,
		.wire_size = wire_size,
		.kind = kind,
	};
	struct zram_sddc_ext *target_ext;
	u8 target_type = kind == ZRAM_SDDC_WIRE_ALIAS ?
		ZRAM_REP_ALIAS : ZRAM_REP_DELTA;
	int ret;
	u32 cost = wire_size + sizeof(struct zram_sddc_ext);

	if (target_txn->snapshot.obj_size > cost)
		publish.saved = target_txn->snapshot.obj_size - cost;

	target_ext = zram_sddc_ext_create(zram, reference, kind, wire_handle,
					  wire_size, integrity, owner);
	if (!target_ext) {
		if (wire_handle)
			zs_free(zram->mem_pool, wire_handle);
		return -ENOMEM;
	}
	ret = zram_slot_txn_prepare(zram, target_txn, target_type,
				    ZRAM_CODEC_NONE, &target_ext->base, GFP_NOIO);
	if (ret) {
		zram_ext_rep_put(zram, &target_ext->base);
		return ret;
	}
	zram_slot_lock(zram, target_txn->index);
	if (atomic_read(&zram->engine.sddc_state) != ZRAM_FEATURE_ENABLED ||
	    !zram_pp_job_is_current_locked(zram, job)) {
		ret = -ESTALE;
		goto out_unlock;
	}
	ret = zram_slot_txn_commit_if_current_locked(zram, target_txn,
				zram_sddc_publish_target, &publish);
	if (!ret)
		WRITE_ONCE(job->generation,
			   zram_rep_mutation_seq_locked(zram, target_txn->index));
out_unlock:
	zram_slot_unlock(zram, target_txn->index);
	return ret;
}

static int zram_sddc_promote_and_commit(
	struct zram *zram, struct zram_pp_job *job,
	struct zram_slot_txn *target_txn,
	struct zram_sddc_candidate *source,
	struct zram_sddc_ref *prepared_reference, u64 owner, u32 full_hash,
	u32 sample_hash, u8 kind, unsigned long wire_handle,
	u32 wire_size, u32 integrity)
{
	struct zram_sddc_publish_target target_publish = {
		.wire_handle = wire_handle,
		.wire_size = wire_size,
		.kind = kind,
	};
	struct zram_sddc_ref *reference;
	struct zram_sddc_ext *source_ext;
	struct zram_sddc_ext *target_ext;
	u8 target_type = kind == ZRAM_SDDC_WIRE_ALIAS ?
		ZRAM_REP_ALIAS : ZRAM_REP_DELTA;
	u32 first = min(source->index, target_txn->index);
	u32 second = max(source->index, target_txn->index);
	bool created_here = false;
	int ret;
	u32 cost = wire_size + sizeof(struct zram_sddc_ref) +
		   2 * sizeof(struct zram_sddc_ext);

	if (target_txn->snapshot.obj_size > cost)
		target_publish.saved = target_txn->snapshot.obj_size - cost;

	reference = prepared_reference;
	if (!reference) {
		reference = zram_sddc_reference_create(zram,
			&source->txn.snapshot, owner, full_hash);
		if (IS_ERR(reference))
			goto free_wire;
		created_here = true;
	}
	source_ext = zram_sddc_ext_create(zram, reference,
					  ZRAM_SDDC_WIRE_REF, 0, 0,
					  full_hash, owner);
	target_ext = zram_sddc_ext_create(zram, reference, kind, wire_handle,
					  wire_size, integrity, owner);
	if (!source_ext || !target_ext) {
		if (source_ext)
			zram_ext_rep_put(zram, &source_ext->base);
		if (target_ext)
			zram_ext_rep_put(zram, &target_ext->base);
		else if (wire_handle)
			zs_free(zram->mem_pool, wire_handle);
		ret = -ENOMEM;
		goto out_reference;
	}
	ret = zram_slot_txn_prepare(zram, &source->txn, ZRAM_REP_REF,
				    ZRAM_CODEC_NONE, &source_ext->base, GFP_NOIO);
	if (ret) {
		zram_ext_rep_put(zram, &source_ext->base);
		zram_ext_rep_put(zram, &target_ext->base);
		goto out_reference;
	}
	ret = zram_slot_txn_prepare(zram, target_txn, target_type,
				    ZRAM_CODEC_NONE, &target_ext->base, GFP_NOIO);
	if (ret) {
		zram_ext_rep_put(zram, &target_ext->base);
		goto out_reference;
	}

	zram_slot_lock(zram, first);
	zram_slot_lock_nested(zram, second, SINGLE_DEPTH_NESTING);
	if (atomic_read(&zram->engine.sddc_state) != ZRAM_FEATURE_ENABLED ||
	    !zram_pp_job_is_current_locked(zram, job) ||
	    !zram_slot_txn_revalidate_locked(zram, &source->txn) ||
	    !zram_slot_txn_revalidate_locked(zram, target_txn)) {
		ret = -ESTALE;
		goto out_unlock;
	}
	ret = zram_slot_txn_commit_pair_if_current_locked(zram,
							  &source->txn,
							  zram_sddc_publish_reference,
							  reference,
							  target_txn,
							  zram_sddc_publish_target,
							  &target_publish);
	if (WARN_ON_ONCE(ret))
		goto out_unlock;
	WRITE_ONCE(job->generation,
		   zram_rep_mutation_seq_locked(zram, target_txn->index));
	zram_sddc_index_insert(zram, source->index,
		zram_rep_mutation_seq_locked(zram, source->index), owner,
		full_hash, sample_hash);
out_unlock:
	zram_slot_unlock(zram, second);
	zram_slot_unlock(zram, first);

out_reference:
	if (created_here) {
		zram_sddc_ref_try_reap(reference);
		zram_sddc_ref_put(reference);
	}
	return ret;

free_wire:
	if (wire_handle)
		zs_free(zram->mem_pool, wire_handle);
	return PTR_ERR(reference);
}

static int __maybe_unused
zram_sddc_try_candidate(struct zram *zram, struct zram_pp_job *job,
			struct zram_slot_txn *target_txn,
			struct page *target_page,
			const struct zram_sddc_cell *cell,
			u64 owner, u32 full_hash, u32 sample_hash)
{
	struct zram_sddc_candidate source;
	struct zram_sddc_ref *prepared_reference = NULL;
	struct lz4kd_delta_ctx ctx;
	struct page *source_page;
	unsigned long wire_handle = 0;
	unsigned int old_class;
	unsigned int new_class;
	u32 integrity = 0;
	u32 source_full_hash;
	u32 source_sample_hash;
	u8 kind;
	bool exact;
	void *source_data;
	void *target_data;
	void *wire = NULL;
	size_t wire_size = 0;
	int ret;

	if (cell->index == target_txn->index)
		return -EAGAIN;
	source_page = alloc_page(GFP_NOIO | __GFP_NOWARN);
	if (!source_page)
		return -ENOMEM;
	ret = zram_sddc_candidate_read(zram, cell, source_page, &source);
	if (ret)
		goto out_page;

	source_data = kmap_local_page(source_page);
	target_data = kmap_local_page(target_page);
	source_full_hash = lz4kd_delta_hash(source_data, PAGE_SIZE);
	source_sample_hash = zram_sddc_sample_hash(source_data);
	exact = !memcmp(source_data, target_data, PAGE_SIZE);
	kunmap_local(target_data);
	kunmap_local(source_data);
	atomic64_inc(&zram->engine.stats.sddc_exact_attempts);
	if (exact) {
		kind = ZRAM_SDDC_WIRE_ALIAS;
		if (target_txn->snapshot.obj_size <=
		    sizeof(struct zram_sddc_ext) +
		    (source.reference ? 0 : sizeof(struct zram_sddc_ref) +
		     sizeof(struct zram_sddc_ext))) {
			ret = -ENOSPC;
			goto out;
		}
	} else {
		kind = ZRAM_SDDC_WIRE_DELTA;
		atomic64_inc(&zram->engine.stats.sddc_delta_attempts);
		wire = kmalloc(PAGE_SIZE, GFP_NOIO | __GFP_NOWARN);
		if (!wire) {
			ret = -ENOMEM;
			goto out;
		}
		ret = lz4kd_delta_ctx_init(&ctx, GFP_NOIO | __GFP_NOWARN);
		if (ret)
			goto out;
		if (!source.reference) {
			prepared_reference = zram_sddc_reference_create(zram,
				&source.txn.snapshot, owner, source_full_hash);
			if (IS_ERR(prepared_reference)) {
				ret = PTR_ERR(prepared_reference);
				prepared_reference = NULL;
				lz4kd_delta_ctx_release(&ctx);
				goto out;
			}
		}
		source_data = kmap_local_page(source_page);
		target_data = kmap_local_page(target_page);
		if (source.reference) {
			ret = lz4kd_delta_encode(&ctx, source_data, PAGE_SIZE,
				target_data, PAGE_SIZE, source.reference->id,
				source.reference->generation, wire, PAGE_SIZE,
				&wire_size);
		} else {
			ret = lz4kd_delta_encode(&ctx, source_data, PAGE_SIZE,
				target_data, PAGE_SIZE, prepared_reference->id,
				prepared_reference->generation, wire, PAGE_SIZE,
				&wire_size);
		}
		kunmap_local(target_data);
		kunmap_local(source_data);
		if (!ret)
			ret = lz4kd_delta_validate(wire, wire_size,
				source.reference ? source.reference->id :
					prepared_reference->id,
				source.reference ? source.reference->generation :
					prepared_reference->generation);
		lz4kd_delta_ctx_release(&ctx);
		if (ret)
			goto out;
		old_class = zs_lookup_class_index(zram->mem_pool,
					 target_txn->snapshot.obj_size);
		new_class = zs_lookup_class_index(zram->mem_pool, wire_size);
		if (wire_size + sizeof(struct zram_sddc_ext) +
		    (source.reference ? 0 :
		     sizeof(struct zram_sddc_ref) +
		     sizeof(struct zram_sddc_ext)) >=
		    target_txn->snapshot.obj_size || new_class >= old_class) {
			ret = -ENOSPC;
			goto out;
		}
		wire_handle = zs_malloc(zram->mem_pool, wire_size,
			__GFP_KSWAPD_RECLAIM | __GFP_NOWARN | __GFP_HIGHMEM |
			__GFP_MOVABLE);
		if (IS_ERR_VALUE(wire_handle)) {
			ret = PTR_ERR((void *)wire_handle);
			wire_handle = 0;
			goto out;
		}
		ret = zs_obj_write(zram->mem_pool, wire_handle, wire, wire_size);
		if (ret)
			goto out;
		integrity = lz4kd_delta_hash(wire, wire_size);
	}
	ret = 0;

	if (source.reference) {
		unsigned long owned_handle = wire_handle;

		wire_handle = 0;
		ret = zram_sddc_commit_existing_ref(zram, job, target_txn,
			source.reference, owner, kind, owned_handle, wire_size,
			integrity);
	} else if (kind == ZRAM_SDDC_WIRE_ALIAS) {
		ret = zram_sddc_promote_and_commit(zram, job, target_txn,
			&source, NULL, owner, source_full_hash,
			source_sample_hash, kind, 0, 0, full_hash);
	} else {
		unsigned long owned_handle = wire_handle;

		wire_handle = 0;
		ret = zram_sddc_promote_and_commit(zram, job, target_txn,
			&source, prepared_reference, owner, source_full_hash,
			source_sample_hash, kind, owned_handle, wire_size,
			integrity);
	}
	if (!ret)
		wire_handle = 0;
	if (!ret) {
		if (kind == ZRAM_SDDC_WIRE_ALIAS)
			atomic64_inc(&zram->engine.stats.sddc_exact_hits);
		else
			atomic64_inc(&zram->engine.stats.sddc_delta_hits);
	}
out:
	if (wire_handle)
		zs_free(zram->mem_pool, wire_handle);
	if (prepared_reference) {
		zram_sddc_ref_try_reap(prepared_reference);
		zram_sddc_ref_put(prepared_reference);
	}
	kfree(wire);
	zram_sddc_candidate_put(zram, &source);
out_page:
	__free_page(source_page);
	return ret;
}

static int zram_engine_adaptive(struct zram *zram, struct zram_pp_job *job)
{
#if IS_ENABLED(CONFIG_ZRAM_ADAPTIVE_RECOMP)
	struct zram_slot_txn txn;
	struct zram_publish_obj publish = { };
	struct page *page;
	struct zcomp_strm *zstrm = NULL;
	unsigned long handle = 0;
	unsigned int old_class;
	unsigned int new_class;
	unsigned int length;
	u64 comp_start;
	u64 comp_ns;
	u64 decomp_start;
	u64 decomp_ns;
	u64 owner;
	u32 prio;
	u8 codec_id = ZRAM_CODEC_NONE;
	enum zcomp_exec_class current_exec = ZCOMP_EXEC_FAST;
	void *src;
	int ret = 0;

	if (atomic_read(&zram->engine.adaptive_state) !=
	    ZRAM_FEATURE_ENABLED)
		return 0;
	page = alloc_page(GFP_NOIO | __GFP_NOWARN);
	if (!page) {
		atomic64_inc(&zram->engine.stats.adaptive_alloc_failures);
		return 0;
	}
	zram_slot_lock(zram, job->index);
	if (!zram_pp_job_is_current_locked(zram, job)) {
		atomic64_inc(&zram->engine.stats.adaptive_stale);
		zram_slot_unlock(zram, job->index);
		goto out_page;
	}
	if (!zram_engine_ordinary(zram_rep_type_locked(zram, job->index))) {
		zram_slot_unlock(zram, job->index);
		goto out_page;
	}
	if ((u32)ktime_get_boottime_seconds() -
	    zram->engine.slot_stamp[job->index] <
	    zram->engine.adaptive_min_age_seconds) {
		zram_slot_unlock(zram, job->index);
		goto out_page;
	}
	zram_slot_txn_snapshot_locked(zram, job->index, &txn);
	owner = zram->engine.slot_owner[job->index];
	ret = zram_read_from_zspool(zram, page, job->index);
	zram_slot_unlock(zram, job->index);
	if (ret)
		goto out_txn;
	old_class = zs_lookup_class_index(zram->mem_pool,
					 txn.snapshot.obj_size);
	if (txn.snapshot.rep.codec_id) {
		struct zcomp *current_comp = zram_codec_by_id(zram,
						     txn.snapshot.rep.codec_id);

		if (current_comp)
			current_exec = zcomp_execution_class(current_comp);
	}

	for (prio = 0; prio < ZRAM_MAX_COMPS; prio++) {
		struct zcomp *comp = zram_comp_at_priority(zram, prio);
		struct zram_codec_perf *perf;

		codec_id = zram_policy_codec_id(zram, prio);
		if (!comp || codec_id == txn.snapshot.rep.codec_id ||
		    zcomp_execution_class(comp) <= current_exec)
			continue;
		perf = &zram->engine.codec_perf[codec_id];
		atomic64_inc(&perf->attempts);
		atomic64_inc(&zram->engine.stats.adaptive_attempts);
		zstrm = zcomp_stream_get(comp);
		src = kmap_local_page(page);
		comp_start = ktime_get_ns();
		ret = zcomp_compress(comp, zstrm, src, &length);
		comp_ns = ktime_get_ns() - comp_start;
		kunmap_local(src);
		atomic64_add(comp_ns, &perf->compression_ns);
		zram_engine_ema_update(&perf->compression_ema_ns, comp_ns);
		atomic64_add(PAGE_SIZE, &perf->input_bytes);
		if (ret || length >= txn.snapshot.obj_size ||
		    length >= PAGE_SIZE) {
			if (ret && ret != -ENOSPC)
				atomic64_inc(&perf->errors);
			else
				atomic64_inc(&zram->engine.stats.adaptive_no_gain);
			zcomp_stream_put(zstrm);
			zstrm = NULL;
			continue;
		}
		new_class = zs_lookup_class_index(zram->mem_pool, length);
		if (new_class >= old_class) {
			atomic64_inc(&perf->no_class_gain);
			atomic64_inc(&zram->engine.stats.adaptive_no_gain);
			zcomp_stream_put(zstrm);
			zstrm = NULL;
			continue;
		}
		decomp_start = ktime_get_ns();
		ret = zcomp_decompress(comp, zstrm, zstrm->buffer, length,
				       zstrm->local_copy);
		decomp_ns = ktime_get_ns() - decomp_start;
		atomic64_add(decomp_ns, &perf->decompression_ns);
		zram_engine_ema_update(&perf->decompression_ema_ns, decomp_ns);
		src = kmap_local_page(page);
		if (!ret && memcmp(src, zstrm->local_copy, PAGE_SIZE))
			ret = -EBADMSG;
		kunmap_local(src);
		if (ret || comp_ns + decomp_ns >
		    zram->engine.adaptive_cpu_limit_ns) {
			atomic64_inc(&perf->errors);
			zcomp_stream_put(zstrm);
			zstrm = NULL;
			continue;
		}
		handle = zs_malloc(zram->mem_pool, length,
			__GFP_KSWAPD_RECLAIM | __GFP_NOWARN | __GFP_HIGHMEM |
			__GFP_MOVABLE);
		if (IS_ERR_VALUE(handle)) {
			handle = 0;
			atomic64_inc(&zram->engine.stats.adaptive_alloc_failures);
			zcomp_stream_put(zstrm);
			zstrm = NULL;
			continue;
		}
		ret = zs_obj_write(zram->mem_pool, handle, zstrm->buffer, length);
		zcomp_stream_put(zstrm);
		zstrm = NULL;
		if (ret) {
			zs_free(zram->mem_pool, handle);
			handle = 0;
			continue;
		}
		publish.handle = handle;
		publish.size = length;
		publish.type = ZRAM_REP_COMPRESSED;
		publish.owner = owner;
		ret = zram_slot_txn_prepare(zram, &txn,
			ZRAM_REP_COMPRESSED, codec_id, NULL, GFP_NOIO);
		if (ret)
			break;
		zram_slot_lock(zram, job->index);
		if (atomic_read(&zram->engine.adaptive_state) !=
		    ZRAM_FEATURE_ENABLED ||
		    !zram_pp_job_is_current_locked(zram, job))
			ret = -ESTALE;
		else
			ret = zram_slot_txn_commit_if_current_locked(zram, &txn,
				zram_publish_object_locked, &publish);
		if (!ret)
			WRITE_ONCE(job->generation,
				   zram_rep_mutation_seq_locked(zram, job->index));
		zram_slot_unlock(zram, job->index);
		if (!ret) {
			handle = 0;
			atomic64_inc(&perf->success);
			atomic64_add(length, &perf->output_bytes);
			atomic64_inc(&zram->engine.stats.adaptive_success);
			atomic64_inc(&zram->engine.stats.adaptive_class_gain);
			ret = 1;
		} else {
			atomic64_inc(&zram->engine.stats.adaptive_stale);
		}
		break;
	}
	if (handle)
		zs_free(zram->mem_pool, handle);
out_txn:
	zram_slot_txn_abort(zram, &txn);
out_page:
	__free_page(page);
	return ret;
#else
	return 0;
#endif
}

static void zram_engine_sddc(struct zram *zram, struct zram_pp_job *job)
{
#if IS_ENABLED(CONFIG_ZRAM_SDDC)
	struct zram_sddc_cell candidates[ZRAM_SDDC_CANDIDATES];
	struct zram_slot_txn txn;
	struct page *page;
	void *data;
	u64 owner;
	u32 full_hash;
	u32 sample_hash;
	u32 count;
	u32 i;
	int ret = -EAGAIN;

	if (atomic_read(&zram->engine.sddc_state) != ZRAM_FEATURE_ENABLED)
		return;
	page = alloc_page(GFP_NOIO | __GFP_NOWARN);
	if (!page)
		return;
	zram_slot_lock(zram, job->index);
	if (!zram_pp_job_is_current_locked(zram, job) ||
	    !zram_engine_ordinary(zram_rep_type_locked(zram, job->index))) {
		zram_slot_unlock(zram, job->index);
		goto out_page;
	}
	zram_slot_txn_snapshot_locked(zram, job->index, &txn);
	owner = zram->engine.slot_owner[job->index];
	ret = zram_read_from_zspool(zram, page, job->index);
	zram_slot_unlock(zram, job->index);
	if (ret)
		goto out_txn;
	data = kmap_local_page(page);
	full_hash = lz4kd_delta_hash(data, PAGE_SIZE);
	sample_hash = zram_sddc_sample_hash(data);
	kunmap_local(data);
	count = zram_sddc_index_candidates(zram, owner, full_hash,
					   sample_hash, candidates);
	for (i = 0; i < count; i++) {
		ret = zram_sddc_try_candidate(zram, job, &txn, page,
					      &candidates[i], owner, full_hash,
					      sample_hash);
		if (!ret)
			break;
		if (txn.state != ZRAM_TXN_SNAPSHOTTED)
			break;
	}
	if (ret)
		zram_sddc_index_insert(zram, job->index,
			txn.snapshot.rep.mutation_seq, owner, full_hash,
			sample_hash);
out_txn:
	zram_slot_txn_abort(zram, &txn);
out_page:
	__free_page(page);
#endif
}

static void zram_engine_observation_work(struct zram *zram,
					 struct zram_pp_job *job)
{
	if (zram_engine_adaptive(zram, job) > 0)
		return;
	zram_engine_sddc(zram, job);
}

void zram_engine_observe(struct zram *zram, u32 index, u64 generation)
{
	if (atomic_read(&zram->engine.adaptive_state) != ZRAM_FEATURE_ENABLED &&
	    atomic_read(&zram->engine.sddc_state) != ZRAM_FEATURE_ENABLED)
		return;
	if (zram_pp_submit_latest(zram, ZRAM_PP_RECOMPRESS,
				  ZRAM_PP_PRIO_LOW, index, generation,
				  zram_engine_observation_work))
		atomic64_inc(&zram->engine.stats.signal_drops);
}

u64 zram_engine_owner_from_bio(struct bio *bio, u32 index)
{
	struct cgroup_subsys_state *css;

	if (!bio)
		return BIT_ULL(63) | ((u64)index + 1);
	css = bio_blkcg_css(bio);
	/* Root or missing ownership is isolated per slot. */
	if (!css || css->id <= 1)
		return BIT_ULL(63) | ((u64)index + 1);
	return css->serial_nr ?: (BIT_ULL(63) | ((u64)index + 1));
}

void zram_engine_record_write_locked(struct zram *zram, u32 index, u64 owner)
{
	if (!zram->engine.slot_owner)
		return;
	zram->engine.slot_owner[index] = owner;
	zram->engine.slot_stamp[index] = (u32)ktime_get_boottime_seconds();
}

static const char *zram_feature_state_name(int state)
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

ssize_t zram_adaptive_state_show(struct zram *zram, char *buf)
{
	return sysfs_emit(buf, "%s\n", zram_feature_state_name(
		atomic_read(&zram->engine.adaptive_state)));
}

ssize_t zram_adaptive_state_store(struct zram *zram, const char *buf,
				  size_t len)
{
	ssize_t ret = len;

	mutex_lock(&zram->engine.state_lock);
	if (atomic_read(&zram->engine.adaptive_state) ==
	    ZRAM_FEATURE_QUIESCING ||
	    READ_ONCE(zram->pp_scheduler.stopping)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (sysfs_streq(buf, "1") || sysfs_streq(buf, "enabled"))
		atomic_set(&zram->engine.adaptive_state, ZRAM_FEATURE_ENABLED);
	else if (sysfs_streq(buf, "0") || sysfs_streq(buf, "disabled"))
		atomic_set(&zram->engine.adaptive_state, ZRAM_FEATURE_DISABLED);
	else
		ret = -EINVAL;
out:
	mutex_unlock(&zram->engine.state_lock);
	return ret;
}

ssize_t zram_sddc_state_show(struct zram *zram, char *buf)
{
	return sysfs_emit(buf, "%s\n", zram_feature_state_name(
		atomic_read(&zram->engine.sddc_state)));
}

ssize_t zram_sddc_state_store(struct zram *zram, const char *buf, size_t len)
{
	struct zram_pp_operation *operation = NULL;
	struct page *page;
	unsigned long nr_slots;
	u32 index;
	bool remaining = false;
	ssize_t result = len;

	mutex_lock(&zram->engine.state_lock);
	if (atomic_read(&zram->engine.sddc_state) ==
	    ZRAM_FEATURE_QUIESCING ||
	    READ_ONCE(zram->pp_scheduler.stopping)) {
		result = -ESHUTDOWN;
		goto out;
	}
	if (sysfs_streq(buf, "1") || sysfs_streq(buf, "enabled")) {
		if (PAGE_SIZE != LZ4KD_DELTA_PAGE_SIZE)
			result = -EOPNOTSUPP;
		else
			atomic_set(&zram->engine.sddc_state,
				   ZRAM_FEATURE_ENABLED);
	} else if (sysfs_streq(buf, "0") || sysfs_streq(buf, "disabled"))
		atomic_set(&zram->engine.sddc_state, ZRAM_FEATURE_DRAINING);
	else if (sysfs_streq(buf, "drain")) {
		atomic_set(&zram->engine.sddc_state, ZRAM_FEATURE_DRAINING);
		operation = zram_pp_operation_begin(&zram->pp_scheduler,
			ZRAM_PP_DEDUP, ZRAM_PP_PRIO_NORMAL, 0, 0);
		if (IS_ERR(operation)) {
			result = PTR_ERR(operation);
			operation = NULL;
			goto out;
		}
		flush_workqueue(zram->pp_scheduler.cpu_wq);
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
			u8 type;
			int ret;

			if (zram_pp_operation_cancelled(operation)) {
				remaining = true;
				break;
			}
			zram_slot_lock(zram, index);
			type = zram_rep_type_locked(zram, index);
			if (type != ZRAM_REP_REF && type != ZRAM_REP_ALIAS &&
			    type != ZRAM_REP_DELTA) {
				zram_slot_unlock(zram, index);
				continue;
			}
			zram_slot_txn_snapshot_locked(zram, index, &txn);
			ret = zram_engine_read_managed_locked(zram, page, index);
			zram_slot_unlock(zram, index);
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
			atomic_set(&zram->engine.sddc_state,
				   ZRAM_FEATURE_DISABLED);
		else
			result = -EBUSY;
	} else
		result = -EINVAL;
out:
	mutex_unlock(&zram->engine.state_lock);
	return result;
}

ssize_t zram_engine_stats_show(struct zram *zram, char *buf)
{
	struct zram_engine_stats *s = &zram->engine.stats;
	ssize_t at = 0;
	u32 id;

	at += sysfs_emit_at(buf, at,
		"adaptive_attempts=%lld adaptive_success=%lld adaptive_stale=%lld adaptive_no_gain=%lld adaptive_class_gain=%lld adaptive_alloc_failures=%lld\n",
		atomic64_read(&s->adaptive_attempts),
		atomic64_read(&s->adaptive_success),
		atomic64_read(&s->adaptive_stale),
		atomic64_read(&s->adaptive_no_gain),
		atomic64_read(&s->adaptive_class_gain),
		atomic64_read(&s->adaptive_alloc_failures));
	at += sysfs_emit_at(buf, at,
		"sddc_refs=%lld sddc_aliases=%lld sddc_deltas=%lld ref_bytes=%lld delta_wire_bytes=%lld saved_bytes=%lld exact_attempts=%lld exact_hits=%lld delta_attempts=%lld delta_hits=%lld stale_cells=%lld cookie_mismatches=%lld decode_failures=%lld integrity_failures=%lld pin_bytes=%lld flatten_fallbacks=%lld signal_drops=%lld\n",
		atomic64_read(&s->sddc_refs),
		atomic64_read(&s->sddc_aliases),
		atomic64_read(&s->sddc_deltas),
		atomic64_read(&s->sddc_ref_bytes),
		atomic64_read(&s->sddc_delta_wire_bytes),
		atomic64_read(&s->sddc_saved_bytes),
		atomic64_read(&s->sddc_exact_attempts),
		atomic64_read(&s->sddc_exact_hits),
		atomic64_read(&s->sddc_delta_attempts),
		atomic64_read(&s->sddc_delta_hits),
		atomic64_read(&s->sddc_stale_cells),
		atomic64_read(&s->sddc_cookie_mismatches),
		atomic64_read(&s->sddc_decode_failures),
		atomic64_read(&s->sddc_integrity_failures),
		atomic64_read(&s->sddc_pin_bytes),
		atomic64_read(&s->sddc_flatten_fallbacks),
		atomic64_read(&s->signal_drops));
	for (id = 1; id <= ZRAM_MAX_CODECS && at < PAGE_SIZE - 320; id++) {
		struct zram_codec_perf *p;

		if (!zram_codec_by_id(zram, id))
			continue;
		p = &zram->engine.codec_perf[id];
		at += sysfs_emit_at(buf, at,
			"codec=%u attempts=%lld success=%lld input_bytes=%lld output_bytes=%lld compression_ns=%lld decompression_ns=%lld compression_ema_ns=%lld decompression_ema_ns=%lld errors=%lld no_class_gain=%lld\n",
			id, atomic64_read(&p->attempts),
			atomic64_read(&p->success),
			atomic64_read(&p->input_bytes),
			atomic64_read(&p->output_bytes),
			atomic64_read(&p->compression_ns),
			atomic64_read(&p->decompression_ns),
			atomic64_read(&p->compression_ema_ns),
			atomic64_read(&p->decompression_ema_ns),
			atomic64_read(&p->errors),
			atomic64_read(&p->no_class_gain));
	}
	return at;
}

int zram_sddc_export_locked(struct zram *zram, u32 index, void *wire,
			    size_t capacity, struct zram_sddc_export *export)
{
	struct zram_sddc_ext *ext;
	u8 type = zram_rep_type_locked(zram, index);
	void *src;

	if (!export || (type != ZRAM_REP_REF && type != ZRAM_REP_ALIAS &&
			type != ZRAM_REP_DELTA))
		return -EOPNOTSUPP;
	{
		struct zram_ext_rep *base = zram_rep_extended_locked(zram, index);

		if (!base)
			return -EIO;
		ext = container_of(base, struct zram_sddc_ext, base);
	}
	if (!ext->reference ||
	    (type == ZRAM_REP_REF && ext->kind != ZRAM_SDDC_WIRE_REF) ||
	    (type == ZRAM_REP_ALIAS && ext->kind != ZRAM_SDDC_WIRE_ALIAS) ||
	    (type == ZRAM_REP_DELTA && ext->kind != ZRAM_SDDC_WIRE_DELTA) ||
	    ext->reference_id != ext->reference->id ||
	    ext->reference_generation != ext->reference->generation ||
	    ext->owner != ext->reference->owner ||
	    !ext->reference->owns_payload ||
	    ext->reference->logical_size != PAGE_SIZE ||
	    (ext->kind != ZRAM_SDDC_WIRE_DELTA &&
	     ext->integrity != ext->reference->hash) ||
	    (ext->kind == ZRAM_SDDC_WIRE_DELTA &&
	     (!ext->wire_size || !ext->wire_handle)) ||
	    (ext->kind != ZRAM_SDDC_WIRE_DELTA && ext->wire_size))
		return -ESTALE;
	if (!zram_sddc_reader_get(ext->reference))
		return -ESTALE;
	memset(export, 0, sizeof(*export));
	export->reference_id = ext->reference_id;
	export->reference_generation = ext->reference_generation;
	export->owner = ext->owner;
	export->integrity = ext->integrity;
	export->wire_size = ext->wire_size;
	export->kind = ext->kind;
	export->codec_id = ext->reference->codec_id;
	export->reference_token = ext->reference;
	if (!ext->wire_size)
		return 0;
	if (!wire || capacity < ext->wire_size) {
		zram_sddc_reader_put(ext->reference);
		export->reference_token = NULL;
		return -ENOSPC;
	}
	src = zs_obj_read_begin(zram->mem_pool, ext->wire_handle,
				ext->wire_size, wire);
	if (!src) {
		zram_sddc_reader_put(ext->reference);
		export->reference_token = NULL;
		return -EIO;
	}
	if (src != wire)
		memcpy(wire, src, ext->wire_size);
	zs_obj_read_end(zram->mem_pool, ext->wire_handle, ext->wire_size, src);
	if (lz4kd_delta_hash(wire, ext->wire_size) != ext->integrity ||
	    lz4kd_delta_validate(wire, ext->wire_size, ext->reference_id,
				 ext->reference_generation)) {
		zram_sddc_reader_put(ext->reference);
		export->reference_token = NULL;
		atomic64_inc(&zram->engine.stats.sddc_integrity_failures);
		return -EBADMSG;
	}
	return 0;
}

int zram_sddc_reference_read(struct zram *zram, u32 id, u32 generation,
			     u64 owner, struct page *page)
{
	struct zram_sddc_ref *reference;
	int ret;

	memzero_page(page, 0, PAGE_SIZE);
	reference = zram_sddc_reference_lookup(zram, id, generation);
	if (!reference)
		return -ESTALE;
	if (reference->owner != owner) {
		zram_sddc_reader_put(reference);
		atomic64_inc(&zram->engine.stats.sddc_cookie_mismatches);
		return -EPERM;
	}
	ret = zram_sddc_ref_read(reference, page);
	zram_sddc_reader_put(reference);
	if (ret)
		atomic64_inc(&zram->engine.stats.sddc_decode_failures);
	if (ret == -EBADMSG)
		atomic64_inc(&zram->engine.stats.sddc_integrity_failures);
	return ret;
}

bool zram_sddc_backing_pin(struct zram *zram,
			   struct zram_sddc_export *export)
{
	struct zram_sddc_ref *reference = export->reference_token;
	unsigned long flags;
	u64 pinned;
	bool allowed = false;

	if (!reference)
		return false;
	spin_lock_irqsave(&zram->engine.pin_lock, flags);
	pinned = atomic64_read(&zram->engine.stats.sddc_pin_bytes);
	if (atomic_read(&reference->backing_refs) ||
	    reference->size <= zram->engine.backing_pin_limit_bytes -
			      min(pinned,
				  zram->engine.backing_pin_limit_bytes)) {
		if (zram_sddc_ref_get(reference)) {
			if (!atomic_read(&reference->backing_refs))
				atomic64_add(reference->size,
					     &zram->engine.stats.sddc_pin_bytes);
			atomic_inc(&reference->backing_refs);
			allowed = true;
		}
	}
	spin_unlock_irqrestore(&zram->engine.pin_lock, flags);
	zram_sddc_reader_put(reference);
	if (!allowed)
		export->reference_token = NULL;
	return allowed;
}

void zram_sddc_backing_unpin(struct zram *zram, void *reference_token)
{
	struct zram_sddc_ref *reference = reference_token;
	unsigned long flags;

	if (!reference)
		return;
	spin_lock_irqsave(&zram->engine.pin_lock, flags);
	if (WARN_ON_ONCE(atomic_read(&reference->backing_refs) <= 0)) {
		spin_unlock_irqrestore(&zram->engine.pin_lock, flags);
		return;
	}
	if (atomic_dec_and_test(&reference->backing_refs))
		atomic64_sub(reference->size,
			     &zram->engine.stats.sddc_pin_bytes);
	spin_unlock_irqrestore(&zram->engine.pin_lock, flags);
	zram_sddc_ref_try_reap(reference);
	zram_sddc_ref_put(reference);
}

int zram_engine_init(struct zram *zram)
{
	struct zram_engine *engine = &zram->engine;
	size_t cells = ZRAM_SDDC_BUCKETS * ZRAM_SDDC_WAYS;

	memset(engine, 0, sizeof(*engine));
	atomic_set(&engine->adaptive_state, ZRAM_FEATURE_DISABLED);
	atomic_set(&engine->sddc_state, ZRAM_FEATURE_DISABLED);
	spin_lock_init(&engine->index_lock);
	spin_lock_init(&engine->pin_lock);
	mutex_init(&engine->state_lock);
	mutex_init(&engine->reference_lock);
	xa_init(&engine->references);
	engine->next_reference_id = 1;
	engine->reference_generation = 1;
	atomic64_set(&engine->next_ext_identity, 0);
	engine->adaptive_cpu_limit_ns = ZRAM_ENGINE_DEFAULT_CPU_NS;
	engine->backing_pin_limit_bytes = ZRAM_ENGINE_DEFAULT_PIN_BYTES;
	if (IS_ENABLED(CONFIG_ZRAM_SDDC) && PAGE_SIZE == 4096) {
		engine->delta_page_pool = mempool_create_page_pool
			(ZRAM_ENGINE_DELTA_RESERVE, 0);
		engine->delta_scratch_pool =
			mempool_create_kmalloc_pool
			(ZRAM_ENGINE_DELTA_RESERVE,
			 sizeof(struct zram_delta_scratch));
	}
	engine->codec_perf = kvcalloc(ZRAM_MAX_CODECS + 1,
				      sizeof(*engine->codec_perf), GFP_KERNEL);
	engine->exact_index = kvcalloc(cells, sizeof(*engine->exact_index),
				       GFP_KERNEL);
	engine->sample_index = kvcalloc(cells, sizeof(*engine->sample_index),
					GFP_KERNEL);
	if ((IS_ENABLED(CONFIG_ZRAM_SDDC) && PAGE_SIZE == 4096 &&
	     (!engine->delta_page_pool || !engine->delta_scratch_pool)) ||
	    !engine->codec_perf || !engine->exact_index ||
	    !engine->sample_index) {
		zram_engine_fini(zram);
		return -ENOMEM;
	}
	return 0;
}

int zram_engine_meta_alloc(struct zram *zram, size_t nr_slots)
{
	zram->engine.slot_owner = kvcalloc(nr_slots,
					  sizeof(*zram->engine.slot_owner),
					  GFP_KERNEL);
	zram->engine.slot_stamp = kvcalloc(nr_slots,
					  sizeof(*zram->engine.slot_stamp),
					  GFP_KERNEL);
	if (!zram->engine.slot_owner || !zram->engine.slot_stamp) {
		zram_engine_meta_free(zram);
		return -ENOMEM;
	}
	return 0;
}

void zram_engine_meta_free(struct zram *zram)
{
	kvfree(zram->engine.slot_stamp);
	kvfree(zram->engine.slot_owner);
	zram->engine.slot_stamp = NULL;
	zram->engine.slot_owner = NULL;
}

void zram_engine_reset(struct zram *zram)
{
	struct zram_sddc_ref *reference;
	unsigned long index;
	size_t bytes = ZRAM_SDDC_BUCKETS * ZRAM_SDDC_WAYS *
		       sizeof(struct zram_sddc_cell);

	atomic_set(&zram->engine.adaptive_state, ZRAM_FEATURE_DISABLED);
	atomic_set(&zram->engine.sddc_state, ZRAM_FEATURE_DISABLED);
	if (zram->engine.exact_index)
		memset(zram->engine.exact_index, 0, bytes);
	if (zram->engine.sample_index)
		memset(zram->engine.sample_index, 0, bytes);
	memset(zram->engine.exact_hand, 0, sizeof(zram->engine.exact_hand));
	memset(zram->engine.sample_hand, 0, sizeof(zram->engine.sample_hand));
	mutex_lock(&zram->engine.reference_lock);
	xa_for_each(&zram->engine.references, index, reference) {
		xa_erase(&zram->engine.references, index);
		reference->in_table = false;
		WARN_ON_ONCE(atomic_read(&reference->resident_refs) ||
			     atomic_read(&reference->backing_refs) ||
			     atomic_read(&reference->reader_refs));
		zram_sddc_ref_put(reference);
	}
	mutex_unlock(&zram->engine.reference_lock);
	zram->engine.next_reference_id = 1;
	zram->engine.reference_generation++;
	if (!zram->engine.reference_generation)
		zram->engine.reference_generation++;
	memset(&zram->engine.stats, 0, sizeof(zram->engine.stats));
	if (zram->engine.codec_perf)
		memset(zram->engine.codec_perf, 0,
		       (ZRAM_MAX_CODECS + 1) *
		       sizeof(*zram->engine.codec_perf));
}

void zram_engine_quiesce(struct zram *zram)
{
	mutex_lock(&zram->engine.state_lock);
	if (atomic_read(&zram->engine.adaptive_state) !=
	    ZRAM_FEATURE_QUIESCING)
		zram->engine.adaptive_resume_state =
			atomic_read(&zram->engine.adaptive_state);
	if (atomic_read(&zram->engine.sddc_state) != ZRAM_FEATURE_QUIESCING)
		zram->engine.sddc_resume_state =
			atomic_read(&zram->engine.sddc_state);
	atomic_set(&zram->engine.adaptive_state, ZRAM_FEATURE_QUIESCING);
	atomic_set(&zram->engine.sddc_state, ZRAM_FEATURE_QUIESCING);
	mutex_unlock(&zram->engine.state_lock);
}

void zram_engine_resume(struct zram *zram)
{
	mutex_lock(&zram->engine.state_lock);
	if (!zram->disksize) {
		atomic_set(&zram->engine.adaptive_state, ZRAM_FEATURE_DISABLED);
		atomic_set(&zram->engine.sddc_state, ZRAM_FEATURE_DISABLED);
	} else {
		atomic_set(&zram->engine.adaptive_state,
			   zram->engine.adaptive_resume_state);
		atomic_set(&zram->engine.sddc_state,
				   zram->engine.sddc_resume_state);
	}
	mutex_unlock(&zram->engine.state_lock);
}

void zram_engine_fini(struct zram *zram)
{
	zram_engine_reset(zram);
	xa_destroy(&zram->engine.references);
	mempool_destroy(zram->engine.delta_scratch_pool);
	mempool_destroy(zram->engine.delta_page_pool);
	kvfree(zram->engine.sample_index);
	kvfree(zram->engine.exact_index);
	kvfree(zram->engine.codec_perf);
	zram->engine.sample_index = NULL;
	zram->engine.exact_index = NULL;
	zram->engine.codec_perf = NULL;
	zram->engine.delta_scratch_pool = NULL;
	zram->engine.delta_page_pool = NULL;
}
