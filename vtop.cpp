
/*
 * Copyright (C) 2018-2019 VMware, Inc.
 * SPDX-License-Identifier: GPL-2.0
 */
#include <inttypes.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <limits.h>
#include <assert.h>
#include <iostream>
#include <string.h>
#include <random>
#include <vector>
#include <fstream>
#include <sstream>
#include <sys/syscall.h>
#include <unordered_map>
#define PROBE_MODE	(0)
#define DIRECT_MODE	(1)

#define MAX_CPUS	(192)
#define GROUP_LOCAL	(0)
#define GROUP_NONLOCAL	(1)
#define GROUP_GLOBAL	(2)


#define min(a,b)	(a < b ? a : b)
#define LAST_CPU_ID	(min(nr_cpus, MAX_CPUS))


int nr_numa_groups;
int nr_cpus;
int PTHREAD_TASK_AMOUNT=10;
int verbose = 0;
int NR_SAMPLES = 30;
int SAMPLE_US = 10000;
int cpu_group_id[MAX_CPUS];
int cpu_pair_id[MAX_CPUS];
int cpu_tt_id[MAX_CPUS];
int active_cpu_bitmap[MAX_CPUS];
int finished = 0;
int return_pair = 0;
int cpu1 = 0;
int cpu2 = 0;
std::vector<int> task_stack;
std::vector<std::vector<int>> top_stack;
pthread_t worker_tasks[MAX_CPUS];
static size_t nr_relax = 0;
pthread_mutex_t ready_check = PTHREAD_MUTEX_INITIALIZER;
//static size_t nr_tested_cores = 0;

std::random_device rd;
std::default_random_engine e1(rd());
typedef unsigned atomic_t;

void moveThreadtoHighPrio(pid_t tid) {

    std::string path = "/sys/fs/cgroup/hi_prgroup/cgroup.threads";
    std::ofstream ofs(path, std::ios_base::app);
    if (!ofs) {
        std::cerr << "Could not open the file\n";
        return;
    }
    ofs << tid << "\n";
    ofs.close();
}

void moveCurrentThread() {
    pid_t tid;
    tid = syscall(SYS_gettid);
    std::string path = "/sys/fs/cgroup//hi_prgroup/cgroup.procs";
    std::ofstream ofs(path, std::ios_base::app);
    if (!ofs) {
        std::cerr << "Could not open the file\n";
        return;
    }
    ofs << tid << "\n";
    ofs.close();
    struct sched_param params;
    params.sched_priority = sched_get_priority_max(SCHED_RR);
    sched_setscheduler(tid,SCHED_RR,&params);
}

std::string_view get_option(
    const std::vector<std::string_view>& args, 
    const std::string_view& option_name) {
    for (auto it = args.begin(), end = args.end(); it != end; ++it) {
        if (*it == option_name)
            if (it + 1 != end)
                return *(it + 1);
    }
    
    return "";
};


bool has_option(
    const std::vector<std::string_view>& args, 
    const std::string_view& option_name) {
    for (auto it = args.begin(), end = args.end(); it != end; ++it) {
        if (*it == option_name)
            return true;
    }
    
    return false;
};


void setArguments(const std::vector<std::string_view>& arguments) {
    verbose = has_option(arguments, "-v");
    
    auto set_option_value = [&](const std::string_view& option, int& target) {
        if (auto value = get_option(arguments, option); !value.empty()) {
            try {
                target = std::stoi(std::string(value));
            } catch(const std::invalid_argument&) {
                throw std::invalid_argument(std::string("Invalid argument for option ") + std::string(option));
            } catch(const std::out_of_range&) {
                throw std::out_of_range(std::string("Out of range argument for option ") + std::string(option));
            }
        }
    };
    
    set_option_value("-p", PTHREAD_TASK_AMOUNT);
    set_option_value("-s", NR_SAMPLES);
    set_option_value("-u", SAMPLE_US);
    set_option_value("-d", cpu1);
    set_option_value("-i",cpu2);
}


typedef union {
	atomic_t x;
	char pad[1024];
} big_atomic_t __attribute__((aligned(1024)));

