#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/syscalls.h>

#include "../kernel/sched/sched.h"


asmlinkage long sys_struhar_start(void) {
	struct task_struct *p;
	struct rt_rq *rt_rq = rt_rq_of_se(&current->rt);
	struct sched_dl_entity *dl_se = dl_group_of(rt_rq);
	struct rq_flags rf;
	struct rq *rq;
	
    trace_printk("XDEBUG:%d:SYSCALL START\n", current->pid);    
    return 0;
}


asmlinkage long sys_struhar_done(void) {
	struct task_struct *p;
	struct rt_rq *rt_rq = rt_rq_of_se(&current->rt);
	struct sched_dl_entity *dl_se = dl_group_of(rt_rq);
	struct rq_flags rf;
	struct rq *rq;
	
	p = current;

    trace_printk("XDEBUG:%d:SYSCALL DONE\n", current->pid);
    /*dl_se->runtime = 0; 
    dl_se->struhar = 1;
    
   

	rq = task_rq_lock(p, &rf);
	dequeue_dl_entity(dl_se);
    start_dl_timer(dl_se);
	dl_se->dl_throttled = 1;
	resched_curr(rq);
	task_rq_unlock(rq, p, &rf);*/
	
	
    return 0;
}
