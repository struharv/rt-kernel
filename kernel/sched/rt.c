// SPDX-License-Identifier: GPL-2.0
/*
 * Real-Time Scheduling Class (mapped to the SCHED_FIFO and SCHED_RR
 * policies)
 */
#include "sched.h"

#include "pelt.h"

int sched_rr_timeslice = RR_TIMESLICE;
int sysctl_sched_rr_timeslice = (MSEC_PER_SEC / HZ) * RR_TIMESLICE;

void init_rt_rq(struct rt_rq *rt_rq)
{
	struct rt_prio_array *array;
	int i;

	array = &rt_rq->active;
	for (i = 0; i < MAX_RT_PRIO; i++) {
		INIT_LIST_HEAD(array->queue + i);
		__clear_bit(i, array->bitmap);
	}
	/* delimiter for bitsearch: */
	__set_bit(MAX_RT_PRIO, array->bitmap);

#if defined CONFIG_SMP
	rt_rq->highest_prio.curr = MAX_RT_PRIO;
	rt_rq->highest_prio.next = MAX_RT_PRIO;
	rt_rq->rt_nr_migratory = 0;
	rt_rq->overloaded = 0;
	plist_head_init(&rt_rq->pushable_tasks);
#endif /* CONFIG_SMP */
}

#ifdef CONFIG_RT_GROUP_SCHED

static inline struct rq *rq_of_rt_rq(struct rt_rq *rt_rq)
{
	return rt_rq->rq;
}

void free_rt_sched_group(struct task_group *tg)
{
	unsigned long flags;
	int i;

	for_each_possible_cpu(i) {
		if (tg->dl_se) {
			dl_init_tg(tg->dl_se[i], 0, tg->dl_se[i]->dl_period);
			raw_spin_lock_irqsave(&cpu_rq(i)->lock, flags);
			BUG_ON(tg->rt_rq[i]->rt_nr_running);
			raw_spin_unlock_irqrestore(&cpu_rq(i)->lock, flags);

			hrtimer_cancel(&tg->dl_se[i]->dl_timer);
			kfree(tg->dl_se[i]);
		}
		if (tg->rt_rq)
			kfree(tg->rt_rq[i]);
	}

	kfree(tg->rt_rq);
	kfree(tg->dl_se);
}

void init_tg_rt_entry(struct task_group *tg, struct rt_rq *rt_rq,
		struct sched_dl_entity *dl_se, int cpu,
		struct sched_dl_entity *parent)
{
	struct rq *rq = cpu_rq(cpu);

	rt_rq->highest_prio.curr = MAX_RT_PRIO;
	rt_rq->rq = rq;
	rt_rq->tg = tg;

	tg->rt_rq[cpu] = rt_rq;
	tg->dl_se[cpu] = dl_se;

	if (!dl_se)
		return;

	dl_se->dl_rq = &rq->dl;
	dl_se->my_q = rt_rq;
	RB_CLEAR_NODE(&dl_se->rb_node);
}

int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent)
{
	struct rt_rq *rt_rq;
	struct sched_dl_entity *dl_se;
	int i;

	tg->rt_rq = kcalloc(nr_cpu_ids, sizeof(rt_rq), GFP_KERNEL);
	if (!tg->rt_rq)
		goto err;
	tg->dl_se = kcalloc(nr_cpu_ids, sizeof(dl_se), GFP_KERNEL);
	if (!tg->dl_se)
		goto err;

	init_dl_bandwidth(&tg->dl_bandwidth,
			def_dl_bandwidth.dl_period, 0);

	for_each_possible_cpu(i) {
		rt_rq = kzalloc_node(sizeof(struct rt_rq),
				     GFP_KERNEL, cpu_to_node(i));
		if (!rt_rq)
			goto err;

		dl_se = kzalloc_node(sizeof(struct sched_dl_entity),
				     GFP_KERNEL, cpu_to_node(i));
		if (!dl_se)
			goto err_free_rq;

		init_rt_rq(rt_rq);
		rt_rq->rq = cpu_rq(i);

		init_dl_task_timer(dl_se);
		init_dl_inactive_task_timer(dl_se);

		dl_se->dl_runtime = tg->dl_bandwidth.dl_runtime;
		dl_se->dl_period = tg->dl_bandwidth.dl_period;
		dl_se->dl_deadline = dl_se->dl_period;
		dl_se->dl_bw = to_ratio(dl_se->dl_period, dl_se->dl_runtime);

		dl_se->dl_throttled = 0;

		init_tg_rt_entry(tg, rt_rq, dl_se, i, parent->dl_se[i]);
	}

	return 1;

err_free_rq:
	kfree(rt_rq);
err:
	return 0;
}

#else /* CONFIG_RT_GROUP_SCHED */

static inline struct rq *rq_of_rt_rq(struct rt_rq *rt_rq)
{
	return container_of(rt_rq, struct rq, rt);
}

void free_rt_sched_group(struct task_group *tg) { }

int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent)
{
	return 1;
}
#endif /* CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_SMP

static void pull_rt_task(struct rq *this_rq);

static inline bool need_pull_rt_task(struct rq *rq, struct task_struct *prev)
{
	/* Try to pull RT tasks here if we lower this rq's prio */
	return rq->rt.highest_prio.curr > prev->prio;
}

static inline int rt_overloaded(struct rq *rq)
{
	return atomic_read(&rq->rd->rto_count);
}

static inline void rt_set_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	cpumask_set_cpu(rq->cpu, rq->rd->rto_mask);
	/*
	 * Make sure the mask is visible before we set
	 * the overload count. That is checked to determine
	 * if we should look at the mask. It would be a shame
	 * if we looked at the mask, but the mask was not
	 * updated yet.
	 *
	 * Matched by the barrier in pull_rt_task().
	 */
	smp_wmb();
	atomic_inc(&rq->rd->rto_count);
}

static inline void rt_clear_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	/* the order here really doesn't matter */
	atomic_dec(&rq->rd->rto_count);
	cpumask_clear_cpu(rq->cpu, rq->rd->rto_mask);
}

static void update_rt_migration(struct rt_rq *rt_rq)
{
	if (rt_rq->rt_nr_migratory && rt_rq->rt_nr_running > 1) {
		if (!rt_rq->overloaded) {
			rt_set_overload(rq_of_rt_rq(rt_rq));
			rt_rq->overloaded = 1;
		}
	} else if (rt_rq->overloaded) {
		rt_clear_overload(rq_of_rt_rq(rt_rq));
		rt_rq->overloaded = 0;
	}
}

static void inc_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	struct task_struct *p;

	p = rt_task_of(rt_se);
	if (p->nr_cpus_allowed > 1)
		rt_rq->rt_nr_migratory++;

	update_rt_migration(rt_rq);
}

static void dec_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	struct task_struct *p;

	p = rt_task_of(rt_se);
	if (p->nr_cpus_allowed > 1)
		rt_rq->rt_nr_migratory--;

	update_rt_migration(rt_rq);
}

static inline int has_pushable_tasks(struct rt_rq *rt_rq)
{
	return !plist_head_empty(&rt_rq->pushable_tasks);
}

static DEFINE_PER_CPU(struct callback_head, rt_push_head);
static DEFINE_PER_CPU(struct callback_head, rt_pull_head);

static void push_rt_tasks(struct rq *);
static void pull_rt_task(struct rq *);

static inline void rt_queue_push_tasks(struct rq *rq)
{
	if (!has_pushable_tasks(&rq->rt))
		return;

	queue_balance_callback(rq, &per_cpu(rt_push_head, rq->cpu), push_rt_tasks);
}

static inline void rt_queue_pull_task(struct rq *rq)
{
	queue_balance_callback(rq, &per_cpu(rt_pull_head, rq->cpu), pull_rt_task);
}

static void enqueue_pushable_task(struct rt_rq *rt_rq, struct task_struct *p)
{
	plist_del(&p->pushable_tasks, &rt_rq->pushable_tasks);
	plist_node_init(&p->pushable_tasks, p->prio);
	plist_add(&p->pushable_tasks, &rt_rq->pushable_tasks);

	/* Update the highest prio pushable task */
	if (p->prio < rt_rq->highest_prio.next)
		rt_rq->highest_prio.next = p->prio;
}

