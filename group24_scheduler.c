/*
 * Lab 4 - Scheduler
 *
 * Authors: Zeyad Ghazal, Avam Neupane, Omar Ahmed, Horace Zeng
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "queue.h"

/*
 * Assumptions / Lab rules:
 * - user priority values are 0,1,2,3 (0 = Real-Time, 1..3 = user queues)
 * - all processes have mem_req > 0
 * - RT process memory is ALWAYS 64MB (reserved); user processes share 960MB
 * - user memory allocation range is [64, 1023], integer MB, contiguous blocks
 * - continuous allocation policy: First Fit (per modified handout)
 * - Processes are sorted by arrival_time in the test files
 */

/* ----------------------------
 * Global “hardware resources”
 * ---------------------------- */
int printers = 2;
int scanners = 1;
int modems = 1;
int cd_drives = 2;

/* Total user-available memory (excluding RT reserved region) */
int memory = 960;
int memory_real_time = 64;

/* ----------------------------
 * Ready queues (provided by queue.h / queue.c)
 * ---------------------------- */
queue_t rt_queue;       /* real-time queue */
queue_t sub_queue;      /* submission queue (user processes wait here until admitted) */
queue_t user_queue[3];  /* user queues: index 0 for priority 1, index 1 for priority 2, index 2 for priority 3 */


/* ----------------------------
 * Optional memory block-list type (NOT mandatory to use)
 * ----------------------------
 * You are NOT required to use a free-block list. You may implement memory using
 * any data structure (array/bitmap/list/etc.) as long as:
 *   - it behaves exactly like First Fit defined in the requirement document
 *   - it returns correct starting addresses (mem_start) for user processes
 */
typedef struct free_block free_block_t;
free_block_t *freelist; /* optional: head pointer if you choose block-list approach */

#define MAX_PROCESSES 128

/* ----------------------------
 * Process state and process struct
 * ---------------------------- */
typedef enum {
    NEW,        /* read in, but not yet arrived (time < arrival_time) */
    SUBMITTED,  /* arrived; in submission queue waiting for ADMIT */
    READY,      /* admitted; in a user queue or RT queue, waiting to run */
    RUNNING,    /* currently running for this tick */
    TERMINATED  /* finished execution */
} proc_state_t;

typedef struct process {
    /* identity */
    int pid;

    /* input fields*/
    int arrival_time;
    int init_prio;   /* 0 = RT, 1..3 = user priority */
    int cpu_total;   /* initial CPU time requested */
    int mem_req;     /* requested memory in MB */
    int printers;
    int scanners;
    int modems;
    int cds;

    /* runtime fields */
    int cpu_remain;      /* remaining CPU time */
    int current_prio;    /* current user priority (1..3); RT stays 0 */
    proc_state_t state;

    /* memory allocation result (must be set for user processes after admission) */
    int mem_start;       /* starting address (MB) of allocated contiguous memory block */
} process_t;

/* Optional free block struct (only used if you choose block-list approach) */
typedef struct free_block {
    int start;                  /* start address (MB) */
    int size;                   /* block size (MB) */
    struct free_block *next;    /* next free block in ascending start order */
} free_block_t;

/* =========================================================
 * MAIN FUNCTIONS
 * =========================================================
 * These functions are called directly by main().
 */

void memory_init(void); // initialize your memory manager state
void admit_process(void); // admit user jobs from sub_queue to user queues when possible
process_t *dispatch(process_t **cur_running_rt); // select the process to run this tick
void run_process(process_t *p); // run the selected process for exactly 1 tick
void post_run(process_t *p, process_t **cur_running_rt); // handle completion / re-queue after 1 tick
int termination_check(int processNo, int process_count, process_t *cur_running_rt); // return 1 if simulation ends


/* =========================================================
 * HELPER FUNCTIONS
 * =========================================================
 * These functions provide external help to the main functions to provide neater code.
 */

/* ---- Memory Helpers ---- */
int memory_can_allocate(int req_size); // check if req_size can be allocated now
int memory_allocate(process_t *p); // allocate memory for a user process and set p -> mem_start
int memory_free(process_t *p); // free a user process memory block

/* ---- Resource Helpers ---- */
int resource_available(process_t *p); // check if resources are available
void resource_occupy(process_t *p); // reserve resources for an admitted user process
void resource_free(process_t *p); // release resources when a process terminates

