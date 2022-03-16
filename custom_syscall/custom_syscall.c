#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/time.h>

#include "../kernel/sched/sched.h"




unsigned long timenow(void) {
	struct timespec timecheck;
	getnstimeofday(&timecheck);
	return timecheck.tv_sec * 1000000000 + (long)timecheck.tv_nsec;
	
}


SYSCALL_DEFINE1(struhar_init, long, response_time) {
	trace_printk("XDEBUG:%d:SYSCALL_INIT:response_time=%lld\n", current->pid, response_time);
	current->struhar_exp_response_time = response_time;
}	



void controller(struct task_struct *p) {
	struct rt_rq *rt_rq = rt_rq_of_se(&current->rt);
	struct sched_dl_entity *dl_se = dl_group_of(rt_rq);
	long response_time = timenow()-p->struhar_instance_start;

	trace_printk("XDEBUG:%d:CONTROLLER\n", p->pid);
	trace_printk("XDEBUG:%d:CONTROLLER:struhar_exp_response_time=%lld\n", p->pid, p->struhar_exp_response_time);
	trace_printk("XDEBUG:%d:CONTROLLER:real_response_time=%lld\n", p->pid, response_time);

	// what is current budget?




}




asmlinkage long sys_struhar_start(void) {
	struct task_struct *p;
	struct rt_rq *rt_rq = rt_rq_of_se(&current->rt);
	struct sched_dl_entity *dl_se = dl_group_of(rt_rq);
	struct rq_flags rf;
	struct rq *rq;
	
	

	
	current->struhar_response_time = 0;
	current->struhar_instance_start = timenow();
	
    trace_printk("XDEBUG:%d:SYSCALL_START\n", current->pid);    
    return 0;
}





asmlinkage long sys_struhar_done(void) {
	struct task_struct *p;
	struct rt_rq *rt_rq = rt_rq_of_se(&current->rt);
	struct sched_dl_entity *dl_se = dl_group_of(rt_rq);
	struct rq_flags rf;
	struct rq *rq;
	long response_time = timenow()-current->struhar_instance_start;

	//p = current;
	//current->struhar_response_time = 0;
	trace_printk("XDEBUG::%d:RESPONSE_TIME:response=%lld\n", response_time);
    trace_printk("XDEBUG:%d:SYSCALL_DONE\n", current->pid);
    
	controller(current);


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
