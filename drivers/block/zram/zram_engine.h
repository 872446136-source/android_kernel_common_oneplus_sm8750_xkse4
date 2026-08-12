/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _ZRAM_ENGINE_H_
#define _ZRAM_ENGINE_H_

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/xarray.h>

struct bio;
struct page;
struct zram;
struct zram_slot_snapshot;

enum zram_feature_state {
	ZRAM_FEATURE_DISABLED = 0,
	ZRAM_FEATURE_ENABLED,
	ZRAM_FEATURE_DRAINING,
	ZRAM_FEATURE_QUIESCING,
};

#define ZRAM_SDDC_BUCKETS	256U
#define ZRAM_SDDC_WAYS		4U
#define ZRAM_SDDC_CANDIDATES	4U

struct zram_sddc_cell {
	u64 hash;
	u64 generation;
	u64 owner;
	u32 index;
};

struct zram_codec_perf {
	atomic64_t attempts;
	atomic64_t success;
	atomic64_t input_bytes;
	atomic64_t output_bytes;
	atomic64_t compression_ns;
	atomic64_t decompression_ns;
	atomic64_t compression_ema_ns;
	atomic64_t decompression_ema_ns;
	atomic64_t errors;
	atomic64_t no_class_gain;
};

struct zram_engine_stats {
	atomic64_t adaptive_attempts;
	atomic64_t adaptive_success;
	atomic64_t adaptive_stale;
	atomic64_t adaptive_no_gain;
	atomic64_t adaptive_class_gain;
	atomic64_t adaptive_alloc_failures;
	atomic64_t sddc_refs;
	atomic64_t sddc_aliases;
	atomic64_t sddc_deltas;
	atomic64_t sddc_ref_bytes;
	atomic64_t sddc_delta_wire_bytes;
	atomic64_t sddc_saved_bytes;
	atomic64_t sddc_exact_attempts;
	atomic64_t sddc_exact_hits;
	atomic64_t sddc_delta_attempts;
	atomic64_t sddc_delta_hits;
	atomic64_t sddc_stale_cells;
	atomic64_t sddc_cookie_mismatches;
	atomic64_t sddc_decode_failures;
	atomic64_t sddc_integrity_failures;
	atomic64_t sddc_pin_bytes;
	atomic64_t sddc_flatten_fallbacks;
	atomic64_t signal_drops;
};

struct zram_engine {
	atomic_t adaptive_state;
	atomic_t sddc_state;
	u8 adaptive_resume_state;
	u8 sddc_resume_state;
	spinlock_t index_lock;
	struct zram_sddc_cell *exact_index;
	struct zram_sddc_cell *sample_index;
	u8 exact_hand[ZRAM_SDDC_BUCKETS];
	u8 sample_hand[ZRAM_SDDC_BUCKETS];
	struct xarray references;
	struct mutex reference_lock;
	spinlock_t pin_lock;
	u32 next_reference_id;
	u32 reference_generation;
	atomic64_t next_ext_identity;
	u64 *slot_owner;
	u32 *slot_stamp;
	struct zram_codec_perf *codec_perf;
	u64 adaptive_cpu_limit_ns;
	u32 adaptive_min_age_seconds;
	u64 backing_pin_limit_bytes;
	struct zram_engine_stats stats;
};

enum zram_sddc_wire_kind {
	ZRAM_SDDC_WIRE_NONE = 0,
	ZRAM_SDDC_WIRE_ALIAS,
	ZRAM_SDDC_WIRE_DELTA,
	ZRAM_SDDC_WIRE_REF,
};

struct zram_sddc_export {
	u32 reference_id;
	u32 reference_generation;
	u64 owner;
	u32 integrity;
	u32 wire_size;
	u8 kind;
	u8 codec_id;
	void *reference_token;
};

#if IS_ENABLED(CONFIG_ZRAM_REP_ENGINE)
int zram_engine_init(struct zram *zram);
void zram_engine_fini(struct zram *zram);
int zram_engine_meta_alloc(struct zram *zram, size_t nr_slots);
void zram_engine_meta_free(struct zram *zram);
void zram_engine_reset(struct zram *zram);
void zram_engine_quiesce(struct zram *zram);
void zram_engine_resume(struct zram *zram);
u64 zram_engine_owner_from_bio(struct bio *bio, u32 index);
void zram_engine_record_write_locked(struct zram *zram, u32 index, u64 owner);
void zram_engine_observe(struct zram *zram, u32 index, u64 generation);
int zram_engine_read_managed_locked(struct zram *zram, struct page *page,
				    u32 index);
void zram_engine_release_managed_locked(
	struct zram *zram, u32 index, const struct zram_slot_snapshot *snapshot);
bool zram_engine_rep_is_managed(u8 type);
ssize_t zram_engine_stats_show(struct zram *zram, char *buf);
ssize_t zram_adaptive_state_show(struct zram *zram, char *buf);
ssize_t zram_adaptive_state_store(struct zram *zram, const char *buf,
				  size_t len);
ssize_t zram_sddc_state_show(struct zram *zram, char *buf);
ssize_t zram_sddc_state_store(struct zram *zram, const char *buf, size_t len);
int zram_sddc_export_locked(struct zram *zram, u32 index, void *wire,
			    size_t capacity, struct zram_sddc_export *export);
int zram_sddc_reference_read(struct zram *zram, u32 id, u32 generation,
			     u64 owner, struct page *page);
bool zram_sddc_backing_pin(struct zram *zram,
			   struct zram_sddc_export *export);
void zram_sddc_backing_unpin(struct zram *zram, void *reference_token);
#else
static inline int zram_engine_init(struct zram *zram) { return 0; }
static inline void zram_engine_fini(struct zram *zram) { }
static inline int zram_engine_meta_alloc(struct zram *zram, size_t nr_slots)
{
	return 0;
}
static inline void zram_engine_meta_free(struct zram *zram) { }
static inline void zram_engine_reset(struct zram *zram) { }
static inline void zram_engine_quiesce(struct zram *zram) { }
static inline void zram_engine_resume(struct zram *zram) { }
static inline u64 zram_engine_owner_from_bio(struct bio *bio, u32 index)
{
	return index + 1;
}
static inline void zram_engine_record_write_locked(struct zram *zram,
						    u32 index, u64 owner) { }
static inline void zram_engine_observe(struct zram *zram, u32 index,
				       u64 generation) { }
static inline int zram_engine_read_managed_locked(struct zram *zram,
						   struct page *page,
						   u32 index)
{
	return -EOPNOTSUPP;
}
static inline void zram_engine_release_managed_locked(
	struct zram *zram, u32 index, const struct zram_slot_snapshot *snapshot) { }
static inline bool zram_engine_rep_is_managed(u8 type) { return false; }
static inline int zram_sddc_export_locked(struct zram *zram, u32 index,
		void *wire, size_t capacity, struct zram_sddc_export *export)
{
	return -EOPNOTSUPP;
}
static inline int zram_sddc_reference_read(struct zram *zram, u32 id,
		u32 generation, u64 owner, struct page *page)
{
	return -EOPNOTSUPP;
}
static inline bool zram_sddc_backing_pin(struct zram *zram,
		struct zram_sddc_export *export)
{
	return false;
}
static inline void zram_sddc_backing_unpin(struct zram *zram,
		void *reference_token) { }
#endif

#endif /* _ZRAM_ENGINE_H_ */