typedef struct {
	cpu_set_t cpus;
	atomic_t me;
	atomic_t buddy;
	big_atomic_t *nr_pingpongs;
	atomic_t  **pingpong_mutex;
	int *stoploops;
	pthread_mutex_t* mutex;
    pthread_cond_t* cond;
    int* flag;
} thread_args_t;

static inline uint64_t now_nsec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * ((uint64_t)1000*1000*1000) + ts.tv_nsec;
}

static void common_setup(thread_args_t *args)
{
	if (sched_setaffinity(0, sizeof(cpu_set_t), &args->cpus)) {
		perror("sched_setaffinity");
		exit(1);
	}

	if (args->me == 0) {
		*(args->pingpong_mutex) = (atomic_t*)mmap(0, getpagesize(), PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
		if (*(args->pingpong_mutex) == MAP_FAILED) {
			perror("mmap");
			exit(1);
		}
		*(*(args->pingpong_mutex)) = args->me;
	}

	// ensure both threads are ready before we leave -- so that
	// both threads have a copy of pingpong_mutex.
	pthread_mutex_lock(args->mutex);
    if (*(args->flag)) {
        *(args->flag) = 0;
        pthread_cond_wait(args->cond, args->mutex);
    } else {
        *(args->flag) = 1;
        pthread_cond_broadcast(args->cond);
    }
    pthread_mutex_unlock(args->mutex);
	
}

static void *thread_fn(void *data)
{
	thread_args_t *args = (thread_args_t *)data;
	common_setup(args);
	big_atomic_t *nr_pingpongs = args->nr_pingpongs;
	atomic_t nr = 0;
	atomic_t me = args->me;
	atomic_t buddy = args->buddy;
	int *stop_loops = args->stoploops;
	atomic_t *cache_pingpong_mutex = *(args->pingpong_mutex);
	while (1) {
		
		if (*stop_loops == 1){
			pthread_exit(0);
		}
		if (__sync_bool_compare_and_swap(cache_pingpong_mutex, me, buddy)) {
			++nr;
			if (nr == 500 && me == 0) {
				__sync_fetch_and_add(&(nr_pingpongs->x), 2 * nr);
				nr = 0;
			}
		}
		for (size_t i = 0; i < nr_relax; ++i)
			asm volatile("rep; nop");
		
	}
	return NULL;
}

int measure_latency_pair(int i, int j)
{
	thread_args_t even, odd;
	CPU_ZERO(&even.cpus);
	CPU_SET(i, &even.cpus);
	even.me = 0;
	even.buddy = 1;
	CPU_ZERO(&odd.cpus);
	CPU_SET(j, &odd.cpus);
	odd.me = 1;
	odd.buddy = 0;
    int stop_loops = 0;
    atomic_t* pingpong_mutex = (atomic_t*) malloc(sizeof(atomic_t));;
	big_atomic_t nr_pingpongs;
	even.nr_pingpongs = &nr_pingpongs;
	odd.nr_pingpongs = &nr_pingpongs;
	even.stoploops = &stop_loops;
	odd.stoploops = &stop_loops;
	even.pingpong_mutex = &pingpong_mutex;
	odd.pingpong_mutex = &pingpong_mutex;
	pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
    int wait_for_buddy = 1;

    even.mutex = &wait_mutex;
    odd.mutex = &wait_mutex;
    even.cond = &wait_cond;
    odd.cond = &wait_cond;
    even.flag = &wait_for_buddy;
    odd.flag = &wait_for_buddy;

	__sync_lock_test_and_set(&nr_pingpongs.x, 0);

	pthread_t t_odd, t_even;
	if (pthread_create(&t_odd, NULL, thread_fn, &odd)) {
		printf("ERROR creating odd thread\n");
		exit(1);
	}
	if (pthread_create(&t_even, NULL, thread_fn, &even)) {
		printf("ERROR creating even thread\n");
		exit(1);
	}
	double best_sample = 1./0.;
	uint64_t last_stamp = now_nsec();
	for (size_t sample_no = 0; sample_no < NR_SAMPLES; ++sample_no) {
		usleep(SAMPLE_US);
		atomic_t s = __sync_lock_test_and_set(&nr_pingpongs.x, 0);
		uint64_t time_stamp = now_nsec();
		double sample = (time_stamp - last_stamp) / (double)s;
		uint64_t newtest = time_stamp - last_stamp;
		last_stamp = time_stamp;

	
		
		if ((sample < best_sample && sample != 1.0/0.)||(best_sample==1.0/0.)){
			best_sample = sample;
		}

	}
	stop_loops = 1;
	pthread_join(t_odd, NULL);
	pthread_join(t_even, NULL);
	stop_loops = 0;
	odd.buddy = 0;
	pingpong_mutex = NULL;
	return (int)(best_sample*100);
}

int stick_this_thread_to_core(int core_id) {
   int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
   if (core_id < 0 || core_id >= num_cores)
      return EINVAL;

   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);
   CPU_SET(core_id, &cpuset);

   pthread_t current_thread = pthread_self();    
   return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}


