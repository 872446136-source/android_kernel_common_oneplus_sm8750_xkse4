/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _ZRAM_REP_H_
#define _ZRAM_REP_H_

#include <linux/gfp_types.h>
#include <linux/types.h>

struct zram;

enum zram_rep_type {
	ZRAM_REP_EMPTY = 0,
	ZRAM_REP_SAME,
	ZRAM_REP_RAW,
	ZRAM_REP_COMPRESSED,
	ZRAM_REP_BACKING,
	/* Reserved representation identities; no data path exists yet. */
	ZRAM_REP_REF,
	ZRAM_REP_ALIAS,
	ZRAM_REP_DELTA,
	ZRAM_REP_PACKED_BACKING,
	ZRAM_REP_MAX,
};

#define ZRAM_CODEC_NONE	0U

struct zram_representation {
	u64 mutation_seq;
	u8 type;
	u8 codec_id;
	u16 reserved;
};

struct zram_slot_meta {
	struct zram_representation rep;
};

struct zram_slot_snapshot {
	struct zram_representation rep;
	unsigned long handle;
	u32 obj_size;
	u32 identity_flags;
};

enum zram_slot_txn_state {
	ZRAM_TXN_UNUSED = 0,
	ZRAM_TXN_SNAPSHOTTED,
	ZRAM_TXN_PREPARED,
	ZRAM_TXN_RESERVED,
	ZRAM_TXN_COMMITTED,
	ZRAM_TXN_ABORTED,
};

struct zram_slot_txn {
	struct zram_slot_snapshot snapshot;
	struct zram_slot_meta *prepared_meta;
	struct zram_slot_meta *reserved_meta;
	u32 index;
	u8 target_type;
	u8 target_codec_id;
	u8 state;
	bool had_meta;
};

void zram_rep_init(struct zram *zram);
void zram_rep_reset(struct zram *zram);
void zram_rep_fini(struct zram *zram);

enum zram_rep_type zram_rep_type_locked(struct zram *zram, u32 index);
u8 zram_rep_codec_id_locked(struct zram *zram, u32 index);
u64 zram_rep_mutation_seq_locked(struct zram *zram, u32 index);
bool zram_rep_allocated_locked(struct zram *zram, u32 index);

void zram_slot_txn_snapshot_locked(struct zram *zram, u32 index,
				   struct zram_slot_txn *txn);
int zram_slot_txn_prepare(struct zram_slot_txn *txn,
			  enum zram_rep_type target_type, u8 target_codec_id,
			  gfp_t gfp);
bool zram_slot_txn_revalidate_locked(struct zram *zram,
				     const struct zram_slot_txn *txn);
int zram_slot_txn_reserve_locked(struct zram *zram,
				 struct zram_slot_txn *txn, gfp_t gfp);
void zram_slot_txn_commit_locked(struct zram *zram,
				 struct zram_slot_txn *txn);
void zram_slot_txn_abort(struct zram *zram, struct zram_slot_txn *txn);

#endif /* _ZRAM_REP_H_ */
