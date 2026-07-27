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

#define KMSG_COMPONENT "zram"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/bio.h>
#include <linux/bitops.h>
#include <linux/bitmap.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/device.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/backing-dev.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/sysfs.h>
#include <linux/debugfs.h>
#include <linux/cpuhotplug.h>
#include <linux/kernel_read_file.h>
#include <linux/part_stat.h>
#include <linux/sysctl.h>

#include "zram_drv.h"

static DEFINE_IDR(zram_index_idr);
/* idr index must be protected */
static DEFINE_MUTEX(zram_index_mutex);

static int zram_major;
static const char *default_compressor = CONFIG_ZRAM_DEF_COMP;
#define ZRAM_MAX_ALGO_NAME_SZ	128

/* Module params (documentation at end) */
static unsigned int num_devices = 1;
#ifdef CONFIG_ZRAM_WRITEBACK
#define ZRAM_WB_BATCH_MAX 8U
#endif
/*
 * Pages that compress to sizes equals or greater than this are stored
 * uncompressed in memory.
 */
static size_t huge_class_size;

static const struct block_device_operations zram_devops;

static void zram_free_page(struct zram *zram, size_t index);
static int zram_read_page(struct zram *zram, struct page *page, u32 index,
			  struct bio *parent);
static int zram_read_from_zspool(struct zram *zram, struct page *page,
				 u32 index);

#define zram_slot_dep_map(zram, index) (&(zram)->table[(index)].dep_map)

static void zram_slot_lock_init(struct zram *zram, u32 index)
{
	static struct lock_class_key __key;

	lockdep_init_map(zram_slot_dep_map(zram, index),
			 "zram->table[index].lock", &__key, 0);
}

#ifdef CONFIG_ZRAM_MULTI_COMP
/*
 * Number of secondary compressors tried during the initial ZRAM write.
 * 0: primary only; 1: priorities 0..1; ...; 3: priorities 0..3.
 */
static u8 sysctl_zram_recomp_immediate __read_mostly = 1;

static struct ctl_table zram_sysctl_table[] = {
	{
		.procname	= "zram_recomp_immediate",
		.data		= &sysctl_zram_recomp_immediate,
		.maxlen		= sizeof(sysctl_zram_recomp_immediate),
		.mode		= 0644,
		.proc_handler	= proc_dou8vec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_THREE,
	},
};

static struct ctl_table_header *zram_sysctl_header;
#endif

/*
 * Entry locking rules:
 *
 * 1) The lock is exclusive.
 * 2) zram_slot_lock() can sleep while waiting and its owner can sleep.
 * 3) Atomic contexts must use zram_slot_trylock() and handle failure.
 */
static __must_check bool zram_slot_trylock(struct zram *zram, u32 index)
{
	unsigned long *lock = &zram->table[index].__lock;

	if (!test_and_set_bit_lock(ZRAM_LOCK, lock)) {
		mutex_acquire(zram_slot_dep_map(zram, index), 0, 1, _RET_IP_);
		lock_acquired(zram_slot_dep_map(zram, index), _RET_IP_);
		return true;
	}

	return false;
}

static void zram_slot_lock(struct zram *zram, u32 index)
{
	unsigned long *lock = &zram->table[index].__lock;

	mutex_acquire(zram_slot_dep_map(zram, index), 0, 0, _RET_IP_);
	wait_on_bit_lock(lock, ZRAM_LOCK, TASK_UNINTERRUPTIBLE);
	lock_acquired(zram_slot_dep_map(zram, index), _RET_IP_);
}

static void zram_slot_unlock(struct zram *zram, u32 index)
{
	unsigned long *lock = &zram->table[index].__lock;

	mutex_release(zram_slot_dep_map(zram, index), _RET_IP_);
	clear_and_wake_up_bit(ZRAM_LOCK, lock);
}

static inline bool init_done(struct zram *zram)
{
	return zram->disksize;
}

static inline struct zram *dev_to_zram(struct device *dev)
{
	return (struct zram *)dev_to_disk(dev)->private_data;
}

static unsigned long zram_get_handle(struct zram *zram, u32 index)
{
	return zram->table[index].handle;
}

static void zram_set_handle(struct zram *zram, u32 index, unsigned long handle)
{
	zram->table[index].handle = handle;
}

/* Flag operations require the table entry bit lock to be held. */
static bool zram_test_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	return zram->table[index].attr.flags & BIT(flag);
}

static void zram_set_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].attr.flags |= BIT(flag);
}

static void zram_clear_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].attr.flags &= ~BIT(flag);
}

static inline void zram_set_element(struct zram *zram, u32 index,
			unsigned long element)
{
	zram->table[index].element = element;
}

static unsigned long zram_get_element(struct zram *zram, u32 index)
{
	return zram->table[index].element;
}

static size_t zram_get_obj_size(struct zram *zram, u32 index)
{
	return zram->table[index].attr.flags & (BIT(ZRAM_FLAG_SHIFT) - 1);
}

static void zram_set_obj_size(struct zram *zram,
					u32 index, size_t size)
{
	u32 flags = zram->table[index].attr.flags >> ZRAM_FLAG_SHIFT;

	zram->table[index].attr.flags = (flags << ZRAM_FLAG_SHIFT) | size;
}

static inline bool zram_allocated(struct zram *zram, u32 index)
{
	return zram_get_obj_size(zram, index) ||
			zram_test_flag(zram, index, ZRAM_SAME) ||
			zram_test_flag(zram, index, ZRAM_WB);
}

#if PAGE_SIZE != 4096
static inline bool is_partial_io(struct bio_vec *bvec)
{
	return bvec->bv_len != PAGE_SIZE;
}
#define ZRAM_PARTIAL_IO		1
#else
static inline bool is_partial_io(struct bio_vec *bvec)
{
	return false;
}
#endif

static inline void zram_set_priority(struct zram *zram, u32 index, u32 prio)
{
	prio &= ZRAM_COMP_PRIORITY_MASK;
	/*
	 * Clear previous priority value first, in case if we recompress
	 * further an already recompressed page
	 */
	zram->table[index].attr.flags &= ~(ZRAM_COMP_PRIORITY_MASK <<
					   ZRAM_COMP_PRIORITY_BIT1);
	zram->table[index].attr.flags |= (prio << ZRAM_COMP_PRIORITY_BIT1);
}

static inline u32 zram_get_priority(struct zram *zram, u32 index)
{
	u32 prio = zram->table[index].attr.flags >>
		   ZRAM_COMP_PRIORITY_BIT1;

	return prio & ZRAM_COMP_PRIORITY_MASK;
}

static void zram_accessed(struct zram *zram, u32 index)
{
	zram_clear_flag(zram, index, ZRAM_IDLE);
	zram_clear_flag(zram, index, ZRAM_PP_SLOT);
#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
	zram->table[index].attr.ac_time =
		(u32)ktime_get_boottime_seconds();
#endif
}

#if defined(CONFIG_ZRAM_WRITEBACK) || defined(CONFIG_ZRAM_MULTI_COMP)
struct zram_pp_slot {
	unsigned long index;
	struct list_head entry;
};

#define PP_BUCKET_SIZE_RANGE	64
#define NUM_PP_BUCKETS		((PAGE_SIZE / PP_BUCKET_SIZE_RANGE) + 1)

struct zram_pp_ctl {
	struct list_head pp_buckets[NUM_PP_BUCKETS];
};

static struct zram_pp_ctl *init_pp_ctl(void)
{
	struct zram_pp_ctl *ctl;
	u32 idx;

	ctl = kmalloc(sizeof(*ctl), GFP_KERNEL);
	if (!ctl)
		return NULL;

	for (idx = 0; idx < NUM_PP_BUCKETS; idx++)
		INIT_LIST_HEAD(&ctl->pp_buckets[idx]);
	return ctl;
}

/* The corresponding slot lock must be held. */
static void release_pp_slot_locked(struct zram *zram,
				   struct zram_pp_slot *pps)
{
	if (!list_empty(&pps->entry))
		list_del_init(&pps->entry);
	zram_clear_flag(zram, pps->index, ZRAM_PP_SLOT);
	kfree(pps);
}

static void release_pp_slot(struct zram *zram, struct zram_pp_slot *pps)
{
	unsigned long index = pps->index;

	zram_slot_lock(zram, index);
	release_pp_slot_locked(zram, pps);
	zram_slot_unlock(zram, index);
}

static void release_pp_ctl(struct zram *zram, struct zram_pp_ctl *ctl)
{
	u32 idx;

	if (!ctl)
		return;

	for (idx = 0; idx < NUM_PP_BUCKETS; idx++) {
		while (!list_empty(&ctl->pp_buckets[idx])) {
			struct zram_pp_slot *pps;

			pps = list_first_entry(&ctl->pp_buckets[idx],
					       struct zram_pp_slot, entry);
			release_pp_slot(zram, pps);
		}
	}
	kfree(ctl);
}

/* The corresponding slot lock must be held. */
static bool place_pp_slot(struct zram *zram, struct zram_pp_ctl *ctl,
			  u32 index)
{
	struct zram_pp_slot *pps;
	u32 bucket;

	pps = kmalloc(sizeof(*pps), GFP_NOIO | __GFP_NOWARN);
	if (!pps)
		return false;

	INIT_LIST_HEAD(&pps->entry);
	pps->index = index;
	bucket = min_t(u32, zram_get_obj_size(zram, index) /
			   PP_BUCKET_SIZE_RANGE, NUM_PP_BUCKETS - 1);
	list_add_tail(&pps->entry, &ctl->pp_buckets[bucket]);
	zram_set_flag(zram, index, ZRAM_PP_SLOT);
	return true;
}

static struct zram_pp_slot *select_pp_slot(struct zram_pp_ctl *ctl)
{
	s32 idx;

	for (idx = NUM_PP_BUCKETS - 1; idx >= 0; idx--) {
		struct zram_pp_slot *pps;

		pps = list_first_entry_or_null(&ctl->pp_buckets[idx],
					       struct zram_pp_slot, entry);
		if (pps)
			return pps;
	}
	return NULL;
}
#endif

static inline void update_used_max(struct zram *zram,
					const unsigned long pages)
{
	unsigned long cur_max = atomic_long_read(&zram->stats.max_used_pages);

	do {
		if (cur_max >= pages)
			return;
	} while (!atomic_long_try_cmpxchg(&zram->stats.max_used_pages,
					  &cur_max, pages));
}

static inline void zram_fill_page(void *ptr, unsigned long len,
					unsigned long value)
{
	WARN_ON_ONCE(!IS_ALIGNED(len, sizeof(unsigned long)));
	memset_l(ptr, value, len / sizeof(unsigned long));
}

static bool page_same_filled(void *ptr, unsigned long *element)
{
	unsigned long *page;
	unsigned long val;
	unsigned int pos, last_pos = PAGE_SIZE / sizeof(*page) - 1;

	page = (unsigned long *)ptr;
	val = page[0];

	if (val != page[last_pos])
		return false;

	for (pos = 1; pos < last_pos; pos++) {
		if (val != page[pos])
			return false;
	}

	*element = val;

	return true;
}

static ssize_t initstate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u32 val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	val = init_done(zram);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%u\n", val);
}

static ssize_t disksize_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", zram->disksize);
}

static ssize_t mem_limit_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	u64 limit;
	char *tmp;
	struct zram *zram = dev_to_zram(dev);

	limit = memparse(buf, &tmp);
	if (buf == tmp) /* no chars parsed, invalid input */
		return -EINVAL;

	down_write(&zram->init_lock);
	zram->limit_pages = PAGE_ALIGN(limit) >> PAGE_SHIFT;
	up_write(&zram->init_lock);

	return len;
}

static ssize_t mem_used_max_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int err;
	unsigned long val;
	struct zram *zram = dev_to_zram(dev);

	err = kstrtoul(buf, 10, &val);
	if (err || val != 0)
		return -EINVAL;

	down_read(&zram->init_lock);
	if (init_done(zram)) {
		atomic_long_set(&zram->stats.max_used_pages,
				zs_get_total_pages(zram->mem_pool));
	}
	up_read(&zram->init_lock);

	return len;
}

/*
 * Mark all pages which are older than or equal to cutoff as IDLE.
 * Callers should hold the zram init lock in read mode
 */