#ifdef CONFIG_RT_GROUP_SCHED
void dequeue_pushable_task(struct rt_rq *rt_rq, struct task_struct *p)
{
	plist_del(&p->pushable_tasks, &rt_rq->pushable_tasks);

	/* Update the new highest prio pushable task */
	if (has_pushable_tasks(rt_rq)) {
		p = plist_first_entry(&rt_rq->pushable_tasks,
				      struct task_struct, pushable_tasks);
		rt_rq->highest_prio.next = p->prio;
	} else
		rt_rq->highest_prio.next = MAX_RT_PRIO;
}
#endif
#else

static inline
void enqueue_pushable_task(struct rt_rq *rt_rq, struct task_struct *p)
{
}

static inline
void inc_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
}

static inline
void dec_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
}

static inline bool need_pull_rt_task(struct rq *rq, struct task_struct *prev)
{
	return false;
}

static inline void pull_rt_task(struct rq *this_rq)
{
}

static inline void rt_queue_push_tasks(struct rq *rq)
{
}

static inline void rt_queue_pull_task(struct rq *rq)
{
}
#endif /* CONFIG_SMP */

static inline int on_rt_rq(struct sched_rt_entity *rt_se)
{
	return rt_se->on_rq;
}

static inline int rt_se_prio(struct sched_rt_entity *rt_se)
{
	return rt_task_of(rt_se)->prio;
}

/*
 * Update the current task's runtime statistics. Skip current tasks that
 * are not in our scheduling class.
 */
static void update_curr_rt(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct rt_rq *rt_rq = rt_rq_of_se(&curr->rt);
	u64 delta_exec;
	u64 now;

	if (curr->sched_class != &rt_sched_class)
		return;

	now = rq_clock_task(rq);
	delta_exec = now - curr->se.exec_start;
	if (unlikely((s64)delta_exec <= 0))
		return;

	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = now;
	cgroup_account_cputime(curr, delta_exec);

	if (!dl_bandwidth_enabled())
		return;
	
	
	if (is_dl_group(rt_rq)) {
		

		struct sched_dl_entity *dl_se = dl_group_of(rt_rq);
		if (dl_se->struhar) {

		}

		if (dl_se->dl_throttled) {
			//trace_printk("STRUHAR: UPDATE_CURR_RT dl_se->dl_throttled\n");

			resched_curr(rq);
			return;
		}

		BUG_ON(rt_rq->rt_nr_running > rq->nr_running);
		dl_se->runtime -= delta_exec;
		trace_printk("STRUHAR:%d: UPDATE_CURR_RT %lld struhar=%d \n", curr->pid, dl_se->runtime, dl_se->struhar);
		trace_printk("XDEBUG:%d:UPDATE_CURR_RT:runtime=%lld\n", curr->pid, dl_se->runtime);
		
		/* A group exhausts the budget. */
		if (dl_runtime_exceeded(dl_se) || dl_se->struhar == 1) {
			trace_printk("STRUHAR:%d: UPDATE_CURR_RT dl_runtime_exceeded\n", curr->pid);
			trace_printk("update_curr_rt: group exhousted budget\n");


			dequeue_dl_entity(dl_se);

			if (likely(start_dl_timer(dl_se))) {
				dl_se->dl_throttled = 1;
				trace_printk("XDEBUG:%d:THROTTLE\n", curr->pid);

				trace_printk("STRUHAR:%d: THROTTLE\n", curr->pid);
			}
			else {
				trace_printk("XDEBUG:%d:THROTTLE1\n", curr->pid);
				enqueue_dl_entity(dl_se, dl_se,
						  ENQUEUE_REPLENISH);
			}

			resched_curr(rq);
		}
	}
}

#if defined CONFIG_SMP

static void
inc_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	if (is_dl_group(rt_rq))
		return;

	if (rq->online && prio < prev_prio)
		cpupri_set(&rq->rd->cpupri, rq->cpu, prio);
}

static void
dec_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	if (is_dl_group(rt_rq))
		return;

	if (rq->online && rt_rq->highest_prio.curr != prev_prio)
		cpupri_set(&rq->rd->cpupri, rq->cpu, rt_rq->highest_prio.curr);
}

#else /* CONFIG_SMP */

static inline
void inc_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio) {}
static inline
void dec_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio) {}

#endif /* CONFIG_SMP */

#if defined(CONFIG_SMP)
static void
inc_rt_prio(struct rt_rq *rt_rq, int prio)
{
	int prev_prio = rt_rq->highest_prio.curr;

	if (is_dl_group(rt_rq))
		return;

	if (prio < prev_prio)
		rt_rq->highest_prio.curr = prio;

	inc_rt_prio_smp(rt_rq, prio, prev_prio);
}

static void
dec_rt_prio(struct rt_rq *rt_rq, int prio)
{
	int prev_prio = rt_rq->highest_prio.curr;

	if (is_dl_group(rt_rq))
		return;

	if (rt_rq->rt_nr_running) {

		WARN_ON(prio < prev_prio);

		/*
		 * This may have been our highest task, and therefore
		 * we may have some recomputation to do
		 */
		if (prio == prev_prio) {
			struct rt_prio_array *array = &rt_rq->active;

			rt_rq->highest_prio.curr =
				sched_find_first_bit(array->bitmap);
		}

	} else
		rt_rq->highest_prio.curr = MAX_RT_PRIO;

	dec_rt_prio_smp(rt_rq, prio, prev_prio);
}

#else

static inline void inc_rt_prio(struct rt_rq *rt_rq, int prio) {}
static inline void dec_rt_prio(struct rt_rq *rt_rq, int prio) {}

#endif /* CONFIG_SMP && !CONFIG_RT_GROUP_SCHED */

static inline
unsigned int rt_se_nr_running(struct sched_rt_entity *rt_se)
{
	return 1;
}

static inline
unsigned int rt_se_rr_nr_running(struct sched_rt_entity *rt_se)
{
	struct task_struct *tsk;

	tsk = rt_task_of(rt_se);

	return (tsk->policy == SCHED_RR) ? 1 : 0;
}

static inline
void inc_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	int prio = rt_se_prio(rt_se);

	WARN_ON(!rt_prio(prio));
	rt_rq->rt_nr_running += rt_se_nr_running(rt_se);
	rt_rq->rr_nr_running += rt_se_rr_nr_running(rt_se);

	inc_rt_prio(rt_rq, prio);

	if (is_dl_group(rt_rq)) {
		struct sched_dl_entity *dl_se = dl_group_of(rt_rq);

		if (!dl_se->dl_throttled)
			add_nr_running(rq_of_rt_rq(rt_rq), 1);
	} else {
		add_nr_running(rq_of_rt_rq(rt_rq), 1);
	}

	inc_rt_migration(rt_se, rt_rq);
}

static inline
void dec_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	WARN_ON(!rt_prio(rt_se_prio(rt_se)));
	rt_rq->rt_nr_running -= rt_se_nr_running(rt_se);
	rt_rq->rr_nr_running -= rt_se_rr_nr_running(rt_se);

	dec_rt_prio(rt_rq, rt_se_prio(rt_se));
	if (is_dl_group(rt_rq)) {
		struct sched_dl_entity *dl_se = dl_group_of(rt_rq);

		if (!dl_se->dl_throttled)
			sub_nr_running(rq_of_rt_rq(rt_rq), 1);
	} else {
		sub_nr_running(rq_of_rt_rq(rt_rq), 1);
	}
	dec_rt_migration(rt_se, rt_rq);
}

/*
 * Change rt_se->run_list location unless SAVE && !MOVE
 *
 * assumes ENQUEUE/DEQUEUE flags match
 */
static inline bool move_entity(unsigned int flags)
{
	if ((flags & (DEQUEUE_SAVE | DEQUEUE_MOVE)) == DEQUEUE_SAVE)
		return false;

	return true;
}

static void __delist_rt_entity(struct sched_rt_entity *rt_se, struct rt_prio_array *array)
{
	list_del_init(&rt_se->run_list);

	if (list_empty(array->queue + rt_se_prio(rt_se)))
		__clear_bit(rt_se_prio(rt_se), array->bitmap);

	rt_se->on_list = 0;
}

