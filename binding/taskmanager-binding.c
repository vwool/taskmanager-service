#include <proc/readproc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <proc/sysinfo.h>
#include <math.h>
#include <unistd.h>
#include <json-c/json.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>

#define AFB_BINDING_VERSION 2
#include <afb/afb-binding.h>

struct pstat {
	unsigned long long utime_ticks;
	unsigned long long cutime_ticks;
	unsigned long long stime_ticks;
	unsigned long long cstime_ticks;
	unsigned long long cpu_total_time;
};

struct cpu_percentage {
	double ucpu_usage;
	double scpu_usage;
};

struct process_container {
	char *process_name;
	int euid;
	struct pstat pstat_values;
};

void get_process_list(struct afb_req request);
int fill_pstat(proc_t *proc_info, struct pstat *pstat);
void cpu_calculate(struct pstat*, struct pstat*, struct cpu_percentage *perc);
void fill_process_container(char* process_name, int euid, struct pstat *pstat_values, struct process_container *pc);

void get_process_list(struct afb_req request){

	struct pstat last_pstat_values, cur_pstat_values;
	struct cpu_percentage cpu_usage;
	struct process_container *process_obj, *elem = NULL;
	struct process_container *object_container[65535]; // array holding process objects
	struct json_object *ret_json, *json_array, *json_obj;
	ret_json = json_object_new_object();
	json_array = json_object_new_array();
	char state_str[2] = "\0";
	int i, ret;

	PROCTAB* proc = openproc(PROC_FILLMEM | PROC_FILLSTAT);
	if (!proc){
		AFB_REQ_ERROR(request, "Unable to open /proc!");
		afb_req_fail(request, "Failed", "Error processing arguments.");
		return;
	}

	memset(object_container, 0, sizeof(*object_container));
	proc_t* proc_info;
	while ((proc_info = readproc(proc, NULL)) != NULL) {
		ret = fill_pstat(proc_info, &last_pstat_values);
		if (ret < 0) {
			AFB_REQ_ERROR(request, "fill_pstat failed");
			continue;
		}
		process_obj = malloc(sizeof(*process_obj));
		if (process_obj == NULL) {
			AFB_REQ_ERROR(request, "allocation of process_obj failed");
			continue;
		}
		fill_process_container(proc_info->cmd, proc_info->euid, &last_pstat_values, process_obj);
		object_container[proc_info->tid] = process_obj;
		freeproc(proc_info);
	}

	sleep(1);

	closeproc(proc);
	proc = openproc(PROC_FILLMEM | PROC_FILLSTAT);
	if (!proc){
		AFB_REQ_ERROR(request, "Unable to open /proc!");
		afb_req_fail(request, "Failed", "Error processing arguments.");
		return;
	}

	while ((proc_info = readproc(proc, NULL)) != NULL) {
		ret = fill_pstat(proc_info, &cur_pstat_values);
		if (ret < 0) {
			AFB_REQ_ERROR(request, "fill_pstat failed");
			continue;
		}
		elem = object_container[proc_info->tid];
		if (elem) {
			cpu_calculate(&elem->pstat_values, &cur_pstat_values, &cpu_usage);
			json_obj = json_object_new_object();
			json_object_object_add(json_obj, "cmd", json_object_new_string(proc_info->cmd));
			json_object_object_add(json_obj, "tid", json_object_new_int(proc_info->tid));
			json_object_object_add(json_obj, "euid", json_object_new_int(proc_info->euid));
			json_object_object_add(json_obj, "scpu", json_object_new_double(cpu_usage.scpu_usage));
			json_object_object_add(json_obj, "ucpu", json_object_new_double(cpu_usage.ucpu_usage));
			json_object_object_add(json_obj, "resident_mem", json_object_new_double((proc_info->resident * getpagesize())/ pow(1024, 2)));
			state_str[0] = proc_info->state;
			json_object_object_add(json_obj, "state", json_object_new_string(state_str));
			json_object_array_add(json_array, json_obj);
		}
		freeproc(proc_info);
	}
	json_object_object_add(ret_json, "processes", json_array);
	afb_req_success(request, ret_json, NULL);

	closeproc(proc);
}

void kill_process(struct afb_req request)
{
	struct json_object *ret_json;
	ret_json = json_object_new_object();
	json_object *queryJ = afb_req_json(request);
	int tid = json_object_get_int(queryJ);
	int ret;

	AFB_REQ_INFO(request, "killing %d\n", tid);
	/* XXX: add checks */
	ret = kill(tid, SIGTERM);
	if (ret < 0)
		afb_req_fail_f(request, "Failed", "Error %d", errno);
	/* we don't signal success, there's no use for it */
}


int fill_pstat(proc_t *proc_info, struct pstat *pstat_values)
{
	long unsigned int cpu_time[9];

	pstat_values->utime_ticks = proc_info->utime;
	pstat_values->cutime_ticks = proc_info->cutime;
	pstat_values->stime_ticks = proc_info->stime;
	pstat_values->cstime_ticks = proc_info->cstime;

	FILE *fstat = fopen("/proc/stat", "r");
	if (fstat == NULL)
		return -1;

	memset(cpu_time, 0, sizeof(cpu_time));
	fscanf(fstat, "%*s %lu %lu %lu %lu %*lu %lu %lu %lu %lu %lu",
		&cpu_time[0], &cpu_time[1], &cpu_time[2], &cpu_time[3],
		&cpu_time[4], &cpu_time[5], &cpu_time[6], &cpu_time[7],
		&cpu_time[8]);

	fclose(fstat);

	/*
	 * Returns total CPU time. It is a sum of user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice
	 *
	 *
	 */
	pstat_values->cpu_total_time = 0;
	for(int i = 0; i < 9; i++)
		pstat_values->cpu_total_time += cpu_time[i];

	return 0;
}

void cpu_calculate(struct pstat *last_pstat_values, struct pstat *cur_pstat_values, struct cpu_percentage *cpu_values)
{

	long unsigned int total_time_diff = cur_pstat_values->cpu_total_time - last_pstat_values->cpu_total_time;

	cpu_values->ucpu_usage = 100.0 * (((cur_pstat_values->utime_ticks + cur_pstat_values->cutime_ticks)
		- (last_pstat_values->utime_ticks + last_pstat_values->cutime_ticks)) / (double) total_time_diff);

	cpu_values->scpu_usage = 100.0 * (((cur_pstat_values->stime_ticks + cur_pstat_values->cstime_ticks)
		- (last_pstat_values->stime_ticks + last_pstat_values->cstime_ticks)) / (double) total_time_diff);
}

void fill_process_container(char *process_name, int euid, struct pstat *pstat_values, struct process_container *pc)
{
	pc->process_name = process_name;
	pc->euid = euid;
	pc->pstat_values = *pstat_values;
}

static const struct afb_verb_v2 _afb_verbs_v2_taskmanager[] = {
	{
		.verb = "get_process_list",
		.callback = get_process_list,
		.auth = NULL,
		.info = "Get an array of all processes currently running on the system",
		.session = AFB_SESSION_NONE_V2
	},
	{
		.verb = "kill_process",
		.callback = kill_process,
		.auth = NULL,
		.info = "Kill the process specified by tid",
		.session = AFB_SESSION_NONE_V2
	}
};


const struct afb_binding_v2 afbBindingV2 = {
    .api = "taskmanager",
    .specification = NULL,
    .info = "Task Manager service",
    .verbs = _afb_verbs_v2_taskmanager,
    .noconcurrency = 0
};

/*UI issues a web socket request to the binding.
  which means calling the callback.
  Token for afb-client-demo API is hello/1234 or system dunit file.
*/
