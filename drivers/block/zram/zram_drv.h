/*
 * Compressed RAM block device
 *
 * Copyright (C) 2008, 2009, 2010  Nitin Gupta
 *               2012, 2013 Minchan Kim
 *
 * This code is released using a dual license strategy: BSD/GPL
 * You can choose the licence that better fits your requirements.
 *
 * Released under the terms of 3-clause BSD License
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#ifndef _ZRAM_DRV_H_
#define _ZRAM_DRV_H_

#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/lockdep.h>
#include <linux/wait.h>
#include <linux/xarray.h>
#include <linux/zsmalloc.h>

#include "zcomp.h"
#include "zram_pp.h"
#include "zram_rep.h"
#include "zram_engine.h"
#include "zram_pwb.h"

#define SECTORS_PER_PAGE_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define SECTORS_PER_PAGE	(1 << SECTORS_PER_PAGE_SHIFT)
#define ZRAM_LOGICAL_BLOCK_SHIFT 12
#define ZRAM_LOGICAL_BLOCK_SIZE	(1 << ZRAM_LOGICAL_BLOCK_SHIFT)
#define ZRAM_SECTOR_PER_LOGICAL_BLOCK	\
	(1 << (ZRAM_LOGICAL_BLOCK_SHIFT - SECTOR_SHIFT))


/*
 * ZRAM is mainly used for memory efficiency so we want to keep memory
 * footprint small and thus squeeze size and zram pageflags into a flags
 * member. The lower ZRAM_FLAG_SHIFT bits is for object size (excluding
 * header), which cannot be larger than PAGE_SIZE (requiring PAGE_SHIFT
 * bits), the higher bits are for zram_pageflags.
 *
 * We use BUILD_BUG_ON() to make sure that zram pageflags don't overflow.
 */
#define ZRAM_FLAG_SHIFT (PAGE_SHIFT + 1)

/* Flags for zram pages (table[page_no].attr.flags) */
enum zram_pageflags {
	/* zram slot is locked */
	ZRAM_LOCK = ZRAM_FLAG_SHIFT,
	ZRAM_HUGE,	/* Incompressible page */
	ZRAM_IDLE,	/* not accessed page since last idle marking */
	ZRAM_INCOMPRESSIBLE, /* none of the algorithms could compress it */

	__NR_ZRAM_PAGEFLAGS,
};

/*-- Data structures */

/*
 * Allocated for each disk page. The entry lock shares storage with flags
 * (and access time, when enabled) to keep the per-slot footprint compact.
 */
struct zram_table_entry {
	union {
		unsigned long handle;
		unsigned long element;
	};
	union {
		unsigned long __lock;
		struct {
			u32 flags;
#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
			u32 ac_time;
#endif
		} attr;
	};
	struct lockdep_map dep_map;
};

struct zram_stats {
	atomic64_t compr_data_size;	/* compressed size of pages stored */
	atomic64_t failed_reads;	/* can happen when memory is too low */
	atomic64_t failed_writes;	/* can happen when memory is too low */
	atomic64_t notify_free;	/* no. of swap slot free notifications */
	atomic64_t same_pages;		/* no. of same element filled pages */
	atomic64_t huge_pages;		/* no. of huge pages */
	atomic64_t huge_pages_since;	/* no. of huge pages since zram set up */
	atomic64_t pages_stored;	/* no. of pages currently stored */
	atomic_long_t max_used_pages;	/* no. of maximum pages stored */
	atomic64_t writestall;		/* no. of write slow paths */
	atomic64_t miss_free;		/* no. of missed free */
#ifdef	CONFIG_ZRAM_WRITEBACK
	atomic64_t bd_count;		/* no. of pages in backing device */
	atomic64_t bd_reads;		/* no. of reads from backing device */
	atomic64_t bd_writes;		/* no. of writes to backing device */
	atomic64_t wb_batches;		/* successful multi-page writeback BIOs */
	atomic64_t wb_batch_pages;	/* pages in successful multi-page BIOs */
	atomic64_t wb_batch_fallbacks;	/* downgraded BIO submissions */
	atomic64_t wb_alloc_failures;	/* backing block allocation failures */
	atomic64_t wb_io_errors;	/* backing write I/O failures */
#endif
};

struct zram_publish_obj {
	unsigned long handle;
	unsigned long element;
	u32 size;
	u64 owner;
	u8 type;
	bool incompressible;
};

