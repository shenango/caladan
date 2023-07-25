/*
 * test_base_lrpc.c - tests LRPC messaging
 */

#include <stdlib.h>
#include <pthread.h>

#include <base/init.h>
#include <base/log.h>
#include <base/assert.h>
#include <base/cpu.h>
#include <base/lrpc.h>
#include <base/time.h>

#define QUEUE_SIZE	128
#define N		1000000
#define QUIT		0XDEADBEEF

struct params {
	struct lrpc_msg	*client_buf, *server_buf;
	uint32_t	*client_wb, *server_wb;
};

static void client(struct params *p)
{
	struct lrpc_chan_out c_out;
	struct lrpc_chan_in c_in;
	double msgs_per_second;
	uint64_t start_us;
	uint64_t cmd;
	unsigned long payload;
	int ret, i;

	ret = lrpc_init_out(&c_out, p->server_buf, QUEUE_SIZE, p->server_wb);
	BUG_ON(ret);

	ret = lrpc_init_in(&c_in, p->client_buf, QUEUE_SIZE, p->client_wb);
	BUG_ON(ret);

	while (!lrpc_send(&c_out, 0, 0))
		cpu_relax();

	while (!lrpc_recv(&c_in, &cmd, &payload))
		cpu_relax();
	BUG_ON(cmd != 0);

	start_us = microtime();
	
	for (i = 0; i < N; i++) {
		while (!lrpc_send(&c_out, i, start_us))
			cpu_relax();

		while (!lrpc_recv(&c_in, &cmd, &payload))
			cpu_relax();
		BUG_ON(cmd != i);
		BUG_ON(payload != start_us);
	}

	msgs_per_second = (double)N / ((microtime() - start_us) * 0.000001);
	log_info("echoed %f messages / second", msgs_per_second);

	while (!lrpc_send(&c_out, QUIT, 0))
		cpu_relax();
}

static void server(struct params *p)
{
	struct lrpc_chan_out c_out;
	struct lrpc_chan_in c_in;
	uint64_t cmd;
	unsigned long payload;
	int ret;

	ret = lrpc_init_in(&c_in, p->server_buf, QUEUE_SIZE, p->server_wb);
	BUG_ON(ret);

	ret = lrpc_init_out(&c_out, p->client_buf, QUEUE_SIZE, p->client_wb);
	BUG_ON(ret);

	while (true) {
		while (!lrpc_recv(&c_in, &cmd, &payload))
			cpu_relax();

		if (cmd == QUIT)
			break;

		while (!lrpc_send(&c_out, cmd, payload))
			cpu_relax();
	}
}

static void *test_thread(void *data)
{
	int ret;

	ret = base_init_thread();
	if (ret) {
		log_err("base_init_thread() failed, ret = %d", ret);
		BUG();
	}
	BUG_ON(!perthread_read(thread_init_done));

	server((struct params *)data);
	return NULL;
}

int main(int argc, char *argv[])
{
	pthread_t tid;
	struct params p;
	int ret;

	ret = base_init();
	if (ret) {
		log_err("base_init() failed, ret = %d", ret);
		return 1;
	}
	BUG_ON(!base_init_done);

	ret = base_init_thread();
	if (ret) {
		log_err("base_init_thread() failed, ret = %d", ret);
		BUG();
	}
	BUG_ON(!perthread_read(thread_init_done));

	p.client_buf = malloc(sizeof(struct lrpc_msg) * QUEUE_SIZE);
	BUG_ON(!p.client_buf);
	memset(p.client_buf, 0, sizeof(struct lrpc_msg) * QUEUE_SIZE);

	p.client_wb = malloc(CACHE_LINE_SIZE);
	BUG_ON(!p.client_wb);
	memset(p.client_wb, 0, CACHE_LINE_SIZE);

	p.server_buf = malloc(sizeof(struct lrpc_msg) * QUEUE_SIZE);
	BUG_ON(!p.server_buf);
	memset(p.server_buf, 0, sizeof(struct lrpc_msg) * QUEUE_SIZE);

	p.server_wb = malloc(CACHE_LINE_SIZE);
	BUG_ON(!p.server_wb);
	memset(p.server_wb, 0, CACHE_LINE_SIZE);

	ret = pthread_create(&tid, NULL, test_thread, &p);
	BUG_ON(ret);

	client(&p);

	ret = pthread_join(tid, NULL);
	BUG_ON(ret);
	return 0;
}