static void enqueue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;
	struct list_head *queue = array->queue + rt_se_prio(rt_se);

	if (move_entity(flags)) {
		WARN_ON_ONCE(rt_se->on_list);
		if (flags & ENQUEUE_HEAD)
			list_add(&rt_se->run_list, queue);
		else
			list_add_tail(&rt_se->run_list, queue);

		__set_bit(rt_se_prio(rt_se), array->bitmap);
		rt_se->on_list = 1;
	}
	rt_se->on_rq = 1;

	inc_rt_tasks(rt_se, rt_rq);
}

static void dequeue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;

	if (move_entity(flags)) {
		WARN_ON_ONCE(!rt_se->on_list);
		__delist_rt_entity(rt_se, array);
	}
	rt_se->on_rq = 0;

	dec_rt_tasks(rt_se, rt_rq);
}

/*
 * Adding/removing a task to/from a priority array:
 */
static void
enqueue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);

	if (flags & ENQUEUE_WAKEUP)
		rt_se->timeout = 0;

	/* Task arriving in an idle group of tasks. */
	if (is_dl_group(rt_rq) && (rt_rq->rt_nr_running == 0)) {
		struct sched_dl_entity *dl_se = dl_group_of(rt_rq);

		if (!dl_se->dl_throttled) {
			enqueue_dl_entity(dl_se, dl_se, ENQUEUE_WAKEUP);
			resched_curr(rq);
		}
	}

	enqueue_rt_entity(rt_se, flags);

	if (!task_current(rq, p) && p->nr_cpus_allowed > 1)
		enqueue_pushable_task(rt_rq, p);
}

static void dequeue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);

	update_curr_rt(rq);
	dequeue_rt_entity(rt_se, flags);

	dequeue_pushable_task(rt_rq_of_se(rt_se), p);

	/* Last task of the task group. */
	if (is_dl_group(rt_rq) && !rt_rq->rt_nr_running) {
		struct sched_dl_entity *dl_se = dl_group_of(rt_rq);

		if (!dl_se->dl_throttled) {
			rt_queue_pull_task(rq);

			if (!rt_rq->rt_nr_running) {
				dequeue_dl_entity(dl_se);
				task_non_contending(dl_se);
				resched_curr(rq);
			}
		}
	}
}

/*
 * Put task to the head or the end of the run list without the overhead of
 * dequeue followed by enqueue.
 */
static void
requeue_rt_entity(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se, int head)
{
	if (on_rt_rq(rt_se)) {
		struct rt_prio_array *array = &rt_rq->active;
		struct list_head *queue = array->queue + rt_se_prio(rt_se);

		if (head)
			list_move(&rt_se->run_list, queue);
		else
			list_move_tail(&rt_se->run_list, queue);
	}
}

static void requeue_task_rt(struct rq *rq, struct task_struct *p, int head)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq;

	rt_rq = rt_rq_of_se(rt_se);
	requeue_rt_entity(rt_rq, rt_se, head);
}

static void yield_task_rt(struct rq *rq)
{
	requeue_task_rt(rq, rq->curr, 0);
}

#ifdef CONFIG_SMP
static int find_lowest_rq(struct task_struct *task);

static int
select_task_rq_rt(struct task_struct *p, int cpu, int sd_flag, int flags)
{
	struct task_struct *curr;
	struct rq *rq;

	/* For anything but wake ups, just return the task_cpu */
	if (sd_flag != SD_BALANCE_WAKE && sd_flag != SD_BALANCE_FORK)
		goto out;

	rq = cpu_rq(cpu);

	rcu_read_lock();
	curr = READ_ONCE(rq->curr); /* unlocked access */

	/*
	 * If the current task on @p's runqueue is an RT task, then
	 * try to see if we can wake this RT task up on another
	 * runqueue. Otherwise simply start this RT task
	 * on its current runqueue.
	 *
	 * We want to avoid overloading runqueues. If the woken
	 * task is a higher priority, then it will stay on this CPU
	 * and the lower prio task should be moved to another CPU.
	 * Even though this will probably make the lower prio task
	 * lose its cache, we do not want to bounce a higher task
	 * around just because it gave up its CPU, perhaps for a
	 * lock?
	 *
	 * For equal prio tasks, we just let the scheduler sort it out.
	 *
	 * Otherwise, just let it ride on the affined RQ and the
	 * post-schedule router will push the preempted task away
	 *
	 * This test is optimistic, if we get it wrong the load-balancer
	 * will have to sort it out.
	 */
	if (curr && unlikely(rt_task(curr)) &&
	    (curr->nr_cpus_allowed < 2 ||
	     curr->prio <= p->prio)) {
		int target = find_lowest_rq(p);

		/*
		 * Don't bother moving it if the destination CPU is
		 * not running a lower priority task.
		 */
		if (target != -1 &&
		    p->prio < cpu_rq(target)->rt.highest_prio.curr)
			cpu = target;
	}
	rcu_read_unlock();

out:
	return cpu;
}

static void check_preempt_equal_prio(struct rq *rq, struct task_struct *p)
{
	/*
	 * Current can't be migrated, useless to reschedule,
	 * let's hope p can move out.
	 */
	if (rq->curr->nr_cpus_allowed == 1 ||
	    !cpupri_find(&rq->rd->cpupri, rq->curr, NULL))
		return;

	/*
	 * p is migratable, so let's not schedule it and
	 * see if it is pushed or pulled somewhere else.
	 */
	if (p->nr_cpus_allowed != 1
	    && cpupri_find(&rq->rd->cpupri, p, NULL))
		return;

	/*
	 * There appear to be other CPUs that can accept
	 * the current task but none can run 'p', so lets reschedule
	 * to try and push the current task away:
	 */
	requeue_task_rt(rq, p, 1);
	resched_curr(rq);
}

#endif /* CONFIG_SMP */

/*
 * Preempt the current task with a newly woken task if needed:
 */
static void check_preempt_curr_rt(struct rq *rq, struct task_struct *p, int flags)
{
	if (is_dl_group(rt_rq_of_se(&p->rt)) &&
	    is_dl_group(rt_rq_of_se(&rq->curr->rt))) {
		struct sched_dl_entity *dl_se, *curr_dl_se;

		dl_se = dl_group_of(rt_rq_of_se(&p->rt));
		curr_dl_se = dl_group_of(rt_rq_of_se(&rq->curr->rt));

		if (dl_entity_preempt(dl_se, curr_dl_se)) {
			resched_curr(rq);
			return;
		} else if (!dl_entity_preempt(curr_dl_se, dl_se)) {
			if (p->prio < rq->curr->prio) {
				resched_curr(rq);
				return;
			}
		}
		return;
	} else if (is_dl_group(rt_rq_of_se(&p->rt))) {
		resched_curr(rq);
		return;
	} else if (is_dl_group(rt_rq_of_se(&rq->curr->rt))) {
		return;
	}

	if (p->prio < rq->curr->prio) {
		resched_curr(rq);
		return;
	}

#ifdef CONFIG_SMP
	/*
	 * If:
	 *
	 * - the newly woken task is of equal priority to the current task
	 * - the newly woken task is non-migratable while current is migratable
	 * - current will be preempted on the next reschedule
	 *
	 * we should check to see if current can readily move to a different
	 * cpu.  If so, we will reschedule to allow the push logic to try
	 * to move current somewhere else, making room for our non-migratable
	 * task.
	 */
	if (p->prio == rq->curr->prio && !test_tsk_need_resched(rq->curr))
		check_preempt_equal_prio(rq, p);
#endif
}

static inline void set_next_task(struct rq *rq, struct task_struct *p)
{
	struct rt_rq *rt_rq = rt_rq_of_se(&p->rt);

	p->se.exec_start = rq_clock_task(rq);

	/* The running task is never eligible for pushing */
	dequeue_pushable_task(rt_rq, p);
}

struct sched_rt_entity *pick_next_rt_entity(struct rq *rq,
						   struct rt_rq *rt_rq)
{
	struct rt_prio_array *array = &rt_rq->active;
	struct sched_rt_entity *next = NULL;
	struct list_head *queue;
	int idx;

	idx = sched_find_first_bit(array->bitmap);
	BUG_ON(idx >= MAX_RT_PRIO);

