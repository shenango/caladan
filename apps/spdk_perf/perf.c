/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"
 #include <math.h>
#include "spdk/env.h"
#include "spdk/fd.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_lcore.h>

#include "spdk/nvme.h"
#include "spdk/string.h"
#include "spdk/nvme_intel.h"

#if HAVE_LIBAIO
#include <libaio.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

#define MAX_LATENCY_SAMPLES_PER_THREAD (100*524288) //1048576 //2097152 //4194304 // 1048576

enum io_type {
	READ,			//0
	WRITE,			//1
	NUM_IO_TYPES	//2
};

struct ctrlr_entry {
	struct spdk_nvme_ctrlr			*ctrlr;
	struct spdk_nvme_intel_rw_latency_page	*latency_page;
	struct ctrlr_entry			*next;
	char					name[1024];
};

enum entry_type {
	ENTRY_TYPE_NVME_NS,
	ENTRY_TYPE_AIO_FILE,
};

struct ns_entry {
	enum entry_type		type;

	union {
		struct {
			struct spdk_nvme_ctrlr	*ctrlr;
			struct spdk_nvme_ns	*ns;
		} nvme;
#if HAVE_LIBAIO
		struct {
			int			fd;
		} aio;
#endif
	} u;

	struct ns_entry		*next;
	uint32_t		io_size_blocks;
	uint64_t		size_in_ios;
	char			name[1024];
};

struct ns_worker_ctx {
	struct ns_entry		*entry;
	uint64_t		current_queue_depth;
	uint64_t		offset_in_ios;
	bool			is_draining;
	
	double			*lat_samples[NUM_IO_TYPES];
	uint64_t		num_samples[NUM_IO_TYPES];
	uint64_t		total_tsc[NUM_IO_TYPES];
	uint64_t		io_completed[NUM_IO_TYPES];

	union {
		struct {
			struct spdk_nvme_qpair	*qpair;
		} nvme;

#if HAVE_LIBAIO
		struct {
			struct io_event		*events;
			io_context_t		ctx;
		} aio;
#endif
	} u;

	struct ns_worker_ctx	*next;
};

struct perf_task {
	struct ns_worker_ctx	*ns_ctx;
	void			*buf;
	uint64_t		submit_tsc;
	enum io_type	req_type;
#if HAVE_LIBAIO
	struct iocb		iocb;
#endif
};

struct worker_thread {
	struct ns_worker_ctx 	*ns_ctx;
	struct worker_thread	*next;
	unsigned		lcore;
};

static int g_outstanding_commands;

static bool g_latency_tracking_enable = false;
static bool g_open_loop = false;

struct rte_mempool *request_mempool;
static struct rte_mempool *task_pool;

static struct ctrlr_entry *g_controllers = NULL;
static struct ns_entry *g_namespaces = NULL;
static int g_num_namespaces = 0;
static struct worker_thread *g_workers = NULL;
static int g_num_workers = 0;
static uint32_t g_fixed = 0;
static int dropped_count = 0;
static bool done = false;
static bool g_precondition = false;


static uint64_t g_tsc_rate;
static uint32_t g_io_size_bytes;
static int g_rw_percentage;
static int g_is_random;
static int g_queue_depth;
static int g_time_in_sec;
static uint32_t g_max_completions;
static uint32_t g_lambda;  //avg requests per second to generate (exponential distribution)
static double g_avg_req_per_cycle;
static char* g_file_name;
static const char *g_core_mask;

static int g_aio_optind; /* Index of first AIO filename in argv */

static void
task_complete(struct perf_task *task);

int 
compare_double (const void *a, const void *b);

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
		       cdata->mn, cdata->sn,
		       spdk_nvme_ns_get_id(ns));
		return;
	}

	if (spdk_nvme_ns_get_size(ns) < g_io_size_bytes ||
	    spdk_nvme_ns_get_sector_size(ns) > g_io_size_bytes) {
		printf("WARNING: controller %-20.20s (%-20.20s) ns %u has invalid "
		       "ns size %" PRIu64 " / block size %u for I/O size %u\n",
		       cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns),
		       spdk_nvme_ns_get_size(ns), spdk_nvme_ns_get_sector_size(ns), g_io_size_bytes);
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->type = ENTRY_TYPE_NVME_NS;
	entry->u.nvme.ctrlr = ctrlr;
	entry->u.nvme.ns = ns;

	entry->size_in_ios = spdk_nvme_ns_get_size(ns) /
			     g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / spdk_nvme_ns_get_sector_size(ns);

	snprintf(entry->name, 44, "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	g_num_namespaces++;
	entry->next = g_namespaces;
	g_namespaces = entry;
}

static void
unregister_namespaces(void)
{
	struct ns_entry *entry = g_namespaces;

	while (entry) {
		struct ns_entry *next = entry->next;
		free(entry);
		entry = next;
	}
}