/* ---- Arrival Helper ---- */
void arrival(process_t *p); // enqueue a newly arrived process


/* =========================================================
 * LOG OUTPUT (DO NOT MODIFY)
 * =========================================================
 * This output format is fixed for grading / diff.
 * Called after run_process() and before post_run().
 */
void print_log(process_t *ready_process, int time) {
    if (ready_process == NULL) {
        printf("[t=%d] IDLE\n", time);
    } else {
        printf(
            "[t=%d] RUN PID=%d PR=%d CPU=%d MEM_ST=%d MEM=%d P=%d S=%d M=%d C=%d\n",
            time,
            ready_process->pid,
            ready_process->current_prio,
            ready_process->cpu_remain,
            ready_process->mem_start,
            ready_process->mem_req,
            ready_process->printers,
            ready_process->scanners,
            ready_process->modems,
            ready_process->cds
        );
    }
}


/* =========================================================
 * MAIN FUNCTION STUBS
 * ========================================================= */

void arrival(process_t *p) {
    if(p -> init_prio == 0) { // based on initial priority, sends process straight to real time queue if 0
        queue_push(&rt_queue, p);
        p -> state = READY; // set state to be able to run immediately because we bypass the submission queue
    } else { // otherwise, send to submission queue
        queue_push(&sub_queue, p); // add to the end of submission queue for FCFS
        p -> state = SUBMITTED; // set state to submitted meaning that the process is sent to submission queue
    }
}

void admit_process(void) {
    // keep going until submission queue is empty
    while(1) {
        if(queue_empty(&sub_queue)) return; // stop if submission queue empty

        process_t *head = queue_peek(&sub_queue); // set pointer to peek at the submission queue's first index, important to note that this doesn't pop the process from the queue

        // resource_available() returns 1 when resources ARE sufficient.
        // memory_can_allocate() returns 1 when memory IS available.
        // stop admitting if resources are not sufficient OR memory cannot be allocated.
        if(!resource_available(head) || !memory_can_allocate(head -> mem_req)) return;

        process_t* new_process = queue_pop(&sub_queue); // pop the process out of the queue if resources and memory are available

        // allocate resources and memory
        memory_allocate(new_process);
        resource_occupy(new_process);

        // assign to corresponding queue, real time priorities are already caught within arrival function
        if(new_process -> init_prio == 1) queue_push(&user_queue[0], new_process);
        else if(new_process -> init_prio == 2) queue_push(&user_queue[1], new_process);
        else if(new_process -> init_prio == 3) queue_push(&user_queue[2], new_process);
        else {
            perror("Process priority out of bounds. "); // error handling
            return;
        }
        new_process -> state = READY; // set state of process to "ready to be executed"
    }
}

process_t *dispatch(process_t **cur_running_rt) {
    if (*cur_running_rt != NULL) { // if still running real time process, continue running it
        return *cur_running_rt;
    }

    if (!queue_empty(&rt_queue)) { // run a process from real time queue
        *cur_running_rt = queue_pop(&rt_queue);
        return *cur_running_rt;
    }

    if (!queue_empty(&user_queue[0])) { // run a process from level 1 user queue
        return queue_pop(&user_queue[0]);
    }

    if (!queue_empty(&user_queue[1])) { // run a process from level 2 user queue
        return queue_pop(&user_queue[1]);
    }

    if (!queue_empty(&user_queue[2])) { // run a process from level 3 user queue
        return queue_pop(&user_queue[2]);
    }

    return NULL;
}

void run_process(process_t *p) {
    if (p == NULL) return; // no process to run

    p -> state = RUNNING; // current process running
    p -> cpu_remain--; // tick decrements process' remaining run time
}


