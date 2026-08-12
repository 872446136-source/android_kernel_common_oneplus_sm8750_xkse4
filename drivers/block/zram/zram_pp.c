// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/err.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#include "zram_drv.h"

#define ZRAM_PP_DEFAULT_PENDING	256U
#define ZRAM_PP_FAULT_CONCURRENCY	4U
#define ZRAM_PP_CPU_CONCURRENCY	2U
#define ZRAM_PP_WRITE_CONCURRENCY	2U

static void zram_pp_operation_get(struct zram_pp_operation *operation)
{
	refcount_inc(&operation->refs);
}

static void zram_pp_operation_put(struct zram_pp_operation *operation)
{
	if (refcount_dec_and_test(&operation->refs))
		kfree(operation);
}

static bool
zram_pp_operation_complete_locked(struct zram_pp_operation *operation)
{
	struct zram_pp_scheduler *scheduler = operation->scheduler;

	if (!test_bit(ZRAM_PP_OPERATION_ENDED, &operation->state) ||
	    atomic_read(&operation->active_jobs))
		return false;
	if (test_bit(ZRAM_PP_OPERATION_FINISHED, &operation->state))
		return false;
	if (WARN_ON_ONCE(list_empty(&operation->entry)))
		return false;

	set_bit(ZRAM_PP_OPERATION_FINISHED, &operation->state);
	list_del_init(&operation->entry);
	clear_bit(operation->type, &scheduler->active_types);
	WARN_ON_ONCE(atomic_dec_return(&scheduler->active_operations) < 0);
	return true;
}

static void zram_pp_operation_maybe_complete(
	struct zram_pp_operation *operation)
{
	struct zram_pp_scheduler *scheduler = operation->scheduler;
	unsigned long flags;
	bool completed;

	spin_lock_irqsave(&scheduler->lock, flags);
	completed = zram_pp_operation_complete_locked(operation);
	spin_unlock_irqrestore(&scheduler->lock, flags);
	if (completed)
		wake_up_all(&scheduler->wait);
}

static bool zram_pp_operation_add_job(struct zram_pp_operation *operation)
{
	struct zram_pp_scheduler *scheduler = operation->scheduler;
	unsigned long flags;
	bool accepted = false;

	spin_lock_irqsave(&scheduler->lock, flags);
	if (!scheduler->stopping &&
	    !test_bit(ZRAM_PP_CANCELLED, &operation->state) &&
	    !test_bit(ZRAM_PP_OPERATION_ENDED, &operation->state) &&
	    !test_bit(ZRAM_PP_OPERATION_FINISHED, &operation->state)) {
		zram_pp_operation_get(operation);
		atomic_inc(&operation->active_jobs);
		accepted = true;
	}
	spin_unlock_irqrestore(&scheduler->lock, flags);
	return accepted;
}

static struct zram_pp_job *
zram_pp_job_alloc(struct zram_pp_scheduler *scheduler, gfp_t gfp)
{
	struct zram_pp_job *job;
	unsigned long flags;

	/*
	 * Admission and quiesce share one gate.  The pending token keeps the
	 * scheduler storage alive until this allocation is either queued or
	 * rolled back.
	 */
	spin_lock_irqsave(&scheduler->lock, flags);
	if (scheduler->stopping ||
	    atomic_read(&scheduler->pending_jobs) >=
		scheduler->max_pending_jobs) {
		spin_unlock_irqrestore(&scheduler->lock, flags);
		return NULL;
	}
	atomic_inc(&scheduler->pending_jobs);
	spin_unlock_irqrestore(&scheduler->lock, flags);
	job = mempool_alloc(scheduler->job_pool, gfp);
	if (!job) {
		WARN_ON_ONCE(atomic_dec_return(&scheduler->pending_jobs) < 0);
		wake_up_all(&scheduler->wait);
		return NULL;
	}
	memset(job, 0, sizeof(*job));
	INIT_LIST_HEAD(&job->entry);
	job->scheduler = scheduler;
	return job;
}

static bool zram_pp_producer_get(struct zram_pp_scheduler *scheduler)
{
	unsigned long flags;
	bool accepted = false;

	spin_lock_irqsave(&scheduler->lock, flags);
	if (!scheduler->stopping) {
		atomic_inc(&scheduler->active_producers);
		accepted = true;
	}
	spin_unlock_irqrestore(&scheduler->lock, flags);
	return accepted;
}