static void mark_idle(struct zram *zram, u32 cutoff, bool mark_all)
{
	int is_idle = 1;
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	int index;

	for (index = 0; index < nr_pages; index++) {
		/*
		 * Do not change the idle state while writeback owns the slot.
		 * ZRAM_PP_SLOT, rather than ZRAM_IDLE, is the generation and
		 * post-processing ownership marker.
		 *
		 * Also do not mark ZRAM_SAME slots as ZRAM_IDLE, because no
		 * post-processing (recompress, writeback) happens to the
		 * ZRAM_SAME slot.
		 *
		 * And ZRAM_WB slots simply cannot be ZRAM_IDLE.
		 */
		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index) ||
		    zram_test_flag(zram, index, ZRAM_WB) ||
		    zram_test_flag(zram, index, ZRAM_UNDER_WB) ||
		    zram_test_flag(zram, index, ZRAM_SAME)) {
			zram_slot_unlock(zram, index);
			continue;
		}

#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
		is_idle = mark_all ||
			!time_after32(zram->table[index].attr.ac_time,
				      cutoff);
#endif
		if (is_idle)
			zram_set_flag(zram, index, ZRAM_IDLE);
		else
			zram_clear_flag(zram, index, ZRAM_IDLE);
		zram_slot_unlock(zram, index);
	}
}

static ssize_t idle_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	u32 cutoff_time = 0;
	bool mark_all = sysfs_streq(buf, "all");
	ssize_t rv = -EINVAL;

	if (!mark_all) {
		/*
		 * If it did not parse as 'all' try to treat it as an integer
		 * when we have memory tracking enabled.
		 */
		u32 age_sec;

		if (IS_ENABLED(CONFIG_ZRAM_TRACK_ENTRY_ACTIME) &&
		    !kstrtou32(buf, 0, &age_sec))
			cutoff_time = (u32)ktime_get_boottime_seconds() -
				      age_sec;
		else
			goto out;
	}

	down_read(&zram->init_lock);
	if (!init_done(zram))
		goto out_unlock;

	/*
	 * The "all" command marks every eligible slot idle.
	 */
	mark_idle(zram, cutoff_time, mark_all);
	rv = len;

out_unlock:
	up_read(&zram->init_lock);
out:
	return rv;
}

#ifdef CONFIG_ZRAM_WRITEBACK
static ssize_t writeback_limit_enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	u64 val;
	ssize_t ret = -EINVAL;

	if (kstrtoull(buf, 10, &val))
		return ret;

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	zram->wb_limit_enable = val;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);
	ret = len;

	return ret;
}

static ssize_t writeback_limit_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	bool val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	val = zram->wb_limit_enable;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t writeback_limit_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	u64 val;
	ssize_t ret = -EINVAL;

	if (kstrtoull(buf, 10, &val))
		return ret;

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	zram->bd_wb_limit = val;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);
	ret = len;

	return ret;
}

static ssize_t writeback_limit_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u64 val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	val = zram->bd_wb_limit;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", val);
}

static void reset_bdev(struct zram *zram)
{
	struct block_device *bdev;

	if (!zram->backing_dev)
		return;

	bdev = zram->bdev;
	blkdev_put(bdev, zram);
	/* hope filp_close flush all of IO */
	filp_close(zram->backing_dev, NULL);
	zram->backing_dev = NULL;
	zram->bdev = NULL;
	zram->disk->fops = &zram_devops;
	kvfree(zram->bitmap);
	zram->bitmap = NULL;
}

static ssize_t backing_dev_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct file *file;
	struct zram *zram = dev_to_zram(dev);
	char *p;
	ssize_t ret;

	down_read(&zram->init_lock);
	file = zram->backing_dev;
	if (!file) {
		memcpy(buf, "none\n", 5);
		up_read(&zram->init_lock);
		return 5;
	}

	p = file_path(file, buf, PAGE_SIZE - 1);
	if (IS_ERR(p)) {
		ret = PTR_ERR(p);
		goto out;
	}

	ret = strlen(p);
	memmove(buf, p, ret);
	buf[ret++] = '\n';
out:
	up_read(&zram->init_lock);
	return ret;
}

static ssize_t backing_dev_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	char *file_name;
	size_t sz;
	struct file *backing_dev = NULL;
	struct inode *inode;
	struct address_space *mapping;
	unsigned int bitmap_sz;
	unsigned long nr_pages, *bitmap = NULL;
	struct block_device *bdev = NULL;
	int err;
	struct zram *zram = dev_to_zram(dev);

	file_name = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!file_name)
		return -ENOMEM;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		pr_info("Can't setup backing device for initialized device\n");
		err = -EBUSY;
		goto out;
	}

	strscpy(file_name, buf, PATH_MAX);
	/* ignore trailing newline */
	sz = strlen(file_name);
	if (sz > 0 && file_name[sz - 1] == '\n')
		file_name[sz - 1] = 0x00;

	backing_dev = filp_open_block(file_name, O_RDWR|O_LARGEFILE, 0);
	if (IS_ERR(backing_dev)) {
		err = PTR_ERR(backing_dev);
		backing_dev = NULL;
		goto out;
	}

	mapping = backing_dev->f_mapping;
	inode = mapping->host;

	/* Support only block device in this moment */
	if (!S_ISBLK(inode->i_mode)) {
		err = -ENOTBLK;
		goto out;
	}

	bdev = blkdev_get_by_dev(inode->i_rdev, BLK_OPEN_READ | BLK_OPEN_WRITE,
				 zram, NULL);
	if (IS_ERR(bdev)) {
		err = PTR_ERR(bdev);
		bdev = NULL;
		goto out;
	}

	nr_pages = i_size_read(inode) >> PAGE_SHIFT;
	/* Refuse to use zero sized device (also prevents self reference) */
	if (!nr_pages) {
		err = -EINVAL;
		goto out;
	}

	bitmap_sz = BITS_TO_LONGS(nr_pages) * sizeof(long);
	bitmap = kvzalloc(bitmap_sz, GFP_KERNEL);
	if (!bitmap) {
		err = -ENOMEM;
		goto out;
	}

	reset_bdev(zram);

	zram->bdev = bdev;
	zram->backing_dev = backing_dev;
	zram->bitmap = bitmap;
	zram->nr_pages = nr_pages;
	zram->wb_next_block = 1;
	up_write(&zram->init_lock);

	pr_info("setup backing device %s\n", file_name);
	kfree(file_name);

	return len;
out:
	kvfree(bitmap);

	if (bdev)
		blkdev_put(bdev, zram);

	if (backing_dev)
		filp_close(backing_dev, NULL);

	up_write(&zram->init_lock);

	kfree(file_name);

	return err;
}

static unsigned long alloc_blocks_bdev(struct zram *zram, unsigned int nr)
{
	unsigned long flags;
	unsigned long blk_idx;
	unsigned long start;

	if (!nr || nr >= zram->nr_pages)
		return 0;

	spin_lock_irqsave(&zram->bitmap_lock, flags);
	start = zram->wb_next_block;
	if (!start || start >= zram->nr_pages)
		start = 1;

	blk_idx = bitmap_find_next_zero_area(zram->bitmap, zram->nr_pages,
					    start, nr, 0);
	if (blk_idx >= zram->nr_pages)
		blk_idx = bitmap_find_next_zero_area(zram->bitmap,
						    zram->nr_pages, 1, nr, 0);

	if (blk_idx < zram->nr_pages) {
		bitmap_set(zram->bitmap, blk_idx, nr);
		zram->wb_next_block = blk_idx + nr;
		if (zram->wb_next_block >= zram->nr_pages)
			zram->wb_next_block = 1;
	} else {
		blk_idx = 0;
	}
	spin_unlock_irqrestore(&zram->bitmap_lock, flags);

	if (blk_idx)
		atomic64_add(nr, &zram->stats.bd_count);
	return blk_idx;
}

static unsigned long alloc_block_bdev(struct zram *zram)
{
	return alloc_blocks_bdev(zram, 1);
}

static void free_blocks_bdev(struct zram *zram, unsigned long blk_idx,
			     unsigned int nr)
{
	unsigned long flags;
	unsigned int cleared = 0;
	unsigned int i;

	if (!blk_idx || !nr)
		return;

	spin_lock_irqsave(&zram->bitmap_lock, flags);
	for (i = 0; i < nr; i++) {
		bool was_set = test_and_clear_bit(blk_idx + i, zram->bitmap);

		WARN_ON_ONCE(!was_set);
		if (was_set)
			cleared++;
	}
	if (cleared &&
	    (!zram->wb_next_block || blk_idx < zram->wb_next_block))
		zram->wb_next_block = blk_idx;
	spin_unlock_irqrestore(&zram->bitmap_lock, flags);

	if (cleared)
		atomic64_sub(cleared, &zram->stats.bd_count);
}

static void free_block_bdev(struct zram *zram, unsigned long blk_idx)
{
	free_blocks_bdev(zram, blk_idx, 1);
}

static void read_from_bdev_async(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	struct bio *bio;

	bio = bio_alloc(zram->bdev, 1, parent->bi_opf, GFP_NOIO);
	bio->bi_iter.bi_sector = entry * (PAGE_SIZE >> 9);
	__bio_add_page(bio, page, PAGE_SIZE, 0);
	bio_chain(bio, parent);
	submit_bio(bio);
}

#define PAGE_WRITEBACK			0
#define HUGE_WRITEBACK			(1<<0)
#define IDLE_WRITEBACK			(1<<1)
#define INCOMPRESSIBLE_WRITEBACK	(1<<2)

struct zram_wb_entry {
	u32 index;
	struct page *page;
	struct zram_pp_slot *pps;
	unsigned long handle;
	size_t obj_size;
};

struct zram_wb_range {
	unsigned long lo;
	unsigned long hi;
	struct list_head entry;
};

struct zram_wb_args {
	struct list_head ranges;
	int mode;
	bool type_set;
	bool have_ranges;
};

static u64 zram_writeback_limit_remaining(struct zram *zram)
{
	u64 remaining = ~0ULL;

	spin_lock(&zram->wb_limit_lock);
	if (zram->wb_limit_enable)
		remaining = zram->bd_wb_limit;
	spin_unlock(&zram->wb_limit_lock);

	return remaining;
}

static int zram_parse_writeback_mode(const char *val, int *mode)
{
	if (!strcmp(val, "idle"))
		*mode = IDLE_WRITEBACK;
	else if (!strcmp(val, "huge"))
		*mode = HUGE_WRITEBACK;
	else if (!strcmp(val, "huge_idle"))
		*mode = IDLE_WRITEBACK | HUGE_WRITEBACK;
	else if (!strcmp(val, "incompressible"))
		*mode = INCOMPRESSIBLE_WRITEBACK;
	else
		return -EINVAL;
	return 0;
}

static int zram_add_writeback_range(struct zram_wb_args *args,
				    unsigned long lo, unsigned long hi)
{
	struct zram_wb_range *range;

	range = kmalloc(sizeof(*range), GFP_KERNEL);
	if (!range)
		return -ENOMEM;

	range->lo = lo;
	range->hi = hi;
	list_add_tail(&range->entry, &args->ranges);
	args->have_ranges = true;
	return 0;
}

static int zram_parse_writeback_index(char *val, unsigned long nr_pages,
				      struct zram_wb_args *args)
{
	unsigned long index;

	if (kstrtoul(val, 10, &index) || index >= nr_pages)
		return -EINVAL;
	return zram_add_writeback_range(args, index, index + 1);
}

static int zram_parse_writeback_indexes(char *val, unsigned long nr_pages,
					struct zram_wb_args *args)
{
	unsigned long lo, hi;
	char *delim;
	int ret;

	delim = strchr(val, '-');
	if (!delim || delim == val || !delim[1] || strchr(delim + 1, '-'))
		return -EINVAL;

	*delim = '\0';
	ret = kstrtoul(val, 10, &lo);
	if (!ret)
		ret = kstrtoul(delim + 1, 10, &hi);
	*delim = '-';
	if (ret || lo > hi || hi >= nr_pages)
		return -EINVAL;
	return zram_add_writeback_range(args, lo, hi + 1);
}

static void zram_free_writeback_args(struct zram_wb_args *args)
{
	struct zram_wb_range *range, *next;

	list_for_each_entry_safe(range, next, &args->ranges, entry) {
		list_del(&range->entry);
		kfree(range);
	}
}

