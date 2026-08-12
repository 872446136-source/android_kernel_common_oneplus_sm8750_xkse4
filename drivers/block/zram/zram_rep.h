/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _ZRAM_REP_H_
#define _ZRAM_REP_H_

#include <linux/gfp_types.h>
#include <linux/refcount.h>
#include <linux/types.h>

struct zram;

enum zram_rep_type {
	ZRAM_REP_EMPTY = 0,
	ZRAM_REP_SAME,
	ZRAM_REP_RAW,
	ZRAM_REP_COMPRESSED,
	ZRAM_REP_BACKING,
	ZRAM_REP_REF,
	ZRAM_REP_ALIAS,
	ZRAM_REP_DELTA,
	ZRAM_REP_PACKED_BACKING,
	ZRAM_REP_MAX,
};

#define ZRAM_CODEC_NONE	0U

/*
 * Dense metadata is deliberately kept outside struct zram_table_entry.
 * Eight bytes per logical slot replace the old per-written-slot allocation.
 */
struct zram_slot_state {
	u32 generation;
	u8 type;
	u8 codec_id;
	u16 codec_generation;
};

struct zram_representation {
	u64 mutation_seq;
	u8 type;
	u8 codec_id;
	u16 codec_generation;
};

/* Base of sparse metadata used only by managed representations. */
struct zram_ext_rep {
	refcount_t refs;
	u64 identity;
	void (*release)(struct zram *zram, struct zram_ext_rep *ext);
};

struct zram_slot_snapshot {
	struct zram_representation rep;
	struct zram_ext_rep *ext;
	u64 ext_identity;
	unsigned long handle;
	u32 obj_size;
	u32 identity_flags;
};

enum zram_slot_txn_state {
	ZRAM_TXN_UNUSED = 0,
	ZRAM_TXN_SNAPSHOTTED,
	ZRAM_TXN_PREPARED,
	ZRAM_TXN_COMMITTED,
	ZRAM_TXN_ABORTED,
};

struct zram_slot_txn {
	struct zram_slot_snapshot snapshot;
	struct zram_ext_rep *target_ext;
	u32 index;
	u8 target_type;
	u8 target_codec_id;
	u16 target_codec_generation;
	u8 state;
	bool xa_reserved;
};

typedef void (*zram_slot_publish_t)(struct zram *zram, u32 index,
				    const struct zram_slot_snapshot *old,
				    void *private);

void zram_ext_rep_init(struct zram_ext_rep *ext, u64 identity,
		       void (*release)(struct zram *, struct zram_ext_rep *));
bool zram_ext_rep_get(struct zram_ext_rep *ext);
void zram_ext_rep_put(struct zram *zram, struct zram_ext_rep *ext);

void zram_rep_init(struct zram *zram);
void zram_rep_reset(struct zram *zram);
void zram_rep_fini(struct zram *zram);
int zram_rep_meta_alloc(struct zram *zram, size_t nr_slots);
void zram_rep_meta_free(struct zram *zram);

enum zram_rep_type zram_rep_type_locked(struct zram *zram, u32 index);
u8 zram_rep_codec_id_locked(struct zram *zram, u32 index);
u64 zram_rep_mutation_seq_locked(struct zram *zram, u32 index);
struct zram_ext_rep *zram_rep_extended_locked(struct zram *zram, u32 index);
bool zram_rep_allocated_locked(struct zram *zram, u32 index);

void zram_slot_txn_snapshot_locked(struct zram *zram, u32 index,
				   struct zram_slot_txn *txn);
int zram_slot_txn_prepare(struct zram *zram, struct zram_slot_txn *txn,
			  enum zram_rep_type target_type, u8 target_codec_id,
			  struct zram_ext_rep *target_ext, gfp_t gfp);
bool zram_slot_txn_revalidate_locked(struct zram *zram,
				     const struct zram_slot_txn *txn);
int zram_slot_txn_commit_if_current_locked(struct zram *zram,
					   struct zram_slot_txn *txn,
					   zram_slot_publish_t publish,
					   void *private);
void zram_slot_txn_abort(struct zram *zram, struct zram_slot_txn *txn);

#endif /* _ZRAM_REP_H_ */