static void
enable_latency_tracking_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("enable_latency_tracking_complete failed\n");
	}
	g_outstanding_commands--;
}

static void
set_latency_tracking_feature(struct spdk_nvme_ctrlr *ctrlr, bool enable)
{
	int res;
	union spdk_nvme_intel_feat_latency_tracking latency_tracking;

	if (enable) {
		latency_tracking.bits.enable = 0x01;
	} else {
		latency_tracking.bits.enable = 0x00;
	}

	res = spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING,
					      latency_tracking.raw, 0, NULL, 0, enable_latency_tracking_complete, NULL);
	if (res) {
		printf("fail to allocate nvme request.\n");
		return;
	}
	g_outstanding_commands++;

	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	int nsid, num_ns;
	struct ctrlr_entry *entry = malloc(sizeof(struct ctrlr_entry));
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	entry->latency_page = rte_zmalloc("nvme latency", sizeof(struct spdk_nvme_intel_rw_latency_page),
					  4096);
	if (entry->latency_page == NULL) {
		printf("Allocation error (latency page)\n");
		exit(1);
	}

	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	entry->ctrlr = ctrlr;
	entry->next = g_controllers;
	g_controllers = entry;

	if (g_latency_tracking_enable &&
	    spdk_nvme_ctrlr_is_feature_supported(ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING))
		set_latency_tracking_feature(ctrlr, true);

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	for (nsid = 1; nsid <= num_ns; nsid++) {
		register_ns(ctrlr, spdk_nvme_ctrlr_get_ns(ctrlr, nsid));
	}

}

#if HAVE_LIBAIO
static int
register_aio_file(const char *path)
{
	struct ns_entry *entry;

	int flags, fd;
	uint64_t size;
	uint32_t blklen;

	if (g_rw_percentage == 100) {
		flags = O_RDONLY;
	} else if (g_rw_percentage == 0) {
		flags = O_WRONLY;
	} else {
		flags = O_RDWR;
	}

	flags |= O_DIRECT;

	fd = open(path, flags);
	if (fd < 0) {
		fprintf(stderr, "Could not open AIO device %s: %s\n", path, strerror(errno));
		return -1;
	}

	size = spdk_file_get_size(fd);
	if (size == 0) {
		fprintf(stderr, "Could not determine size of AIO device %s\n", path);
		close(fd);
		return -1;
	}

	blklen = spdk_dev_get_blocklen(fd);
	if (blklen == 0) {
		fprintf(stderr, "Could not determine block size of AIO device %s\n", path);
		close(fd);
		return -1;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		close(fd);
		perror("aio ns_entry malloc");
		return -1;
	}

	entry->type = ENTRY_TYPE_AIO_FILE;
	entry->u.aio.fd = fd;
	entry->size_in_ios = size / g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / blklen;

	snprintf(entry->name, sizeof(entry->name), "%s", path);

	g_num_namespaces++;
	entry->next = g_namespaces;
	g_namespaces = entry;

	return 0;
}

static int
aio_submit(io_context_t aio_ctx, struct iocb *iocb, int fd, enum io_iocb_cmd cmd, void *buf,
	   unsigned long nbytes, uint64_t offset, void *cb_ctx)
{
	iocb->aio_fildes = fd;
	iocb->aio_reqprio = 0;
	iocb->aio_lio_opcode = cmd;
	iocb->u.c.buf = buf;
	iocb->u.c.nbytes = nbytes;
	iocb->u.c.offset = offset;
	iocb->data = cb_ctx;

	if (io_submit(aio_ctx, 1, &iocb) < 0) {
		printf("io_submit");
		return -1;
	}

	return 0;
}

static void
aio_check_io(struct ns_worker_ctx *ns_ctx)
{
	int count, i;
	struct timespec timeout;

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	count = io_getevents(ns_ctx->u.aio.ctx, 1, g_queue_depth, ns_ctx->u.aio.events, &timeout);
	if (count < 0) {
		fprintf(stderr, "io_getevents error\n");
		exit(1);
	}

	for (i = 0; i < count; i++) {
		task_complete(ns_ctx->u.aio.events[i].data);
	}
}
#endif /* HAVE_LIBAIO */

static void task_ctor(struct rte_mempool *mp, void *arg, void *__task, unsigned id)
{
	struct perf_task *task = __task;
	task->buf = rte_malloc(NULL, g_io_size_bytes, 0x200);
	if (task->buf == NULL) {
		fprintf(stderr, "task->buf rte_malloc failed\n");
		exit(1);
	}
}

static void io_complete(void *ctx, const struct spdk_nvme_cpl *completion);

static __thread unsigned int seed = 0;

