#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/time.h>
#include <linux/math64.h>

#include "../kernel/sched/sched.h"

#define TOTAL_BUDGET 5000000

unsigned long timenow(void) {
	struct timespec timecheck;
	getnstimeofday(&timecheck);
	return timecheck.tv_sec * 1000000000 + (long)timecheck.tv_nsec;
	
}

long long total_budget = TOTAL_BUDGET;

SYSCALL_DEFINE1(struhar_init, long, response_time) {
	trace_printk("XDEBUG:%d:struhar_init:response_time=%lld\n", current->pid, response_time);

	current->struhar_exp_response_time = response_time;
	if (current->node_controller->containerA_pid != 0 && current->node_controller->containerB_pid != 0) {
		// reset
		trace_printk("XDEBUG:%d:struhar_init-restart\n", current->pid);		
		current->node_controller->containerA_pid = 0;
		current->node_controller->containerB_pid = 0;
		total_budget = TOTAL_BUDGET;
	}


	if (current->node_controller->containerA_pid == 0) {
		current->node_controller->containerA_pid = current->pid;
		
		trace_printk("XDEBUG:%d:nodes_pid_pidA:id1=%lld\n", current->pid, current->node_controller->containerA_pid);
	} else {
		current->node_controller->containerB_pid = current->pid;
		
		trace_printk("XDEBUG:%d:nodes_pid_pidB:id1=%lld\n", current->pid, current->node_controller->containerB_pid);

	}

	trace_printk("XDEBUG:%d:nodes_pid:id1=%lld:id2=%lld\n", 
		current->pid, 
		current->node_controller->containerA_pid, 
		current->node_controller->containerB_pid);
}	

SYSCALL_DEFINE2(struhar_xcontrol, long, id, long, total_budget) {
	trace_printk("XDEBUG:%d:struhar_xcontrol:id=%lld:total_budget=%lld\n", current->pid, id, total_budget);
}

void node_control(struct task_struct *p) {
	trace_printk("XDEBUG:%d:node_controller:pidA=%lld:pidB=%lld\n", 
		current->pid, 
		current->node_controller->containerA_pid, 
		current->node_controller->containerB_pid);

	struct rt_rq *rt_rq = rt_rq_of_se(&current->rt);
	struct sched_dl_entity *dl_se = dl_group_of(rt_rq);
	long response_time = timenow()-p->struhar_instance_start;

	if (current->node_controller->containerA_pid == 0 ||
		current->node_controller->containerB_pid == 0) {
		trace_printk("XDEBUG:%dnode_controller-nothing-to-control\n", p->pid);
		return;
	}

	//u64
	// control logic
	/*if (current->struhar_exp_response_time == -1) {
		trace_printk("XDEBUG:%d:NODE-CONTROLLER-NEW\n", p->pid);
		return;
	}*/


	if (total_budget > 0) {
		trace_printk("XDEBUG:%d:NODE-CONTROLLER:runtime=%lld\n", p->pid, dl_se->dl_runtime);
		int portion = total_budget * 5 / 100;
		total_budget -= portion;

		dl_se->dl_runtime += portion;  //0.95 
		trace_printk("XDEBUG:%d:NODE-CONTROLLER:xnew-runtime=%lld\n", p->pid, dl_se->dl_runtime);
	}

}


void container_controller(struct task_struct *p) {
	struct rt_rq *rt_rq = rt_rq_of_se(&current->rt);
	struct sched_dl_entity *dl_se = dl_group_of(rt_rq);
	long response_time = timenow()-p->struhar_instance_start;

	if (current->struhar_exp_response_time == -1) {
		trace_printk("XDEBUG:%d:CONTROLLER-NEW-DISABLED\n", p->pid);
		return;
	}

	long disturbance = current->struhar_exp_response_time - response_time;

	trace_printk("XDEBUG:%d:CONTROLLER-NEW\n", p->pid);
	trace_printk("XDEBUG:%d:CONTROLLER\n", p->pid);
	trace_printk("XDEBUG:%d:CONTROLLER:struhar_exp_response_time=%lld\n", p->pid, p->struhar_exp_response_time);
	trace_printk("XDEBUG:%d:CONTROLLER:real_response_time=%lld\n", p->pid, response_time);
	trace_printk("XDEBUG:%d:CONTROLLER:disturbance=%lld\n", p->pid, disturbance);
	trace_printk("XDEBUG:%d:CONTROLLER:runtime=%lld\n", p->pid, dl_se->dl_runtime);

	if (disturbance > 0) { // lower budget is needed
		
		//dl_se->dl_runtime = dl_se->dl_runtime - disturbance/100;  //0.95


		if (dl_se->dl_runtime < 5000000) {
			dl_se->dl_runtime = 5000000;
		}
		// trace_printk("XDEBUG:%d:CONTROLLER:xnew-runtime=%lld\n", p->pid, dl_se->dl_runtime);
	} else{
		
		//dl_se->dl_runtime = dl_se->dl_runtime - disturbance/100;
	}



	trace_printk("XDEBUG:%d:CONTROLLER:xnew-runtime=%lld\n", p->pid, dl_se->dl_runtime);
	
	/*
	if (current->struhar_exp_response_time > response_time) {
		trace_printk("XDEBUG:%d:CONTROLLER:runtime=%lld\n", p->pid, dl_se->dl_runtime);
		dl_se->dl_runtime = dl_se->dl_runtime * 95 / 100;  //0.95
		if (dl_se->dl_runtime < 5000000) {
			dl_se->dl_runtime = 5000000;
		}
		trace_printk("XDEBUG:%d:CONTROLLER:xnew-runtime=%lld\n", p->pid, dl_se->dl_runtime);
	} else {
		trace_printk("XDEBUG:%d:CONTROLLER:runtime=%lld\n", p->pid, dl_se->dl_runtime);
		dl_se->dl_runtime = dl_se->dl_runtime * 105 / 100;  //1.05 
		if (dl_se->dl_runtime < 22000000) {
			dl_se->dl_runtime = 22000000;
		}
		trace_printk("XDEBUG:%d:CONTROLLER:xnew-runtime=%lld\n", p->pid, dl_se->dl_runtime);
	}*/

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
	
    trace_printk("XDEBUG:%d:SYSCALL_DONE\n", current->pid);
    trace_printk("XDEBUG:%d:RESPONSE_TIME:response=%lld:budget=%lld:target_response_time=%lld:now=%ld\n", 
    	current->pid, 
    	response_time, 
    	dl_se->dl_runtime,
    	current->struhar_exp_response_time,
    	timenow());
    
    printk("XDEBUG:%d:RESPONSE_TIME:response=%lld:budget=%lld:target_response_time=%lld:now=%ld\n", 
    	current->pid, 
    	response_time, 
    	dl_se->dl_runtime,
    	current->struhar_exp_response_time,
    	timenow());

	//container_controller(current);
	node_control(current);

	
	
    return 0;
}