	queue = array->queue + idx;
	next = list_entry(queue->next, struct sched_rt_entity, run_list);

	return next;
}

static struct task_struct *_pick_next_task_rt(struct rq *rq)
{
	struct sched_rt_entity *rt_se;
	struct rt_rq *rt_rq  = &rq->rt;

	rt_se = pick_next_rt_entity(rq, rt_rq);
	BUG_ON(!rt_se);

	return rt_task_of(rt_se);
}

static struct task_struct *
pick_next_task_rt(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	struct task_struct *p;
	struct rt_rq *rt_rq = &rq->rt;

	if (need_pull_rt_task(rq, prev)) {
		/*
		 * This is OK, because current is on_cpu, which avoids it being
		 * picked for load-balance and preemption/IRQs are still
		 * disabled avoiding further scheduler activity on it and we're
		 * being very careful to re-start the picking loop.
		 */
		rq_unpin_lock(rq, rf);
		pull_rt_task(rq);
		rq_repin_lock(rq, rf);
		/*
		 * pull_rt_task() can drop (and re-acquire) rq->lock; this
		 * means a dl or stop task can slip in, in which case we need
		 * to re-start task selection.
		 */
		if (unlikely((rq->stop && task_on_rq_queued(rq->stop)) ||
			     rq->dl.dl_nr_running))
			return RETRY_TASK;
	}

	if (prev->sched_class == &rt_sched_class)
		update_curr_rt(rq);

	if (!rt_rq->rt_nr_running)
		return NULL;

	put_prev_task(rq, prev);

	p = _pick_next_task_rt(rq);

	set_next_task(rq, p);

	rt_queue_push_tasks(rq);

	/*
	 * If prev task was rt, put_prev_task() has already updated the
	 * utilization. We only care of the case where we start to schedule a
	 * rt task
	 */
	if (rq->curr->sched_class != &rt_sched_class)
		update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 0);

	return p;
}

static void put_prev_task_rt(struct rq *rq, struct task_struct *p)
{
	struct rt_rq *rt_rq = rt_rq_of_se(&p->rt);

	update_curr_rt(rq);

	update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 1);

	/*
	 * The previous task needs to be made eligible for pushing
	 * if it is still active
	 */
	if (on_rt_rq(&p->rt) && p->nr_cpus_allowed > 1)
		enqueue_pushable_task(rt_rq, p);
#if defined(CONFIG_RT_GROUP_SCHED) && defined(CONFIG_SMP)
	if (is_dl_group(rt_rq)) {
		struct sched_dl_entity *dl_se = dl_group_of(rt_rq);

		if (dl_se->dl_throttled)
			queue_push_from_group(rq, rt_rq, 2);
	}
#endif
}

#ifdef CONFIG_SMP

/* Only try algorithms three times */
#define RT_MAX_TRIES 3

static int pick_rt_task(struct rt_rq *rt_rq, struct task_struct *p, int cpu)
{
	if (!task_running(rq_of_rt_rq(rt_rq), p) &&
	    cpumask_test_cpu(cpu, &p->cpus_allowed))
		return 1;

	return 0;
}

/*
 * Return the highest pushable rq's task, which is suitable to be executed
 * on the CPU, NULL otherwise
 */
static
struct task_struct *pick_highest_pushable_task(struct rt_rq *rt_rq, int cpu)
{
	struct plist_head *head = &rt_rq->pushable_tasks;
	struct task_struct *p;

	if (!has_pushable_tasks(rt_rq))
		return NULL;

	plist_for_each_entry(p, head, pushable_tasks) {
		if (pick_rt_task(rt_rq, p, cpu))
			return p;
	}

	return NULL;
}

static DEFINE_PER_CPU(cpumask_var_t, local_cpu_mask);

static int find_lowest_rq(struct task_struct *task)
{
	struct sched_domain *sd;
	struct cpumask *lowest_mask = this_cpu_cpumask_var_ptr(local_cpu_mask);
	int this_cpu = smp_processor_id();
	int cpu      = task_cpu(task);

	/* Make sure the mask is initialized first */
	if (unlikely(!lowest_mask))
		return -1;

	if (task->nr_cpus_allowed == 1)
		return -1; /* No other targets possible */

	if (!cpupri_find(&task_rq(task)->rd->cpupri, task, lowest_mask))
		return -1; /* No targets found */

	/*
	 * At this point we have built a mask of CPUs representing the
	 * lowest priority tasks in the system.  Now we want to elect
	 * the best one based on our affinity and topology.
	 *
	 * We prioritize the last CPU that the task executed on since
	 * it is most likely cache-hot in that location.
	 */
	if (cpumask_test_cpu(cpu, lowest_mask))
		return cpu;

	/*
	 * Otherwise, we consult the sched_domains span maps to figure
	 * out which CPU is logically closest to our hot cache data.
	 */
	if (!cpumask_test_cpu(this_cpu, lowest_mask))
		this_cpu = -1; /* Skip this_cpu opt if not among lowest */

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		if (sd->flags & SD_WAKE_AFFINE) {
			int best_cpu;

			/*
			 * "this_cpu" is cheaper to preempt than a
			 * remote processor.
			 */
			if (this_cpu != -1 &&
			    cpumask_test_cpu(this_cpu, sched_domain_span(sd))) {
				rcu_read_unlock();
				return this_cpu;
			}

			best_cpu = cpumask_first_and(lowest_mask,
						     sched_domain_span(sd));
			if (best_cpu < nr_cpu_ids) {
				rcu_read_unlock();
				return best_cpu;
			}
		}
	}
	rcu_read_unlock();

	/*
	 * And finally, if there were no matches within the domains
	 * just give the caller *something* to work with from the compatible
	 * locations.
	 */
	if (this_cpu != -1)
		return this_cpu;

	cpu = cpumask_any(lowest_mask);
	if (cpu < nr_cpu_ids)
		return cpu;

	return -1;
}

/* Will lock the rq it finds */
static struct rq *find_lock_lowest_rq(struct task_struct *task, struct rq *rq)
{
	struct rq *lowest_rq = NULL;
	int tries;
	int cpu;

	for (tries = 0; tries < RT_MAX_TRIES; tries++) {
		cpu = find_lowest_rq(task);

		if ((cpu == -1) || (cpu == rq->cpu))
			break;

		lowest_rq = cpu_rq(cpu);

		if (lowest_rq->rt.highest_prio.curr <= task->prio) {
			/*
			 * Target rq has tasks of equal or higher priority,
			 * retrying does not release any lock and is unlikely
			 * to yield a different result.
			 */
			lowest_rq = NULL;
			break;
		}

		/* if the prio of this runqueue changed, try again */
		if (double_lock_balance(rq, lowest_rq)) {
			/*
			 * We had to unlock the run queue. In
			 * the mean time, task could have
			 * migrated already or had its affinity changed.
			 * Also make sure that it wasn't scheduled on its rq.
			 */
			if (unlikely(task_rq(task) != rq ||
				     !cpumask_test_cpu(lowest_rq->cpu, &task->cpus_allowed) ||
				     task_running(rq, task) ||
				     !rt_task(task) ||
				     !task_on_rq_queued(task))) {

				double_unlock_balance(rq, lowest_rq);
				lowest_rq = NULL;
				break;
			}
		}

		/* If this rq is still suitable use it. */
		if (lowest_rq->rt.highest_prio.curr > task->prio)
			break;

		/* try again */
		double_unlock_balance(rq, lowest_rq);
		lowest_rq = NULL;
	}

	return lowest_rq;
}

static struct task_struct *pick_next_pushable_task(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);
	struct task_struct *p;

	if (!has_pushable_tasks(rt_rq))
		return NULL;

	p = plist_first_entry(&rt_rq->pushable_tasks,
			      struct task_struct, pushable_tasks);

	BUG_ON(rq->cpu != task_cpu(p));
	BUG_ON(task_current(rq, p));
	BUG_ON(p->nr_cpus_allowed <= 1);

	BUG_ON(!task_on_rq_queued(p));
	BUG_ON(!rt_task(p));

	return p;
}

/*
 * If the current CPU has more than one RT task, see if the non
 * running task can migrate over to a CPU that is running a task
 * of lesser priority.
 */