int unpin_thread(){
   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);
   int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
   for(int z = 0;z<num_cores;z++){
   	CPU_SET(z, &cpuset);
   }

   pthread_t current_thread = pthread_self();    
   return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);

}


int get_pair_to_test(){

	bool valid_pair_exists = false;

	for(int i=0;i<LAST_CPU_ID;i++){
		for(int j=0;j<LAST_CPU_ID;j++){
			if(top_stack[i][j] == 0 || top_stack[i][j] == -1){
					valid_pair_exists = true;
					if(top_stack[i][j] ==0 && (active_cpu_bitmap[i]==0 && active_cpu_bitmap[j]==0)){
						top_stack[i][j] == -1;
						return(i * LAST_CPU_ID + j);
					}
			}

		}
	}
	if(valid_pair_exists){
		return -1;
	}

	return -2;
}

int get_latency_class(int latency){
	if(latency<0){
		return 1;
	}

	if(latency< 1000){
		return 2;
	}
	if(latency< 7000){
		return 3;
	}
	
	return 4;
}

void set_latency_pair(int x,int y,int latency_class){
	top_stack[x][y] = latency_class;
	top_stack[y][x] = latency_class;
}

void apply_optimization(int best, int testing_value){
	int i = testing_value%LAST_CPU_ID;
	int j =(testing_value-(testing_value%LAST_CPU_ID))/LAST_CPU_ID;
	int latency_class = get_latency_class(best);
	int sub_rel;
	set_latency_pair(i,j,latency_class);
	for(int x=0;x<LAST_CPU_ID;x++){
		if(x==i){
			continue;
		}
		//get all neightbors of x cpu
		for(int y=0;y<LAST_CPU_ID;y++){
			//get relationship between x cpu and neighbor
			sub_rel = top_stack[y][x];
			
			//compare x cpu and neighbor
			for(int z=0;z<LAST_CPU_ID;z++){
				//validation
				if((top_stack[y][z]<sub_rel && top_stack[y][z]!=0)){
					if(top_stack[x][z] == 0){
						set_latency_pair(x,z,sub_rel);
					}else if(top_stack[x][z]!=top_stack[y][x]){
						//std::cout<<"error"<<std::endl;
						//exit(1);
					}
				}
			}

		}
	}

}


void apply_optimization_recur(int cpu, int last_cpu,int latency_class,std::unordered_map<int,int>& tested_arr){
	tested_arr[cpu] = 1;
	int sub_rel = top_stack[cpu][last_cpu];
	for(int x=0;x<LAST_CPU_ID;x++){
		if(top_stack[last_cpu][x]!=0 && (top_stack[last_cpu][x] < sub_rel && top_stack[cpu][x]==0)){
			set_latency_pair(cpu,x,sub_rel);
		}
	}
	for(int x=0;x<LAST_CPU_ID;x++){
		if((top_stack[cpu][x] != 0 && tested_arr[x] != 1)){
			apply_optimization_recur(x,cpu,latency_class,tested_arr);
		}

	}

}

