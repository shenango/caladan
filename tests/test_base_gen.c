/*
 * test_base_gen.c - tests generation numbers
 */

#include <base/gen.h>
#include <base/init.h>
#include <base/log.h>

static void test_gen(void)
{
	uint32_t gen;
	struct gen_num gen_writer, gen_reader;

	/* init */
	gen = 0;
	gen_init(&gen_writer, &gen);
	gen_init(&gen_reader, &gen);

	/* no gen -> gen */
	gen_active(&gen_writer);
	BUG_ON(gen_in_same_gen(&gen_reader));
	BUG_ON(!gen_in_same_gen(&gen_reader));

	/* gen -> gen */
	gen_active(&gen_writer);
	BUG_ON(!gen_in_same_gen(&gen_reader));

	/* gen -> no gen -> gen */
	gen_inactive(&gen_writer);
	gen_active(&gen_writer);
	BUG_ON(gen_in_same_gen(&gen_reader));

	/* gen -> no gen */
	gen_inactive(&gen_writer);
	BUG_ON(gen_in_same_gen(&gen_reader));
	BUG_ON(gen_in_same_gen(&gen_reader));

	/* no gen -> no gen */
	gen_inactive(&gen_writer);
	BUG_ON(gen_in_same_gen(&gen_reader));

	/* no gen -> gen -> no gen */
	gen_active(&gen_writer);
	gen_inactive(&gen_writer);
	BUG_ON(gen_in_same_gen(&gen_reader));

	log_debug("success");
}

int main(int argc, char *argv[])
{
	int ret;

	ret = base_init();
	if (ret) {
		log_err("base_init() failed, ret = %d", ret);
		return 1;
	}
	BUG_ON(!base_init_done);

	test_gen();
	return 0;
}