static void
submit_single_io(struct ns_worker_ctx *ns_ctx)
{
	struct perf_task	*task = NULL;
	uint64_t		offset_in_ios;
	int			rc;
	struct ns_entry		*entry = ns_ctx->entry;

	if (g_open_loop && ns_ctx->current_queue_depth >= (uint64_t) g_queue_depth){
		//printf("Drop request\n");
		dropped_count++;
		return;
	}

	if (rte_mempool_get(task_pool, (void **)&task) != 0) {
		fprintf(stderr, "task_pool rte_mempool_get failed\n");
		exit(1);
	}

	task->ns_ctx = ns_ctx;

	if (g_is_random) {
		offset_in_ios = rand_r(&seed) % entry->size_in_ios;
	} else {
		offset_in_ios = ns_ctx->offset_in_ios++;
		if (ns_ctx->offset_in_ios == entry->size_in_ios) {
			ns_ctx->offset_in_ios = 0;
			if (g_precondition){
				printf("Finished writing sequentially to whole device\n");
				done = true;
				return;
			}
		}
	}

	task->submit_tsc = rte_get_timer_cycles();

	if ((g_rw_percentage == 100) ||
	    (g_rw_percentage != 0 && ((rand_r(&seed) % 100) < g_rw_percentage))) {
		task->req_type = READ;
#if HAVE_LIBAIO
		if (entry->type == ENTRY_TYPE_AIO_FILE) {
			rc = aio_submit(ns_ctx->u.aio.ctx, &task->iocb, entry->u.aio.fd, IO_CMD_PREAD, task->buf,
					g_io_size_bytes, offset_in_ios * g_io_size_bytes, task);
		} else
#endif
		{
			rc = spdk_nvme_ns_cmd_read(entry->u.nvme.ns, ns_ctx->u.nvme.qpair, task->buf,
						   offset_in_ios * entry->io_size_blocks,
						   entry->io_size_blocks, io_complete, task, 0);
		}
	} else {
		task->req_type = WRITE;
#if HAVE_LIBAIO
		if (entry->type == ENTRY_TYPE_AIO_FILE) {
			rc = aio_submit(ns_ctx->u.aio.ctx, &task->iocb, entry->u.aio.fd, IO_CMD_PWRITE, task->buf,
					g_io_size_bytes, offset_in_ios * g_io_size_bytes, task);
		} else
#endif
		{
			rc = spdk_nvme_ns_cmd_write(entry->u.nvme.ns, ns_ctx->u.nvme.qpair, task->buf,
						    offset_in_ios * entry->io_size_blocks,
						    entry->io_size_blocks, io_complete, task, 0);
		}
	}

	if (rc != 0) {
		rte_mempool_put(task_pool, task);
		dropped_count++;
	} else {
		ns_ctx->current_queue_depth++;
	}
}

static void
task_complete(struct perf_task *task)
{
	struct ns_worker_ctx	*ns_ctx;
	enum io_type			iotype;
	uint64_t				io_latency;
	uint64_t				num_samples;	
	double	 				g_tsc_to_us;
	int						index;

	ns_ctx = task->ns_ctx;
	iotype = task->req_type;
	ns_ctx->current_queue_depth--;
	ns_ctx->io_completed[iotype]++;
	io_latency = rte_get_timer_cycles() - task->submit_tsc; //in cycles
	ns_ctx->total_tsc[iotype] += io_latency; //rte_get_timer_cycles() - task->submit_tsc;

	g_tsc_to_us = (double) 1000000 / (double) g_tsc_rate;
	io_latency = io_latency * g_tsc_to_us; //in us
	
	// (Modified) reservoir sampling
	num_samples = ns_ctx->num_samples[iotype];
	if (num_samples < MAX_LATENCY_SAMPLES_PER_THREAD){ // keep all samples
		ns_ctx->lat_samples[iotype][num_samples] = io_latency;
		ns_ctx->num_samples[iotype]++;
	}
	else if (drand48() < 0.5){ // randomly decide whether to keep sample and overwrite old
		index = (ns_ctx->io_completed[iotype]) % MAX_LATENCY_SAMPLES_PER_THREAD;
		ns_ctx->lat_samples[iotype][index] = io_latency;
	}
	
	rte_mempool_put(task_pool, task);

	/*
	 * is_draining indicates when time has expired for the test run
	 * and we are just waiting for the previously submitted I/O
	 * to complete.  In this case, do not submit a new I/O to replace
	 * the one just completed.
	 */
	if (!g_open_loop && !ns_ctx->is_draining) {
		submit_single_io(ns_ctx);
	}
}

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *completion)
{
	task_complete((struct perf_task *)ctx);
}

static void
check_io(struct ns_worker_ctx *ns_ctx)
{
#if HAVE_LIBAIO
	if (ns_ctx->entry->type == ENTRY_TYPE_AIO_FILE) {
		aio_check_io(ns_ctx);
	} else
#endif
	{
		spdk_nvme_qpair_process_completions(ns_ctx->u.nvme.qpair, g_max_completions);
	}
}

