#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/syscalls.h>

#include "../kernel/sched/sched.h"



asmlinkage long sys_struhar_hello(void) {
	struct task_struct *p;
	struct rt_rq *rt_rq = rt_rq_of_se(&current->rt);
	struct sched_dl_entity *dl_se = dl_group_of(rt_rq);
	struct rq_flags rf;
	struct rq *rq;
	
	p = current;
    printk("Vaclav!!!! %d %lld\n", p->pid, dl_se->runtime);
    trace_printk("struharv START");
    dl_se->runtime = 0;

   //	dequeue_dl_entity(dl_se);

	/*if (likely(start_dl_timer(dl_se))) {
			dl_se->dl_throttled = 1;
			printk("update_curr_rt: group exhousted budget -> throttled");
	}
		else {
			printk("update_curr_rt: group exhousted budget -> enqueue_dl_entity");
			enqueue_dl_entity(dl_se, dl_se,
						  ENQUEUE_REPLENISH);
		}*/

	rq = task_rq_lock(p, &rf);
	p->sched_class->update_curr(rq);
	task_rq_unlock(rq, p, &rf);


    return 0;
}