static void zram_pp_producer_put(struct zram_pp_scheduler *scheduler)
{
	WARN_ON_ONCE(atomic_dec_return(&scheduler->active_producers) < 0);
	wake_up_all(&scheduler->wait);
}

static void zram_pp_job_drop(struct zram_pp_job *job)
{
	struct zram_pp_scheduler *scheduler = job->scheduler;
	struct zram_pp_operation *operation = job->operation;

	if (operation) {
		int active_jobs = atomic_dec_return(&operation->active_jobs);

		if (WARN_ON_ONCE(active_jobs < 0))
			active_jobs = 0;
		if (!active_jobs)
			zram_pp_operation_maybe_complete(operation);
		zram_pp_operation_put(operation);
	}
	mempool_free(job, scheduler->job_pool);
	WARN_ON_ONCE(atomic_dec_return(&scheduler->pending_jobs) < 0);
	wake_up_all(&scheduler->wait);
}

static void zram_pp_async_work(struct work_struct *work)
{
	struct zram_pp_job *job = container_of(work, struct zram_pp_job, work);
	struct zram_pp_scheduler *scheduler = job->scheduler;
	u64 generation;
	void *mapped_job;

	for (;;) {
		generation = READ_ONCE(job->generation);
		if (test_bit(ZRAM_PP_CANCELLED, &job->state) ||
		    READ_ONCE(scheduler->stopping))
			break;
		set_bit(ZRAM_PP_RUNNING, &job->state);
		job->run(scheduler->zram, job);
		clear_bit(ZRAM_PP_RUNNING, &job->state);

		zram_slot_lock(scheduler->zram, job->index);
		mapped_job = xa_load(&scheduler->slot_jobs, job->index);
		if (mapped_job == job &&
		    !test_bit(ZRAM_PP_CANCELLED, &job->state) &&
		    !READ_ONCE(scheduler->stopping) &&
		    generation != READ_ONCE(job->generation)) {
			zram_slot_unlock(scheduler->zram, job->index);
			cond_resched();
			continue;
		}
		set_bit(ZRAM_PP_DONE, &job->state);
		mapped_job = xa_cmpxchg(&scheduler->slot_jobs, job->index,
					job, NULL, 0);
		zram_slot_unlock(scheduler->zram, job->index);
		WARN_ON_ONCE(xa_is_err(mapped_job));
		zram_pp_job_drop(job);
		return;
	}

	zram_slot_lock(scheduler->zram, job->index);
	set_bit(ZRAM_PP_DONE, &job->state);
	mapped_job = xa_cmpxchg(&scheduler->slot_jobs, job->index, job, NULL, 0);
	zram_slot_unlock(scheduler->zram, job->index);
	WARN_ON_ONCE(xa_is_err(mapped_job));
	zram_pp_job_drop(job);
}

int zram_pp_scheduler_init(struct zram_pp_scheduler *scheduler,
			   struct zram *zram)
{
	memset(scheduler, 0, sizeof(*scheduler));
	scheduler->zram = zram;
	xa_init(&scheduler->slot_jobs);
	spin_lock_init(&scheduler->lock);
	INIT_LIST_HEAD(&scheduler->operations);
	init_waitqueue_head(&scheduler->wait);
	atomic64_set(&scheduler->next_id, 0);
	atomic_set(&scheduler->active_operations, 0);
	atomic_set(&scheduler->active_producers, 0);
	atomic_set(&scheduler->pending_jobs, 0);
	atomic_set(&scheduler->active_io, 0);
	scheduler->max_pending_jobs = ZRAM_PP_DEFAULT_PENDING;

