// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/err.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "zram_drv.h"

#define ZRAM_TXN_IDENTITY_FLAGS	(BIT(ZRAM_HUGE) | \
				 BIT(ZRAM_INCOMPRESSIBLE))

static bool zram_rep_is_managed(enum zram_rep_type type)
{
	return type == ZRAM_REP_REF || type == ZRAM_REP_ALIAS ||
	       type == ZRAM_REP_DELTA || type == ZRAM_REP_PACKED_BACKING;
}

static struct zram_ext_rep *zram_rep_xa_load(struct zram *zram, u32 index)
{
	void *entry = xa_load(&zram->slot_ext, index);

	return xa_is_value(entry) ? NULL : entry;
}

void zram_ext_rep_init(struct zram_ext_rep *ext, u64 identity,
		       void (*release)(struct zram *, struct zram_ext_rep *))
{
	refcount_set(&ext->refs, 1);
	ext->identity = identity;
	ext->release = release;
}

bool zram_ext_rep_get(struct zram_ext_rep *ext)
{
	return ext && refcount_inc_not_zero(&ext->refs);
}

void zram_ext_rep_put(struct zram *zram, struct zram_ext_rep *ext)
{
	if (ext && refcount_dec_and_test(&ext->refs))
		ext->release(zram, ext);
}

static struct zram_slot_state *zram_slot_state_locked(struct zram *zram,
						       u32 index)
{
	return &zram->slot_state[index];
}

static struct zram_representation
zram_representation_locked(struct zram *zram, u32 index)
{
	struct zram_slot_state *state = zram_slot_state_locked(zram, index);
	u32 epoch = (u32)atomic_read(&zram->rep_epoch);

	return (struct zram_representation) {
		.mutation_seq = ((u64)epoch << 32) | state->generation,
		.type = state->type,
		.codec_id = state->codec_id,
		.codec_generation = state->codec_generation,
	};
}

static u64 zram_advance_generation_locked(struct zram *zram, u32 index)
{
	struct zram_slot_state *state = zram_slot_state_locked(zram, index);
	u32 epoch;

	state->generation++;
	if (unlikely(!state->generation)) {
		epoch = (u32)atomic_inc_return(&zram->rep_epoch);
		if (unlikely(!epoch))
			atomic_inc(&zram->rep_epoch);
		state->generation = 1;
	}
	epoch = (u32)atomic_read(&zram->rep_epoch);
	return ((u64)epoch << 32) | state->generation;
}

void zram_rep_init(struct zram *zram)
{
	xa_init(&zram->slot_ext);
	atomic_set(&zram->rep_epoch, 1);
}

int zram_rep_meta_alloc(struct zram *zram, size_t nr_slots)
{
	size_t bytes;

	BUILD_BUG_ON(sizeof(struct zram_slot_state) != 8);
	if (check_mul_overflow(nr_slots, sizeof(*zram->slot_state), &bytes))
		return -EOVERFLOW;
	zram->slot_state = vzalloc(bytes);
	return zram->slot_state ? 0 : -ENOMEM;
}

void zram_rep_meta_free(struct zram *zram)
{
	vfree(zram->slot_state);
	zram->slot_state = NULL;
}

void zram_rep_reset(struct zram *zram)
{
	struct zram_ext_rep *ext;
	unsigned long index;
	u32 epoch;

	xa_for_each(&zram->slot_ext, index, ext) {
		xa_erase(&zram->slot_ext, index);
		if (!xa_is_value(ext))
			zram_ext_rep_put(zram, ext);
	}
	WARN_ON_ONCE(!xa_empty(&zram->slot_ext));
	xa_destroy(&zram->slot_ext);
	xa_init(&zram->slot_ext);
	epoch = (u32)atomic_inc_return(&zram->rep_epoch);
	if (unlikely(!epoch))
		atomic_inc(&zram->rep_epoch);
}

void zram_rep_fini(struct zram *zram)
{
	WARN_ON_ONCE(!xa_empty(&zram->slot_ext));
	xa_destroy(&zram->slot_ext);
}

enum zram_rep_type zram_rep_type_locked(struct zram *zram, u32 index)
{
	return zram_slot_state_locked(zram, index)->type;
}

