#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/time.h>
#include <linux/math64.h>

#include "../kernel/sched/sched.h"

#define TOTAL_BUDGET 10000000
#define DIST_EXCEEDED 40
#define RES_EXCEEDED 1
#define HISTORY_LEN 1000
#define LAST_VALUES 10

#define QOC_HIGH 20
#define QOC_NORMAL 100
#define QOC_LOW 150

#define PERIOD_VERY_HIGH_NS 200000000
#define PERIOD_HIGH_NS 300000000
#define PERIOD_NORMAL_NS 500000000
#define PERIOD_LOW_NS 800000000


#define RUNTIME_VERY_HIGH base_runtime * 120 / 100
#define RUNTIME_HIGH_NS base_runtime * 110 / 100
#define RUNTIME_NORMAL_NS base_runtime * 100 / 100
#define RUNTIME_LOW_NS base_runtime * 50 / 100


unsigned long timenow(void) {
	struct timespec timecheck;
	getnstimeofday(&timecheck);
	return timecheck.tv_sec * 1000000000 + (long)timecheck.tv_nsec;
}

long long total_budget = TOTAL_BUDGET;


struct History_item {
	long time;
	long value;
	int used;
};


struct History_item history[HISTORY_LEN];
int history_cnt;
long base_runtime;

long compute_QoC(int last_values) {
	long res = 0;
	int pointer = history_cnt;
	int i;

	for (i = 0; i < last_values; i++) {
		pointer--;

		if (pointer < 0) {
			pointer = HISTORY_LEN-1; 
		} 

		if (history[pointer].used == 0) {
			res += history[pointer].value;
		}
	}


	return res;
}


void add_history(long value) {
	//struct timeval tv;
	//do_gettimeofday(&tv);

	history[history_cnt % HISTORY_LEN].value = value,
	history[history_cnt % HISTORY_LEN].used = 0;
	//history[history_cnt].time = 
	history_cnt++;
}




SYSCALL_DEFINE1(struhar_init, long, response_time) {
	trace_printk("XDEBUG:%d:struhar_init:response_time=%lld\n", current->pid, response_time);
	struct rt_rq *rt_rq = rt_rq_of_se(&current->rt);
	struct sched_dl_entity *dl_se = dl_group_of(rt_rq);
	int i;
	total_budget = TOTAL_BUDGET;
	history_cnt = 0;
	

	for(i = 0; i < HISTORY_LEN; i++) {
		history[i].used = -1;
	}

	base_runtime = dl_se->dl_runtime;
	current->struhar_exp_response_time = response_time;
}

SYSCALL_DEFINE2(struhar_xcontrol, long, id, long, total_budget) {
	trace_printk("XDEBUG:%d:struhar_xcontrol:id=%lld:total_budget=%lld\n", current->pid, id, total_budget);
}

SYSCALL_DEFINE2(struhar_done2, long, expected, long, actual) {
	trace_printk("XDEBUG:%d:struhar_done2:expected=%ld:actual=%ld\n", current->pid, expected, actual);
	long response_time = timenow()-current->struhar_instance_start;
	long QoC = 0;
	long err = abs(expected - actual);
	long period;
	struct rt_rq *rt_rq = rt_rq_of_se(&current->rt);
	struct sched_dl_entity *dl_se = dl_group_of(rt_rq);
	struct sched_dl_entity *pi_se = &current->dl;

	add_history(err);
	QoC = compute_QoC(LAST_VALUES);

	trace_printk("XDEBUG:%d:struhar_done2_pi_se:pi_se=%ld\n", current->pid, pi_se->dl_runtime);
	trace_printk("XDEBUG:%d:struhar_done2_err:err=%ld\n", current->pid, err);
	trace_printk("XDEBUG:%d:struhar_done2_qoc:qoc=%ld\n", current->pid, QoC);
	trace_printk("XDEBUG:%d:struhar_done2_response_time:response_time=%ld\n", current->pid, response_time);
	

	trace_printk("XDEBUG:%d:truhar_done2_runtime:runtime=%lld\n", current->pid, dl_se->dl_runtime);


	


	/*
		#define QOC_HIGH 10
	#define QOC_NORMAL 100
	#define QOC_LOW 1000

	#define PERIOD_HIGH_NS 300000000
	#define PERIOD_NORMAL_NS 500000000
	#define PERIOD_LOW_NS 800000000
*/


	if (QoC < QOC_HIGH) {
		period = PERIOD_LOW_NS;
		dl_se->dl_runtime = RUNTIME_LOW_NS;
	} else if (QoC < QOC_NORMAL) {
		period = PERIOD_NORMAL_NS;
		dl_se->dl_runtime = RUNTIME_NORMAL_NS;
	} else if (QoC < QOC_LOW) {
		period = PERIOD_HIGH_NS;
		dl_se->dl_runtime = RUNTIME_HIGH_NS;
	} else {
		period = PERIOD_VERY_HIGH_NS;
		dl_se->dl_runtime = RUNTIME_VERY_HIGH;
	}
	
	trace_printk("XDEBUG:%d:truhar_done2_newruntime:runtime=%lld\n", current->pid, dl_se->dl_runtime);	
	trace_printk("XDEBUG:%d:struhar_done2_period:period=%ld\n", current->pid, period);

	return period;
}

void node_control(struct task_struct *p) {
	trace_printk("XDEBUG:%d:node_controller:pidA=%lld:pidB=%lld\n", 
		current->pid, 
		current->node_controller->containerA_pid, 
		current->node_controller->containerB_pid);
}


long container_controller(struct task_struct *p) {
	
	return 0;
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
	long disturbance = current->struhar_exp_response_time - response_time;
	
	current->struhar_job_instance += 1;
	
  trace_printk("XDEBUG:%d:SYSCALL_DONE\n", current->pid);
  
  return 0;
}