static int push_rt_task(struct rq *rq)
{
	struct task_struct *next_task;
	struct rq *lowest_rq;
	int ret = 0;

	if (!rq->rt.overloaded)
		return 0;

	next_task = pick_next_pushable_task(&rq->rt);
	if (!next_task)
		return 0;

retry:
	if (WARN_ON(next_task == rq->curr))
		return 0;

	/*
	 * It's possible that the next_task slipped in of
	 * higher priority than current. If that's the case
	 * just reschedule current.
	 */
	if (unlikely(next_task->prio < rq->curr->prio)) {
		resched_curr(rq);
		return 0;
	}

	/* We might release rq lock */
	get_task_struct(next_task);

	/* find_lock_lowest_rq locks the rq if found */
	lowest_rq = find_lock_lowest_rq(next_task, rq);
	if (!lowest_rq) {
		struct task_struct *task;
		/*
		 * find_lock_lowest_rq releases rq->lock
		 * so it is possible that next_task has migrated.
		 *
		 * We need to make sure that the task is still on the same
		 * run-queue and is also still the next task eligible for
		 * pushing.
		 */
		task = pick_next_pushable_task(&rq->rt);
		if (task == next_task) {
			/*
			 * The task hasn't migrated, and is still the next
			 * eligible task, but we failed to find a run-queue
			 * to push it to.  Do not retry in this case, since
			 * other CPUs will pull from us when ready.
			 */
			goto out;
		}

		if (!task)
			/* No more tasks, just exit */
			goto out;

		/*
		 * Something has shifted, try again.
		 */
		put_task_struct(next_task);
		next_task = task;
		goto retry;
	}

	deactivate_task(rq, next_task, 0);
	set_task_cpu(next_task, lowest_rq->cpu);
	activate_task(lowest_rq, next_task, 0);
	ret = 1;

	resched_curr(lowest_rq);

	double_unlock_balance(rq, lowest_rq);

out:
	put_task_struct(next_task);

	return ret;
}

static void push_rt_tasks(struct rq *rq)
{
	/* push_rt_task will return true if it moved an RT */
	while (push_rt_task(rq))
		;
}

#ifdef HAVE_RT_PUSH_IPI

/*
 * When a high priority task schedules out from a CPU and a lower priority
 * task is scheduled in, a check is made to see if there's any RT tasks
 * on other CPUs that are waiting to run because a higher priority RT task
 * is currently running on its CPU. In this case, the CPU with multiple RT
 * tasks queued on it (overloaded) needs to be notified that a CPU has opened
 * up that may be able to run one of its non-running queued RT tasks.
 *
 * All CPUs with overloaded RT tasks need to be notified as there is currently
 * no way to know which of these CPUs have the highest priority task waiting
 * to run. Instead of trying to take a spinlock on each of these CPUs,
 * which has shown to cause large latency when done on machines with many
 * CPUs, sending an IPI to the CPUs to have them push off the overloaded
 * RT tasks waiting to run.
 *
 * Just sending an IPI to each of the CPUs is also an issue, as on large
 * count CPU machines, this can cause an IPI storm on a CPU, especially
 * if its the only CPU with multiple RT tasks queued, and a large number
 * of CPUs scheduling a lower priority task at the same time.
 *
 * Each root domain has its own irq work function that can iterate over
 * all CPUs with RT overloaded tasks. Since all CPUs with overloaded RT
 * tassk must be checked if there's one or many CPUs that are lowering
 * their priority, there's a single irq work iterator that will try to
 * push off RT tasks that are waiting to run.
 *
 * When a CPU schedules a lower priority task, it will kick off the
 * irq work iterator that will jump to each CPU with overloaded RT tasks.
 * As it only takes the first CPU that schedules a lower priority task
 * to start the process, the rto_start variable is incremented and if
 * the atomic result is one, then that CPU will try to take the rto_lock.
 * This prevents high contention on the lock as the process handles all
 * CPUs scheduling lower priority tasks.
 *
 * All CPUs that are scheduling a lower priority task will increment the
 * rt_loop_next variable. This will make sure that the irq work iterator
 * checks all RT overloaded CPUs whenever a CPU schedules a new lower
 * priority task, even if the iterator is in the middle of a scan. Incrementing
 * the rt_loop_next will cause the iterator to perform another scan.
 *
 */
static int rto_next_cpu(struct root_domain *rd)
{
	int next;
	int cpu;

	/*
	 * When starting the IPI RT pushing, the rto_cpu is set to -1,
	 * rt_next_cpu() will simply return the first CPU found in
	 * the rto_mask.
	 *
	 * If rto_next_cpu() is called with rto_cpu is a valid CPU, it
	 * will return the next CPU found in the rto_mask.
	 *
	 * If there are no more CPUs left in the rto_mask, then a check is made
	 * against rto_loop and rto_loop_next. rto_loop is only updated with
	 * the rto_lock held, but any CPU may increment the rto_loop_next
	 * without any locking.
	 */
	for (;;) {

		/* When rto_cpu is -1 this acts like cpumask_first() */
		cpu = cpumask_next(rd->rto_cpu, rd->rto_mask);

		rd->rto_cpu = cpu;

		if (cpu < nr_cpu_ids)
			return cpu;

		rd->rto_cpu = -1;

		/*
		 * ACQUIRE ensures we see the @rto_mask changes
		 * made prior to the @next value observed.
		 *
		 * Matches WMB in rt_set_overload().
		 */
		next = atomic_read_acquire(&rd->rto_loop_next);

		if (rd->rto_loop == next)
			break;

		rd->rto_loop = next;
	}

	return -1;
}

static inline bool rto_start_trylock(atomic_t *v)
{
	return !atomic_cmpxchg_acquire(v, 0, 1);
}

static inline void rto_start_unlock(atomic_t *v)
{
	atomic_set_release(v, 0);
}

static void tell_cpu_to_push(struct rq *rq)
{
	int cpu = -1;

	/* Keep the loop going if the IPI is currently active */
	atomic_inc(&rq->rd->rto_loop_next);

	/* Only one CPU can initiate a loop at a time */
	if (!rto_start_trylock(&rq->rd->rto_loop_start))
		return;

	raw_spin_lock(&rq->rd->rto_lock);

	/*
	 * The rto_cpu is updated under the lock, if it has a valid CPU
	 * then the IPI is still running and will continue due to the
	 * update to loop_next, and nothing needs to be done here.
	 * Otherwise it is finishing up and an ipi needs to be sent.
	 */
	if (rq->rd->rto_cpu < 0)
		cpu = rto_next_cpu(rq->rd);

	raw_spin_unlock(&rq->rd->rto_lock);

	rto_start_unlock(&rq->rd->rto_loop_start);

	if (cpu >= 0) {
		/* Make sure the rd does not get freed while pushing */
		sched_get_rd(rq->rd);
		irq_work_queue_on(&rq->rd->rto_push_work, cpu);
	}
}

/* Called from hardirq context */
void rto_push_irq_work_func(struct irq_work *work)
{
	struct root_domain *rd =
		container_of(work, struct root_domain, rto_push_work);
	struct rq *rq;
	int cpu;

	rq = this_rq();

	/*
	 * We do not need to grab the lock to check for has_pushable_tasks.
	 * When it gets updated, a check is made if a push is possible.
	 */
	if (has_pushable_tasks(&rq->rt)) {
		raw_spin_lock(&rq->lock);
		push_rt_tasks(rq);
		raw_spin_unlock(&rq->lock);
	}

	raw_spin_lock(&rd->rto_lock);

	/* Pass the IPI to the next rt overloaded queue */
	cpu = rto_next_cpu(rd);

	raw_spin_unlock(&rd->rto_lock);

	if (cpu < 0) {
		sched_put_rd(rd);
		return;
	}

	/* Try the next RT overloaded CPU */
	irq_work_queue_on(&rd->rto_push_work, cpu);
}
#endif /* HAVE_RT_PUSH_IPI */