	scheduler->job_pool = mempool_create_kmalloc_pool(
		ZRAM_PP_DEFAULT_PENDING, sizeof(struct zram_pp_job));
	if (!scheduler->job_pool)
		goto fail;
	scheduler->fault_wq = alloc_workqueue("zram_pp_fault",
		WQ_UNBOUND | WQ_HIGHPRI | WQ_MEM_RECLAIM,
		ZRAM_PP_FAULT_CONCURRENCY);
	if (!scheduler->fault_wq)
		goto fail;
	scheduler->write_wq = alloc_workqueue("zram_pp_write",
		WQ_UNBOUND | WQ_MEM_RECLAIM, ZRAM_PP_WRITE_CONCURRENCY);
	if (!scheduler->write_wq)
		goto fail;
	scheduler->cpu_wq = alloc_workqueue("zram_pp_cpu",
		WQ_UNBOUND | WQ_MEM_RECLAIM,
		ZRAM_PP_CPU_CONCURRENCY);
	if (!scheduler->cpu_wq)
		goto fail;
	return 0;

fail:
	if (scheduler->cpu_wq)
		destroy_workqueue(scheduler->cpu_wq);
	if (scheduler->write_wq)
		destroy_workqueue(scheduler->write_wq);
	if (scheduler->fault_wq)
		destroy_workqueue(scheduler->fault_wq);
	mempool_destroy(scheduler->job_pool);
	xa_destroy(&scheduler->slot_jobs);
	return -ENOMEM;
}

void zram_pp_scheduler_quiesce(struct zram_pp_scheduler *scheduler)
{
	struct zram_pp_operation *operation;
	unsigned long flags;

	spin_lock_irqsave(&scheduler->lock, flags);
	scheduler->stopping = true;
	list_for_each_entry(operation, &scheduler->operations, entry)
		set_bit(ZRAM_PP_CANCELLED, &operation->state);
	spin_unlock_irqrestore(&scheduler->lock, flags);
	flush_workqueue(scheduler->cpu_wq);
	flush_workqueue(scheduler->write_wq);
	flush_workqueue(scheduler->fault_wq);
	wait_event(scheduler->wait,
		   !atomic_read(&scheduler->active_operations) &&
		   !atomic_read(&scheduler->active_producers) &&
		   !atomic_read(&scheduler->pending_jobs) &&
		   !atomic_read(&scheduler->active_io));
	WARN_ON_ONCE(!xa_empty(&scheduler->slot_jobs));
}

void zram_pp_scheduler_resume(struct zram_pp_scheduler *scheduler)
{
	unsigned long flags;

	spin_lock_irqsave(&scheduler->lock, flags);
	WARN_ON_ONCE(atomic_read(&scheduler->active_operations));
	WARN_ON_ONCE(atomic_read(&scheduler->active_producers));
	WARN_ON_ONCE(atomic_read(&scheduler->pending_jobs));
	WARN_ON_ONCE(atomic_read(&scheduler->active_io));
	scheduler->stopping = false;
	spin_unlock_irqrestore(&scheduler->lock, flags);
}

void zram_pp_scheduler_fini(struct zram_pp_scheduler *scheduler)
{
	zram_pp_scheduler_quiesce(scheduler);
	destroy_workqueue(scheduler->cpu_wq);
	destroy_workqueue(scheduler->write_wq);
	destroy_workqueue(scheduler->fault_wq);
	mempool_destroy(scheduler->job_pool);
	xa_destroy(&scheduler->slot_jobs);
}

struct zram_pp_operation *
zram_pp_operation_begin(struct zram_pp_scheduler *scheduler,
			enum zram_pp_job_type type,
			enum zram_pp_priority priority,
			u64 max_pages, u64 max_bytes)
{
	struct zram_pp_operation *operation;
	unsigned long flags;

	if ((unsigned int)type >= ZRAM_PP_JOB_MAX ||
	    (unsigned int)priority >= ZRAM_PP_PRIO_MAX)
		return ERR_PTR(-EINVAL);
	operation = kzalloc(sizeof(*operation), GFP_KERNEL);
	if (!operation)
		return ERR_PTR(-ENOMEM);

	operation->scheduler = scheduler;
	operation->type = type;
	operation->priority = priority;
	operation->budget.logical_limit = max_pages;
	operation->budget.resident_limit = max_bytes;
	operation->budget.candidate_limit = scheduler->max_pending_jobs;
	spin_lock_init(&operation->budget.lock);
	INIT_LIST_HEAD(&operation->entry);
	refcount_set(&operation->refs, 1);
	atomic_set(&operation->active_jobs, 0);

	spin_lock_irqsave(&scheduler->lock, flags);
	if (scheduler->stopping || test_bit(type, &scheduler->active_types)) {
		spin_unlock_irqrestore(&scheduler->lock, flags);
		kfree(operation);
		return ERR_PTR(-EBUSY);
	}
	set_bit(type, &scheduler->active_types);
	operation->id = (u64)atomic64_inc_return(&scheduler->next_id);
	if (unlikely(!operation->id))
		operation->id = (u64)atomic64_inc_return(&scheduler->next_id);
	list_add_tail(&operation->entry, &scheduler->operations);
	atomic_inc(&scheduler->active_operations);
	spin_unlock_irqrestore(&scheduler->lock, flags);
	return operation;
}