static void
submit_io(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	while (queue_depth-- > 0) {
		submit_single_io(ns_ctx);
	}
}

static void
drain_io(struct ns_worker_ctx *ns_ctx)
{
	ns_ctx->is_draining = true;
	while (ns_ctx->current_queue_depth > 0) {
		check_io(ns_ctx);
	}
}

static int
init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	int j;
	
	for (j=0; j< NUM_IO_TYPES; j++) {
		ns_ctx->lat_samples[j] = calloc(NUM_IO_TYPES * MAX_LATENCY_SAMPLES_PER_THREAD,
									   sizeof(double));
		if (!ns_ctx->lat_samples[j]){
			printf("ERROR: Could not allocate ioq samples array\n");
			return -1;
		}
	}


	if (ns_ctx->entry->type == ENTRY_TYPE_AIO_FILE) {
#ifdef HAVE_LIBAIO
		ns_ctx->u.aio.events = calloc(g_queue_depth, sizeof(struct io_event));
		if (!ns_ctx->u.aio.events) {
			return -1;
		}
		ns_ctx->u.aio.ctx = 0;
		if (io_setup(g_queue_depth, &ns_ctx->u.aio.ctx) < 0) {
			free(ns_ctx->u.aio.events);
			perror("io_setup");
			return -1;
		}
#endif
	} else {
		/*
		 * TODO: If a controller has multiple namespaces, they could all use the same queue.
		 *  For now, give each namespace/thread combination its own queue.
		 */
		ns_ctx->u.nvme.qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_ctx->entry->u.nvme.ctrlr, NULL, 0);
		if (!ns_ctx->u.nvme.qpair) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair failed\n");
			return -1;
		}
		fprintf(stderr, "working context allocated %p\n", ns_ctx->u.nvme.qpair);

	}

	
	return 0;
}

static void
cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	if (ns_ctx->entry->type == ENTRY_TYPE_AIO_FILE) {
#ifdef HAVE_LIBAIO
		io_destroy(ns_ctx->u.aio.ctx);
		free(ns_ctx->u.aio.events);
#endif
	} else {
		spdk_nvme_ctrlr_free_io_qpair(ns_ctx->u.nvme.qpair);
	}
}


static uint64_t
time_to_gen_next_req(void)
{
	double time_delta_cycles;

	if (g_fixed) {
		time_delta_cycles = (double) 1 / g_avg_req_per_cycle;
	} else { //exponential
		time_delta_cycles = - log(drand48())/g_avg_req_per_cycle;
	}

	return rte_get_timer_cycles() + time_delta_cycles;
}

static int
work_fn(void *arg)
{
	uint64_t tsc_end;
	struct worker_thread *worker = (struct worker_thread *)arg;
	struct ns_worker_ctx *ns_ctx = NULL;
	uint64_t tsc_next_gen;

	printf("Starting thread on core %u\n", worker->lcore);

	/* Allocate a queue pair for each namespace. */
	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		if (init_ns_worker_ctx(ns_ctx) != 0) {
			printf("ERROR: init_ns_worker_ctx() failed\n");
			return 1;
		}
		ns_ctx = ns_ctx->next;
	}

	tsc_end = rte_get_timer_cycles() + g_time_in_sec * g_tsc_rate;

	if (g_open_loop) {
		ns_ctx = worker->ns_ctx;
		submit_single_io(ns_ctx);
		tsc_next_gen = time_to_gen_next_req();
		if (ns_ctx->next != NULL){
			printf("WARNING: open-loop load generation uses a single worker. User specificed more workers; will be ignored.\n");
		}
		while (1) {
			/*
			 * Check for completed I/O for each controller. A new
			 * I/O will be submitted in the io_complete callback
			 * to replace each I/O that is completed.
			 */
		//	ns_ctx = worker->ns_ctx;
			
			if (rte_get_timer_cycles() > tsc_next_gen) {
				//count_dropped += submit_single_io(ns_ctx);
				submit_single_io(ns_ctx);
				tsc_next_gen = time_to_gen_next_req();
				//count++;
			}

		//	while (ns_ctx != NULL) {
				check_io(ns_ctx);
		//		ns_ctx = ns_ctx->next;
		//	}

			
			if (rte_get_timer_cycles() > tsc_end) {
				break;
			}
		}
	}

	else { // closed-loop load gen (default)
		/* Submit initial I/O for each namespace. */
		ns_ctx = worker->ns_ctx;
		while (ns_ctx != NULL) {
			submit_io(ns_ctx, g_queue_depth);
			ns_ctx = ns_ctx->next;
		}

		while (1) {
			/*
			 * Check for completed I/O for each controller. A new
			 * I/O will be submitted in the io_complete callback
			 * to replace each I/O that is completed.
			 */
			ns_ctx = worker->ns_ctx;
			while (ns_ctx != NULL) {
				check_io(ns_ctx);
				ns_ctx = ns_ctx->next;
			}

			if (rte_get_timer_cycles() > tsc_end || done) {
				break;
			}
		}
	}

	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		drain_io(ns_ctx);
		cleanup_ns_worker_ctx(ns_ctx);
		ns_ctx = ns_ctx->next;
	}

	return 0;
}