static void pull_rt_task(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu, cpu;
	bool resched = false;
	struct task_struct *p;
	struct rt_rq *src_rt_rq;
	struct rq *src_rq;
	int rt_overload_count = rt_overloaded(this_rq);

	if (likely(!rt_overload_count))
		return;

	/*
	 * Match the barrier from rt_set_overloaded; this guarantees that if we
	 * see overloaded we must also see the rto_mask bit.
	 */
	smp_rmb();

	/* If we are the only overloaded CPU do nothing */
	if (rt_overload_count == 1 &&
	    cpumask_test_cpu(this_rq->cpu, this_rq->rd->rto_mask))
		return;

#ifdef HAVE_RT_PUSH_IPI
	if (sched_feat(RT_PUSH_IPI)) {
		tell_cpu_to_push(this_rq);
		return;
	}
#endif

	for_each_cpu(cpu, this_rq->rd->rto_mask) {
		if (this_cpu == cpu)
			continue;

		src_rq = cpu_rq(cpu);
		src_rt_rq = &src_rq->rt;

		/*
		 * Don't bother taking the src_rq->lock if the next highest
		 * task is known to be lower-priority than our current task.
		 * This may look racy, but if this value is about to go
		 * logically higher, the src_rq will push this task away.
		 * And if its going logically lower, we do not care
		 */
		if (src_rt_rq->highest_prio.next >=
		    this_rq->rt.highest_prio.curr)
			continue;

		/*
		 * We can potentially drop this_rq's lock in
		 * double_lock_balance, and another CPU could
		 * alter this_rq
		 */
		double_lock_balance(this_rq, src_rq);

		/*
		 * We can pull only a task, which is pushable
		 * on its rq, and no others.
		 */
		p = pick_highest_pushable_task(src_rt_rq, this_cpu);

		/*
		 * Do we have an RT task that preempts
		 * the to-be-scheduled task?
		 */
		if (p && (p->prio < this_rq->rt.highest_prio.curr)) {
			WARN_ON(p == src_rq->curr);
			WARN_ON(!task_on_rq_queued(p));

			/*
			 * There's a chance that p is higher in priority
			 * than what's currently running on its CPU.
			 * This is just that p is wakeing up and hasn't
			 * had a chance to schedule. We only pull
			 * p if it is lower in priority than the
			 * current task on the run queue
			 */
			if (p->prio < src_rq->curr->prio)
				goto skip;

			resched = true;

			deactivate_task(src_rq, p, 0);
			set_task_cpu(p, this_cpu);
			activate_task(this_rq, p, 0);
			/*
			 * We continue with the search, just in
			 * case there's an even higher prio task
			 * in another runqueue. (low likelihood
			 * but possible)
			 */
		}
skip:
		double_unlock_balance(this_rq, src_rq);
	}

	if (resched)
		resched_curr(this_rq);
}

#ifdef CONFIG_RT_GROUP_SCHED
struct rt_rq *group_find_lock_rt_rq(struct task_struct *task,
				    struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq), *first_rq;
	struct sched_dl_entity *first_dl_se;
	struct rt_rq *first_rt_rq = NULL;
	int cpu, tries;

	BUG_ON(!is_dl_group(rt_rq));

	for_each_online_cpu(cpu) {
		if (cpu == -1)
			continue;
		if (cpu == rq->cpu)
			continue;

		first_dl_se = rt_rq->tg->dl_se[cpu];
		first_rt_rq = first_dl_se->my_q;
		first_rq = rq_of_rt_rq(first_rt_rq);

		tries = 0;
retry_cpu_push:
		if (++tries > RT_MAX_TRIES) {
			first_rt_rq = NULL;
			continue;
		}

		if (first_dl_se->dl_throttled) {
			first_rt_rq = NULL;
			continue;
		}

		if (double_lock_balance(rq, first_rq)) {

			if (unlikely(task_rq(task) != rq ||
			    task_running(rq, task) ||
			    !task->on_rq)) {
				double_unlock_balance(rq, first_rq);

				return NULL;
			}

			if (unlikely(!cpumask_test_cpu(first_rq->cpu,
						&task->cpus_allowed) ||
			    first_dl_se->dl_throttled)) {
				double_unlock_balance(rq, first_rq);

				goto retry_cpu_push;
			}
		}

		if (first_rt_rq->highest_prio.curr > task->prio)
			break;

		double_unlock_balance(rq, first_rq);
		first_rt_rq = NULL;
	}

	return first_rt_rq;
}

int group_push_rt_task_from_group(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq), *first_rq;
	struct rt_rq *first_rt_rq;
	struct task_struct *p;
	int tries = 0;

try_another_task:
	p = pick_next_pushable_task(rt_rq);
	if (!p)
		return 0;

	get_task_struct(p);

	first_rt_rq = group_find_lock_rt_rq(p, rt_rq);
	if (!first_rt_rq) {
		put_task_struct(p);

		if (tries++ > RT_MAX_TRIES)
			return 0;

		goto try_another_task;
	}

	first_rq = rq_of_rt_rq(first_rt_rq);

	deactivate_task(rq, p, 0);
	set_task_cpu(p, first_rq->cpu);
	activate_task(first_rq, p, 0);

	resched_curr(first_rq);

	double_unlock_balance(rq, first_rq);
	put_task_struct(p);

	return 1;
}

int group_pull_rt_task_from_group(struct rt_rq *this_rt_rq)
{
	struct rq *this_rq = rq_of_rt_rq(this_rt_rq), *src_rq;
	struct sched_dl_entity *this_dl_se, *src_dl_se;
	struct rt_rq *src_rt_rq;
	struct task_struct *p;
	int this_cpu = this_rq->cpu, cpu, tries = 0, ret = 0;

	this_dl_se = dl_group_of(this_rt_rq);
	for_each_online_cpu(cpu) {
		if (cpu == -1)
			continue;
		if (cpu == this_rq->cpu)
			continue;

		src_dl_se = this_rt_rq->tg->dl_se[cpu];
		src_rt_rq = src_dl_se->my_q;

		if ((src_rt_rq->rt_nr_running <= 1) && !src_dl_se->dl_throttled)
			continue;

		src_rq = rq_of_rt_rq(src_rt_rq);

		if (++tries > RT_MAX_TRIES)
			continue;

		double_lock_balance(this_rq, src_rq);

		p = pick_highest_pushable_task(src_rt_rq, this_cpu);

		if (p && (p->prio < this_rt_rq->highest_prio.curr)) {
			WARN_ON(p == src_rq->curr);
			WARN_ON(!p->on_rq);

			ret = 1;

			deactivate_task(src_rq, p, 0);
			set_task_cpu(p, this_cpu);
			activate_task(this_rq, p, 0);
		}
		double_unlock_balance(this_rq, src_rq);
	}

	return ret;
}

int group_push_rt_task(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	if (is_dl_group(rt_rq))
		return group_push_rt_task_from_group(rt_rq);

	return push_rt_task(rq);
}

int group_pull_rt_task(struct rt_rq *this_rt_rq)
{
	struct rq *this_rq = rq_of_rt_rq(this_rt_rq);

	if (is_dl_group(this_rt_rq))
		return group_pull_rt_task_from_group(this_rt_rq);

	pull_rt_task(this_rq);

	return 1;
}

void group_push_rt_tasks(struct rt_rq *rt_rq)
{
	while (group_push_rt_task(rt_rq))
		;
}
#else
void group_push_rt_tasks(struct rt_rq *rt_rq)
{
	push_rt_tasks(rq_of_rt_rq(rt_rq));
}
#endif

/*
 * If we are not running and we are not going to reschedule soon, we should
 * try to push tasks away now
 */
static void task_woken_rt(struct rq *rq, struct task_struct *p)
{
	struct rt_rq *rt_rq = rt_rq_of_se(&p->rt);

	if (!task_running(rq, p) &&
	    !test_tsk_need_resched(rq->curr) &&
	    p->nr_cpus_allowed > 1 &&
	    (dl_task(rq->curr) || rt_task(rq->curr)) &&
	    (rq->curr->nr_cpus_allowed < 2 ||
	     rq->curr->prio <= p->prio))
		group_push_rt_tasks(rt_rq);
}

/* Assumes rq->lock is held */
static void rq_online_rt(struct rq *rq)
{
	if (rq->rt.overloaded)
		rt_set_overload(rq);

	cpupri_set(&rq->rd->cpupri, rq->cpu, rq->rt.highest_prio.curr);
}

