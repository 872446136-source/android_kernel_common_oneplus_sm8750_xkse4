// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/err.h>
#include <linux/slab.h>

#include "zram_drv.h"

#define ZRAM_TXN_IDENTITY_FLAGS	(BIT(ZRAM_HUGE) | \
				 BIT(ZRAM_INCOMPRESSIBLE))

static struct zram_slot_meta *zram_slot_meta_locked(struct zram *zram,
						     u32 index)
{
	return xa_load(&zram->slot_meta, index);
}

static struct zram_representation
zram_representation_locked(struct zram *zram, u32 index)
{
	struct zram_slot_meta *meta = zram_slot_meta_locked(zram, index);

	if (meta)
		return meta->rep;
	return (struct zram_representation) {
		.type = ZRAM_REP_EMPTY,
		.codec_id = ZRAM_CODEC_NONE,
	};
}

static u64 zram_next_mutation_seq(struct zram *zram)
{
	u64 seq = (u64)atomic64_inc_return(&zram->mutation_seq);

	/* Zero denotes an untouched slot. Keep it out of the live sequence. */
	if (unlikely(!seq))
		seq = (u64)atomic64_inc_return(&zram->mutation_seq);
	WARN_ON_ONCE(!seq);
	return seq;
}

void zram_rep_init(struct zram *zram)
{
	xa_init(&zram->slot_meta);
	atomic64_set(&zram->mutation_seq, 0);
}

void zram_rep_reset(struct zram *zram)
{
	struct zram_slot_meta *meta;
	unsigned long index;

	xa_for_each(&zram->slot_meta, index, meta)
		kfree(meta);
	xa_destroy(&zram->slot_meta);
	xa_init(&zram->slot_meta);
	atomic64_set(&zram->mutation_seq, 0);
}

void zram_rep_fini(struct zram *zram)
{
	WARN_ON_ONCE(!xa_empty(&zram->slot_meta));
	xa_destroy(&zram->slot_meta);
}

enum zram_rep_type zram_rep_type_locked(struct zram *zram, u32 index)
{
	return zram_representation_locked(zram, index).type;
}

u8 zram_rep_codec_id_locked(struct zram *zram, u32 index)
{
	return zram_representation_locked(zram, index).codec_id;
}

u64 zram_rep_mutation_seq_locked(struct zram *zram, u32 index)
{
	return zram_representation_locked(zram, index).mutation_seq;
}

bool zram_rep_allocated_locked(struct zram *zram, u32 index)
{
	return zram_rep_type_locked(zram, index) != ZRAM_REP_EMPTY;
}

void zram_slot_txn_snapshot_locked(struct zram *zram, u32 index,
				   struct zram_slot_txn *txn)
{
	struct zram_slot_meta *meta = zram_slot_meta_locked(zram, index);
	u32 flags = zram->table[index].attr.flags;

	memset(txn, 0, sizeof(*txn));
	txn->index = index;
	txn->had_meta = !!meta;
	txn->snapshot.rep = zram_representation_locked(zram, index);
	txn->snapshot.handle = zram->table[index].handle;
	txn->snapshot.obj_size = flags & (BIT(ZRAM_FLAG_SHIFT) - 1);
	txn->snapshot.identity_flags = flags & ZRAM_TXN_IDENTITY_FLAGS;
	txn->state = ZRAM_TXN_SNAPSHOTTED;
}

static int zram_rep_validate_target(enum zram_rep_type type, u8 codec_id)
{
	if ((unsigned int)type > ZRAM_REP_BACKING)
		return -EOPNOTSUPP;
	if (codec_id > ZRAM_MAX_CODECS)
		return -EINVAL;
	if (type == ZRAM_REP_COMPRESSED)
		return codec_id == ZRAM_CODEC_NONE ? -EINVAL : 0;
	if (type != ZRAM_REP_BACKING && codec_id != ZRAM_CODEC_NONE)
		return -EINVAL;
	return 0;
}