u8 zram_rep_codec_id_locked(struct zram *zram, u32 index)
{
	return zram_slot_state_locked(zram, index)->codec_id;
}

u64 zram_rep_mutation_seq_locked(struct zram *zram, u32 index)
{
	return zram_representation_locked(zram, index).mutation_seq;
}

struct zram_ext_rep *zram_rep_extended_locked(struct zram *zram, u32 index)
{
	return zram_rep_xa_load(zram, index);
}

bool zram_rep_allocated_locked(struct zram *zram, u32 index)
{
	return zram_rep_type_locked(zram, index) != ZRAM_REP_EMPTY;
}

void zram_slot_txn_snapshot_locked(struct zram *zram, u32 index,
				   struct zram_slot_txn *txn)
{
	struct zram_ext_rep *ext = zram_rep_extended_locked(zram, index);
	u32 flags = zram->table[index].attr.flags;

	memset(txn, 0, sizeof(*txn));
	txn->index = index;
	txn->snapshot.rep = zram_representation_locked(zram, index);
	if (ext && zram_ext_rep_get(ext)) {
		txn->snapshot.ext = ext;
		txn->snapshot.ext_identity = ext->identity;
	}
	txn->snapshot.handle = zram->table[index].handle;
	txn->snapshot.obj_size = flags & (BIT(ZRAM_FLAG_SHIFT) - 1);
	txn->snapshot.identity_flags = flags & ZRAM_TXN_IDENTITY_FLAGS;
	txn->state = ZRAM_TXN_SNAPSHOTTED;
}

static int zram_rep_validate_target(enum zram_rep_type type, u8 codec_id,
				    struct zram_ext_rep *ext)
{
	if ((unsigned int)type >= ZRAM_REP_MAX)
		return -EINVAL;
	if (zram_rep_is_managed(type) != !!ext)
		return -EINVAL;
	if (ext && (!ext->identity || !ext->release))
		return -EINVAL;
	if (type == ZRAM_REP_COMPRESSED && codec_id == ZRAM_CODEC_NONE)
		return -EINVAL;
	if ((type == ZRAM_REP_EMPTY || type == ZRAM_REP_SAME ||
	     type == ZRAM_REP_RAW || type == ZRAM_REP_REF ||
	     type == ZRAM_REP_ALIAS || type == ZRAM_REP_DELTA) &&
	    codec_id != ZRAM_CODEC_NONE)
		return -EINVAL;
	return 0;
}

int zram_slot_txn_prepare(struct zram *zram, struct zram_slot_txn *txn,
			  enum zram_rep_type target_type, u8 target_codec_id,
			  struct zram_ext_rep *target_ext, gfp_t gfp)
{
	int ret;

	if (txn->state != ZRAM_TXN_SNAPSHOTTED)
		return -EINVAL;
	ret = zram_rep_validate_target(target_type, target_codec_id,
				       target_ext);
	if (ret)
		return ret;
	if (target_ext) {
		void *stored;
		void *token;

		if (target_ext == txn->snapshot.ext)
			return -EINVAL;
		/*
		 * An existing managed entry already provides a non-allocating
		 * replacement path.  Ordinary -> managed transitions reserve the
		 * exact slot with a transaction-owned token so concurrent prepares
		 * cannot release each other's reservation.
		 */
		if (!txn->snapshot.ext) {
			token = xa_mk_value((unsigned long)target_ext >> 2);
			stored = xa_cmpxchg(&zram->slot_ext, txn->index, NULL,
					    token, gfp);
			if (xa_is_err(stored))
				return xa_err(stored);
			if (stored)
				return -EAGAIN;
			txn->xa_reservation = token;
		}
	}
	txn->target_type = target_type;
	txn->target_codec_id = target_codec_id;
	txn->target_codec_generation = target_codec_id ?
		zram->codec_generation[target_codec_id] : 0;
	txn->target_ext = target_ext;
	txn->state = ZRAM_TXN_PREPARED;
	return 0;
}