void post_run(process_t *p, process_t **cur_running_rt) {
    if (p == NULL) return; // no process to process

    /* If the process finished, terminate it. */
    if (p -> cpu_remain == 0) {
        p -> state = TERMINATED; // process is set to terminated

        /* Clear RT pointer if the finished process was real-time. */
        if (p -> current_prio == 0) {
            *cur_running_rt = NULL;
        } else { // free resources and memory for non-RT processes that are terminated
            resource_free(p);
            memory_free(p);
        }

        return;
    }

    /* Real-time process continues until completion. */
    if (p -> current_prio == 0) {
        *cur_running_rt = p;
        return;
    }

    /* User process did not finish: Demote and requeue. */
    p -> state = READY;

    if (p -> current_prio == 1) {
        p -> current_prio = 2;
        queue_push(&user_queue[1], p);
    } else if (p -> current_prio == 2) {
        p -> current_prio = 3;
        queue_push(&user_queue[2], p);
    } else {
        /* priority 3 stays at priority 3. */
        p -> current_prio = 3;
        queue_push(&user_queue[2], p);
    }
}


int termination_check(int processNo, int process_count, process_t *cur_running_rt) {
    /* TODO or reuse the provided logic 
     You may keep this logic or adjust as long as termination condition remains correct. */
    return  processNo == process_count  &&
            cur_running_rt == NULL      &&
            queue_empty(&rt_queue)      &&
            queue_empty(&sub_queue)     &&
            queue_empty(&user_queue[0]) &&
            queue_empty(&user_queue[1]) &&
            queue_empty(&user_queue[2]);
}


/* =========================================================
 * HELPER FUNCTION STUBS
 * ========================================================= */

/* Create one free block covering the entire user memory region [64, 1023] (960 MB).
 * The real-time region [0, 63] is permanently reserved and not tracked here. */
void memory_init(void) {
    freelist = (free_block_t *)malloc(sizeof(free_block_t));
    freelist -> start = 64;
    freelist -> size  = 960;
    freelist -> next  = NULL;
}

/* Scan the free list from lowest address; return 1 if any block can satisfy req_size. */
int memory_can_allocate(int req_size) {
    free_block_t *curr = freelist;
    while (curr != NULL) {
        if (curr -> size >= req_size) return 1;
        curr = curr -> next;
    }
    return 0;
}

/* First Fit: scan from the lowest address, pick the first block >= p -> mem_req.
 * Allocate from the LOW end of that block (per lab spec).
 * If the block is larger, split it: remainder becomes a new free block starting
 * at (original_start + req_size). */
int memory_allocate(process_t *p) {
    free_block_t *curr = freelist;
    free_block_t *prev = NULL;

    while (curr != NULL) {
        if (curr -> size >= p -> mem_req) {
            p -> mem_start = curr -> start;   /* take from the low end */

            if (curr -> size == p -> mem_req) {
                /* Exact fit: remove the block entirely */
                if (prev == NULL) freelist = curr -> next;
                else prev -> next = curr -> next;
                free(curr);
            } else {
                /* Partial fit: shrink the block from the low end */
                curr -> start += p -> mem_req;
                curr -> size -= p -> mem_req;
            }
            return 0; // success
        }
        prev = curr;
        curr = curr -> next;
    }
    return -1; // no suitable block found
}

int memory_free(process_t *p) {
    /* Create a new free block for the released memory. */
    free_block_t *blk = (free_block_t *)malloc(sizeof(free_block_t));
    blk -> start = p -> mem_start;
    blk -> size  = p -> mem_req;
    blk -> next  = NULL;

    /* Insert into the free list in strictly ascending start-address order. */
    free_block_t *curr = freelist;
    free_block_t *prev = NULL;
    while (curr != NULL && curr -> start < blk -> start) {
        prev = curr;
        curr = curr -> next;
    }
    blk -> next = curr;
    if (prev == NULL) freelist = blk;
    else prev -> next = blk;

    /* Coalesce with the NEXT block if adjacent. */
    if (blk -> next != NULL && blk -> start + blk -> size == blk -> next -> start) {
        free_block_t *to_merge = blk -> next;
        blk -> size += to_merge -> size;
        blk -> next = to_merge -> next;
        free(to_merge);
    }

    /* Coalesce with the PREV block if adjacent. */
    if (prev != NULL && prev -> start + prev -> size == blk -> start) {
        prev -> size += blk -> size;
        prev -> next = blk -> next;
        free(blk);
    }

    return 0;
}

int resource_available(process_t *p) {
    if(p -> printers > printers) return 0; // not enough resources
    if(p -> scanners > scanners) return 0;
    if(p -> modems > modems) return 0;
    if(p -> cds > cd_drives) return 0;
    return 1; // enough resources
}