static void usage(char *program_name)
{
	printf("%s options", program_name);
#if HAVE_LIBAIO
	printf(" [AIO device(s)]...");
#endif
	printf("\n");
	printf("\t[-q io depth]\n");
	printf("\t[-s io size in bytes]\n");
	printf("\t[-w io pattern type, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw)]\n");
	printf("\t[-M rwmixread (100 for reads, 0 for writes)]\n");
	printf("\t[-l enable latency tracking, default: disabled]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-c core mask for I/O submission/completion.]\n");
	printf("\t\t(default: 1)]\n");
	printf("\t[-m max completions per poll]\n");
	printf("\t\t(default: 0 - unlimited)\n");
	printf("\t[-L lambda (avg arrival req rate)]\n");
	printf("\t\t(if specificied, I/O generator is open-loop with exponential distribution)\n");
	printf("\t\t(default: no lambda (closed-loop I/O generator based on queue depth) )\n");
	printf("\t[-d distribution of inter-arrival requests (for open-loop)]\n");
	printf("\t\t(exp, fixed)\n");
	printf("\t\t(default: exp, i.e. exponential)\n");
	printf("\t[-p preconditioning flag, indicates to stop test when finished writing seq to whole addr space]\n");
}

static double
get_nth_percentile(struct ns_worker_ctx *ctx, int io_type, int n)
{
	return ctx->lat_samples[io_type][n*ctx->num_samples[io_type]/100];
}

static double
get_npnth_percentile(struct ns_worker_ctx *ctx, int io_type, double n)
{
        return ctx->lat_samples[io_type][(int)(n*(double)ctx->num_samples[io_type])];
}

int
compare_double (const void *a, const void *b)
{
	const double *da = (const double *) a;
	const double *db = (const double *) b;

	return (*da > *db) - (*da < *db);
}