static bool
zram_slot_txn_revalidate_locked_common(struct zram *zram,
				       const struct zram_slot_txn *txn,
				       bool check_epoch)
{
	struct zram_representation rep;
	struct zram_ext_rep *ext;
	void *raw_ext;
	u32 flags;

	if (txn->state != ZRAM_TXN_SNAPSHOTTED &&
	    txn->state != ZRAM_TXN_PREPARED)
		return false;
	rep = zram_representation_locked(zram, txn->index);
	raw_ext = xa_load(&zram->slot_ext, txn->index);
	ext = xa_is_value(raw_ext) ? NULL : raw_ext;
	flags = zram->table[txn->index].attr.flags;

	return (!check_epoch ||
		(u32)(rep.mutation_seq >> 32) ==
		(u32)(txn->snapshot.rep.mutation_seq >> 32)) &&
	       (u32)rep.mutation_seq ==
		(u32)txn->snapshot.rep.mutation_seq &&
	       rep.type == txn->snapshot.rep.type &&
	       rep.codec_id == txn->snapshot.rep.codec_id &&
	       rep.codec_generation == txn->snapshot.rep.codec_generation &&
	       (!txn->xa_reservation ? ext == txn->snapshot.ext :
		raw_ext == txn->xa_reservation && !txn->snapshot.ext) &&
	       (!ext || ext->identity == txn->snapshot.ext_identity) &&
	       zram->table[txn->index].handle == txn->snapshot.handle &&
	       (flags & (BIT(ZRAM_FLAG_SHIFT) - 1)) ==
			txn->snapshot.obj_size &&
	       (flags & ZRAM_TXN_IDENTITY_FLAGS) ==
			txn->snapshot.identity_flags;
}

bool zram_slot_txn_revalidate_locked(struct zram *zram,
				     const struct zram_slot_txn *txn)
{
	return zram_slot_txn_revalidate_locked_common(zram, txn, true);
}

static int zram_slot_txn_commit_locked(struct zram *zram,
				       struct zram_slot_txn *txn,
				       zram_slot_publish_t publish,
				       void *private, bool check_epoch)
{
	struct zram_slot_state *state;
	struct zram_ext_rep *old_ext;
	void *stored;
	int ret;

	if (txn->state != ZRAM_TXN_PREPARED)
		return -EINVAL;
	if (!zram_slot_txn_revalidate_locked_common(zram, txn, check_epoch))
		return -ESTALE;
	if (txn->target_codec_id &&
	    zram->codec_generation[txn->target_codec_id] !=
		txn->target_codec_generation)
		return -ESTALE;

	ret = zram_codec_rep_get(zram, txn->target_codec_id);
	if (ret)
		return ret;

	old_ext = zram_rep_extended_locked(zram, txn->index);
	if (txn->target_ext) {
		void *expected = txn->xa_reservation ?: old_ext;

		stored = xa_cmpxchg(&zram->slot_ext, txn->index, expected,
				    txn->target_ext, GFP_NOWAIT);
		if (xa_is_err(stored) || stored != expected) {
			zram_codec_rep_put(zram, txn->target_codec_id);
			return xa_is_err(stored) ? xa_err(stored) : -ESTALE;
		}
		txn->target_ext = NULL;
		txn->xa_reservation = NULL;
	} else {
		xa_erase(&zram->slot_ext, txn->index);
	}

	if (publish)
		publish(zram, txn->index, &txn->snapshot, private);

	state = zram_slot_state_locked(zram, txn->index);
	state->type = txn->target_type;
	state->codec_id = txn->target_codec_id;
	state->codec_generation = txn->target_codec_generation;
	zram_advance_generation_locked(zram, txn->index);
	zram_codec_rep_put(zram, txn->snapshot.rep.codec_id);
	if (old_ext)
		zram_ext_rep_put(zram, old_ext);
	txn->state = ZRAM_TXN_COMMITTED;
	return 0;
}

int zram_slot_txn_commit_if_current_locked(struct zram *zram,
					   struct zram_slot_txn *txn,
					   zram_slot_publish_t publish,
					   void *private)
{
	return zram_slot_txn_commit_locked(zram, txn, publish, private, true);
}

