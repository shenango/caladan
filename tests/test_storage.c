/*
 * test_storage.c - writes and reads to the storage device
 */

#include <stdio.h>
#include <time.h>

#include <base/log.h>
#include <runtime/runtime.h>
#include <runtime/storage.h>
#include <runtime/timer.h>
#include <runtime/sync.h>

static void main_handler(void *arg)
{
	int ret;
	uint32_t block_size;
	char *buf;

	block_size = storage_block_size();
	log_info("num blocks: %lu", storage_num_blocks());
	log_info("block size: %u", block_size);
	log_info("writing 'hello world' to device...");
	buf = malloc(block_size);
	BUG_ON(!buf);
	sprintf(buf, "hello world");
	ret = storage_write(buf, 0, 1);
	if (ret) {
		log_err("failed to init storage");
		return;
	}
	sprintf(buf, "cleared");

	log_debug("reading from device...");
	ret = storage_read(buf, 0, 1);
	if (ret) {
		log_err("failed to read");
	}
	log_info("data read: %s", buf);
	free(buf);
}

int main(int argc, char *argv[])
{
	int ret;

	ret = runtime_init(argv[1], main_handler, NULL);
	if (ret) {
		log_err("failed to start runtime");
		return ret;
	}
	return 0;
}