void resource_occupy(process_t *p) {
    // occupy I/O resources
    printers -= p -> printers;
    scanners -= p -> scanners;
    modems -= p -> modems;
    cd_drives -= p -> cds;
}

void resource_free(process_t *p) {
    // replenish resources
    printers += p -> printers;
    scanners += p -> scanners;
    modems += p -> modems;
    cd_drives += p -> cds;
}



/* =========================================================
 * MAIN (DO NOT CHANGE LOOP ORDER)
 * =========================================================
 * This main reads processes from stdin, then simulates 1-second ticks.
 *
 * Input format: each line has 8 integers:
 *   <arrival> <priority> <cpu> <mem> <printers> <scanners> <modems> <cds>
 *
 * IMPORTANT:
 * - For determinism with your current arrival loop, input should be sorted by arrival_time.
 *   (If unsorted, later arrivals may never be enqueued due to the break condition.)
 * - In all the test files, the inputs are sorted by arrival_time.
 */
int main(void) {
    /* Initialize queues (provided by queue.h) */
    queue_init(&rt_queue);
    queue_init(&sub_queue);
    for (int i = 0; i < 3; i++) {
        queue_init(&user_queue[i]);
    }

    /* Initialize memory manager (YOUR implementation) */
    memory_init();

    /* Read processes from stdin */
    process_t processes[MAX_PROCESSES];
    int process_count = 0;

    while (process_count < MAX_PROCESSES) {
        int a, p, cpu, mem, pr, sc, mo, cd;
        if (scanf("%d %d %d %d %d %d %d %d",
                  &a, &p, &cpu, &mem, &pr, &sc, &mo, &cd) != 8) {
            break; /* EOF or invalid input */
        }

        processes[process_count].arrival_time = a;
        processes[process_count].init_prio    = p;
        processes[process_count].cpu_total    = cpu;
        processes[process_count].mem_req      = mem;
        processes[process_count].printers     = pr;
        processes[process_count].scanners     = sc;
        processes[process_count].modems       = mo;
        processes[process_count].cds          = cd;

        processes[process_count].pid          = process_count;
        processes[process_count].cpu_remain   = cpu;
        processes[process_count].current_prio = p;     /* initial priority */
        processes[process_count].state        = NEW;
        processes[process_count].mem_start    = 0;     /* will be set when admitted (user processes) */

        process_count++;
    }

    /* Simulation state:
    * - cur_running_rt: holds the currently running RT job (if any). In this lab, an RT job,
    *   once dispatched, stays as the selected RT job across ticks until it terminates.
    *   (This is just a state variable kept by main; your dispatch/post_run can manage it.)
    * - ready_process: the process selected to run for THIS tick only (may be RT/user/NULL).
    * - Note: You are free to implement RT handling differently internally, as long as
    *   the external behavior matches the lab requirements and the main loop order is unchanged.
 
    */
    int processNo = 0;                 /* index of next process that has not arrived yet */
    process_t *cur_running_rt = NULL;  /* if an RT job is running, it persists until completion */

    /* Tick-by-tick simulation */
    for (int time = 0; ; time++) {
        /* 1) ARRIVAL: move any processes arriving at this tick into rt_queue or sub_queue */
        for (; processNo < process_count; processNo++) {
            if (processes[processNo].arrival_time == time) {
                arrival(&processes[processNo]);
            } else {
                break; /* important for determinism; assumes arrivals are sorted */
            }
        }

        /* 2) ADMIT: move as many from submission queue to user queues as possible */
        admit_process();

        /* 3) DISPATCH: pick the process to run for this tick
        * dispatch() returns the process that should run in the current tick.
        * It may also update cur_running_rt to remember a running RT job across ticks.
        */
        process_t *ready_process = dispatch(&cur_running_rt);

        /* 4) RUN: execute exactly 1 tick */
        run_process(ready_process);

        /* 5) PRINT: fixed log format for grading (after run, before post-run updates) */
        print_log(ready_process, time);

        /* 6) POST-RUN: terminate/requeue/demote as needed */
        post_run(ready_process, &cur_running_rt);

        /* Terminate when all work is done */
        if (termination_check(processNo, process_count, cur_running_rt)) {
            break;
        }
    }

    return 0;
}
