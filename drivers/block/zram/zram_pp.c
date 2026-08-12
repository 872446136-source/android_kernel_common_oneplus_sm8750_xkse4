// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/err.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#include "zram_drv.h"

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

void zram_pp_scheduler_init(struct zram_pp_scheduler *scheduler)
{
	xa_init(&scheduler->slot_jobs);
	spin_lock_init(&scheduler->lock);
	INIT_LIST_HEAD(&scheduler->operations);
	init_waitqueue_head(&scheduler->wait);
	atomic64_set(&scheduler->next_id, 0);
	atomic_set(&scheduler->active_operations, 0);
	scheduler->active_types = 0;
	scheduler->stopping = false;
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

	wait_event(scheduler->wait,
		   !atomic_read(&scheduler->active_operations));
}

void zram_pp_scheduler_resume(struct zram_pp_scheduler *scheduler)
{
	unsigned long flags;

	spin_lock_irqsave(&scheduler->lock, flags);
	WARN_ON_ONCE(atomic_read(&scheduler->active_operations));
	scheduler->stopping = false;
	spin_unlock_irqrestore(&scheduler->lock, flags);
}

void zram_pp_scheduler_fini(struct zram_pp_scheduler *scheduler)
{
	zram_pp_scheduler_quiesce(scheduler);
	WARN_ON_ONCE(!xa_empty(&scheduler->slot_jobs));
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
	operation->budget.max_pages = max_pages;
	operation->budget.max_bytes = max_bytes;
	spin_lock_init(&operation->budget.lock);
	INIT_LIST_HEAD(&operation->entry);
	refcount_set(&operation->refs, 1);
	atomic_set(&operation->active_jobs, 0);

	spin_lock_irqsave(&scheduler->lock, flags);
	if (scheduler->stopping ||
	    test_bit(type, &scheduler->active_types)) {
		spin_unlock_irqrestore(&scheduler->lock, flags);
		kfree(operation);
		return ERR_PTR(-EBUSY);
	}
	set_bit(type, &scheduler->active_types);
	operation->id = (u64)atomic64_inc_return(&scheduler->next_id);
	if (unlikely(!operation->id))
		operation->id =
			(u64)atomic64_inc_return(&scheduler->next_id);
	WARN_ON_ONCE(!operation->id);
	list_add_tail(&operation->entry, &scheduler->operations);
	atomic_inc(&scheduler->active_operations);
	spin_unlock_irqrestore(&scheduler->lock, flags);

	return operation;
}

void zram_pp_operation_cancel(struct zram_pp_operation *operation)
{
	set_bit(ZRAM_PP_CANCELLED, &operation->state);
}

bool zram_pp_operation_cancelled(const struct zram_pp_operation *operation)
{
	return test_bit(ZRAM_PP_CANCELLED, &operation->state);
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

static void zram_pp_job_drop(struct zram_pp_job *job)
{
	struct zram_pp_operation *operation = job->operation;
	int active_jobs;

	active_jobs = atomic_dec_return(&operation->active_jobs);
	if (WARN_ON_ONCE(active_jobs < 0))
		active_jobs = 0;
	if (!active_jobs)
		zram_pp_operation_maybe_complete(operation);
	zram_pp_operation_put(operation);
	kfree(job);
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
	job = kzalloc(sizeof(*job), gfp);
	if (!job)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&job->entry);
	job->operation = operation;
	job->index = index;
	job->generation = zram_rep_mutation_seq_locked(zram, index);
	if (!zram_pp_operation_add_job(operation)) {
		kfree(job);
		return ERR_PTR(-ECANCELED);
	}

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
		    !zram_pp_operation_cancelled(
			    ((struct zram_pp_job *)current_job)->operation) &&
		    ((struct zram_pp_job *)current_job)->operation->priority >=
			    operation->priority) {
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

	if (unlikely(zram_pp_operation_cancelled(operation))) {
		zram_pp_cancel_slot_locked(zram, index);
		zram_pp_job_drop(job);
		return ERR_PTR(-ECANCELED);
	}
	return job;
}

bool zram_pp_job_is_current_locked(struct zram *zram,
					   const struct zram_pp_job *job)
{
	struct zram_pp_scheduler *scheduler = job->operation->scheduler;

	return !test_bit(ZRAM_PP_CANCELLED, &job->state) &&
	       !zram_pp_operation_cancelled(job->operation) &&
	       xa_load(&scheduler->slot_jobs, job->index) == job &&
	       zram_rep_mutation_seq_locked(zram, job->index) ==
			job->generation;
}

bool zram_pp_job_charge(struct zram_pp_job *job, u64 bytes)
{
	struct zram_pp_budget *budget = &job->operation->budget;
	unsigned long flags;
	u64 next_pages;
	u64 next_bytes;
	bool charged = false;

	if (test_bit(ZRAM_PP_CHARGED, &job->state))
		return true;
	spin_lock_irqsave(&budget->lock, flags);
	if (test_bit(ZRAM_PP_CHARGED, &job->state)) {
		charged = true;
		goto out;
	}
	if (check_add_overflow(budget->used_pages, 1ULL, &next_pages))
		goto out;
	if (budget->max_pages && next_pages > budget->max_pages)
		goto out;
	if (check_add_overflow(budget->used_bytes, bytes, &next_bytes))
		goto out;
	if (budget->max_bytes && next_bytes > budget->max_bytes)
		goto out;

	budget->used_pages = next_pages;
	budget->used_bytes = next_bytes;
	job->charged_bytes = bytes;
	set_bit(ZRAM_PP_CHARGED, &job->state);
	charged = true;
out:
	spin_unlock_irqrestore(&budget->lock, flags);
	return charged;
}

void zram_pp_job_finish_locked(struct zram *zram,
				       struct zram_pp_job *job)
{
	struct zram_pp_scheduler *scheduler;
	void *current_job;

	if (!job || test_and_set_bit(ZRAM_PP_DONE, &job->state))
		return;
	scheduler = job->operation->scheduler;
	current_job = xa_cmpxchg(&scheduler->slot_jobs, job->index, job, NULL,
				 0);
	WARN_ON_ONCE(xa_is_err(current_job));
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