static int zram_parse_writeback_args(char *buf, unsigned long nr_pages,
				     struct zram_wb_args *wb_args)
{
	char *args, *param, *val;
	int ret;

	INIT_LIST_HEAD(&wb_args->ranges);
	wb_args->mode = PAGE_WRITEBACK;
	wb_args->type_set = false;
	wb_args->have_ranges = false;

	args = skip_spaces(buf);
	if (!*args)
		return -EINVAL;

	while (*args) {
		args = next_arg(args, &param, &val);

		if (!val || !*val) {
			if (wb_args->type_set)
				return -EINVAL;
			ret = zram_parse_writeback_mode(param, &wb_args->mode);
			if (ret)
				return -EINVAL;
			wb_args->type_set = true;
			continue;
		}

		if (!strcmp(param, "type")) {
			if (wb_args->type_set)
				return -EINVAL;
			ret = zram_parse_writeback_mode(val, &wb_args->mode);
			if (ret)
				return -EINVAL;
			wb_args->type_set = true;
			continue;
		}

		if (!strcmp(param, "page_index")) {
			ret = zram_parse_writeback_index(val, nr_pages,
							 wb_args);
			if (ret)
				return ret;
			continue;
		}

		if (!strcmp(param, "page_indexes")) {
			ret = zram_parse_writeback_indexes(val, nr_pages,
							   wb_args);
			if (ret)
				return ret;
			continue;
		}

		return -EINVAL;
	}
	return 0;
}

static bool zram_writeback_candidate_locked(struct zram *zram, u32 index,
					    int mode)
{
	if (!zram_allocated(zram, index) ||
	    zram_test_flag(zram, index, ZRAM_WB) ||
	    zram_test_flag(zram, index, ZRAM_SAME) ||
	    zram_test_flag(zram, index, ZRAM_UNDER_WB))
		return false;

	if ((mode & IDLE_WRITEBACK) &&
	    !zram_test_flag(zram, index, ZRAM_IDLE))
		return false;
	if ((mode & HUGE_WRITEBACK) &&
	    !zram_test_flag(zram, index, ZRAM_HUGE))
		return false;
	if ((mode & INCOMPRESSIBLE_WRITEBACK) &&
	    !zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
		return false;
	return true;
}

static int zram_scan_writeback_range(struct zram *zram, int mode,
				     unsigned long lo, unsigned long hi,
				     struct zram_pp_ctl *ctl)
{
	unsigned long index;

	for (index = lo; index < hi; index++) {
		bool placed = true;

		zram_slot_lock(zram, index);
		if (!zram_writeback_candidate_locked(zram, index, mode) ||
		    zram_test_flag(zram, index, ZRAM_PP_SLOT))
			goto next;
		placed = place_pp_slot(zram, ctl, index);
next:
		zram_slot_unlock(zram, index);
		if (!placed)
			return -ENOMEM;
	}
	return 0;
}

static int zram_scan_writeback_slots(struct zram *zram,
				     struct zram_wb_args *args,
				     unsigned long nr_pages,
				     struct zram_pp_ctl *ctl)
{
	struct zram_wb_range *range;
	int ret;

	if (!args->have_ranges)
		return zram_scan_writeback_range(zram, args->mode, 0,
						 nr_pages, ctl);

	list_for_each_entry(range, &args->ranges, entry) {
		ret = zram_scan_writeback_range(zram, args->mode, range->lo,
						range->hi, ctl);
		if (ret)
			return ret;
	}
	return 0;
}

static bool zram_writeback_prepare_slot(struct zram *zram,
					struct zram_pp_slot *pps, int mode,
					struct zram_wb_entry *entry,
					struct page *page)
{
	unsigned long index = pps->index;

	zram_slot_lock(zram, index);
	if (!zram_test_flag(zram, index, ZRAM_PP_SLOT) ||
	    !zram_writeback_candidate_locked(zram, index, mode))
		goto reject;

	zram_set_flag(zram, index, ZRAM_UNDER_WB);
	if (zram_read_from_zspool(zram, page, index)) {
		zram_clear_flag(zram, index, ZRAM_UNDER_WB);
		goto reject;
	}

	entry->index = index;
	entry->page = page;
	entry->pps = pps;
	entry->handle = zram_get_handle(zram, index);
	entry->obj_size = zram_get_obj_size(zram, index);
	list_del_init(&pps->entry);
	zram_slot_unlock(zram, index);
	return true;

reject:
	release_pp_slot_locked(zram, pps);
	zram_slot_unlock(zram, index);
	return false;
}

static void zram_writeback_abort_entry(struct zram *zram,
				       struct zram_wb_entry *entry)
{
	u32 index = entry->index;

	zram_slot_lock(zram, index);
	zram_clear_flag(zram, index, ZRAM_UNDER_WB);
	release_pp_slot_locked(zram, entry->pps);
	entry->pps = NULL;
	zram_slot_unlock(zram, index);
}

static bool zram_writeback_commit_entry(struct zram *zram,
					struct zram_wb_entry *entry,
					unsigned long blk_idx)
{
	u32 index = entry->index;
	bool committed = false;

	zram_slot_lock(zram, index);
	if (!zram_allocated(zram, index) ||
	    !zram_test_flag(zram, index, ZRAM_PP_SLOT) ||
	    !zram_test_flag(zram, index, ZRAM_UNDER_WB) ||
	    zram_test_flag(zram, index, ZRAM_WB) ||
	    zram_test_flag(zram, index, ZRAM_SAME) ||
	    zram_get_handle(zram, index) != entry->handle ||
	    zram_get_obj_size(zram, index) != entry->obj_size)
		goto stale;

	zram_free_page(zram, index);
	zram_set_flag(zram, index, ZRAM_WB);
	zram_set_element(zram, index, blk_idx);
	zram_clear_flag(zram, index, ZRAM_UNDER_WB);
	atomic64_inc(&zram->stats.pages_stored);
	atomic64_inc(&zram->stats.bd_writes);
	committed = true;
	goto release;

stale:
	zram_clear_flag(zram, index, ZRAM_UNDER_WB);
release:
	release_pp_slot_locked(zram, entry->pps);
	entry->pps = NULL;
	zram_slot_unlock(zram, index);

	if (!committed) {
		free_block_bdev(zram, blk_idx);
		return false;
	}

	spin_lock(&zram->wb_limit_lock);
	if (zram->wb_limit_enable) {
		u64 units = 1ULL << (PAGE_SHIFT - 12);

		if (zram->bd_wb_limit > units)
			zram->bd_wb_limit -= units;
		else
			zram->bd_wb_limit = 0;
	}
	spin_unlock(&zram->wb_limit_lock);
	return true;
}

static int zram_submit_writeback_batch(struct zram *zram,
				       struct zram_wb_entry *entries,
				       unsigned int nr,
				       unsigned long blk_start)
{
	struct bio_vec bvecs[ZRAM_WB_BATCH_MAX];
	struct bio bio;
	unsigned int i;
	int err = 0;

	bio_init(&bio, zram->bdev, bvecs, nr, REQ_OP_WRITE | REQ_SYNC);
	bio.bi_iter.bi_sector = blk_start * (PAGE_SIZE >> 9);

	for (i = 0; i < nr; i++)
		__bio_add_page(&bio, entries[i].page, PAGE_SIZE, 0);

	if (!err)
		err = submit_bio_wait(&bio);

	if (err) {
		atomic64_inc(&zram->stats.wb_io_errors);
		for (i = 0; i < nr; i++)
			zram_writeback_abort_entry(zram, &entries[i]);
		free_blocks_bdev(zram, blk_start, nr);
		return err;
	}

	if (nr > 1) {
		atomic64_inc(&zram->stats.wb_batches);
		atomic64_add(nr, &zram->stats.wb_batch_pages);
	}

	for (i = 0; i < nr; i++)
		zram_writeback_commit_entry(zram, &entries[i], blk_start + i);
	return 0;
}

static int zram_submit_writeback_fallback(struct zram *zram,
					  struct zram_wb_entry *entries,
					  unsigned int nr)
{
	unsigned int i;
	int ret = 0;

	atomic64_inc(&zram->stats.wb_batch_fallbacks);
	for (i = 0; i < nr; i++) {
		unsigned long blk_idx;
		int err;

		blk_idx = alloc_block_bdev(zram);
		if (!blk_idx) {
			atomic64_inc(&zram->stats.wb_alloc_failures);
			zram_writeback_abort_entry(zram, &entries[i]);
			if (!ret)
				ret = -ENOSPC;
			continue;
		}

		err = zram_submit_writeback_batch(zram, &entries[i], 1,
						 blk_idx);
		if (err && (!ret || ret == -ENOSPC))
			ret = err;
	}
	return ret;
}

static ssize_t writeback_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	unsigned long nr_pages;
	struct zram_wb_entry entries[ZRAM_WB_BATCH_MAX];
	struct page *pages[ZRAM_WB_BATCH_MAX] = { };
	struct zram_wb_args args;
	struct zram_pp_ctl *pp_ctl = NULL;
	unsigned int batch_max;
	unsigned int allocated_pages = 0;
	ssize_t ret;
	bool pp_owned = false;
	unsigned int i;
	int err;

	INIT_LIST_HEAD(&args.ranges);

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		ret = -EINVAL;
		goto out_unlock;
	}
	if (atomic_cmpxchg(&zram->pp_in_progress, 0, 1)) {
		ret = -EBUSY;
		goto out_unlock;
	}
	pp_owned = true;

	mutex_lock(&zram->writeback_lock);
	if (!zram->backing_dev) {
		ret = -ENODEV;
		goto out;
	}

	nr_pages = zram->disksize >> PAGE_SHIFT;
	err = zram_parse_writeback_args((char *)buf, nr_pages, &args);
	if (err) {
		ret = err;
		goto out;
	}

	pp_ctl = init_pp_ctl();
	if (!pp_ctl) {
		ret = -ENOMEM;
		goto out;
	}
	err = zram_scan_writeback_slots(zram, &args, nr_pages, pp_ctl);
	if (err) {
		ret = err;
		goto out;
	}

	batch_max = IS_ENABLED(CONFIG_ZRAM_WRITEBACK_BATCH) ?
		ZRAM_WB_BATCH_MAX : 1;
	batch_max = min_t(unsigned long, batch_max, nr_pages);
	for (i = 0; i < batch_max; i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		if (!pages[i])
			break;
		allocated_pages++;
	}
	if (!allocated_pages) {
		ret = -ENOMEM;
		goto out;
	}
	batch_max = allocated_pages;
	ret = len;

	while (select_pp_slot(pp_ctl)) {
		u64 remaining = zram_writeback_limit_remaining(zram);
		unsigned int cap;
		unsigned int nr = 0;
		unsigned long blk_start;
		int err;

		if (!remaining) {
			if (ret >= 0)
				ret = -EIO;
			break;
		}

		cap = batch_max;
		if (remaining != ~0ULL) {
			u64 units_per_page = 1ULL << (PAGE_SHIFT - 12);
			u64 remaining_pages = div_u64(remaining, units_per_page);

			cap = min_t(u64, cap, remaining_pages);
		}
		if (!cap) {
			if (ret >= 0)
				ret = -EIO;
			break;
		}

		while (nr < cap) {
			struct zram_pp_slot *pps = select_pp_slot(pp_ctl);

			if (!pps)
				break;
			if (!zram_writeback_prepare_slot(zram, pps, args.mode,
							 &entries[nr],
							 pages[nr]))
				continue;
			nr++;
		}

		if (!nr)
			continue;

		blk_start = alloc_blocks_bdev(zram, nr);
		if (!blk_start) {
			atomic64_inc(&zram->stats.wb_alloc_failures);
			if (nr > 1)
				err = zram_submit_writeback_fallback(zram,
								    entries, nr);
			else {
				zram_writeback_abort_entry(zram, &entries[0]);
				err = -ENOSPC;
			}
		} else {
			err = zram_submit_writeback_batch(zram, entries, nr,
							 blk_start);
		}

		if (err && (ret >= 0 ||
		    (ret == -ENOSPC && err != -ENOSPC)))
			ret = err;
		cond_resched();
	}

