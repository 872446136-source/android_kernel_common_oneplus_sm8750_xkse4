/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _ZRAM_PWB_H_
#define _ZRAM_PWB_H_

#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/xarray.h>

struct page;
struct zram;
struct zram_slot_snapshot;
struct zram_slot_txn;

struct zram_pwb_stats {
	atomic64_t packs;
	atomic64_t objects;
	atomic64_t logical_bytes;
	atomic64_t wire_bytes;
	atomic64_t physical_bytes;
	atomic64_t dead_bytes;
	atomic64_t physical_reads;
	atomic64_t physical_writes;
	atomic64_t coalesced_reads;
	atomic64_t snapshot_mismatches;
	atomic64_t enospc;
	atomic64_t fallback;
	atomic64_t gc_input_packs;
	atomic64_t gc_output_packs;
	atomic64_t gc_physical_writes;
	atomic64_t checksum_errors;
};

struct zram_pwb {
	atomic_t state;
	atomic_t native_state;
	atomic_t gc_state;
	u8 resume_state;
	u8 native_resume_state;
	u8 gc_resume_state;
	struct xarray packs;
	struct xarray read_cache;
	struct mutex pack_lock;
	struct mutex read_lock;
	u32 next_pack_id;
	u32 pack_generation;
	u32 gc_dead_percent;
	atomic64_t next_ext_identity;
	struct zram_pwb_stats stats;
};

#if IS_ENABLED(CONFIG_ZRAM_PACKED_WRITEBACK)
int zram_pwb_init(struct zram *zram);
void zram_pwb_reset(struct zram *zram);
void zram_pwb_fini(struct zram *zram);
void zram_pwb_quiesce(struct zram *zram);
void zram_pwb_resume(struct zram *zram);
bool zram_pwb_enabled(struct zram *zram);
ssize_t zram_pwb_state_show(struct zram *zram, char *buf);
ssize_t zram_pwb_state_store(struct zram *zram, const char *buf, size_t len);
ssize_t zram_pwb_native_state_show(struct zram *zram, char *buf);
ssize_t zram_pwb_native_state_store(struct zram *zram, const char *buf,
				    size_t len);
ssize_t zram_pwb_gc_state_show(struct zram *zram, char *buf);
ssize_t zram_pwb_gc_state_store(struct zram *zram, const char *buf,
				size_t len);
ssize_t zram_pwb_stats_show(struct zram *zram, char *buf);
ssize_t zram_pwb_writeback(struct zram *zram, const char *buf, size_t len);
ssize_t zram_pwb_gc_run(struct zram *zram, const char *buf, size_t len);
int zram_pwb_read(struct zram *zram, struct page *page,
		  const struct zram_slot_txn *txn);
void zram_pwb_release_slot_locked(
	struct zram *zram, u32 index, const struct zram_slot_snapshot *snapshot);
#else
static inline int zram_pwb_init(struct zram *zram) { return 0; }
static inline void zram_pwb_reset(struct zram *zram) { }
static inline void zram_pwb_fini(struct zram *zram) { }
static inline void zram_pwb_quiesce(struct zram *zram) { }
static inline void zram_pwb_resume(struct zram *zram) { }
static inline bool zram_pwb_enabled(struct zram *zram) { return false; }
static inline int zram_pwb_read(struct zram *zram, struct page *page,
				const struct zram_slot_txn *txn)
{
	return -EOPNOTSUPP;
}
static inline void zram_pwb_release_slot_locked(
	struct zram *zram, u32 index, const struct zram_slot_snapshot *snapshot) { }
#endif

#endif /* _ZRAM_PWB_H_ */