/* Assumes rq->lock is held */
static void rq_offline_rt(struct rq *rq)
{
	if (rq->rt.overloaded)
		rt_clear_overload(rq);

	cpupri_set(&rq->rd->cpupri, rq->cpu, CPUPRI_INVALID);
}

/*
 * When switch from the rt queue, we bring ourselves to a position
 * that we might want to pull RT tasks from other runqueues.
 */
static void switched_from_rt(struct rq *rq, struct task_struct *p)
{
	struct rt_rq *rt_rq = rt_rq_of_se(&p->rt);

	/*
	 * If there are other RT tasks then we will reschedule
	 * and the scheduling of the other RT tasks will handle
	 * the balancing. But if we are the last RT task
	 * we may need to handle the pulling of RT tasks
	 * now.
	 */
	if (!task_on_rq_queued(p) || rt_rq->rt_nr_running)
		return;

	if (!is_dl_group(rt_rq))
		rt_queue_pull_task(rq);
	else
		queue_pull_to_group(rq, rt_rq);
}

void __init init_sched_rt_class(void)
{
	unsigned int i;

	for_each_possible_cpu(i) {
		zalloc_cpumask_var_node(&per_cpu(local_cpu_mask, i),
					GFP_KERNEL, cpu_to_node(i));
	}
}
#endif /* CONFIG_SMP */

/*
 * When switching a task to RT, we may overload the runqueue
 * with RT tasks. In this case we try to push them off to
 * other runqueues.
 */
static void switched_to_rt(struct rq *rq, struct task_struct *p)
{
	/*
	 * If we are already running, then there's nothing
	 * that needs to be done. But if we are not running
	 * we may need to preempt the current running task.
	 * If that current running task is also an RT task
	 * then see if we can move to another run queue.
	 */
	if (task_on_rq_queued(p) && rq->curr != p) {
#ifdef CONFIG_SMP
		if (!is_dl_group(rt_rq_of_se(&p->rt)) && p->nr_cpus_allowed > 1 && rq->rt.overloaded)
			rt_queue_push_tasks(rq);
		else if (is_dl_group(rt_rq_of_se(&p->rt)) && rt_rq_of_se(&p->rt)->overloaded) {
			queue_push_from_group(rq, rt_rq_of_se(&p->rt), 3);
		} else {
			if (p->prio < rq->curr->prio)
				resched_curr(rq);
		}
#endif /* CONFIG_SMP */
		if (p->prio < rq->curr->prio && cpu_online(cpu_of(rq)))
			resched_curr(rq);
	}
}

/*
 * Priority of the task has changed. This may cause
 * us to initiate a push or pull.
 */
static void
prio_changed_rt(struct rq *rq, struct task_struct *p, int oldprio)
{
#ifdef CONFIG_SMP
	struct rt_rq *rt_rq = rt_rq_of_se(&p->rt);
#endif

	if (!task_on_rq_queued(p))
		return;

	if (rq->curr == p) {
#ifdef CONFIG_SMP
		/*
		 * If our priority decreases while running, we
		 * may need to pull tasks to this runqueue.
		 */
		if (oldprio < p->prio) {
			if (!is_dl_group(rt_rq))
				rt_queue_pull_task(rq);
			else
				queue_pull_to_group(rq, rt_rq);
		}

		/*
		 * If there's a higher priority task waiting to run
		 * then reschedule.
		 */
		if (p->prio > rt_rq->highest_prio.curr)
			resched_curr(rq);
#else
		/* For UP simply resched on drop of prio */
		if (oldprio < p->prio)
			resched_curr(rq);
#endif /* CONFIG_SMP */
	} else {
		/*
		 * This task is not running, but if it is
		 * greater than the current running task
		 * then reschedule.
		 */
		if (p->prio < rq->curr->prio)
			resched_curr(rq);
	}
}

#ifdef CONFIG_POSIX_TIMERS
static void watchdog(struct rq *rq, struct task_struct *p)
{
	unsigned long soft, hard;

	/* max may change after cur was read, this will be fixed next tick */
	soft = task_rlimit(p, RLIMIT_RTTIME);
	hard = task_rlimit_max(p, RLIMIT_RTTIME);

	if (soft != RLIM_INFINITY) {
		unsigned long next;

		if (p->rt.watchdog_stamp != jiffies) {
			p->rt.timeout++;
			p->rt.watchdog_stamp = jiffies;
		}

		next = DIV_ROUND_UP(min(soft, hard), USEC_PER_SEC/HZ);
		if (p->rt.timeout > next)
			p->cputime_expires.sched_exp = p->se.sum_exec_runtime;
	}
}
#else
static inline void watchdog(struct rq *rq, struct task_struct *p) { }
#endif

/*
 * scheduler tick hitting a task of our scheduling class.
 *
 * NOTE: This function can be called remotely by the tick offload that
 * goes along full dynticks. Therefore no local assumption can be made
 * and everything must be accessed through the @rq and @curr passed in
 * parameters.
 */
static void task_tick_rt(struct rq *rq, struct task_struct *p, int queued)
{
	struct sched_rt_entity *rt_se = &p->rt;

	update_curr_rt(rq);
#ifdef CONFIG_RT_GROUP_SCHED
	if (is_dl_group(&rq->rt)) {
		struct sched_dl_entity *dl_se = dl_group_of(&rq->rt);

		if (hrtick_enabled(rq) && queued && dl_se->runtime > 0)
			start_hrtick_dl(rq, dl_se);
	}
#endif
	update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 1);

	watchdog(rq, p);

	/*
	 * RR tasks need a special form of timeslice management.
	 * FIFO tasks have no timeslices.
	 */
	if (p->policy != SCHED_RR)
		return;

	if (--p->rt.time_slice)
		return;

	p->rt.time_slice = sched_rr_timeslice;

	/*
	 * Requeue to the end of queue if we (and all of our ancestors) are not
	 * the only element on the queue
	 */
	if (rt_se->run_list.prev != rt_se->run_list.next) {
		requeue_task_rt(rq, p, 0);
		set_tsk_need_resched(p);
		return;
	}
}

static void set_curr_task_rt(struct rq *rq)
{
	set_next_task(rq, rq->curr);
}

static unsigned int get_rr_interval_rt(struct rq *rq, struct task_struct *task)
{
	/*
	 * Time slice is 0 for SCHED_FIFO tasks
	 */
	if (task->policy == SCHED_RR)
		return sched_rr_timeslice;
	else
		return 0;
}

const struct sched_class rt_sched_class = {
	.next			= &fair_sched_class,
	.enqueue_task		= enqueue_task_rt,
	.dequeue_task		= dequeue_task_rt,
	.yield_task		= yield_task_rt,

	.check_preempt_curr	= check_preempt_curr_rt,

	.pick_next_task		= pick_next_task_rt,
	.put_prev_task		= put_prev_task_rt,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_rt,

	.set_cpus_allowed       = set_cpus_allowed_common,
	.rq_online              = rq_online_rt,
	.rq_offline             = rq_offline_rt,
	.task_woken		= task_woken_rt,
	.switched_from		= switched_from_rt,
#endif

	.set_curr_task          = set_curr_task_rt,
	.task_tick		= task_tick_rt,

	.get_rr_interval	= get_rr_interval_rt,

	.prio_changed		= prio_changed_rt,
	.switched_to		= switched_to_rt,

	.update_curr		= update_curr_rt,
};

#ifdef CONFIG_RT_GROUP_SCHED
/*
 * Ensure that the real time constraints are schedulable.
 */
static DEFINE_MUTEX(rt_constraints_mutex);

/* Must be called with tasklist_lock held */
static inline int tg_has_rt_tasks(struct task_group *tg)
{
	struct task_struct *g, *p;

	/*
	 * Autogroups do not have RT tasks; see autogroup_create().
	 */
	if (task_group_is_autogroup(tg))
		return 0;

	for_each_process_thread(g, p) {
		if (rt_task(p) && task_group(p) == tg)
			return 1;
	}

	return 0;
}

struct rt_schedulable_data {
	struct task_group *tg;
	u64 rt_period;
	u64 rt_runtime;
};