out:
	for (i = 0; i < allocated_pages; i++)
		__free_page(pages[i]);
	release_pp_ctl(zram, pp_ctl);
	zram_free_writeback_args(&args);
	mutex_unlock(&zram->writeback_lock);
	if (pp_owned)
		atomic_set(&zram->pp_in_progress, 0);
out_unlock:
	up_read(&zram->init_lock);
	return ret;
}


struct zram_work {
	struct work_struct work;
	struct zram *zram;
	unsigned long entry;
	struct page *page;
	int error;
};

static void zram_sync_read(struct work_struct *work)
{
	struct zram_work *zw = container_of(work, struct zram_work, work);
	struct bio_vec bv;
	struct bio bio;

	bio_init(&bio, zw->zram->bdev, &bv, 1, REQ_OP_READ);
	bio.bi_iter.bi_sector = zw->entry * (PAGE_SIZE >> 9);
	__bio_add_page(&bio, zw->page, PAGE_SIZE, 0);
	zw->error = submit_bio_wait(&bio);
}

/*
 * Block layer want one ->submit_bio to be active at a time, so if we use
 * chained IO with parent IO in same context, it's a deadlock. To avoid that,
 * use a worker thread context.
 */
static int read_from_bdev_sync(struct zram *zram, struct page *page,
				unsigned long entry)
{
	struct zram_work work;

	work.page = page;
	work.zram = zram;
	work.entry = entry;

	INIT_WORK_ONSTACK(&work.work, zram_sync_read);
	queue_work(system_unbound_wq, &work.work);
	flush_work(&work.work);
	destroy_work_on_stack(&work.work);

	return work.error;
}

static int read_from_bdev(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	atomic64_inc(&zram->stats.bd_reads);
	if (!parent) {
		if (WARN_ON_ONCE(!IS_ENABLED(ZRAM_PARTIAL_IO)))
			return -EIO;
		return read_from_bdev_sync(zram, page, entry);
	}
	read_from_bdev_async(zram, page, entry, parent);
	return 0;
}

static ssize_t writeback_stat_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	/*
	 * batches batch_pages fragmented_fallbacks allocation_failures io_errors
	 */
	return scnprintf(buf, PAGE_SIZE, "%lld %lld %lld %lld %lld\n",
		atomic64_read(&zram->stats.wb_batches),
		atomic64_read(&zram->stats.wb_batch_pages),
		atomic64_read(&zram->stats.wb_batch_fallbacks),
		atomic64_read(&zram->stats.wb_alloc_failures),
		atomic64_read(&zram->stats.wb_io_errors));
}
#else
static inline void reset_bdev(struct zram *zram) {};
static int read_from_bdev(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	return -EIO;
}

static void free_block_bdev(struct zram *zram, unsigned long blk_idx) {};
#endif

#ifdef CONFIG_ZRAM_MEMORY_TRACKING

static struct dentry *zram_debugfs_root;

static void zram_debugfs_create(void)
{
	zram_debugfs_root = debugfs_create_dir("zram", NULL);
}

static void zram_debugfs_destroy(void)
{
	debugfs_remove_recursive(zram_debugfs_root);
}

static ssize_t read_block_state(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	char *kbuf;
	ssize_t index, written = 0;
	struct zram *zram = file->private_data;
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;

	kbuf = kvmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		kvfree(kbuf);
		return -EINVAL;
	}

	for (index = *ppos; index < nr_pages; index++) {
		int copied;

		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index))
			goto next;

		copied = snprintf(kbuf + written, count,
			"%12zd %12u.%06u %c%c%c%c%c%c%c\n",
			index, zram->table[index].attr.ac_time, 0U,
			zram_test_flag(zram, index, ZRAM_SAME) ? 's' : '.',
			zram_test_flag(zram, index, ZRAM_WB) ? 'w' : '.',
			zram_test_flag(zram, index, ZRAM_PP_SLOT) ? 'p' : '.',
			zram_test_flag(zram, index, ZRAM_HUGE) ? 'h' : '.',
			zram_test_flag(zram, index, ZRAM_IDLE) ? 'i' : '.',
			zram_get_priority(zram, index) ? 'r' : '.',
			zram_test_flag(zram, index,
				       ZRAM_INCOMPRESSIBLE) ? 'n' : '.');

		if (count <= copied) {
			zram_slot_unlock(zram, index);
			break;
		}
		written += copied;
		count -= copied;
next:
		zram_slot_unlock(zram, index);
		*ppos += 1;
	}

	up_read(&zram->init_lock);
	if (copy_to_user(buf, kbuf, written))
		written = -EFAULT;
	kvfree(kbuf);

	return written;
}

static const struct file_operations proc_zram_block_state_op = {
	.open = simple_open,
	.read = read_block_state,
	.llseek = default_llseek,
};

static void zram_debugfs_register(struct zram *zram)
{
	if (!zram_debugfs_root)
		return;

	zram->debugfs_dir = debugfs_create_dir(zram->disk->disk_name,
						zram_debugfs_root);
	debugfs_create_file("block_state", 0400, zram->debugfs_dir,
				zram, &proc_zram_block_state_op);
}

static void zram_debugfs_unregister(struct zram *zram)
{
	debugfs_remove_recursive(zram->debugfs_dir);
}
#else
static void zram_debugfs_create(void) {};
static void zram_debugfs_destroy(void) {};
static void zram_debugfs_register(struct zram *zram) {};
static void zram_debugfs_unregister(struct zram *zram) {};
#endif

/*
 * We switched to per-cpu streams and this attr is not needed anymore.
 * However, we will keep it around for some time, because:
 * a) we may revert per-cpu streams in the future
 * b) it's visible to user space and we need to follow our 2 years
 *    retirement rule; but we already have a number of 'soon to be
 *    altered' attrs, so max_comp_streams need to wait for the next
 *    layoff cycle.
 */
static ssize_t max_comp_streams_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", num_online_cpus());
}

static ssize_t max_comp_streams_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	return len;
}

static void comp_params_reset(struct zram *zram, u32 prio)
{
	struct zcomp_params *params = &zram->params[prio];

	vfree(params->dict);
	params->dict = NULL;
	params->dict_sz = 0;
	params->level = ZCOMP_PARAM_NOT_SET;
	params->deflate.winbits = ZCOMP_PARAM_NOT_SET;
	params->drv_data = NULL;
}

static void comp_algorithm_set(struct zram *zram, u32 prio, const char *alg)
{
	const char *name = alg ? zcomp_lookup_backend_name(alg) : NULL;

	if (WARN_ON_ONCE(alg && !name))
		return;
	if (zram->comp_algs[prio] == name)
		return;

	comp_params_reset(zram, prio);
	zram->comp_algs[prio] = name;
}

static ssize_t __comp_algorithm_show(struct zram *zram, u32 prio, char *buf)
{
	ssize_t sz;

	down_read(&zram->init_lock);
	sz = zcomp_available_show(zram->comp_algs[prio], buf, 0);
	up_read(&zram->init_lock);

	return sz;
}

static int __comp_algorithm_store(struct zram *zram, u32 prio, const char *buf)
{
	const char *compressor;
	size_t sz;

	sz = strlen(buf);
	if (sz >= ZRAM_MAX_ALGO_NAME_SZ)
		return -E2BIG;

	compressor = zcomp_lookup_backend_name(buf);
	if (!compressor)
		return -EINVAL;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		up_write(&zram->init_lock);
		pr_info("Can't change algorithm for initialized device\n");
		return -EBUSY;
	}

	comp_algorithm_set(zram, prio, compressor);
	up_write(&zram->init_lock);
	return 0;
}

static int lookup_algo_priority(struct zram *zram, const char *algo)
{
	u32 prio;

	for (prio = ZRAM_PRIMARY_COMP; prio < ZRAM_MAX_COMPS; prio++) {
		if (zram->comp_algs[prio] &&
		    !strcmp(zram->comp_algs[prio], algo))
			return prio;
	}
	return -EINVAL;
}

static int validate_algo_priority(struct zram *zram, const char *algo, u32 prio)
{
	if (prio >= ZRAM_MAX_COMPS || !zram->comp_algs[prio] ||
	    strcmp(zram->comp_algs[prio], algo))
		return -EINVAL;
	return 0;
}

static ssize_t algorithm_params_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t len)
{
	enum {
		SEEN_ALGO	= BIT(0),
		SEEN_PRIORITY	= BIT(1),
		SEEN_LEVEL	= BIT(2),
		SEEN_DICT	= BIT(3),
		SEEN_WINBITS	= BIT(4),
	};
	struct zram *zram = dev_to_zram(dev);
	struct deflate_params deflate = {
		.winbits = ZCOMP_PARAM_NOT_SET,
	};
	s32 prio = ZRAM_PRIMARY_COMP;
	s32 level = ZCOMP_PARAM_NOT_SET;
	char *args, *param, *val;
	char *algo = NULL;
	char *dict_path = NULL;
	unsigned long seen = 0;
	void *new_dict = NULL;
	void *old_dict;
	ssize_t dict_sz = 0;
	ssize_t ret;

	args = skip_spaces((char *)buf);
	while (*args) {
		unsigned long key;

		args = next_arg(args, &param, &val);
		if (!val || !*val)
			return -EINVAL;

		if (!strcmp(param, "algo")) {
			key = SEEN_ALGO;
			algo = val;
		} else if (!strcmp(param, "priority")) {
			key = SEEN_PRIORITY;
			ret = kstrtoint(val, 10, &prio);
			if (ret)
				return ret;
		} else if (!strcmp(param, "level")) {
			key = SEEN_LEVEL;
			ret = kstrtoint(val, 10, &level);
			if (ret)
				return ret;
		} else if (!strcmp(param, "dict")) {
			key = SEEN_DICT;
			if (*val != '/')
				return -EINVAL;
			dict_path = val;
		} else if (!strcmp(param, "deflate.winbits")) {
			key = SEEN_WINBITS;
			ret = kstrtoint(val, 10, &deflate.winbits);
			if (ret)
				return ret;
		} else {
			return -EINVAL;
		}

		if (seen & key)
			return -EINVAL;
		seen |= key;
	}
	if (!seen)
		return -EINVAL;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		ret = -EBUSY;
		goto out_unlock;
	}

	if (seen & SEEN_PRIORITY) {
		if (prio < ZRAM_PRIMARY_COMP || prio >= ZRAM_MAX_COMPS ||
		    !zram->comp_algs[prio]) {
			ret = -EINVAL;
			goto out_unlock;
		}
	}

	if ((seen & SEEN_ALGO) && (seen & SEEN_PRIORITY)) {
		ret = validate_algo_priority(zram, algo, prio);
		if (ret)
			goto out_unlock;
	} else if (seen & SEEN_ALGO) {
		prio = lookup_algo_priority(zram, algo);
		if (prio < 0) {
			ret = -EINVAL;
			goto out_unlock;
		}
	} else if (!zram->comp_algs[prio]) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (dict_path) {
		dict_sz = read_comp_algo_dictionary(&new_dict, dict_path);
		if (dict_sz < 0) {
			ret = dict_sz;
			goto out_unlock;
		}
	}

	old_dict = zram->params[prio].dict;
	zram->params[prio].dict = new_dict;
	zram->params[prio].dict_sz = dict_sz;
	zram->params[prio].level = level;
	zram->params[prio].deflate.winbits = deflate.winbits;
	zram->params[prio].drv_data = NULL;
	vfree(old_dict);
	ret = len;

out_unlock:
	up_write(&zram->init_lock);
	if (ret < 0)
		vfree(new_dict);
	return ret;
}

static ssize_t comp_algorithm_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return __comp_algorithm_show(zram, ZRAM_PRIMARY_COMP, buf);
}

static ssize_t comp_algorithm_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf,
				    size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	int ret;

	ret = __comp_algorithm_store(zram, ZRAM_PRIMARY_COMP, buf);
	return ret ? ret : len;
}

#ifdef CONFIG_ZRAM_MULTI_COMP
static ssize_t recomp_algorithm_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t sz = 0;
	u32 prio;

	down_read(&zram->init_lock);
	for (prio = ZRAM_SECONDARY_COMP; prio < ZRAM_MAX_COMPS; prio++) {
		if (!zram->comp_algs[prio])
			continue;

		sz += sysfs_emit_at(buf, sz, "#%u: ", prio);
		sz = zcomp_available_show(zram->comp_algs[prio], buf, sz);
	}
	up_read(&zram->init_lock);

	return sz;
}