void apply_optimization1(int best, int testing_value){
	int i = testing_value%LAST_CPU_ID;
	int j =(testing_value-(testing_value%LAST_CPU_ID))/LAST_CPU_ID;
	int latency_class = get_latency_class(best);
	int sub_rel;
	set_latency_pair(i,j,latency_class);

	std::unordered_map<int,int> tested_arr_1;
	std::unordered_map<int,int> tested_arr_2;
	tested_arr_1[i] = 1;
	tested_arr_2[j] = 1;
	for(int x=0;x<LAST_CPU_ID;x++){
		if(top_stack[i][x]<latency_class && top_stack[i][x]!=0){
			set_latency_pair(x,j,latency_class);
		}
		if(top_stack[j][x]<latency_class && top_stack[j][x]!=0){
			set_latency_pair(x,i,latency_class);
		}

	}

	for(int x=0;x<LAST_CPU_ID;x++){
		if((tested_arr_1[x] != 1) && (true && top_stack[i][x]!=0)){
			apply_optimization_recur(x,i,latency_class,tested_arr_1);
		}

		if((tested_arr_2[x] != 1) && top_stack[j][x]!=0){
			apply_optimization_recur(x,j,latency_class,tested_arr_2);
		}
	}
}

static void *thread_fn1(void *data)
{	
	int testing_value;
	int random_index;
	while (1) {

		pthread_mutex_lock(&ready_check);
		
		int me_index = -1;
		

		testing_value = get_pair_to_test();
		
		
		while(testing_value == -1){
			pthread_mutex_unlock(&ready_check);
			usleep(10);
			pthread_mutex_lock(&ready_check);
			testing_value = get_pair_to_test();
		}
		if(testing_value == -2){
                        pthread_mutex_unlock(&ready_check);
                        finished=1;
                        break;
                }


		

		active_cpu_bitmap[testing_value%LAST_CPU_ID] = 1;
		active_cpu_bitmap[(testing_value-(testing_value%LAST_CPU_ID))/LAST_CPU_ID] = 1;
		for(int g=0;g<LAST_CPU_ID;g++){
                        if(active_cpu_bitmap[g] ==0){
                                active_cpu_bitmap[g] = 1;
                                stick_this_thread_to_core(g);
                                me_index=g;
                                break;
                        }
                }

		pthread_mutex_unlock(&ready_check);
		task_stack.pop_back();
		int best = measure_latency_pair(testing_value%LAST_CPU_ID,(testing_value-(testing_value%LAST_CPU_ID))/LAST_CPU_ID);
		pthread_mutex_lock(&ready_check);
		apply_optimization(best,testing_value);
		if(me_index != -1){
			active_cpu_bitmap[me_index] = 0;
			unpin_thread();
		}
		active_cpu_bitmap[testing_value%LAST_CPU_ID] = 0;
		active_cpu_bitmap[(testing_value-(testing_value%LAST_CPU_ID))/LAST_CPU_ID] = 0;
		pthread_mutex_unlock(&ready_check);
		
		std::cout << "myvector stores " << int(task_stack.size()) << " numbers.\n"<<"X Val "<<testing_value%LAST_CPU_ID<<"Y VaL"<<(testing_value-(testing_value%LAST_CPU_ID))/LAST_CPU_ID <<" Sample passed: "<< best<< "   ";
		
	}
	return NULL;
}

static void populate_latency_matrix(void)
{
	int i, j;
	nr_cpus = get_nprocs();
	for (i = 0; i < LAST_CPU_ID; i++) {
		active_cpu_bitmap[i] = 0;
		std::vector<int> cpumap(LAST_CPU_ID);
		top_stack.push_back(cpumap);

		for (j = i + 1; j < LAST_CPU_ID; j++) {
			task_stack.push_back(LAST_CPU_ID * i + j);
		}
	}
	for(int p=0;p< LAST_CPU_ID;p++){
		top_stack[p][p] = 1;
	}


	for (i = 0; i < PTHREAD_TASK_AMOUNT; i++) {
		std::cout << "pthread task amount  " << PTHREAD_TASK_AMOUNT << " numbers.\n";
		thread_args_t newtest;
		pthread_create(&worker_tasks[i], NULL, thread_fn1, &newtest);
	}
	std::cout << "myvector stores " << int(task_stack.size()) << " numbers.\n";
	while(finished == 0){
		sleep(0.5);
	}
	for (int i = 0; i < PTHREAD_TASK_AMOUNT; i++) {
    	pthread_join(worker_tasks[i], NULL);
  	}

}

