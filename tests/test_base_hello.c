/*
 * test_base_hello.c - this tests verifies that the base library can initialize
 */

#include <base/init.h>
#include <base/log.h>
#include <base/assert.h>

int main(int argc, char *argv[])
{
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
		return 1;
	}
	BUG_ON(!perthread_read(thread_init_done));

	log_info("hello world!");
	return 0;
}