static int tg_rt_schedulable(struct task_group *tg, void *data)
{
	struct rt_schedulable_data *d = data;
	struct task_group *child;
	unsigned long total, sum = 0;
	u64 period, runtime;

	period  = tg->dl_bandwidth.dl_period;
	runtime = tg->dl_bandwidth.dl_runtime;

	if (tg == d->tg) {
		period = d->rt_period;
		runtime = d->rt_runtime;
	}

	/*
	 * Cannot have more runtime than the period.
	 */
	if (runtime > period && runtime != RUNTIME_INF)
		return -EINVAL;

	/*
	 * Ensure we don't starve existing RT tasks.
	 */
	if (dl_bandwidth_enabled() && !runtime && tg_has_rt_tasks(tg))
		return -EBUSY;

	total = to_ratio(period, runtime);

	/*
	 * Nobody can have more than the global setting allows.
	 */
	if (total > to_ratio(global_rt_period(), global_rt_runtime()))
		return -EINVAL;

	if (tg == &root_task_group) {
		if (!dl_check_tg(total))
			return -EBUSY;
	}

	/*
	 * The sum of our children's runtime should not exceed our own.
	 */
	list_for_each_entry_rcu(child, &tg->children, siblings) {
		period  = child->dl_bandwidth.dl_period;
		runtime = child->dl_bandwidth.dl_runtime;

		if (child == d->tg) {
			period = d->rt_period;
			runtime = d->rt_runtime;
		}

		sum += to_ratio(period, runtime);
	}

	if (sum > total)
		return -EINVAL;

	return 0;
}

static int __rt_schedulable(struct task_group *tg, u64 period, u64 runtime)
{
	int ret;

	struct rt_schedulable_data data = {
		.tg = tg,
		.rt_period = period,
		.rt_runtime = runtime,
	};

	if (!((s64)(period - runtime) >= 0) ||
	    (runtime && !(runtime >= (2 << (DL_SCALE - 1))))) {

		return 1;
	}


	rcu_read_lock();
	ret = walk_tg_tree(tg_rt_schedulable, tg_nop, &data);
	rcu_read_unlock();

	return ret;
}

static int tg_set_rt_bandwidth(struct task_group *tg,
		u64 rt_period, u64 rt_runtime)
{
	int i, err = 0;

	/*
	 * Disallowing the root group RT runtime is BAD, it would disallow the
	 * kernel creating (and or operating) RT threads.
	 */
	if (tg == &root_task_group && rt_runtime == 0)
		return -EINVAL;

	/*
	 * Do not allow to set a RT runtime > 0 if the parent has RT tasks
	 * (and is not the root group)
	 */
	if (rt_runtime && (tg != &root_task_group) && (tg->parent != &root_task_group) && tg_has_rt_tasks(tg->parent)) {
		return -EINVAL;
	}

	/* No period doesn't make any sense. */
	if (rt_period == 0)
		return -EINVAL;

	mutex_lock(&rt_constraints_mutex);
	read_lock(&tasklist_lock);
	err = __rt_schedulable(tg, rt_period, rt_runtime);
	if (err)
		goto unlock;

	raw_spin_lock_irq(&tg->dl_bandwidth.dl_runtime_lock);
	tg->dl_bandwidth.dl_period  = rt_period;
	tg->dl_bandwidth.dl_runtime = rt_runtime;

	if (tg == &root_task_group)
		goto unlock_bandwidth;

	for_each_possible_cpu(i) {
		dl_init_tg(tg->dl_se[i], rt_runtime, rt_period);
	}
unlock_bandwidth:
	raw_spin_unlock_irq(&tg->dl_bandwidth.dl_runtime_lock);
unlock:
	read_unlock(&tasklist_lock);
	mutex_unlock(&rt_constraints_mutex);

	return err;
}

int sched_group_set_rt_runtime(struct task_group *tg, long rt_runtime_us)
{
	u64 rt_runtime, rt_period;

	rt_period  = tg->dl_bandwidth.dl_period;
	rt_runtime = (u64)rt_runtime_us * NSEC_PER_USEC;
	if (rt_runtime_us < 0)
		rt_runtime = RUNTIME_INF;
	else if ((u64)rt_runtime_us > U64_MAX / NSEC_PER_USEC)
		return -EINVAL;

	return tg_set_rt_bandwidth(tg, rt_period, rt_runtime);
}

long sched_group_rt_runtime(struct task_group *tg)
{
	u64 rt_runtime_us;

	if (tg->dl_bandwidth.dl_runtime == RUNTIME_INF)
		return -1;

	rt_runtime_us = tg->dl_bandwidth.dl_runtime;
	do_div(rt_runtime_us, NSEC_PER_USEC);
	return rt_runtime_us;
}

int sched_group_set_rt_period(struct task_group *tg, u64 rt_period_us)
{
	u64 rt_runtime, rt_period;

	if (rt_period_us > U64_MAX / NSEC_PER_USEC)
		return -EINVAL;

	rt_period = rt_period_us * NSEC_PER_USEC;
	rt_runtime = tg->dl_bandwidth.dl_runtime;

	return tg_set_rt_bandwidth(tg, rt_period, rt_runtime);
}

long sched_group_rt_period(struct task_group *tg)
{
	u64 rt_period_us;

	rt_period_us = tg->dl_bandwidth.dl_period;
	do_div(rt_period_us, NSEC_PER_USEC);
	return rt_period_us;
}

static int sched_rt_global_constraints(void)
{
	int ret = 0;

	mutex_lock(&rt_constraints_mutex);
	read_lock(&tasklist_lock);
	ret = __rt_schedulable(NULL, 0, 0);
	read_unlock(&tasklist_lock);
	mutex_unlock(&rt_constraints_mutex);

	return ret;
}

int sched_rt_can_attach(struct task_group *tg, struct task_struct *tsk)
{
	int can_attach = 1;

	/* Don't accept realtime tasks when there is no way for them to run */
	if (rt_task(tsk) && tg->dl_bandwidth.dl_runtime == 0)
		return 0;

	/* If one of the children has runtime > 0, cannot attach RT tasks! */
	if ((tg != &root_task_group) && rt_task(tsk)) {
		struct task_group *child;

		list_for_each_entry_rcu(child, &tg->children, siblings) {
			if (child->dl_bandwidth.dl_runtime) {
				can_attach = 0;
			}
		}
	}

	return can_attach;
}

#else /* !CONFIG_RT_GROUP_SCHED */
static int sched_rt_global_constraints(void)
{
	return 0;
}
#endif /* CONFIG_RT_GROUP_SCHED */

static int sched_rt_global_validate(void)
{
	if (sysctl_sched_rt_period <= 0)
		return -EINVAL;

	if ((sysctl_sched_rt_runtime != RUNTIME_INF) &&
		(sysctl_sched_rt_runtime > sysctl_sched_rt_period))
		return -EINVAL;

	return 0;
}

int sched_rt_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos)
{
	int old_period, old_runtime;
	static DEFINE_MUTEX(mutex);
	int ret;

	mutex_lock(&mutex);
	old_period = sysctl_sched_rt_period;
	old_runtime = sysctl_sched_rt_runtime;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);

	if (!ret && write) {
		ret = sched_rt_global_validate();
		if (ret)
			goto undo;

		ret = sched_dl_global_validate();
		if (ret)
			goto undo;

		ret = sched_rt_global_constraints();
		if (ret)
			goto undo;

		sched_dl_do_global();
	}
	if (0) {
undo:
		sysctl_sched_rt_period = old_period;
		sysctl_sched_rt_runtime = old_runtime;
	}
	mutex_unlock(&mutex);

	return ret;
}

int sched_rr_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos)
{
	int ret;
	static DEFINE_MUTEX(mutex);

	mutex_lock(&mutex);
	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	/*
	 * Make sure that internally we keep jiffies.
	 * Also, writing zero resets the timeslice to default:
	 */
	if (!ret && write) {
		sched_rr_timeslice =
			sysctl_sched_rr_timeslice <= 0 ? RR_TIMESLICE :
			msecs_to_jiffies(sysctl_sched_rr_timeslice);
	}
	mutex_unlock(&mutex);

	return ret;
}

#ifdef CONFIG_SCHED_DEBUG
void print_rt_stats(struct seq_file *m, int cpu)
{
}
#endif /* CONFIG_SCHED_DEBUG */