int zram_slot_txn_prepare(struct zram_slot_txn *txn,
			  enum zram_rep_type target_type, u8 target_codec_id,
			  gfp_t gfp)
{
	int ret;

	if (txn->state != ZRAM_TXN_SNAPSHOTTED)
		return -EINVAL;
	ret = zram_rep_validate_target(target_type, target_codec_id);
	if (ret)
		return ret;

	if (!txn->had_meta) {
		txn->prepared_meta = kzalloc(sizeof(*txn->prepared_meta), gfp);
		if (!txn->prepared_meta)
			return -ENOMEM;
		txn->prepared_meta->rep = txn->snapshot.rep;
	}
	txn->target_type = target_type;
	txn->target_codec_id = target_codec_id;
	txn->state = ZRAM_TXN_PREPARED;
	return 0;
}

bool zram_slot_txn_revalidate_locked(struct zram *zram,
				     const struct zram_slot_txn *txn)
{
	struct zram_representation rep;
	u32 flags;

	if (txn->state != ZRAM_TXN_SNAPSHOTTED &&
	    txn->state != ZRAM_TXN_PREPARED)
		return false;
	rep = zram_representation_locked(zram, txn->index);
	flags = zram->table[txn->index].attr.flags;

	return rep.mutation_seq == txn->snapshot.rep.mutation_seq &&
	       rep.type == txn->snapshot.rep.type &&
	       rep.codec_id == txn->snapshot.rep.codec_id &&
	       zram->table[txn->index].handle == txn->snapshot.handle &&
	       (flags & (BIT(ZRAM_FLAG_SHIFT) - 1)) ==
			txn->snapshot.obj_size &&
	       (flags & ZRAM_TXN_IDENTITY_FLAGS) ==
			txn->snapshot.identity_flags;
}

int zram_slot_txn_reserve_locked(struct zram *zram,
				 struct zram_slot_txn *txn, gfp_t gfp)
{
	struct zram_slot_meta *meta;
	void *old;

	if (txn->state != ZRAM_TXN_PREPARED)
		return -EINVAL;
	meta = zram_slot_meta_locked(zram, txn->index);
	if (!meta) {
		if (WARN_ON_ONCE(!txn->prepared_meta))
			return -EUCLEAN;
		old = xa_cmpxchg(&zram->slot_meta, txn->index, NULL,
				 txn->prepared_meta, gfp);
		if (xa_is_err(old))
			return xa_err(old);
		if (WARN_ON_ONCE(old))
			return -EUCLEAN;
		txn->reserved_meta = txn->prepared_meta;
		txn->prepared_meta = NULL;
	}
	txn->state = ZRAM_TXN_RESERVED;
	return 0;
}

void zram_slot_txn_commit_locked(struct zram *zram,
				 struct zram_slot_txn *txn)
{
	struct zram_slot_meta *meta;

	if (WARN_ON_ONCE(txn->state != ZRAM_TXN_RESERVED))
		return;
	meta = zram_slot_meta_locked(zram, txn->index);
	if (WARN_ON_ONCE(!meta))
		return;

	meta->rep.type = txn->target_type;
	meta->rep.codec_id = txn->target_codec_id;
	meta->rep.mutation_seq = zram_next_mutation_seq(zram);
	txn->reserved_meta = NULL;
	txn->state = ZRAM_TXN_COMMITTED;
}

void zram_slot_txn_abort(struct zram *zram, struct zram_slot_txn *txn)
{
	void *old;

	if (!txn)
		return;
	if (txn->state == ZRAM_TXN_RESERVED && txn->reserved_meta) {
		old = xa_cmpxchg(&zram->slot_meta, txn->index,
				 txn->reserved_meta, NULL, 0);
		if (WARN_ON_ONCE(xa_is_err(old) || old != txn->reserved_meta))
			return;
		kfree(txn->reserved_meta);
		txn->reserved_meta = NULL;
	}
	if (txn->state != ZRAM_TXN_COMMITTED)
		txn->state = ZRAM_TXN_ABORTED;
	kfree(txn->prepared_meta);
	txn->prepared_meta = NULL;
}