static ssize_t recomp_algorithm_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	int prio = ZRAM_SECONDARY_COMP;
	char *args, *param, *val;
	char *alg = NULL;
	int ret;

	args = skip_spaces(buf);
	while (*args) {
		args = next_arg(args, &param, &val);

		if (!val || !*val)
			return -EINVAL;

		if (!strcmp(param, "algo")) {
			alg = val;
			continue;
		}

		if (!strcmp(param, "priority")) {
			ret = kstrtoint(val, 10, &prio);
			if (ret)
				return ret;
			continue;
		}
	}

	if (!alg)
		return -EINVAL;

	if (prio < ZRAM_SECONDARY_COMP || prio >= ZRAM_MAX_COMPS)
		return -EINVAL;

	ret = __comp_algorithm_store(zram, prio, alg);
	return ret ? ret : len;
}
#endif

static ssize_t compact_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		return -EINVAL;
	}

	zs_compact(zram->mem_pool);
	up_read(&zram->init_lock);

	return len;
}

static ssize_t io_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
			"%8llu %8llu 0 %8llu\n",
			(u64)atomic64_read(&zram->stats.failed_reads),
			(u64)atomic64_read(&zram->stats.failed_writes),
			(u64)atomic64_read(&zram->stats.notify_free));
	up_read(&zram->init_lock);

	return ret;
}

static ssize_t mm_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	struct zs_pool_stats pool_stats;
	u64 orig_size, mem_used = 0;
	long max_used;
	ssize_t ret;

	memset(&pool_stats, 0x00, sizeof(struct zs_pool_stats));

	down_read(&zram->init_lock);
	if (init_done(zram)) {
		mem_used = zs_get_total_pages(zram->mem_pool);
		zs_pool_stats(zram->mem_pool, &pool_stats);
	}

	orig_size = atomic64_read(&zram->stats.pages_stored);
	max_used = atomic_long_read(&zram->stats.max_used_pages);

	ret = scnprintf(buf, PAGE_SIZE,
			"%8llu %8llu %8llu %8lu %8ld %8llu %8lu %8llu %8llu\n",
			orig_size << PAGE_SHIFT,
			(u64)atomic64_read(&zram->stats.compr_data_size),
			mem_used << PAGE_SHIFT,
			zram->limit_pages << PAGE_SHIFT,
			max_used << PAGE_SHIFT,
			(u64)atomic64_read(&zram->stats.same_pages),
			atomic_long_read(&pool_stats.pages_compacted),
			(u64)atomic64_read(&zram->stats.huge_pages),
			(u64)atomic64_read(&zram->stats.huge_pages_since));
	up_read(&zram->init_lock);

	return ret;
}

#ifdef CONFIG_ZRAM_WRITEBACK
#define FOUR_K(x) ((x) * (1 << (PAGE_SHIFT - 12)))
static ssize_t bd_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
		"%8llu %8llu %8llu\n",
			FOUR_K((u64)atomic64_read(&zram->stats.bd_count)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_reads)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_writes)));
	up_read(&zram->init_lock);

	return ret;
}
#endif

static ssize_t debug_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int version = 1;
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
			"version: %d\n%8llu %8llu\n",
			version,
			(u64)atomic64_read(&zram->stats.writestall),
			(u64)atomic64_read(&zram->stats.miss_free));
	up_read(&zram->init_lock);

	return ret;
}

static DEVICE_ATTR_RO(io_stat);
static DEVICE_ATTR_RO(mm_stat);
#ifdef CONFIG_ZRAM_WRITEBACK
static DEVICE_ATTR_RO(bd_stat);
#endif
static DEVICE_ATTR_RO(debug_stat);

static void zram_meta_free(struct zram *zram, u64 disksize)
{
	size_t num_pages = disksize >> PAGE_SHIFT;
	size_t index;

	if (!zram->table)
		return;

	/* Free all pages that are still in this zram device */
	for (index = 0; index < num_pages; index++)
		zram_free_page(zram, index);

	zs_destroy_pool(zram->mem_pool);
	vfree(zram->table);
	zram->table = NULL;
}

static bool zram_meta_alloc(struct zram *zram, u64 disksize)
{
	size_t num_pages, index;

	num_pages = disksize >> PAGE_SHIFT;
	zram->table = vzalloc(array_size(num_pages, sizeof(*zram->table)));
	if (!zram->table)
		return false;

	zram->mem_pool = zs_create_pool(zram->disk->disk_name);
	if (!zram->mem_pool) {
		vfree(zram->table);
		zram->table = NULL;
		return false;
	}

	if (!huge_class_size)
		huge_class_size = zs_huge_class_size(zram->mem_pool);

	for (index = 0; index < num_pages; index++)
		zram_slot_lock_init(zram, index);

	return true;
}

/*
 * To protect concurrent access to the same index entry,
 * caller should hold this table index entry's bit lock to
 * indicate this index entry is accessing.
 */