void zram_pp_operation_cancel(struct zram_pp_operation *operation)
{
	if (operation)
		set_bit(ZRAM_PP_CANCELLED, &operation->state);
}

bool zram_pp_operation_cancelled(const struct zram_pp_operation *operation)
{
	return !operation || test_bit(ZRAM_PP_CANCELLED, &operation->state);
}

void zram_pp_operation_end(struct zram_pp_operation *operation)
{
	struct zram_pp_scheduler *scheduler;
	unsigned long flags;
	bool completed;

	if (!operation)
		return;
	scheduler = operation->scheduler;
	spin_lock_irqsave(&scheduler->lock, flags);
	if (WARN_ON_ONCE(test_bit(ZRAM_PP_OPERATION_ENDED,
				 &operation->state))) {
		spin_unlock_irqrestore(&scheduler->lock, flags);
		return;
	}
	set_bit(ZRAM_PP_OPERATION_ENDED, &operation->state);
	completed = zram_pp_operation_complete_locked(operation);
	spin_unlock_irqrestore(&scheduler->lock, flags);
	if (completed)
		wake_up_all(&scheduler->wait);
	zram_pp_operation_put(operation);
}

struct zram_pp_job *zram_pp_job_claim_locked(struct zram *zram,
					      struct zram_pp_operation *operation,
					      u32 index, gfp_t gfp)
{
	struct zram_pp_scheduler *scheduler = operation->scheduler;
	struct zram_pp_job *job;
	void *current_job;

	if (zram_pp_operation_cancelled(operation) ||
	    test_bit(ZRAM_PP_OPERATION_ENDED, &operation->state))
		return ERR_PTR(-ECANCELED);
	job = zram_pp_job_alloc(scheduler, gfp);
	if (!job)
		return ERR_PTR(-EAGAIN);
	job->index = index;
	job->type = operation->type;
	job->priority = operation->priority;
	job->generation = zram_rep_mutation_seq_locked(zram, index);
	if (!zram_pp_operation_add_job(operation)) {
		zram_pp_job_drop(job);
		return ERR_PTR(-ECANCELED);
	}
	job->operation = operation;

	for (;;) {
		current_job = xa_cmpxchg(&scheduler->slot_jobs, index, NULL,
					 job, gfp);
		if (xa_is_err(current_job)) {
			zram_pp_job_drop(job);
			return ERR_PTR(xa_err(current_job));
		}
		if (!current_job)
			break;
		if (!test_bit(ZRAM_PP_CANCELLED,
			      &((struct zram_pp_job *)current_job)->state) &&
		    ((struct zram_pp_job *)current_job)->priority >=
				job->priority) {
			zram_pp_job_drop(job);
			return ERR_PTR(-EBUSY);
		}
		if (xa_cmpxchg(&scheduler->slot_jobs, index, current_job, job,
			       gfp) == current_job) {
			set_bit(ZRAM_PP_CANCELLED,
				&((struct zram_pp_job *)current_job)->state);
			break;
		}
	}
	return job;
}