static void
print_performance(void)
{
	uint64_t total_tsc[NUM_IO_TYPES], total_io_completed[NUM_IO_TYPES];
	float io_per_second[NUM_IO_TYPES], mb_per_second[NUM_IO_TYPES], average_latency[NUM_IO_TYPES];
	float total_io_per_second[NUM_IO_TYPES], total_mb_per_second[NUM_IO_TYPES], total_average_latency[NUM_IO_TYPES];
	struct worker_thread	*worker;
	struct ns_worker_ctx	*ns_ctx;
	FILE 					*f = NULL;
	int i;

	if (g_file_name){
		f = fopen(g_file_name, "a");
		if (f == NULL){
			printf("Cannot open output file %s!!\n", g_file_name);
		}

	}
	
	for (i = 0; i < NUM_IO_TYPES; i++){
		total_io_per_second[i] = 0;
		total_mb_per_second[i] = 0;
		total_tsc[i] = 0;
		total_io_completed[i] = 0;
	}

	worker = g_workers;
	while (worker) {
		ns_ctx = worker->ns_ctx;
		while (ns_ctx) {
			for (i = 0; i < NUM_IO_TYPES; i++){
				if (ns_ctx->io_completed[i] > 0){
					io_per_second[i] = (float)ns_ctx->io_completed[i] / g_time_in_sec;
					mb_per_second[i] = io_per_second[i] * g_io_size_bytes / (1024 * 1024);
					average_latency[i] = (float)(ns_ctx->total_tsc[i] / ns_ctx->io_completed[i]) * 1000 * 1000 / g_tsc_rate;
					if (i == READ){
						printf("Read Performance Summary:\n");
					}
					else if (i == WRITE){
						printf("Write Performance Summary:\n");
					}
					printf("%-43.43s from core %u: %10.2f IO/s %10.2f MB/s %10.2f us(average latency)\n",
						   ns_ctx->entry->name, worker->lcore,
						   io_per_second[i], mb_per_second[i],
						   average_latency[i]);
					total_io_per_second[i] += io_per_second[i];
					total_mb_per_second[i] += mb_per_second[i];
					total_tsc[i] += ns_ctx->total_tsc[i];
					total_io_completed[i] += ns_ctx->io_completed[i];

					qsort(ns_ctx->lat_samples[i], ns_ctx->num_samples[i], sizeof(double), &compare_double);
					//quickSort(ns_ctx->lat_samples[i], 0, ns_ctx->num_samples[i] -1);
					printf("10th percentile: %f\n", get_nth_percentile(ns_ctx, i, 10)); 
					printf("50th percentile: %f\n", get_nth_percentile(ns_ctx, i, 50)); 
					printf("95th percentile: %f\n", get_nth_percentile(ns_ctx, i, 95)); 
					printf("99th percentile: %f\n", get_nth_percentile(ns_ctx, i, 99));
					printf("99.9th percentile: %f\n", get_npnth_percentile(ns_ctx, i, 0.999));
					printf("99.99th percentile: %f\n", get_npnth_percentile(ns_ctx, i, 0.9999));
					printf("Num_sampled = %lu, Num_ios = %lu\n", ns_ctx->num_samples[i],
						   ns_ctx->io_completed[i]);
				}

			}
			ns_ctx = ns_ctx->next;
		}
		worker = worker->next;
	}

	assert(total_io_completed[READ] + total_io_completed[WRITE] != 0);
	for (i = 0; i < NUM_IO_TYPES; i++){
		if (total_io_completed[i] == 0)
			continue;

		total_average_latency[i] = (float)(total_tsc[i] / total_io_completed[i]) * 1000 * 1000 / g_tsc_rate;;

		printf("========================================================\n");
		if (i == READ){
			printf("%-55s: %10.2f IO/s %10.2f MB/s %10.2f us(average latency)\n",
				   "Total reads", total_io_per_second[i], total_mb_per_second[i], total_average_latency[i]);
		}
		else if (i == WRITE){
			printf("%-55s: %10.2f IO/s %10.2f MB/s %10.2f us(average latency)\n",
				   "Total writes", total_io_per_second[i], total_mb_per_second[i], total_average_latency[i]);
		}
	}

	if (g_open_loop){
		printf("Dropped %d requests\n", dropped_count);
	}
	printf("\n");




	worker = g_workers;
	ns_ctx = worker->ns_ctx;

	double *nctt = calloc(ns_ctx->num_samples[READ] + ns_ctx->num_samples[WRITE], sizeof(double));
	memcpy(nctt, ns_ctx->lat_samples[READ], ns_ctx->num_samples[READ] * sizeof(double));
	memcpy(&nctt[ns_ctx->num_samples[READ]], ns_ctx->lat_samples[WRITE], ns_ctx->num_samples[WRITE] * sizeof(double));

	qsort(nctt, ns_ctx->num_samples[READ] + ns_ctx->num_samples[WRITE], sizeof(double), &compare_double);

	int indx = (double)(ns_ctx->num_samples[READ] + ns_ctx->num_samples[WRITE]) * 0.999;

	if (f) {
		fprintf(f, "%10.2f; %10.2f; %f; %f; %f; %f; %f; %f; %f; %f; %f; %f; %f; %d \n",
			total_io_per_second[READ], total_io_per_second[WRITE],
			total_average_latency[READ],
			get_nth_percentile(ns_ctx, READ, 95),
			get_nth_percentile(ns_ctx, READ, 99),
			get_npnth_percentile(ns_ctx, READ, 0.999),
			get_npnth_percentile(ns_ctx, READ, 0.9999),
			total_average_latency[WRITE],
			get_nth_percentile(ns_ctx, WRITE, 95),
			get_nth_percentile(ns_ctx, WRITE, 99),
			get_npnth_percentile(ns_ctx, WRITE, 0.999),
			get_npnth_percentile(ns_ctx, WRITE, 0.9999),
			nctt[indx],
			dropped_count);
	}
}

static void
print_latency_page(struct ctrlr_entry *entry)
{
	int i;

	printf("\n");
	printf("%s\n", entry->name);
	printf("--------------------------------------------------------\n");

	for (i = 0; i < 32; i++) {
		if (entry->latency_page->buckets_32us[i])
			printf("Bucket %dus - %dus: %d\n", i * 32, (i + 1) * 32, entry->latency_page->buckets_32us[i]);
	}
	for (i = 0; i < 31; i++) {
		if (entry->latency_page->buckets_1ms[i])
			printf("Bucket %dms - %dms: %d\n", i + 1, i + 2, entry->latency_page->buckets_1ms[i]);
	}
	for (i = 0; i < 31; i++) {
		if (entry->latency_page->buckets_32ms[i])
			printf("Bucket %dms - %dms: %d\n", (i + 1) * 32, (i + 2) * 32,
			       entry->latency_page->buckets_32ms[i]);
	}
}