static void zram_free_page(struct zram *zram, size_t index)
{
	unsigned long handle;

#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
	zram->table[index].attr.ac_time = 0;
#endif
	if (zram_test_flag(zram, index, ZRAM_IDLE))
		zram_clear_flag(zram, index, ZRAM_IDLE);
	if (zram_test_flag(zram, index, ZRAM_PP_SLOT))
		zram_clear_flag(zram, index, ZRAM_PP_SLOT);

	if (zram_test_flag(zram, index, ZRAM_HUGE)) {
		zram_clear_flag(zram, index, ZRAM_HUGE);
		atomic64_dec(&zram->stats.huge_pages);
	}

	if (zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
		zram_clear_flag(zram, index, ZRAM_INCOMPRESSIBLE);

	zram_set_priority(zram, index, 0);

	if (zram_test_flag(zram, index, ZRAM_WB)) {
		zram_clear_flag(zram, index, ZRAM_WB);
		free_block_bdev(zram, zram_get_element(zram, index));
		goto out;
	}

	/*
	 * No memory is allocated for same element filled pages.
	 * Simply clear same page flag.
	 */
	if (zram_test_flag(zram, index, ZRAM_SAME)) {
		zram_clear_flag(zram, index, ZRAM_SAME);
		atomic64_dec(&zram->stats.same_pages);
		goto out;
	}

	handle = zram_get_handle(zram, index);
	if (!handle)
		return;

	zs_free(zram->mem_pool, handle);

	atomic64_sub(zram_get_obj_size(zram, index),
			&zram->stats.compr_data_size);
out:
	atomic64_dec(&zram->stats.pages_stored);
	zram_set_handle(zram, index, 0);
	zram_set_obj_size(zram, index, 0);
	WARN_ON_ONCE(zram->table[index].attr.flags &
		~(1U << ZRAM_LOCK | 1U << ZRAM_UNDER_WB));
}

/*
 * Reads (decompresses if needed) a page from zspool (zsmalloc).
 * Corresponding ZRAM slot should be locked.
 */
static int zram_read_from_zspool(struct zram *zram, struct page *page,
				 u32 index)
{
	struct zcomp_strm *zstrm;
	unsigned long handle;
	unsigned int size;
	void *src, *dst;
	u32 prio;
	int ret;

	handle = zram_get_handle(zram, index);
	if (!handle || zram_test_flag(zram, index, ZRAM_SAME)) {
		unsigned long value;
		void *mem;

		value = handle ? zram_get_element(zram, index) : 0;
		mem = kmap_atomic(page);
		zram_fill_page(mem, PAGE_SIZE, value);
		kunmap_atomic(mem);
		return 0;
	}

	size = zram_get_obj_size(zram, index);

	if (size != PAGE_SIZE) {
		prio = zram_get_priority(zram, index);
		zstrm = zcomp_stream_get(zram->comps[prio]);
	}

	src = zs_obj_read_begin(zram->mem_pool, handle, size,
				size == PAGE_SIZE ? NULL : zstrm->local_copy);
	if (unlikely(!src)) {
		if (size != PAGE_SIZE)
			zcomp_stream_put(zstrm);
		return -EINVAL;
	}

	if (size == PAGE_SIZE) {
		dst = kmap_atomic(page);
		copy_page(dst, src);
		kunmap_atomic(dst);
		ret = 0;
	} else {
		dst = kmap_local_page(page);
		ret = zcomp_decompress(zram->comps[prio], zstrm, src, size,
				       dst);
		kunmap_local(dst);
	}
	zs_obj_read_end(zram->mem_pool, handle, size, src);
	if (size != PAGE_SIZE)
		zcomp_stream_put(zstrm);
	return ret;
}

static int zram_read_page(struct zram *zram, struct page *page, u32 index,
			  struct bio *parent)
{
	int ret;

	zram_slot_lock(zram, index);
	if (!zram_test_flag(zram, index, ZRAM_WB)) {
		/* Slot should be locked through out the function call */
		ret = zram_read_from_zspool(zram, page, index);
		zram_slot_unlock(zram, index);
	} else {
		/*
		 * The slot should be unlocked before reading from the backing
		 * device.
		 */
		zram_slot_unlock(zram, index);

		ret = read_from_bdev(zram, page, zram_get_element(zram, index),
				     parent);
	}

	/* Should NEVER happen. Return bio error if it does. */
	if (WARN_ON(ret < 0))
		pr_err("Decompression failed! err=%d, page=%u\n", ret, index);

	return ret;
}

/*
 * Use a temporary buffer to decompress the page, as the decompressor
 * always expects a full page for the output.
 */
static int zram_bvec_read_partial(struct zram *zram, struct bio_vec *bvec,
				  u32 index, int offset)
{
	struct page *page = alloc_page(GFP_NOIO);
	int ret;

	if (!page)
		return -ENOMEM;
	ret = zram_read_page(zram, page, index, NULL);
	if (likely(!ret))
		memcpy_to_bvec(bvec, page_address(page) + offset);
	__free_page(page);
	return ret;
}

static int zram_bvec_read(struct zram *zram, struct bio_vec *bvec,
			  u32 index, int offset, struct bio *bio)
{
	if (is_partial_io(bvec))
		return zram_bvec_read_partial(zram, bvec, index, offset);
	return zram_read_page(zram, bvec->bv_page, index, bio);
}

static int zram_write_page(struct zram *zram, struct page *page, u32 index)
{
	int ret = 0;
	unsigned long alloced_pages;
	unsigned long handle = -ENOMEM;
	unsigned int comp_len = 0;
	unsigned int alloc_len = 0;
	void *src, *mem;
	struct zcomp_strm *zstrm = NULL;
	unsigned long element = 0;
	enum zram_pageflags flags = 0;
	u8 prio;
	u8 selected_prio = ZRAM_PRIMARY_COMP;
	u8 prio_max = 1;
	bool incompressible = false;
#ifdef CONFIG_ZRAM_MULTI_COMP
	u8 tried_comps = 0;
	bool secondary_error = false;
#endif

	mem = kmap_atomic(page);
	if (page_same_filled(mem, &element)) {
		kunmap_atomic(mem);
		/* Free memory associated with this sector now. */
		flags = ZRAM_SAME;
		atomic64_inc(&zram->stats.same_pages);
		goto out;
	}
	kunmap_atomic(mem);

#ifdef CONFIG_ZRAM_MULTI_COMP
	prio_max = min_t(u8, ZRAM_MAX_COMPS,
			 READ_ONCE(sysctl_zram_recomp_immediate) + 1);
#endif
	/* A fully initialized ZRAM device always has the primary compressor. */
	if (unlikely(!prio_max))
		prio_max = 1;

	/*
	 * Try compressors in priority order. Keep the first stream that reduces
	 * the page below huge_class_size. Higher priorities are only attempted
	 * when the previous compressor would have stored an uncompressed page.
	 */
	for (prio = ZRAM_PRIMARY_COMP; prio < prio_max; prio++) {
		if (!zram->comps[prio])
			continue;

#ifdef CONFIG_ZRAM_MULTI_COMP
		tried_comps++;
#endif

		zstrm = zcomp_stream_get(zram->comps[prio]);
		src = kmap_local_page(page);
		ret = zcomp_compress(zram->comps[prio], zstrm, src,
				     &comp_len);
		kunmap_local(src);

		if (unlikely(ret)) {
			zcomp_stream_put(zstrm);
			zstrm = NULL;
			if (ret == -ENOSPC)
				continue;
			if (prio == ZRAM_PRIMARY_COMP) {
				pr_err("Compression failed! err=%d, priority=%u\n",
				       ret, prio);
				return ret;
			}
#ifdef CONFIG_ZRAM_MULTI_COMP
			secondary_error = true;
#endif
			pr_err_ratelimited(
				"Secondary compression failed! err=%d, priority=%u\n",
				ret, prio);
			continue;
		}

		if (comp_len < huge_class_size) {
			selected_prio = prio;
			break;
		}

		zcomp_stream_put(zstrm);
		zstrm = NULL;
	}

	if (!zstrm) {
		/*
		 * None of the permitted compressors helped. Store the original page,
		 * while retaining a primary stream as required by the original slow
		 * allocation / CPU-hotplug exclusion contract.
		 */
		selected_prio = ZRAM_PRIMARY_COMP;
		zstrm = zcomp_stream_get(zram->comps[selected_prio]);
		comp_len = PAGE_SIZE;
#ifdef CONFIG_ZRAM_MULTI_COMP
		incompressible = !secondary_error &&
			tried_comps == (u8)zram->num_active_comps;
#endif
	}

	alloc_len = comp_len;

	/*
	 * Handle allocation has two paths:
	 * a) fast path avoids direct reclaim while holding the selected stream;
	 * b) slow path drops the stream, allocates with reclaim enabled, then
	 *    recompresses with the SAME selected compressor.
	 */
	if (IS_ERR_VALUE(handle))
		handle = zs_malloc(zram->mem_pool, comp_len,
				__GFP_KSWAPD_RECLAIM |
				__GFP_NOWARN |
				__GFP_HIGHMEM |
				__GFP_MOVABLE |
				__GFP_CMA);
	if (IS_ERR_VALUE(handle)) {
		zcomp_stream_put(zstrm);
		zstrm = NULL;
		atomic64_inc(&zram->stats.writestall);

		handle = zs_malloc(zram->mem_pool, comp_len,
				GFP_NOIO | __GFP_HIGHMEM |
				__GFP_MOVABLE | __GFP_CMA);
		if (IS_ERR_VALUE(handle))
			return PTR_ERR((void *)handle);

		zstrm = zcomp_stream_get(zram->comps[selected_prio]);
		if (comp_len != PAGE_SIZE) {
			src = kmap_local_page(page);
			ret = zcomp_compress(zram->comps[selected_prio],
					     zstrm, src, &comp_len);
			kunmap_local(src);
			if (unlikely(ret)) {
				zcomp_stream_put(zstrm);
				zs_free(zram->mem_pool, handle);
				pr_err("Recompression failed! err=%d, priority=%u\n",
				       ret, selected_prio);
				return ret;
			}

			/* Never copy more data than the slow-path allocation requested. */
			if (unlikely(comp_len > alloc_len)) {
				zcomp_stream_put(zstrm);
				zs_free(zram->mem_pool, handle);
				pr_err("Recompression size changed: %u > %u, priority=%u\n",
				       comp_len, alloc_len, selected_prio);
				return -EIO;
			}
		}
	}

	alloced_pages = zs_get_total_pages(zram->mem_pool);
	update_used_max(zram, alloced_pages);

	if (zram->limit_pages && alloced_pages > zram->limit_pages) {
		zcomp_stream_put(zstrm);
		zs_free(zram->mem_pool, handle);
		return -ENOMEM;
	}

	src = zstrm->buffer;
	if (comp_len == PAGE_SIZE)
		src = kmap_atomic(page);
	ret = zs_obj_write(zram->mem_pool, handle, src, comp_len);
	if (comp_len == PAGE_SIZE)
		kunmap_atomic(src);

	zcomp_stream_put(zstrm);
	if (unlikely(ret)) {
		zs_free(zram->mem_pool, handle);
		return ret;
	}
	atomic64_add(comp_len, &zram->stats.compr_data_size);
out:
	/*
	 * Keep the old slot intact until compression, allocation and copying of
	 * the replacement object have all succeeded.
	 */
	zram_slot_lock(zram, index);
	zram_free_page(zram, index);

	if (comp_len == PAGE_SIZE) {
		zram_set_flag(zram, index, ZRAM_HUGE);
		atomic64_inc(&zram->stats.huge_pages);
		atomic64_inc(&zram->stats.huge_pages_since);
	}

	if (flags) {
		zram_set_flag(zram, index, flags);
		zram_set_element(zram, index, element);
	} else {
		zram_set_handle(zram, index, handle);
		zram_set_obj_size(zram, index, comp_len);
		zram_set_priority(zram, index, selected_prio);
		if (incompressible)
			zram_set_flag(zram, index, ZRAM_INCOMPRESSIBLE);
	}
	zram_slot_unlock(zram, index);

	/* Update stats */
	atomic64_inc(&zram->stats.pages_stored);
	return ret;
}

/*
 * This is a partial IO. Read the full page before writing the changes.
 */
static int zram_bvec_write_partial(struct zram *zram, struct bio_vec *bvec,
				   u32 index, int offset)
{
	struct page *page = alloc_page(GFP_NOIO);
	int ret;

	if (!page)
		return -ENOMEM;

	ret = zram_read_page(zram, page, index, NULL);
	if (!ret) {
		memcpy_from_bvec(page_address(page) + offset, bvec);
		ret = zram_write_page(zram, page, index);
	}
	__free_page(page);
	return ret;
}

static int zram_bvec_write(struct zram *zram, struct bio_vec *bvec,
			   u32 index, int offset)
{
	if (is_partial_io(bvec))
		return zram_bvec_write_partial(zram, bvec, index, offset);
	return zram_write_page(zram, bvec->bv_page, index);
}

#ifdef CONFIG_ZRAM_MULTI_COMP
#define RECOMPRESS_IDLE		(1 << 0)
#define RECOMPRESS_HUGE		(1 << 1)

static int zram_next_recompress_prio(struct zram *zram, u32 current_prio,
				     u32 prio, u32 prio_max)
{
	prio = max(prio, current_prio + 1);
	for (; prio < prio_max; prio++) {
		if (zram->comps[prio])
			return prio;
	}
	return -ENOENT;
}

static bool zram_recompress_candidate_locked(struct zram *zram, u32 index,
					     u32 mode, u32 threshold,
					     u32 prio, u32 prio_max)
{
	if (!zram_allocated(zram, index) ||
	    zram_test_flag(zram, index, ZRAM_WB) ||
	    zram_test_flag(zram, index, ZRAM_UNDER_WB) ||
	    zram_test_flag(zram, index, ZRAM_SAME) ||
	    zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
		return false;

	if ((mode & RECOMPRESS_IDLE) &&
	    !zram_test_flag(zram, index, ZRAM_IDLE))
		return false;
	if ((mode & RECOMPRESS_HUGE) &&
	    !zram_test_flag(zram, index, ZRAM_HUGE))
		return false;
	if (zram_get_obj_size(zram, index) < threshold)
		return false;

	return zram_next_recompress_prio(zram,
			zram_get_priority(zram, index), prio, prio_max) >= 0;
}

static int zram_scan_recompress_slots(struct zram *zram, u32 mode,
				      u32 threshold, u32 prio,
				      u32 prio_max,
				      struct zram_pp_ctl *ctl)
{
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	unsigned long index;

	for (index = 0; index < nr_pages; index++) {
		bool placed = true;

		zram_slot_lock(zram, index);
		if (zram_test_flag(zram, index, ZRAM_PP_SLOT) ||
		    !zram_recompress_candidate_locked(zram, index, mode,
						       threshold, prio,
						       prio_max))
			goto next;
		placed = place_pp_slot(zram, ctl, index);
next:
		zram_slot_unlock(zram, index);
		if (!placed)
			return -ENOMEM;
	}
	return 0;
}

/*
 * This function will decompress (unless it's ZRAM_HUGE) the page and then
 * attempt to compress it using provided compression algorithm priority
 * (which is potentially more effective).
 *
 * Corresponding ZRAM slot should be locked.
 */
static int zram_recompress(struct zram *zram, u32 index, struct page *page,
			   u32 threshold, u32 prio, u32 prio_max,
			   bool mark_incompressible, bool *attempted)
{
	struct zcomp_strm *zstrm = NULL;
	unsigned long handle_old;
	unsigned long handle_new;
	unsigned int comp_len_old;
	unsigned int comp_len_new;
	unsigned int class_index_old;
	unsigned int class_index_new;
	bool class_gain = false;
	void *src;
	int next_prio;
	int ret;

	*attempted = false;
	handle_old = zram_get_handle(zram, index);
	if (!handle_old)
		return -EINVAL;

	comp_len_old = zram_get_obj_size(zram, index);
	/*
	 * Do not recompress objects that are already "small enough".
	 */
	if (comp_len_old < threshold)
		return 0;

	ret = zram_read_from_zspool(zram, page, index);
	if (ret)
		return ret;

	/*
	 * We touched this entry so mark it as non-IDLE. This makes sure that
	 * we don't preserve IDLE flag and don't incorrectly pick this entry
	 * for different post-processing type (e.g. writeback).
	 */
	zram_clear_flag(zram, index, ZRAM_IDLE);

	class_index_old = zs_lookup_class_index(zram->mem_pool, comp_len_old);
	next_prio = zram_next_recompress_prio(zram,
			zram_get_priority(zram, index), prio, prio_max);
	if (next_prio < 0)
		return 0;
	prio = next_prio;

	/*
	 * Iterate the secondary comp algorithms list (in order of priority)
	 * and try to recompress the page.
	 */
	for (; prio < prio_max; prio++) {
		if (!zram->comps[prio])
			continue;

		zstrm = zcomp_stream_get(zram->comps[prio]);
		src = kmap_local_page(page);
		*attempted = true;
		ret = zcomp_compress(zram->comps[prio], zstrm, src,
				     &comp_len_new);
		kunmap_local(src);

		if (ret) {
			zcomp_stream_put(zstrm);
			zstrm = NULL;
			if (ret == -ENOSPC) {
				ret = 0;
				continue;
			}
			return ret;
		}

		class_index_new = zs_lookup_class_index(zram->mem_pool,
							comp_len_new);
		if (class_index_new < class_index_old)
			class_gain = true;

		/* Continue until we make progress */
		if (class_index_new >= class_index_old ||
		    (threshold && comp_len_new >= threshold)) {
			zcomp_stream_put(zstrm);
			zstrm = NULL;
			continue;
		}

		/* Recompression was successful so break out */
		break;
	}

	if (!zstrm) {
		if (mark_incompressible && *attempted && !class_gain)
			zram_set_flag(zram, index, ZRAM_INCOMPRESSIBLE);
		return 0;
	}

	/*
	 * No direct reclaim (slow path) for handle allocation and no
	 * re-compression attempt (unlike in zram_write_bvec()) since
	 * we already have stored that object in zsmalloc. If we cannot
	 * alloc memory for recompressed object then we bail out and
	 * simply keep the old (existing) object in zsmalloc.
	 */
	handle_new = zs_malloc(zram->mem_pool, comp_len_new,
			       __GFP_KSWAPD_RECLAIM |
			       __GFP_NOWARN |
			       __GFP_HIGHMEM |
			       __GFP_MOVABLE);
	if (IS_ERR_VALUE(handle_new)) {
		zcomp_stream_put(zstrm);
		return PTR_ERR((void *)handle_new);
	}

	ret = zs_obj_write(zram->mem_pool, handle_new, zstrm->buffer,
			   comp_len_new);
	zcomp_stream_put(zstrm);
	if (unlikely(ret)) {
		zs_free(zram->mem_pool, handle_new);
		return ret;
	}

	zram_free_page(zram, index);
	zram_set_handle(zram, index, handle_new);
	zram_set_obj_size(zram, index, comp_len_new);
	zram_set_priority(zram, index, prio);

	atomic64_add(comp_len_new, &zram->stats.compr_data_size);
	atomic64_inc(&zram->stats.pages_stored);

	return 0;
}

static ssize_t recompress_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	char *args, *param, *val, *algo = NULL;
	u32 mode = 0, threshold = 0;
	u32 prio = ZRAM_SECONDARY_COMP;
	u32 prio_max = ZRAM_MAX_COMPS;
	u64 max_pages = 0, attempts = 0;
	struct zram_pp_ctl *ctl = NULL;
	struct page *page = NULL;
	bool type_set = false;
	bool threshold_set = false;
	bool algo_set = false;
	bool priority_set = false;
	bool max_pages_set = false;
	bool explicit_target;
	bool pp_owned = false;
	ssize_t ret;
	u32 priority = ZRAM_SECONDARY_COMP;
	int err;

	args = skip_spaces(buf);
	while (*args) {
		args = next_arg(args, &param, &val);

		if (!val || !*val)
			return -EINVAL;

		if (!strcmp(param, "type")) {
			if (type_set)
				return -EINVAL;
			if (!strcmp(val, "idle"))
				mode = RECOMPRESS_IDLE;
			else if (!strcmp(val, "huge"))
				mode = RECOMPRESS_HUGE;
			else if (!strcmp(val, "huge_idle"))
				mode = RECOMPRESS_IDLE | RECOMPRESS_HUGE;
			else
				return -EINVAL;
			type_set = true;
			continue;
		}

		if (!strcmp(param, "threshold")) {
			if (threshold_set)
				return -EINVAL;
			/*
			 * We will re-compress only idle objects equal or
			 * greater in size than watermark.
			 */
			if (kstrtouint(val, 10, &threshold))
				return -EINVAL;
			threshold_set = true;
			continue;
		}

		if (!strcmp(param, "algo")) {
			if (algo_set)
				return -EINVAL;
			algo = val;
			algo_set = true;
			continue;
		}

		if (!strcmp(param, "priority")) {
			if (priority_set || kstrtouint(val, 10, &priority))
				return -EINVAL;
			priority_set = true;
			continue;
		}

		if (!strcmp(param, "max_pages")) {
			if (max_pages_set ||
			    kstrtoull(val, 10, &max_pages))
				return -EINVAL;
			max_pages_set = true;
			continue;
		}

		return -EINVAL;
	}

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		ret = -EINVAL;
		goto out_unlock;
	}
	if (atomic_cmpxchg(&zram->pp_in_progress, 0, 1)) {
		ret = -EBUSY;
		goto out_unlock;
	}
	pp_owned = true;

	if (threshold >= huge_class_size) {
		ret = -EINVAL;
		goto out;
	}

	if (priority_set &&
	    (priority < ZRAM_SECONDARY_COMP || priority >= ZRAM_MAX_COMPS)) {
		ret = -EINVAL;
		goto out;
	}

	if (algo_set && !priority_set) {
		for (priority = ZRAM_SECONDARY_COMP;
		     priority < ZRAM_MAX_COMPS; priority++) {
			if (zram->comp_algs[priority] &&
			    !strcmp(zram->comp_algs[priority], algo))
				break;
		}
		if (priority == ZRAM_MAX_COMPS) {
			ret = -EINVAL;
			goto out;
		}
	}

	explicit_target = algo_set || priority_set;
	if (explicit_target) {
		if (!zram->comps[priority] ||
		    (algo_set &&
		     strcmp(zram->comp_algs[priority], algo))) {
			ret = -EINVAL;
			goto out;
		}
		prio = priority;
		prio_max = priority + 1;
	}

	page = alloc_page(GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		goto out;
	}

	ctl = init_pp_ctl();
	if (!ctl) {
		ret = -ENOMEM;
		goto out;
	}

	err = zram_scan_recompress_slots(zram, mode, threshold, prio,
					 prio_max, ctl);
	if (err) {
		ret = err;
		goto out;
	}

	ret = len;
	while (select_pp_slot(ctl)) {
		struct zram_pp_slot *pps = select_pp_slot(ctl);
		unsigned long index = pps->index;
		bool attempted = false;

		err = 0;
		if (max_pages && attempts >= max_pages)
			break;

		zram_slot_lock(zram, index);
		if (!zram_test_flag(zram, index, ZRAM_PP_SLOT) ||
		    !zram_recompress_candidate_locked(zram, index, mode,
						       threshold, prio,
						       prio_max))
			goto next;

		err = zram_recompress(zram, index, page, threshold, prio,
				      prio_max, !explicit_target, &attempted);
next:
		if (attempted)
			attempts++;
		release_pp_slot_locked(zram, pps);
		zram_slot_unlock(zram, index);
		if (err) {
			ret = err;
			break;
		}

		cond_resched();
	}

out:
	if (page)
		__free_page(page);
	release_pp_ctl(zram, ctl);
	if (pp_owned)
		atomic_set(&zram->pp_in_progress, 0);
out_unlock:
	up_read(&zram->init_lock);
	return ret;
}
#endif