int zram_pp_submit_latest(struct zram *zram, enum zram_pp_job_type type,
			  enum zram_pp_priority priority, u32 index,
			  u64 generation, zram_pp_job_fn run)
{
	struct zram_pp_scheduler *scheduler = &zram->pp_scheduler;
	struct workqueue_struct *wq;
	struct zram_pp_job *job;
	void *mapped_job;
	int ret = 0;

	if ((unsigned int)type >= ZRAM_PP_JOB_MAX || !run)
		return -EINVAL;
	if (!zram_pp_producer_get(scheduler))
		return -ESHUTDOWN;
	zram_slot_lock(zram, index);
	if (generation != zram_rep_mutation_seq_locked(zram, index)) {
		ret = -ESTALE;
		goto out_unlock;
	}
	mapped_job = xa_load(&scheduler->slot_jobs, index);
	if (mapped_job &&
	    !test_bit(ZRAM_PP_CANCELLED,
		      &((struct zram_pp_job *)mapped_job)->state) &&
	    ((struct zram_pp_job *)mapped_job)->type == type &&
	    test_bit(ZRAM_PP_ASYNC,
		     &((struct zram_pp_job *)mapped_job)->state)) {
		WRITE_ONCE(((struct zram_pp_job *)mapped_job)->generation,
			   generation);
		goto out_unlock;
	}
	if (mapped_job) {
		ret = -EBUSY;
		goto out_unlock;
	}

	job = zram_pp_job_alloc(scheduler, GFP_NOWAIT | __GFP_NOWARN);
	if (!job) {
		ret = READ_ONCE(scheduler->stopping) ? -ESHUTDOWN : -EAGAIN;
		goto out_unlock;
	}
	INIT_WORK(&job->work, zram_pp_async_work);
	job->run = run;
	job->index = index;
	job->type = type;
	job->priority = priority;
	job->generation = generation;
	set_bit(ZRAM_PP_ASYNC, &job->state);
	mapped_job = xa_cmpxchg(&scheduler->slot_jobs, index, NULL,
				job, GFP_NOWAIT);
	if (xa_is_err(mapped_job) || mapped_job) {
		zram_pp_job_drop(job);
		ret = xa_is_err(mapped_job) ? xa_err(mapped_job) : -EBUSY;
		goto out_unlock;
	}
	zram_slot_unlock(zram, index);

	if (type == ZRAM_PP_PACKED_READ)
		wq = scheduler->fault_wq;
	else if (type == ZRAM_PP_WRITEBACK)
		wq = scheduler->write_wq;
	else
		wq = scheduler->cpu_wq;
	if (WARN_ON_ONCE(!queue_work(wq, &job->work))) {
		zram_slot_lock(zram, index);
		set_bit(ZRAM_PP_CANCELLED, &job->state);
		set_bit(ZRAM_PP_DONE, &job->state);
		mapped_job = xa_cmpxchg(&scheduler->slot_jobs, index, job, NULL, 0);
		zram_slot_unlock(zram, index);
		WARN_ON_ONCE(xa_is_err(mapped_job));
		zram_pp_job_drop(job);
		ret = -EIO;
	}
	zram_pp_producer_put(scheduler);
	return ret;

out_unlock:
	zram_slot_unlock(zram, index);
	zram_pp_producer_put(scheduler);
	return ret;
}

bool zram_pp_job_is_current_locked(struct zram *zram,
				   const struct zram_pp_job *job)
{
	return !test_bit(ZRAM_PP_CANCELLED, &job->state) &&
	       (!job->operation ||
		!zram_pp_operation_cancelled(job->operation)) &&
	       xa_load(&job->scheduler->slot_jobs, job->index) == job &&
	       zram_rep_mutation_seq_locked(zram, job->index) ==
			READ_ONCE(job->generation);
}

static bool zram_pp_budget_add(u64 *used, u64 limit, u64 amount)
{
	u64 next;

	if (check_add_overflow(*used, amount, &next) ||
	    (limit && next > limit))
		return false;
	*used = next;
	return true;
}

bool zram_pp_job_charge(struct zram_pp_job *job, u64 bytes)
{
	struct zram_pp_budget *budget;
	unsigned long flags;
	bool logical;
	bool charged = false;

	if (test_bit(ZRAM_PP_CHARGED, &job->state))
		return true;
	if (!job->operation)
		return true;
	budget = &job->operation->budget;
	spin_lock_irqsave(&budget->lock, flags);
	logical = zram_pp_budget_add(&budget->logical_used,
				     budget->logical_limit, 1);
	if (logical)
		charged = zram_pp_budget_add(&budget->resident_used,
					     budget->resident_limit, bytes);
	if (charged) {
		job->charged_bytes = bytes;
		set_bit(ZRAM_PP_CHARGED, &job->state);
	} else if (logical)
		budget->logical_used--;
	spin_unlock_irqrestore(&budget->lock, flags);
	return charged;
}

