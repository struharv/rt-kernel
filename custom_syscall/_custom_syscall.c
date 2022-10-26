#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/time.h>
#include <linux/math64.h>

#include "../kernel/sched/sched.h"




unsigned long timenow(void) {
	struct timespec timecheck;
	getnstimeofday(&timecheck);
	return timecheck.tv_sec * 1000000000 + (long)timecheck.tv_nsec;
	
}


SYSCALL_DEFINE1(struhar_init, long, response_time) {
	trace_printk("XDEBUG:%d:SYSCALL_INIT:response_time=%lld\n", current->pid, response_time);
	trace_printk("XDEBUG:%d:SYSCALL_TEST:response_time=%lld\n", current->pid, response_time);
	current->struhar_exp_response_time = response_time;
}	

SYSCALL_DEFINE2(struhar_xcontrol, long, id, long, total_budget) {
	trace_printk("XDEBUG:%d:struhar_xcontrol:id=%lld:total_budget=%lld\n", current->pid, id, total_budget);
	//current->struhar_exp_response_time = response_time;
}

void node_control(void) {
	//
}



void controller(struct task_struct *p) {
	struct rt_rq *rt_rq = rt_rq_of_se(&current->rt);
	struct sched_dl_entity *dl_se = dl_group_of(rt_rq);
	long response_time = timenow()-p->struhar_instance_start;

	

	
	node_control();
	//
	struct struhar_node_controller *node_controller;
	node_controller = p->node_controller;




	long runtime_max = 18000000;
	long runtime_min = 2000000; //2 ms


	trace_printk("XDEBUG:%d:CONTROLLER\n", p->pid);
	trace_printk("XDEBUG:%d:CONTROLLER:struhar_exp_response_time=%lld\n", p->pid, p->struhar_exp_response_time);
	trace_printk("XDEBUG:%d:CONTROLLER:real_response_time=%lld\n", p->pid, response_time);

	double K = 10;
	s64 error = response_time-p->struhar_exp_response_time;
	s64 res = div_s64(error, 100); 


	dl_se->dl_runtime += res;
	

	trace_printk("XDEBUG:%d:CONTROLLER:error=%lld:dl_runtime=%lld:div=%lld\n", p->pid, error, dl_se->dl_runtime, res);

	trace_printk("XDEBUG:%d:CONTROLLER:xnew-runtime=%lld\n", p->pid, dl_se->dl_runtime);	
	dl_se->dl_runtime = min(max(dl_se->dl_runtime, runtime_min), runtime_max);
	trace_printk("XDEBUG:%d:CONTROLLER:new-runtime=%lld\n", p->pid, dl_se->dl_runtime);	

	// I need some space shared between processes
	


	//if (response_time > p->struhar_exp_response_time) {
	//if (response_time > p->struhar_exp_response_time)
	//dl_se->dl_runtime += 100000;


	//if (response_time < )

	/*if (dl_se->dl_runtime > runtime_max)
	{
		dl_se->dl_runtime = runtime_min;	
	}*/

	//} else {
	//	dl_se->dl_runtime -= 1000000;

	//}
	
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
	current->struhar_job_instance += 1;
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