static void zram_bio_discard(struct zram *zram, struct bio *bio)
{
	size_t n = bio->bi_iter.bi_size;
	u32 index = bio->bi_iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
	u32 offset = (bio->bi_iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
			SECTOR_SHIFT;

	/*
	 * zram manages data in physical block size units. Because logical block
	 * size isn't identical with physical block size on some arch, we
	 * could get a discard request pointing to a specific offset within a
	 * certain physical block.  Although we can handle this request by
	 * reading that physiclal block and decompressing and partially zeroing
	 * and re-compressing and then re-storing it, this isn't reasonable
	 * because our intent with a discard request is to save memory.  So
	 * skipping this logical block is appropriate here.
	 */
	if (offset) {
		if (n <= (PAGE_SIZE - offset))
			goto end_bio;

		n -= (PAGE_SIZE - offset);
		index++;
	}

	while (n >= PAGE_SIZE) {
		zram_slot_lock(zram, index);
		zram_free_page(zram, index);
		zram_slot_unlock(zram, index);
		atomic64_inc(&zram->stats.notify_free);
		index++;
		n -= PAGE_SIZE;
	}

end_bio:
	bio_endio(bio);
}

static void zram_bio_read(struct zram *zram, struct bio *bio)
{
	unsigned long start_time = bio_start_io_acct(bio);
	struct bvec_iter iter = bio->bi_iter;

	do {
		u32 index = iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
		u32 offset = (iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
				SECTOR_SHIFT;
		struct bio_vec bv = bio_iter_iovec(bio, iter);

		bv.bv_len = min_t(u32, bv.bv_len, PAGE_SIZE - offset);

		zram_slot_lock(zram, index);
		zram_accessed(zram, index);
		zram_slot_unlock(zram, index);

		if (zram_bvec_read(zram, &bv, index, offset, bio) < 0) {
			atomic64_inc(&zram->stats.failed_reads);
			bio->bi_status = BLK_STS_IOERR;
			break;
		}
		flush_dcache_page(bv.bv_page);

		bio_advance_iter_single(bio, &iter, bv.bv_len);
	} while (iter.bi_size);

	bio_end_io_acct(bio, start_time);
	bio_endio(bio);
}

static void zram_bio_write(struct zram *zram, struct bio *bio)
{
	unsigned long start_time = bio_start_io_acct(bio);
	struct bvec_iter iter = bio->bi_iter;

	do {
		u32 index = iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
		u32 offset = (iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
				SECTOR_SHIFT;
		struct bio_vec bv = bio_iter_iovec(bio, iter);

		bv.bv_len = min_t(u32, bv.bv_len, PAGE_SIZE - offset);

		zram_slot_lock(zram, index);
		zram_accessed(zram, index);
		zram_slot_unlock(zram, index);

		if (zram_bvec_write(zram, &bv, index, offset) < 0) {
			atomic64_inc(&zram->stats.failed_writes);
			bio->bi_status = BLK_STS_IOERR;
			break;
		}

		bio_advance_iter_single(bio, &iter, bv.bv_len);
	} while (iter.bi_size);

	bio_end_io_acct(bio, start_time);
	bio_endio(bio);
}

/*
 * Handler function for all zram I/O requests.
 */
static void zram_submit_bio(struct bio *bio)
{
	struct zram *zram = bio->bi_bdev->bd_disk->private_data;

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		zram_bio_read(zram, bio);
		break;
	case REQ_OP_WRITE:
		zram_bio_write(zram, bio);
		break;
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		zram_bio_discard(zram, bio);
		break;
	default:
		WARN_ON_ONCE(1);
		bio_endio(bio);
	}
}

static void zram_slot_free_notify(struct block_device *bdev,
				unsigned long index)
{
	struct zram *zram;

	zram = bdev->bd_disk->private_data;

	atomic64_inc(&zram->stats.notify_free);
	if (!zram_slot_trylock(zram, index)) {
		atomic64_inc(&zram->stats.miss_free);
		return;
	}

	zram_free_page(zram, index);
	zram_slot_unlock(zram, index);
}

static void zram_destroy_comps(struct zram *zram)
{
	u32 prio;

	for (prio = 0; prio < ZRAM_MAX_COMPS; prio++) {
		struct zcomp *comp = zram->comps[prio];

		zram->comps[prio] = NULL;
		if (!comp)
			continue;
		zcomp_destroy(comp);
		zram->num_active_comps--;
	}
}

static void zram_reset_comp_config(struct zram *zram)
{
	u32 prio;

	for (prio = ZRAM_PRIMARY_COMP; prio < ZRAM_MAX_COMPS; prio++)
		comp_algorithm_set(zram, prio, NULL);
	comp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);
}

static void zram_reset_device(struct zram *zram)
{
	down_write(&zram->init_lock);

	zram->limit_pages = 0;

	set_capacity_and_notify(zram->disk, 0);
	part_stat_set_all(zram->disk->part0, 0);

	/* I/O operation under all of CPU are done so let's free */
	zram_meta_free(zram, zram->disksize);
	zram->disksize = 0;
	zram_destroy_comps(zram);
	zram_reset_comp_config(zram);
	memset(&zram->stats, 0, sizeof(zram->stats));
	reset_bdev(zram);
#if defined(CONFIG_ZRAM_WRITEBACK) || defined(CONFIG_ZRAM_MULTI_COMP)
	atomic_set(&zram->pp_in_progress, 0);
#endif

	up_write(&zram->init_lock);
}

static ssize_t disksize_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	u64 disksize;
	struct zcomp *comp;
	struct zram *zram = dev_to_zram(dev);
	int err;
	u32 prio;

	disksize = memparse(buf, NULL);
	if (!disksize)
		return -EINVAL;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		pr_info("Cannot change disksize for initialized device\n");
		err = -EBUSY;
		goto out_unlock;
	}

	disksize = PAGE_ALIGN(disksize);
	if (!zram_meta_alloc(zram, disksize)) {
		err = -ENOMEM;
		goto out_unlock;
	}

	for (prio = 0; prio < ZRAM_MAX_COMPS; prio++) {
		if (!zram->comp_algs[prio])
			continue;

		comp = zcomp_create(zram->comp_algs[prio],
				    &zram->params[prio]);
		if (IS_ERR(comp)) {
			pr_err("Cannot initialise %s compressing backend\n",
			       zram->comp_algs[prio]);
			err = PTR_ERR(comp);
			goto out_free_comps;
		}

		zram->comps[prio] = comp;
		zram->num_active_comps++;
	}
	zram->disksize = disksize;
	set_capacity_and_notify(zram->disk, zram->disksize >> SECTOR_SHIFT);
	up_write(&zram->init_lock);

	return len;

out_free_comps:
	zram_destroy_comps(zram);
	zram_meta_free(zram, disksize);
out_unlock:
	up_write(&zram->init_lock);
	return err;
}

static ssize_t reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	unsigned short do_reset;
	struct zram *zram;
	struct gendisk *disk;

	ret = kstrtou16(buf, 10, &do_reset);
	if (ret)
		return ret;

	if (!do_reset)
		return -EINVAL;

	zram = dev_to_zram(dev);
	disk = zram->disk;

	mutex_lock(&disk->open_mutex);
	/* Do not reset an active device or claimed device */
	if (disk_openers(disk) || zram->claim) {
		mutex_unlock(&disk->open_mutex);
		return -EBUSY;
	}

	/* From now on, anyone can't open /dev/zram[0-9] */
	zram->claim = true;
	mutex_unlock(&disk->open_mutex);

	/* Make sure all the pending I/O are finished */
	sync_blockdev(disk->part0);
	zram_reset_device(zram);

	mutex_lock(&disk->open_mutex);
	zram->claim = false;
	mutex_unlock(&disk->open_mutex);

	return len;
}

