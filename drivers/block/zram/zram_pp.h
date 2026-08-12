/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _ZRAM_PP_H_
#define _ZRAM_PP_H_

#include <linux/gfp_types.h>
#include <linux/list.h>
#include <linux/mempool.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

struct zram;

enum zram_pp_job_type {
	ZRAM_PP_RECOMPRESS = 0,
	ZRAM_PP_DEDUP,
	ZRAM_PP_DELTA,
	ZRAM_PP_WRITEBACK,
	ZRAM_PP_PACKED_READ,
	ZRAM_PP_GC,
	ZRAM_PP_JOB_MAX,
};

enum zram_pp_priority {
	ZRAM_PP_PRIO_LOW = 0,
	ZRAM_PP_PRIO_NORMAL,
	ZRAM_PP_PRIO_HIGH,
	ZRAM_PP_PRIO_MAX,
};

enum zram_pp_state_bit {
	ZRAM_PP_CANCELLED = 0,
	ZRAM_PP_DONE,
	ZRAM_PP_CHARGED,
	ZRAM_PP_OPERATION_ENDED,
	ZRAM_PP_OPERATION_FINISHED,
	ZRAM_PP_ASYNC,
	ZRAM_PP_RUNNING,
};

/* Independent budgets: none is implicitly converted into another. */
struct zram_pp_budget {
	spinlock_t lock;
	u64 scan_limit;
	u64 candidate_limit;
	u64 logical_limit;
	u64 resident_limit;
	u64 backing_pin_limit;
	u64 scan_used;
	u64 candidates_used;
	u64 logical_used;
	u64 resident_used;
	u64 backing_pins_used;
	u64 physical_reserved;
	u64 physical_committed;
	u64 physical_rollback;
	u64 gc_physical_writes;
};

struct zram_pp_scheduler {
	struct zram *zram;
	struct xarray slot_jobs;
	spinlock_t lock;
	struct list_head operations;
	wait_queue_head_t wait;
	atomic64_t next_id;
	atomic_t active_operations;
	atomic_t pending_jobs;
	atomic_t active_io;
	unsigned long active_types;
	u32 max_pending_jobs;
	bool stopping;
	struct workqueue_struct *fault_wq;
	struct workqueue_struct *write_wq;
	struct workqueue_struct *cpu_wq;
	mempool_t *job_pool;
};

struct zram_pp_operation {
	struct zram_pp_scheduler *scheduler;
	struct list_head entry;
	refcount_t refs;
	atomic_t active_jobs;
	struct zram_pp_budget budget;
	u64 id;
	unsigned long state;
	u8 type;
	u8 priority;
};

struct zram_pp_job;
typedef void (*zram_pp_job_fn)(struct zram *zram,
			       struct zram_pp_job *job);

struct zram_pp_job {
	struct work_struct work;
	struct list_head entry;
	struct zram_pp_scheduler *scheduler;
	struct zram_pp_operation *operation;
	zram_pp_job_fn run;
	u64 generation;
	u64 charged_bytes;
	unsigned long state;
	u32 index;
	u8 type;
	u8 priority;
};

int zram_pp_scheduler_init(struct zram_pp_scheduler *scheduler,
			   struct zram *zram);
void zram_pp_scheduler_quiesce(struct zram_pp_scheduler *scheduler);
void zram_pp_scheduler_resume(struct zram_pp_scheduler *scheduler);
void zram_pp_scheduler_fini(struct zram_pp_scheduler *scheduler);

struct zram_pp_operation *
zram_pp_operation_begin(struct zram_pp_scheduler *scheduler,
			enum zram_pp_job_type type,
			enum zram_pp_priority priority,
			u64 max_pages, u64 max_bytes);
void zram_pp_operation_cancel(struct zram_pp_operation *operation);
bool zram_pp_operation_cancelled(const struct zram_pp_operation *operation);
void zram_pp_operation_end(struct zram_pp_operation *operation);

struct zram_pp_job *zram_pp_job_claim_locked(struct zram *zram,
					      struct zram_pp_operation *operation,
					      u32 index, gfp_t gfp);
int zram_pp_submit_latest(struct zram *zram, enum zram_pp_job_type type,
			  enum zram_pp_priority priority, u32 index,
			  u64 generation, zram_pp_job_fn run);
bool zram_pp_job_is_current_locked(struct zram *zram,
				   const struct zram_pp_job *job);
bool zram_pp_job_charge(struct zram_pp_job *job, u64 bytes);
bool zram_pp_operation_charge_scan(struct zram_pp_operation *operation,
				   u64 slots);
bool zram_pp_operation_charge_candidate(struct zram_pp_operation *operation);
bool zram_pp_budget_reserve_physical(struct zram_pp_budget *budget,
				     u64 blocks);
void zram_pp_budget_commit_physical(struct zram_pp_budget *budget,
				    u64 blocks, bool gc);
void zram_pp_budget_rollback_physical(struct zram_pp_budget *budget,
				      u64 blocks);
void zram_pp_job_finish_locked(struct zram *zram,
			       struct zram_pp_job *job);
void zram_pp_cancel_slot_locked(struct zram *zram, u32 index);
bool zram_pp_slot_active_locked(struct zram *zram, u32 index);

bool zram_pp_io_get(struct zram_pp_scheduler *scheduler);
void zram_pp_io_put(struct zram_pp_scheduler *scheduler);

#endif /* _ZRAM_PP_H_ */