static void
print_latency_statistics(const char *op_name, enum spdk_nvme_intel_log_page log_page)
{
	struct ctrlr_entry	*ctrlr;

	printf("%s Latency Statistics:\n", op_name);
	printf("========================================================\n");
	ctrlr = g_controllers;
	while (ctrlr) {
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr->ctrlr, log_page)) {
			if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr->ctrlr, log_page, SPDK_NVME_GLOBAL_NS_TAG,
							     ctrlr->latency_page, sizeof(struct spdk_nvme_intel_rw_latency_page),
							     0, enable_latency_tracking_complete,
							     NULL)) {
				printf("nvme_ctrlr_cmd_get_log_page() failed\n");
				exit(1);
			}

			g_outstanding_commands++;
		} else {
			printf("Controller %s: %s latency statistics not supported\n", ctrlr->name, op_name);
		}
		ctrlr = ctrlr->next;
	}

	while (g_outstanding_commands) {
		ctrlr = g_controllers;
		while (ctrlr) {
			spdk_nvme_ctrlr_process_admin_completions(ctrlr->ctrlr);
			ctrlr = ctrlr->next;
		}
	}

	ctrlr = g_controllers;
	while (ctrlr) {
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr->ctrlr, log_page)) {
			print_latency_page(ctrlr);
		}
		ctrlr = ctrlr->next;
	}
	printf("\n");
}

static void
print_stats(void)
{
	print_performance();
	if (g_latency_tracking_enable) {
		if (g_rw_percentage != 0) {
			print_latency_statistics("Read", SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY);
		}
		if (g_rw_percentage != 100) {
			print_latency_statistics("Write", SPDK_NVME_INTEL_LOG_WRITE_CMD_LATENCY);
		}
	}
}

static int
parse_args(int argc, char **argv)
{
	const char *workload_type;
	int op;
	bool mix_specified = false;
	const char *distribution = "exp";

	/* default value*/
	g_queue_depth = 0;
	g_io_size_bytes = 0;
	workload_type = NULL;
	g_time_in_sec = 0;
	g_rw_percentage = -1;
	g_core_mask = NULL;
	g_max_completions = 0;
	g_open_loop = false;
	g_lambda = 1;
	g_file_name = NULL;

	while ((op = getopt(argc, argv, "c:l:m:q:s:t:w:M:L:d:o:p:")) != -1) {
		switch (op) {
		case 'c':
			g_core_mask = optarg;
			break;
		case 'l':
			g_latency_tracking_enable = true;
			break;
		case 'm':
			g_max_completions = atoi(optarg);
			break;
		case 'q':
			g_queue_depth = atoi(optarg);
			break;
		case 's':
			g_io_size_bytes = atoi(optarg);
			break;
		case 't':
			g_time_in_sec = atoi(optarg);
			break;
		case 'w':
			workload_type = optarg;
			break;
		case 'M':
			g_rw_percentage = atoi(optarg);
			mix_specified = true;
			break;
		case 'L':
			g_open_loop = true;
			g_lambda = strtod(optarg, NULL);
			break;
		case 'd':
			distribution = optarg;
			break;
		case 'o':
			g_file_name = optarg;
			break;
		case 'p':
			g_precondition = true;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!g_queue_depth) {
		usage(argv[0]);
		return 1;
	}
	if (!g_io_size_bytes) {
		usage(argv[0]);
		return 1;
	}
	if (!workload_type) {
		usage(argv[0]);
		return 1;
	}
	if (!g_time_in_sec) {
		usage(argv[0]);
		return 1;
	}

	if (g_open_loop && !strcmp(distribution, "fixed")){
		g_fixed = 1;
	}
	
	if (strcmp(workload_type, "read") &&
	    strcmp(workload_type, "write") &&
	    strcmp(workload_type, "randread") &&
	    strcmp(workload_type, "randwrite") &&
	    strcmp(workload_type, "rw") &&
	    strcmp(workload_type, "randrw")) {
		fprintf(stderr,
			"io pattern type must be one of\n"
			"(read, write, randread, randwrite, rw, randrw)\n");
		return 1;
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "randread")) {
		g_rw_percentage = 100;
	}

	if (!strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "randwrite")) {
		g_rw_percentage = 0;
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "randread") ||
	    !strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "randwrite")) {
		if (mix_specified) {
			fprintf(stderr, "Ignoring -M option... Please use -M option"
				" only when using rw or randrw.\n");
		}
	}

	if (!strcmp(workload_type, "rw") ||
	    !strcmp(workload_type, "randrw")) {
		if (g_rw_percentage < 0 || g_rw_percentage > 100) {
			fprintf(stderr,
				"-M must be specified to value from 0 to 100 "
				"for rw or randrw.\n");
			return 1;
		}
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "rw")) {
		g_is_random = 0;
	} else {
		g_is_random = 1;
	}

	g_aio_optind = optind;
	optind = 1;
	return 0;
}

static int
register_workers(void)
{
	unsigned lcore;
	struct worker_thread *worker;
	struct worker_thread *prev_worker;

	worker = malloc(sizeof(struct worker_thread));
	if (worker == NULL) {
		perror("worker_thread malloc");
		return -1;
	}

	memset(worker, 0, sizeof(struct worker_thread));
	worker->lcore = rte_get_master_lcore();

	g_workers = worker;
	g_num_workers = 1;

	RTE_LCORE_FOREACH_SLAVE(lcore) {
		prev_worker = worker;
		worker = malloc(sizeof(struct worker_thread));
		if (worker == NULL) {
			perror("worker_thread malloc");
			return -1;
		}

		memset(worker, 0, sizeof(struct worker_thread));
		worker->lcore = lcore;
		prev_worker->next = worker;
		g_num_workers++;
	}

	return 0;
}