static int zram_open(struct gendisk *disk, blk_mode_t mode)
{
	struct zram *zram = disk->private_data;

	WARN_ON(!mutex_is_locked(&disk->open_mutex));

	/* zram was claimed to reset so open request fails */
	if (zram->claim)
		return -EBUSY;
	return 0;
}

static const struct block_device_operations zram_devops = {
	.open = zram_open,
	.submit_bio = zram_submit_bio,
	.swap_slot_free_notify = zram_slot_free_notify,
	.owner = THIS_MODULE
};

static DEVICE_ATTR_WO(compact);
static DEVICE_ATTR_RW(disksize);
static DEVICE_ATTR_RO(initstate);
static DEVICE_ATTR_WO(reset);
static DEVICE_ATTR_WO(mem_limit);
static DEVICE_ATTR_WO(mem_used_max);
static DEVICE_ATTR_WO(idle);
static DEVICE_ATTR_RW(max_comp_streams);
static DEVICE_ATTR_RW(comp_algorithm);
static DEVICE_ATTR_WO(algorithm_params);
#ifdef CONFIG_ZRAM_WRITEBACK
static DEVICE_ATTR_RW(backing_dev);
static DEVICE_ATTR_WO(writeback);
static DEVICE_ATTR_RW(writeback_limit);
static DEVICE_ATTR_RW(writeback_limit_enable);
static DEVICE_ATTR_RO(writeback_stat);
#endif
#ifdef CONFIG_ZRAM_MULTI_COMP
static DEVICE_ATTR_RW(recomp_algorithm);
static DEVICE_ATTR_WO(recompress);
#endif

static struct attribute *zram_disk_attrs[] = {
	&dev_attr_disksize.attr,
	&dev_attr_initstate.attr,
	&dev_attr_reset.attr,
	&dev_attr_compact.attr,
	&dev_attr_mem_limit.attr,
	&dev_attr_mem_used_max.attr,
	&dev_attr_idle.attr,
	&dev_attr_max_comp_streams.attr,
	&dev_attr_comp_algorithm.attr,
	&dev_attr_algorithm_params.attr,
#ifdef CONFIG_ZRAM_WRITEBACK
	&dev_attr_backing_dev.attr,
	&dev_attr_writeback.attr,
	&dev_attr_writeback_limit.attr,
	&dev_attr_writeback_limit_enable.attr,
	&dev_attr_writeback_stat.attr,
#endif
	&dev_attr_io_stat.attr,
	&dev_attr_mm_stat.attr,
#ifdef CONFIG_ZRAM_WRITEBACK
	&dev_attr_bd_stat.attr,
#endif
	&dev_attr_debug_stat.attr,
#ifdef CONFIG_ZRAM_MULTI_COMP
	&dev_attr_recomp_algorithm.attr,
	&dev_attr_recompress.attr,
#endif
	NULL,
};

ATTRIBUTE_GROUPS(zram_disk);

/*
 * Allocate and initialize new zram device. the function returns
 * '>= 0' device_id upon success, and negative value otherwise.
 */
static int zram_add(void)
{
	struct zram *zram;
	int ret, device_id;
	u32 prio;

	zram = kzalloc(sizeof(struct zram), GFP_KERNEL);
	if (!zram)
		return -ENOMEM;

	ret = idr_alloc(&zram_index_idr, zram, 0, 0, GFP_KERNEL);
	if (ret < 0)
		goto out_free_dev;
	device_id = ret;

	init_rwsem(&zram->init_lock);
	for (prio = 0; prio < ZRAM_MAX_COMPS; prio++)
		comp_params_reset(zram, prio);
#if defined(CONFIG_ZRAM_WRITEBACK) || defined(CONFIG_ZRAM_MULTI_COMP)
	atomic_set(&zram->pp_in_progress, 0);
#endif
#ifdef CONFIG_ZRAM_WRITEBACK
	mutex_init(&zram->writeback_lock);
	spin_lock_init(&zram->wb_limit_lock);
	spin_lock_init(&zram->bitmap_lock);
	zram->wb_next_block = 1;
#endif

	/* gendisk structure */
	zram->disk = blk_alloc_disk(NUMA_NO_NODE);
	if (!zram->disk) {
		pr_err("Error allocating disk structure for device %d\n",
			device_id);
		ret = -ENOMEM;
		goto out_free_idr;
	}

	zram->disk->major = zram_major;
	zram->disk->first_minor = device_id;
	zram->disk->minors = 1;
	zram->disk->flags |= GENHD_FL_NO_PART;
	zram->disk->fops = &zram_devops;
	zram->disk->private_data = zram;
	snprintf(zram->disk->disk_name, 16, "zram%d", device_id);

	comp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);

	/* Actual capacity set using sysfs (/sys/block/zram<id>/disksize */
	set_capacity(zram->disk, 0);
	/* zram devices sort of resembles non-rotational disks */
	blk_queue_flag_set(QUEUE_FLAG_NONROT, zram->disk->queue);
	blk_queue_flag_set(QUEUE_FLAG_SYNCHRONOUS, zram->disk->queue);

	/*
	 * To ensure that we always get PAGE_SIZE aligned
	 * and n*PAGE_SIZED sized I/O requests.
	 */
	blk_queue_physical_block_size(zram->disk->queue, PAGE_SIZE);
	blk_queue_logical_block_size(zram->disk->queue,
					ZRAM_LOGICAL_BLOCK_SIZE);
	blk_queue_io_min(zram->disk->queue, PAGE_SIZE);
	blk_queue_io_opt(zram->disk->queue, PAGE_SIZE);
	zram->disk->queue->limits.discard_granularity = PAGE_SIZE;
	blk_queue_max_discard_sectors(zram->disk->queue, UINT_MAX);

	/*
	 * zram_bio_discard() will clear all logical blocks if logical block
	 * size is identical with physical block size(PAGE_SIZE). But if it is
	 * different, we will skip discarding some parts of logical blocks in
	 * the part of the request range which isn't aligned to physical block
	 * size.  So we can't ensure that all discarded logical blocks are
	 * zeroed.
	 */
	if (ZRAM_LOGICAL_BLOCK_SIZE == PAGE_SIZE)
		blk_queue_max_write_zeroes_sectors(zram->disk->queue, UINT_MAX);

	blk_queue_flag_set(QUEUE_FLAG_STABLE_WRITES, zram->disk->queue);
	ret = device_add_disk(NULL, zram->disk, zram_disk_groups);
	if (ret)
		goto out_cleanup_disk;

	zram_debugfs_register(zram);
	pr_info("Added device: %s\n", zram->disk->disk_name);
	return device_id;

out_cleanup_disk:
	put_disk(zram->disk);
out_free_idr:
	idr_remove(&zram_index_idr, device_id);
out_free_dev:
	kfree(zram);
	return ret;
}

static int zram_remove(struct zram *zram)
{
	bool claimed;

	mutex_lock(&zram->disk->open_mutex);
	if (disk_openers(zram->disk)) {
		mutex_unlock(&zram->disk->open_mutex);
		return -EBUSY;
	}

	claimed = zram->claim;
	if (!claimed)
		zram->claim = true;
	mutex_unlock(&zram->disk->open_mutex);

	zram_debugfs_unregister(zram);

	if (claimed) {
		/*
		 * If we were claimed by reset_store(), del_gendisk() will
		 * wait until reset_store() is done, so nothing need to do.
		 */
		;
	} else {
		/* Make sure all the pending I/O are finished */
		sync_blockdev(zram->disk->part0);
		zram_reset_device(zram);
	}

	pr_info("Removed device: %s\n", zram->disk->disk_name);

	del_gendisk(zram->disk);

	/* del_gendisk drains pending reset_store */
	WARN_ON_ONCE(claimed && zram->claim);

	/*
	 * disksize_store() may be called in between zram_reset_device()
	 * and del_gendisk(), so run the last reset to avoid leaking
	 * anything allocated with disksize_store()
	 */
	zram_reset_device(zram);

	put_disk(zram->disk);
	kfree(zram);
	return 0;
}

/* zram-control sysfs attributes */

/*
 * NOTE: hot_add attribute is not the usual read-only sysfs attribute. In a
 * sense that reading from this file does alter the state of your system -- it
 * creates a new un-initialized zram device and returns back this device's
 * device_id (or an error code if it fails to create a new device).
 */
static ssize_t hot_add_show(const struct class *class,
			const struct class_attribute *attr,
			char *buf)
{
	int ret;

	mutex_lock(&zram_index_mutex);
	ret = zram_add();
	mutex_unlock(&zram_index_mutex);

	if (ret < 0)
		return ret;
	return scnprintf(buf, PAGE_SIZE, "%d\n", ret);
}
/* This attribute must be set to 0400, so CLASS_ATTR_RO() can not be used */
static struct class_attribute class_attr_hot_add =
	__ATTR(hot_add, 0400, hot_add_show, NULL);

static ssize_t hot_remove_store(const struct class *class,
			const struct class_attribute *attr,
			const char *buf,
			size_t count)
{
	struct zram *zram;
	int ret, dev_id;

	/* dev_id is gendisk->first_minor, which is `int' */
	ret = kstrtoint(buf, 10, &dev_id);
	if (ret)
		return ret;
	if (dev_id < 0)
		return -EINVAL;

	mutex_lock(&zram_index_mutex);

	zram = idr_find(&zram_index_idr, dev_id);
	if (zram) {
		ret = zram_remove(zram);
		if (!ret)
			idr_remove(&zram_index_idr, dev_id);
	} else {
		ret = -ENODEV;
	}

	mutex_unlock(&zram_index_mutex);
	return ret ? ret : count;
}
static CLASS_ATTR_WO(hot_remove);

static struct attribute *zram_control_class_attrs[] = {
	&class_attr_hot_add.attr,
	&class_attr_hot_remove.attr,
	NULL,
};
ATTRIBUTE_GROUPS(zram_control_class);

static struct class zram_control_class = {
	.name		= "zram-control",
	.class_groups	= zram_control_class_groups,
};

static int zram_remove_cb(int id, void *ptr, void *data)
{
	WARN_ON_ONCE(zram_remove(ptr));
	return 0;
}

static void destroy_devices(void)
{
	class_unregister(&zram_control_class);
	idr_for_each(&zram_index_idr, &zram_remove_cb, NULL);
	zram_debugfs_destroy();
	idr_destroy(&zram_index_idr);
	unregister_blkdev(zram_major, "zram");
	cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
}

static int __init zram_init(void)
{
	int ret;

	BUILD_BUG_ON(__NR_ZRAM_PAGEFLAGS >
		     sizeof_field(struct zram_table_entry, attr.flags) *
		     BITS_PER_BYTE);

	ret = cpuhp_setup_state_multi(CPUHP_ZCOMP_PREPARE, "block/zram:prepare",
				      zcomp_cpu_up_prepare, zcomp_cpu_dead);
	if (ret < 0)
		return ret;

	ret = class_register(&zram_control_class);
	if (ret) {
		pr_err("Unable to register zram-control class\n");
		cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
		return ret;
	}

	zram_debugfs_create();
	zram_major = register_blkdev(0, "zram");
	if (zram_major <= 0) {
		pr_err("Unable to get major number\n");
		class_unregister(&zram_control_class);
		cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
		return -EBUSY;
	}

	while (num_devices != 0) {
		mutex_lock(&zram_index_mutex);
		ret = zram_add();
		mutex_unlock(&zram_index_mutex);
		if (ret < 0)
			goto out_error;
		num_devices--;
	}



#ifdef CONFIG_ZRAM_MULTI_COMP
	zram_sysctl_header = register_sysctl("vm", zram_sysctl_table);
	if (!zram_sysctl_header)
		pr_warn("failed to register ZRAM-IR sysctl\n");
	else
		pr_info("ZRAM Immediate Recompression enabled\n");
#endif

	return 0;

out_error:
	destroy_devices();
	return ret;
}

static void __exit zram_exit(void)
{
#ifdef CONFIG_ZRAM_MULTI_COMP
	if (zram_sysctl_header)
		unregister_sysctl_table(zram_sysctl_header);
#endif
	destroy_devices();
}

module_init(zram_init);
module_exit(zram_exit);

module_param(num_devices, uint, 0);
MODULE_PARM_DESC(num_devices, "Number of pre-created zram devices");

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Nitin Gupta <ngupta@vflare.org>");
MODULE_DESCRIPTION("Compressed RAM Block Device");