static void print_population_matrix(void)
{
	int i, j;

	for (i = 0; i < LAST_CPU_ID; i++) {
		for (j = 0; j < LAST_CPU_ID; j++)
			printf("%7d", (int)(top_stack[i][j]));
		printf("\n");
	}
}

static double get_min_latency(int cpu, int group)
{
	int j;
	double min = INT_MAX;

	for (j = 0; j < LAST_CPU_ID; j++) {
		if (top_stack[cpu][j] == 0)
			continue;

		/* global check */
		if (group == GROUP_GLOBAL && top_stack[cpu][j] < min)
			min = top_stack[cpu][j];

		/* local check */
		if (group == GROUP_LOCAL && cpu_group_id[cpu] == cpu_group_id[j]
			&& top_stack[cpu][j] < min)
			min = top_stack[cpu][j];

		/* non-local check */
		if (group == GROUP_NONLOCAL && cpu_group_id[cpu] != cpu_group_id[j]
			&& top_stack[cpu][j] < min)
			min = top_stack[cpu][j];
	}

	return min == INT_MAX ? 0 : min;
}


static double get_min2_latency(int cpu, int group, double val)
{
	int j;
	double min = INT_MAX;

	for (j = 0; j < LAST_CPU_ID; j++) {
		if (top_stack[cpu][j] == 0)
			continue;

		/* global check */
		if (group == GROUP_GLOBAL && top_stack[cpu][j] < min && top_stack[cpu][j] > val)
			min = top_stack[cpu][j];

		/* local check */
		if (group == GROUP_LOCAL && cpu_group_id[cpu] == cpu_group_id[j]
			&& top_stack[cpu][j] < min && top_stack[cpu][j] > val)
			min = top_stack[cpu][j];

		/* non-local check */
		if (group == GROUP_NONLOCAL && cpu_group_id[cpu] != cpu_group_id[j]
			&& top_stack[cpu][j] < min && top_stack[cpu][j] > val)
			min = top_stack[cpu][j];
	}

	return min == INT_MAX ? 0 : min;
}

static double get_max_latency(int cpu, int group)
{
	int j;
	double max = -1;

	for (j = 0; j < LAST_CPU_ID; j++) {
		if (top_stack[cpu][j] == 0)
			continue;

		/* global check */
		if (group == GROUP_GLOBAL && top_stack[cpu][j] > max)
			max = top_stack[cpu][j];

		/* local check */
		if (group == GROUP_LOCAL && cpu_group_id[cpu] == cpu_group_id[j]
			&& top_stack[cpu][j] > max)
			max = top_stack[cpu][j];

		/* non-local check */
		if (group == GROUP_NONLOCAL && cpu_group_id[cpu] != cpu_group_id[j]
			&& top_stack[cpu][j] > max)
			max = top_stack[cpu][j];
	}

	return max == -1 ? INT_MAX : max;
}

/*
 * For proper assignment, the following invariant must hold:
 * The maximum latency between two CPUs in the same group (any group)
 * should be less than the minimum latency between any two CPUs from
 * different groups.
 */
static void validate_group_assignment()
{
	int i;
	double local_max = 0, nonlocal_min = INT_MAX;

	for (i = 0; i < LAST_CPU_ID; i++) {
		local_max = get_max_latency(i, GROUP_LOCAL);
		nonlocal_min = get_min_latency(i, GROUP_NONLOCAL);
		if (local_max == INT_MAX || nonlocal_min == 0)
			continue;

		if(local_max > 1.10 * nonlocal_min) {
			printf("FAIL!!!\n");
			printf("local max is bigger than NonLocal min for CPU: %d %d %d\n",
							i, (int)local_max, (int)nonlocal_min);
			exit(1);
		}
	}
	printf("PASS!!!\n");
}