static void
unregister_workers(void)
{
	struct worker_thread *worker = g_workers;

	/* Free namespace context and worker thread */
	while (worker) {
		struct worker_thread *next_worker = worker->next;
		struct ns_worker_ctx *ns_ctx = worker->ns_ctx;

		while (ns_ctx) {
			struct ns_worker_ctx *next_ns_ctx = ns_ctx->next;
			free(ns_ctx);
			ns_ctx = next_ns_ctx;
		}

		free(worker);
		worker = next_worker;
	}
}

static bool
probe_cb(void *cb_ctx,  const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts)
{

	printf("Attaching to\n");

	return true;
}

static void
attach_cb (void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{

	register_ctrlr(ctrlr);
}

static int
register_controllers(void)
{
	printf("Initializing NVMe Controllers\n");

	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	return 0;
}

static void
unregister_controllers(void)
{
	struct ctrlr_entry *entry = g_controllers;

	while (entry) {
		struct ctrlr_entry *next = entry->next;
		rte_free(entry->latency_page);
		if (g_latency_tracking_enable &&
		    spdk_nvme_ctrlr_is_feature_supported(entry->ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING))
			set_latency_tracking_feature(entry->ctrlr, false);
		spdk_nvme_detach(entry->ctrlr);
		free(entry);
		entry = next;
	}
}

static int
register_aio_files(int argc, char **argv)
{
#if HAVE_LIBAIO
	int i;

	/* Treat everything after the options as files for AIO */
	for (i = g_aio_optind; i < argc; i++) {
		if (register_aio_file(argv[i]) != 0) {
			return 1;
		}
	}
#endif /* HAVE_LIBAIO */

	return 0;
}

static int
associate_workers_with_ns(void)
{
	struct ns_entry		*entry = g_namespaces;
	struct worker_thread	*worker = g_workers;
	struct ns_worker_ctx	*ns_ctx;
	int			i, count;

	count = g_num_namespaces > g_num_workers ? g_num_namespaces : g_num_workers;

	for (i = 0; i < count; i++) {
		if (entry == NULL) {
			break;
		}

		ns_ctx = malloc(sizeof(struct ns_worker_ctx));
		if (!ns_ctx) {
			return -1;
		}
		memset(ns_ctx, 0, sizeof(*ns_ctx));

		printf("Associating %s with lcore %d\n", entry->name, worker->lcore);
		ns_ctx->entry = entry;
		ns_ctx->next = worker->ns_ctx;
		worker->ns_ctx = ns_ctx;

		worker = worker->next;
		if (worker == NULL) {
			worker = g_workers;
		}

		entry = entry->next;
		if (entry == NULL) {
			entry = g_namespaces;
		}

	}

	return 0;
}

int main(int argc, char **argv)
{
	int rc;
	struct worker_thread *worker;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}
	struct spdk_env_opts opts;
	spdk_env_opts_init(&opts);
	opts.core_mask = g_core_mask ? g_core_mask : "0x1";
	if (spdk_env_init(&opts) < 0) {
		printf("Unable to initialize SPDK env");
		return 1;
	}

	task_pool = rte_mempool_create("task_pool", 8192,
				       sizeof(struct perf_task),
				       250, 0, NULL, NULL, task_ctor, NULL,
				       SOCKET_ID_ANY, 0);


	if (task_pool == NULL) {
		fprintf(stderr, "could not initialize  mempool\n");
		return 1;
	}
	g_tsc_rate = rte_get_timer_hz();
	g_avg_req_per_cycle = (double) g_lambda / (double) g_tsc_rate; 

	if (register_workers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (register_aio_files(argc, argv) != 0) {
		rc = -1;
		goto cleanup;
	}

	if (register_controllers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (associate_workers_with_ns() != 0) {
		rc = -1;
		goto cleanup;
	}

	printf("Initialization complete. Launching workers.\n");

	/* Launch all of the slave workers */
	worker = g_workers->next;
	while (worker != NULL) {
		rte_eal_remote_launch(work_fn, worker, worker->lcore);
		worker = worker->next;
	}

	rc = work_fn(g_workers);

	worker = g_workers->next;
	while (worker != NULL) {
		if (rte_eal_wait_lcore(worker->lcore) < 0) {
			rc = -1;
		}
		worker = worker->next;
	}

	print_stats();

cleanup:
	unregister_namespaces();
	unregister_controllers();
	unregister_workers();

	if (rc != 0) {
		fprintf(stderr, "%s: errors occured\n", argv[0]);
	}

	return rc;
}
