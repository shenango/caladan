/*-----------------------------------------------------------------------*/
/* Program: Stream                                                       */
/* Revision: $Id: stream.c,v 5.9 2009/04/11 16:35:00 mccalpin Exp $ */
/* Original code developed by John D. McCalpin                           */
/* Programmers: John D. McCalpin                                         */
/*              Joe R. Zagar                                             */
/*                                                                       */
/* This program measures memory transfer rates in MB/s for simple        */
/* computational kernels coded in C.                                     */
/*-----------------------------------------------------------------------*/
/* Copyright 1991-2005: John D. McCalpin                                 */
/*-----------------------------------------------------------------------*/
/* License:                                                              */
/*  1. You are free to use this program and/or to redistribute           */
/*     this program.                                                     */
/*  2. You are free to modify this program for your own use,             */
/*     including commercial use, subject to the publication              */
/*     restrictions in item 3.                                           */
/*  3. You are free to publish results obtained from running this        */
/*     program, or from works that you derive from this program,         */
/*     with the following limitations:                                   */
/*     3a. In order to be referred to as "STREAM benchmark results",     */
/*         published results must be in conformance to the STREAM        */
/*         Run Rules, (briefly reviewed below) published at              */
/*         http://www.cs.virginia.edu/stream/ref.html                    */
/*         and incorporated herein by reference.                         */
/*         As the copyright holder, John McCalpin retains the            */
/*         right to determine conformity with the Run Rules.             */
/*     3b. Results based on modified source code or on runs not in       */
/*         accordance with the STREAM Run Rules must be clearly          */
/*         labelled whenever they are published.  Examples of            */
/*         proper labelling include:                                     */
/*         "tuned STREAM benchmark results"                              */
/*         "based on a variant of the STREAM benchmark code"             */
/*         Other comparable, clear and reasonable labelling is           */
/*         acceptable.                                                   */
/*     3c. Submission of results to the STREAM benchmark web site        */
/*         is encouraged, but not required.                              */
/*  4. Use of this program or creation of derived works based on this    */
/*     program constitutes acceptance of these licensing restrictions.   */
/*  5. Absolutely no warranty is expressed or implied.                   */
/*-----------------------------------------------------------------------*/

#include <float.h>
#include <limits.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdint>

#define CACHELINE 64
#define SHM_KEY (0x123)

/* INSTRUCTIONS:
 *
 *	1) Stream requires a good bit of memory to run.  Adjust the
 *          value of 'N' (below) to give a 'timing calibration' of
 *          at least 20 clock-ticks.  This will provide rate estimates
 *          that should be good to about 5% precision.
 */

#define MAX_NUM_THREADS 48

#ifndef STRIDE
#define STRIDE (64 / sizeof(double))
#endif

/*
  node   0   1   2   3   4   5   6   7
  0:  10  16  16  22  16  22  16  22
  1:  16  10  16  22  22  16  22  16
  2:  16  16  10  16  16  16  16  22
  3:  22  22  16  10  16  16  22  16
  4:  16  22  16  16  10  16  16  16
  5:  22  16  16  16  16  10  22  22
  6:  16  22  16  22  16  22  10  16
  7:  22  16  22  16  16  22  16  10
*/

#ifndef OFFSET
#define OFFSET 0
#endif

/*
 *	3) Compile the code with full optimization.  Many compilers
 *	   generate unreasonably bad code before the optimizer tightens
 *	   things up.  If the results are unreasonably good, on the
 *	   other hand, the optimizer might be too smart for me!
 *
 *         Try compiling with:
 *               cc -O stream_omp.c -o stream_omp
 *
 *         This is known to work on Cray, SGI, IBM, and Sun machines.
 *
 *
 *	4) Mail the results to mccalpin@cs.virginia.edu
 *	   Be sure to include:
 *		a) computer hardware model number and software revision
 *		b) the compiler flags
 *		c) all of the output from the test case.
 * Thanks!
 *
 */

#define HLINE "-------------------------------------------------------------\n"

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif
#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

#define OPS_BATCH (1 << 14)
#define YIELD_CNT (1 << 6)

static double **a, **b, **c;
static const char *label[4] = {"Copy", "Scale", "Add", "Triad"};
static double ops_factor[4] = {0, 1, 1, 2};
volatile double *ops;

int N;

void *copyProc(void *arg) {
	int local_ops = 0;
	int yield_cnt = 0;
	int me = (long long)arg;

	double *a2 = a[me];
	double *c2 = c[me];
	int ops_idx = me * CACHELINE / sizeof(double);
	ops[ops_idx] = 0;

	while (1) {
		for (int j = 0; j < N; j += STRIDE) {
			c2[j] = a2[j];
			local_ops++;
			if (local_ops == OPS_BATCH) {
				local_ops = 0;
				ops[ops_idx] += ops_factor[0] * OPS_BATCH;
				yield_cnt++;
				if (yield_cnt == YIELD_CNT) {
					yield_cnt = 0;
					sched_yield();
				}
			}
		}
	}
}