static void construct_vnuma_groups(void)
{
	int i, j, count = 0;
	int nr_numa_groups = 0;
	int nr_pair_groups = 0;
	int nr_tt_groups = 0;
	double min, min_2;
	nr_cpus = get_nprocs();
	/* Invalidate group IDs */
	for (i = 0; i < LAST_CPU_ID; i++){
		cpu_group_id[i] = -1;
		cpu_pair_id[i] = -1;
		cpu_tt_id[i] = -1;
	}


	for (i = 0; i < LAST_CPU_ID; i++) {
		
		if (cpu_group_id[i] == -1){
			cpu_group_id[i] = nr_numa_groups;
			nr_numa_groups++;
		}
		if (cpu_pair_id[i] == -1){
			cpu_pair_id[i] = nr_pair_groups;
			nr_pair_groups++;
		}
		if (cpu_tt_id[i] == -1){
			cpu_tt_id[i] = nr_tt_groups;
			nr_tt_groups++;
		}

		
		for (j = 0 ; j < LAST_CPU_ID; j++) {
				if (top_stack[i][j]<4 && cpu_group_id[i] != -1){
					cpu_group_id[j] = cpu_group_id[i];
				}
				if (top_stack[i][j]<3 && cpu_pair_id[i] != -1){
					cpu_pair_id[j] = cpu_pair_id[i];
				}
				if (top_stack[i][j]<2 && cpu_tt_id[i] != -1){
					cpu_tt_id[j] = cpu_tt_id[i];
				}
		}
	}
	printf("%d ", nr_numa_groups);	
	printf("%d ", nr_pair_groups);	
	printf("%d ", nr_tt_groups);	

	for (j = 0; j < LAST_CPU_ID; j++){
		printf("%d ", cpu_group_id[j]);	
		printf("%d ", cpu_pair_id[j]);	
		printf("%d ", cpu_tt_id[j]);	
	}
	
}

#define CPU_ID_SHIFT		(16)
/*
 * %4 is specific to our platform.
 */
#define CPU_NUMA_GROUP(mode, i)	(mode == PROBE_MODE ? cpu_group_id[i] : i % 4)
static void configure_os_numa_groups(int mode)
{
	int i;
	unsigned long val;

	/*
	 * pass vcpu & numa group id in a single word using a simple encoding:
	 * first 16 bits store the cpu identifier
	 * next 16 bits store the numa group identifier
	 * */
	for(i = 0; i < LAST_CPU_ID; i++) {
		/* store cpu identifier and left shift */
		val = i;
		val = val << CPU_ID_SHIFT;
		/* store the numa group identifier*/
		val |= CPU_NUMA_GROUP(mode, i);
	}
}


int main(int argc, char *argv[])
{
	moveCurrentThread();
	int nr_pages = 0;
	const std::vector<std::string_view> args(argv, argv + argc);
  	setArguments(args);
	uint64_t popul_laten_last = now_nsec();
	populate_latency_matrix();
	int result = measure_latency_pair(cpu1,cpu2);
	std::cout<<"Result of 2CPUS:"<<result<<std::endl;
	uint64_t popul_laten_now = now_nsec();
	printf("This time it took for latency matrix to be populated%lf\n", (popul_laten_now-popul_laten_last)/(double)1000000);
	if (verbose)
		print_population_matrix();
	printf("constructing NUMA groups...\n");
	popul_laten_last = now_nsec();
	construct_vnuma_groups();
	popul_laten_now = now_nsec();
	printf("This time it took for NUma groups to be contstructed%lf\n", (popul_laten_now-popul_laten_last)/(double)1000000);
	popul_laten_last = now_nsec();
	printf("validating group assignment...");
	validate_group_assignment();
	popul_laten_now = now_nsec();
	printf("This time it took for group assignment to be verified%lf\n", (popul_laten_now-popul_laten_last)/(double)1000000);

	configure_os_numa_groups(1);
	printf("Done...\n");
}