int zram_slot_txn_commit_pair_if_current_locked(struct zram *zram,
						struct zram_slot_txn *first,
						zram_slot_publish_t first_publish,
						void *first_private,
						struct zram_slot_txn *second,
						zram_slot_publish_t second_publish,
						void *second_private)
{
	XA_STATE(first_xas, &zram->slot_ext, first->index);
	XA_STATE(second_xas, &zram->slot_ext, second->index);
	struct zram_slot_state *state;

	/*
	 * This specialized ordinary-to-managed pair commit is used for reference
	 * promotion.  Both XArray nodes were reserved by their transactions, so
	 * all fallible work finishes before either publish callback mutates a slot.
	 */
	if (first->index == second->index || !first_publish || !second_publish ||
	    first->target_ext == second->target_ext ||
	    first->target_type != ZRAM_REP_REF ||
	    (second->target_type != ZRAM_REP_ALIAS &&
	     second->target_type != ZRAM_REP_DELTA) ||
	    (first->snapshot.rep.type != ZRAM_REP_RAW &&
	     first->snapshot.rep.type != ZRAM_REP_COMPRESSED) ||
	    (second->snapshot.rep.type != ZRAM_REP_RAW &&
	     second->snapshot.rep.type != ZRAM_REP_COMPRESSED) ||
	    first->target_codec_id != ZRAM_CODEC_NONE ||
	    second->target_codec_id != ZRAM_CODEC_NONE)
		return -EINVAL;
	if (first->state != ZRAM_TXN_PREPARED ||
	    second->state != ZRAM_TXN_PREPARED ||
	    first->snapshot.ext || second->snapshot.ext ||
	    !first->target_ext || !second->target_ext ||
	    !first->xa_reservation || !second->xa_reservation ||
	    !zram_slot_txn_revalidate_locked(zram, first) ||
	    !zram_slot_txn_revalidate_locked(zram, second))
		return -ESTALE;

	/*
	 * Validate both reservations under one XArray lock before replacing
	 * either.  Replacing existing value entries cannot allocate, so after
	 * this point publication has no rollback path and is pair-atomic.
	 */
	xa_lock(&zram->slot_ext);
	if (xas_load(&first_xas) != first->xa_reservation ||
	    xas_load(&second_xas) != second->xa_reservation) {
		xa_unlock(&zram->slot_ext);
		return -ESTALE;
	}
	WARN_ON_ONCE(xas_store(&first_xas, first->target_ext) !=
		     first->xa_reservation);
	WARN_ON_ONCE(xas_store(&second_xas, second->target_ext) !=
		     second->xa_reservation);
	xa_unlock(&zram->slot_ext);

	first->target_ext = NULL;
	first->xa_reservation = NULL;
	second->target_ext = NULL;
	second->xa_reservation = NULL;
	if (first_publish)
		first_publish(zram, first->index, &first->snapshot,
			      first_private);
	if (second_publish)
		second_publish(zram, second->index, &second->snapshot,
			       second_private);

	state = zram_slot_state_locked(zram, first->index);
	state->type = first->target_type;
	state->codec_id = ZRAM_CODEC_NONE;
	state->codec_generation = 0;
	zram_advance_generation_locked(zram, first->index);
	state = zram_slot_state_locked(zram, second->index);
	state->type = second->target_type;
	state->codec_id = ZRAM_CODEC_NONE;
	state->codec_generation = 0;
	zram_advance_generation_locked(zram, second->index);
	zram_codec_rep_put(zram, first->snapshot.rep.codec_id);
	zram_codec_rep_put(zram, second->snapshot.rep.codec_id);
	first->state = ZRAM_TXN_COMMITTED;
	second->state = ZRAM_TXN_COMMITTED;
	return 0;
}

void zram_slot_txn_abort(struct zram *zram, struct zram_slot_txn *txn)
{
	if (!txn)
		return;
	if (txn->xa_reservation) {
		xa_cmpxchg(&zram->slot_ext, txn->index,
			   txn->xa_reservation, NULL, 0);
		txn->xa_reservation = NULL;
	}
	if (txn->target_ext) {
		zram_ext_rep_put(zram, txn->target_ext);
		txn->target_ext = NULL;
	}
	if (txn->snapshot.ext) {
		zram_ext_rep_put(zram, txn->snapshot.ext);
		txn->snapshot.ext = NULL;
	}
	if (txn->state != ZRAM_TXN_COMMITTED)
		txn->state = ZRAM_TXN_ABORTED;
}