bool zram_pp_operation_charge_scan(struct zram_pp_operation *operation,
				   u64 slots)
{
	struct zram_pp_budget *budget = &operation->budget;
	unsigned long flags;
	bool charged;

	spin_lock_irqsave(&budget->lock, flags);
	charged = zram_pp_budget_add(&budget->scan_used,
				     budget->scan_limit, slots);
	spin_unlock_irqrestore(&budget->lock, flags);
	return charged;
}

bool zram_pp_operation_charge_candidate(struct zram_pp_operation *operation)
{
	struct zram_pp_budget *budget = &operation->budget;
	unsigned long flags;
	bool charged;

	spin_lock_irqsave(&budget->lock, flags);
	charged = zram_pp_budget_add(&budget->candidates_used,
				     budget->candidate_limit, 1);
	spin_unlock_irqrestore(&budget->lock, flags);
	return charged;
}

bool zram_pp_budget_reserve_physical(struct zram_pp_budget *budget,
				     u64 blocks)
{
	unsigned long flags;
	bool charged;

	spin_lock_irqsave(&budget->lock, flags);
	charged = zram_pp_budget_add(&budget->physical_reserved, 0, blocks);
	spin_unlock_irqrestore(&budget->lock, flags);
	return charged;
}

void zram_pp_budget_commit_physical(struct zram_pp_budget *budget,
				    u64 blocks, bool gc)
{
	unsigned long flags;
	u64 next;

	spin_lock_irqsave(&budget->lock, flags);
	WARN_ON_ONCE(budget->physical_reserved < blocks);
	budget->physical_reserved -= min(budget->physical_reserved, blocks);
	if (WARN_ON_ONCE(check_add_overflow(budget->physical_committed,
					   blocks, &next)))
		budget->physical_committed = U64_MAX;
	else
		budget->physical_committed = next;
	if (gc) {
		if (WARN_ON_ONCE(check_add_overflow(
				budget->gc_physical_writes, blocks, &next)))
			budget->gc_physical_writes = U64_MAX;
		else
			budget->gc_physical_writes = next;
	}
	spin_unlock_irqrestore(&budget->lock, flags);
}

void zram_pp_budget_rollback_physical(struct zram_pp_budget *budget,
				      u64 blocks)
{
	unsigned long flags;
	u64 next;

	spin_lock_irqsave(&budget->lock, flags);
	WARN_ON_ONCE(budget->physical_reserved < blocks);
	budget->physical_reserved -= min(budget->physical_reserved, blocks);
	if (WARN_ON_ONCE(check_add_overflow(budget->physical_rollback,
					   blocks, &next)))
		budget->physical_rollback = U64_MAX;
	else
		budget->physical_rollback = next;
	spin_unlock_irqrestore(&budget->lock, flags);
}

void zram_pp_job_finish_locked(struct zram *zram, struct zram_pp_job *job)
{
	void *mapped_job;

	if (!job || test_and_set_bit(ZRAM_PP_DONE, &job->state))
		return;
	mapped_job = xa_cmpxchg(&job->scheduler->slot_jobs, job->index,
				job, NULL, 0);
	WARN_ON_ONCE(xa_is_err(mapped_job));
	zram_pp_job_drop(job);
}

void zram_pp_cancel_slot_locked(struct zram *zram, u32 index)
{
	struct zram_pp_job *job;

	job = xa_erase(&zram->pp_scheduler.slot_jobs, index);
	if (job)
		set_bit(ZRAM_PP_CANCELLED, &job->state);
}

bool zram_pp_slot_active_locked(struct zram *zram, u32 index)
{
	struct zram_pp_job *job;

	job = xa_load(&zram->pp_scheduler.slot_jobs, index);
	return job && !test_bit(ZRAM_PP_CANCELLED, &job->state);
}

bool zram_pp_io_get(struct zram_pp_scheduler *scheduler)
{
	unsigned long flags;
	bool accepted = false;

	spin_lock_irqsave(&scheduler->lock, flags);
	if (!scheduler->stopping) {
		atomic_inc(&scheduler->active_io);
		accepted = true;
	}
	spin_unlock_irqrestore(&scheduler->lock, flags);
	return accepted;
}

void zram_pp_io_put(struct zram_pp_scheduler *scheduler)
{
	WARN_ON_ONCE(atomic_dec_return(&scheduler->active_io) < 0);
	wake_up_all(&scheduler->wait);
}