#ifdef CONFIG_ZRAM_MULTI_COMP
#define ZRAM_PRIMARY_COMP	0U
#define ZRAM_SECONDARY_COMP	1U
#define ZRAM_MAX_COMPS	4U
#else
#define ZRAM_PRIMARY_COMP	0U
#define ZRAM_SECONDARY_COMP	0U
#define ZRAM_MAX_COMPS	1U
#endif

/* u8 on-representation IDs; policy remains the existing four ranks. */
#define ZRAM_MAX_CODECS	U8_MAX

struct zram {
	struct zram_table_entry *table;
	struct zram_slot_state *slot_state;
	struct zs_pool *mem_pool;
	/* Stable decoder instances, indexed by codec identity (zero is none). */
	struct zcomp *codecs[ZRAM_MAX_CODECS + 1];
	atomic_t codec_rep_refs[ZRAM_MAX_CODECS + 1];
	u16 codec_generation[ZRAM_MAX_CODECS + 1];
	u8 next_codec_id;
	/* Mutable policy priority to stable codec identity mapping. */
	u8 policy_codecs[ZRAM_MAX_COMPS];
	struct zcomp_params params[ZRAM_MAX_COMPS];
	/* Sparse metadata exists only for REF/ALIAS/DELTA/PACKED_BACKING. */
	struct xarray slot_ext;
	atomic_t rep_epoch;
	struct zram_pp_scheduler pp_scheduler;
	struct zram_engine engine;
	struct zram_pwb pwb;
	struct gendisk *disk;
	/* Prevent concurrent execution of device init */
	struct rw_semaphore init_lock;
	/*
	 * the number of pages zram can consume for storing compressed data
	 */
	unsigned long limit_pages;

	struct zram_stats stats;
	/*
	 * This is the limit on amount of *uncompressed* worth of data
	 * we can store in a disk.
	 */
	u64 disksize;	/* bytes */
	const char *comp_algs[ZRAM_MAX_COMPS];
	s8 num_active_comps;
	/*
	 * zram is claimed so open request will be failed
	 */
	bool claim; /* Protected by disk->open_mutex */
#ifdef CONFIG_ZRAM_WRITEBACK
	struct file *backing_dev;
	bool wb_limit_enable;
	bool compressed_wb;
	u64 bd_wb_limit;
	u32 wb_batch_size;
	u32 wb_bio_pages;
	struct block_device *bdev;
	unsigned long *bitmap;
	unsigned long nr_pages;
	spinlock_t bitmap_lock;
	unsigned long wb_next_block;
	atomic_t rb_inflight;
	wait_queue_head_t rb_wait;
#endif
#ifdef CONFIG_ZRAM_MEMORY_TRACKING
	struct dentry *debugfs_dir;
#endif
};

int zram_codec_rep_get(struct zram *zram, u8 codec_id);
void zram_codec_rep_put(struct zram *zram, u8 codec_id);
void zram_slot_lock(struct zram *zram, u32 index);
void zram_slot_lock_nested(struct zram *zram, u32 index, int subclass);
void zram_slot_unlock(struct zram *zram, u32 index);
size_t zram_get_obj_size(struct zram *zram, u32 index);
unsigned long zram_get_handle(struct zram *zram, u32 index);
void zram_set_handle(struct zram *zram, u32 index, unsigned long handle);
void zram_set_obj_size(struct zram *zram, u32 index, size_t size);
struct zcomp *zram_codec_by_id(struct zram *zram, u8 codec_id);
struct zcomp *zram_comp_at_priority(struct zram *zram, u32 prio);
u8 zram_policy_codec_id(struct zram *zram, u32 prio);
int zram_read_from_zspool(struct zram *zram, struct page *page, u32 index);
void zram_release_slot_data_locked(
	struct zram *zram, size_t index,
	const struct zram_slot_snapshot *snapshot);
void zram_publish_object_locked(struct zram *zram, u32 index,
		const struct zram_slot_snapshot *old, void *private);
int zram_store_page_if_current(struct zram *zram, struct page *page,
			       struct zram_slot_txn *txn);
#ifdef CONFIG_ZRAM_WRITEBACK
unsigned long zram_backing_alloc(struct zram *zram, u32 blocks);
void zram_backing_free(struct zram *zram, unsigned long block, u32 blocks);
bool zram_backing_write_reserve(struct zram *zram, u32 blocks);
void zram_backing_write_rollback(struct zram *zram, u32 blocks);
#endif
#endif