void *scaleProc(void *arg) {
	int local_ops = 0;
	int yield_cnt = 0;
	int me = (long long)arg;

	double *b2 = b[me];
	double *c2 = c[me];
	int ops_idx = me * CACHELINE / sizeof(double);
	ops[ops_idx] = 0;

	while (1) {
		for (int j = 0; j < N; j += STRIDE) {
			b2[j] = 3.0 * c2[j];
			local_ops++;
			if (local_ops == OPS_BATCH) {
				local_ops = 0;
				ops[ops_idx] += ops_factor[1] * OPS_BATCH;
				yield_cnt++;
				if (yield_cnt == YIELD_CNT) {
					yield_cnt = 0;
					sched_yield();
				}
			}
		}
	}
}

void *addProc(void *arg) {
	int local_ops = 0;
	int yield_cnt = 0;
	int me = (long long)arg;

	double *a2 = a[me];
	double *b2 = b[me];
	double *c2 = c[me];
	int ops_idx = me * CACHELINE / sizeof(double);
	ops[ops_idx] = 0;

	while (1) {
		for (int j = 0; j < N; j += STRIDE) {
			c2[j] = a2[j] + b2[j];
			local_ops++;
			if (local_ops == OPS_BATCH) {
				local_ops = 0;
				ops[ops_idx] += ops_factor[2] * OPS_BATCH;
				yield_cnt++;
				if (yield_cnt == YIELD_CNT) {
					yield_cnt = 0;
					sched_yield();
				}
			}
		}
	}
}

void *triadProc(void *arg) {
	int local_ops = 0;
	int yield_cnt = 0;
	int me = (long long)arg;

	double *a2 = a[me];
	double *b2 = b[me];
	double *c2 = c[me];
	int ops_idx = me * CACHELINE / sizeof(double);
	ops[ops_idx] = 0;

	while (1) {
		for (int j = 0; j < N; j += STRIDE) {
			a2[j] = b2[j] + 3.0 * c2[j];
			local_ops++;
			if (local_ops == OPS_BATCH) {
				local_ops = 0;
				ops[ops_idx] += ops_factor[3] * OPS_BATCH;
				yield_cnt++;
				if (yield_cnt == YIELD_CNT) {
					yield_cnt = 0;
					sched_yield();
				}
			}
		}
	}
}

void *aligned_malloc(int size) {
	char *mem = (char *)malloc(size + CACHELINE + sizeof(void *));
	void **ptr = (void **)((uintptr_t)(mem + CACHELINE + sizeof(void *)) &
			       ~(CACHELINE - 1));
	ptr[-1] = mem;
	return ptr;
}

int main(int argc, char **argv) {

	if (argc != 4) {
		puts("Usage: stream [N] [#threads] [spec]");
		exit(-1);
	}

	N = atoi(argv[1]);
	int num_threads = atoi(argv[2]);

	int spec_idx = -1;
	for (int i = 0; i < 4; i++) {
		if (!strcmp(argv[3], label[i])) {
			spec_idx = i;
			break;
		}
	}
	if (spec_idx == -1) {
		puts("Unknown work spec.");
		exit(-1);
	}

	int shmid =
		shmget((key_t)SHM_KEY, CACHELINE * MAX_NUM_THREADS, 0666 | IPC_CREAT);
	void *shm = NULL;
	shm = shmat(shmid, 0, 0);
	ops = (double *)shm;

	pthread_t *threads = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
	pthread_attr_t pthread_custom_attr;
	pthread_attr_init(&pthread_custom_attr);

	/* Allocate memory for the threads */
	a = (double **)malloc(sizeof(double *) * num_threads);
	b = (double **)malloc(sizeof(double *) * num_threads);
	c = (double **)malloc(sizeof(double *) * num_threads);

	for (int i = 0; i < num_threads; i++) {
		a[i] = (double *)aligned_malloc(sizeof(double) * N);
		b[i] = (double *)aligned_malloc(sizeof(double) * N);
		c[i] = (double *)aligned_malloc(sizeof(double) * N);
		if ((a[i] == NULL) || (b[i] == NULL) || (c[i] == NULL)) {
			printf("Failed to allocate %d bytes\n", (int)sizeof(double) * N);
			exit(-1);
		}

		for (int j = 0; j < N; j++) {
			a[i][j] = 1.0;
			b[i][j] = 2.0;
			c[i][j] = 0.0;
		}
	}

	if (spec_idx == 0) {
		/* Copy */
		for (long long i = 0; i < num_threads; i++) {
			pthread_create(&threads[i], &pthread_custom_attr, copyProc, (void *)i);
		}
	} else if (spec_idx == 1) {
		/* SCALE */
		for (long long i = 0; i < num_threads; i++) {
			pthread_create(&threads[i], &pthread_custom_attr, scaleProc, (void *)i);
		}
	} else if (spec_idx == 2) {
		/* ADD */
		for (long long i = 0; i < num_threads; i++) {
			pthread_create(&threads[i], &pthread_custom_attr, addProc, (void *)i);
		}
	} else if (spec_idx == 3) {
		/* TRIAD */
		for (long long i = 0; i < num_threads; i++) {
			pthread_create(&threads[i], &pthread_custom_attr, triadProc, (void *)i);
		}
	}
	pthread_join(threads[0], NULL);

	return 0;
}
