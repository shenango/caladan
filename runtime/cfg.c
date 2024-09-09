/*
 * cfg.c - configuration file support
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#include <base/stddef.h>
#include <base/bitmap.h>
#include <base/log.h>
#include <base/cpu.h>
#include <base/mem.h>

#include "defs.h"

static size_t arp_static_sz;
size_t arp_static_count;
struct cfg_arp_static_entry *static_entries;
int preferred_socket = 0;

/*
 * Configuration Options
 */

static int str_to_ip(const char *str, uint32_t *addr)
{
	uint8_t a, b, c, d;
	if(sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) {
		return -EINVAL;
	}

	*addr = MAKE_IP_ADDR(a, b, c, d);
	return 0;
}

static int str_to_mac(const char *str, struct eth_addr *addr)
{
	int i;
	static const char *fmts[] = {
		"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
		"%hhx-%hhx-%hhx-%hhx-%hhx-%hhx",
		"%hhx%hhx%hhx%hhx%hhx%hhx"
	};

	for (i = 0; i < ARRAY_SIZE(fmts); i++) {
		if (sscanf(str, fmts[i], &addr->addr[0], &addr->addr[1],
			   &addr->addr[2], &addr->addr[3], &addr->addr[4],
			   &addr->addr[5]) == 6) {
			return 0;
		}
	}
	return -EINVAL;
}

static int str_to_long(const char *str, long *val)
{
	char *endptr;

	*val = strtol(str, &endptr, 10);
	if (endptr == str || (*endptr != '\0' && *endptr != '\n') ||
	    ((*val == LONG_MIN || *val == LONG_MAX) && errno == ERANGE))
		return -EINVAL;
	return 0;
}

static int parse_host_ip(const char *name, const char *val)
{
	uint32_t *addr;
	int ret;

	if (!strcmp(name, "host_addr"))
		addr = &netcfg.addr;
	else if (!strcmp(name, "host_netmask"))
		addr = &netcfg.netmask;
	else if (!strcmp(name, "host_gateway"))
		addr = &netcfg.gateway;
	else
		return -EINVAL;

	if (!val)
		return -EINVAL;

	ret = str_to_ip(val, addr);
	if (ret)
		return ret;

	if (!strcmp(name, "host_netmask") &&
	    (!*addr || ((~*addr + 1) & ~*addr))) {
		log_err("invalid netmask");
		return -EINVAL;
	}

	if (*addr >> 24 == 127) {
		log_err("IP address can't be local subnet");
		return -EINVAL;
	}

	return 0;
}

static int parse_runtime_kthreads(const char *name, const char *val)
{
	long tmp;
	int ret;

	ret = str_to_long(val, &tmp);
	if (ret)
		return ret;

	if (tmp < 1 || tmp > cpu_count - 1) {
		log_err("invalid number of kthreads requested, '%ld'", tmp);
		log_err("must be > 0 and < %d (number of CPUs)", cpu_count);
		return -EINVAL;
	}

	maxks = tmp;
	return 0;
}

static int parse_runtime_spinning_kthreads(const char *name, const char *val)
{
	long tmp;
	int ret;

	ret = str_to_long(val, &tmp);
	if (ret)
		return ret;

	if (tmp < 0) {
		log_err("invalid number of spinning kthreads requests, '%ld', "
			"must be > 0", tmp);
		return -EINVAL;
	}

	spinks = tmp;
	return 0;
}

static int parse_runtime_guaranteed_kthreads(const char *name, const char *val)
{
	long tmp;
	int ret;

	ret = str_to_long(val, &tmp);
	if (ret)
		return ret;

	if (tmp > cpu_count - 1) {
		log_err("invalid number of guaranteed kthreads requested, '%ld'", tmp);
		log_err("must be < %d (number of CPUs)", cpu_count);
		return -EINVAL;
	}

	guaranteedks = tmp;
	return 0;
}

static int parse_runtime_priority(const char *name, const char *val)
{
	if (!strcmp(val, "lc")) {
		cfg_prio_is_lc = true;
	} else if (!strcmp(val, "be")) {
		cfg_prio_is_lc = false;
	} else {
		log_err("invalid runtime priority");
		return -EINVAL;
	}

	return 0;
}

static int parse_runtime_ht_punish_us(const char *name, const char *val)
{
	long tmp;
	int ret;

	ret = str_to_long(val, &tmp);
	if (ret)
		return ret;

	if (tmp < 0) {
		log_err("runtime_ht_punish_us must be positive");
		return -EINVAL;
	}

	cfg_ht_punish_us = tmp;
	return 0;
}

static int parse_runtime_qdelay_us(const char *name, const char *val)
{
	long tmp;
	int ret;

	ret = str_to_long(val, &tmp);
	if (ret)
		return ret;

	if (tmp < 0) {
		log_err("runtime_qdelay_us must be positive");
		return -EINVAL;
	}

	cfg_qdelay_us = tmp;
	return 0;
}

static int parse_runtime_quantum_us(const char *name, const char *val)
{
	long tmp;
	int ret;

	ret = str_to_long(val, &tmp);
	if (ret)
		return ret;

	if (tmp < 0) {
		log_err("runtime_quantum_us must be positive");
		return -EINVAL;
	}

	cfg_quantum_us = tmp;
	return 0;
}

static int parse_mac_address(const char *name, const char *val)
{
	log_warn("specifying mac address is deprecated.");
	return 0;
}

static int parse_mtu(const char *num, const char *val)
{
	long tmp;
	int ret;

	ret = str_to_long(val, &tmp);
	if (ret)
		return ret;

	if (tmp < 0 || tmp > ETH_MAX_MTU) {
		log_err("MTU must be positive and <= %d, got %ld",
			ETH_MAX_MTU, tmp);
		return -EINVAL;
	}

	eth_mtu = tmp;
	return 0;
}

static int parse_watchdog_flag(const char *name, const char *val)
{
	disable_watchdog = true;
	return 0;
}

static int parse_static_arp_entry(const char *name, const char *val)
{
	int ret;

	if (arp_static_sz == arp_static_count) {
		arp_static_sz = MAX(32, (arp_static_count + 1) * 2);
		static_entries = reallocarray(static_entries, arp_static_sz, sizeof(*static_entries));
		if (!static_entries)
			return -ENOMEM;
	}

	ret = str_to_ip(val, &static_entries[arp_static_count].ip);
	if (ret) {
		log_err("Could not parse ip: %s", val);
		return ret;
	}

	ret = str_to_mac(strtok(NULL, " "),
			 &static_entries[arp_static_count].addr);
	if (ret) {
		log_err("Could not parse mac: %s", val);
		return ret;
	}

	arp_static_count++;

	return 0;
}

static int parse_log_level(const char *name, const char *val)
{
	long tmp;
	int ret;

	ret = str_to_long(val, &tmp);
	if (ret)
		return ret;

	if (tmp < LOG_EMERG || tmp > LOG_DEBUG) {
		log_err("log level must be between %d and %d",
			LOG_EMERG, LOG_DEBUG);
		return -EINVAL;
	}

	max_loglevel = tmp;
	return 0;
}

static int parse_preferred_socket(const char *name, const char *val)
{
	long tmp;
	int ret;

	ret = str_to_long(val, &tmp);
	if (ret)
		return ret;

	if (tmp < 0 || tmp > numa_count - 1) {
		log_err("invalid preferred socket requested, '%ld'", tmp);
		log_err("must be >=0 and < %d (number of NUMA nodes)", numa_count);
		return -EINVAL;
	}

	preferred_socket = tmp;
	return 0;
}

static int parse_enable_storage(const char *name, const char *val)
{
#ifdef DIRECT_STORAGE
	cfg_storage_enabled = true;
	return 0;
#else
	log_err("cfg: cannot enable storage, "
		"please recompile with storage support");
	return -EINVAL;
#endif
}

static int parse_enable_directpath(const char *name, const char *val)
{
#ifdef DIRECTPATH
	return directpath_parse_arg(name, val);
#else
	log_err("cfg: cannot enable directpath, "
		"please recompile with directpath support");
	return -EINVAL;
#endif
}

static int parse_enable_gc(const char *name, const char *val)
{
#ifdef GC
	panic("GC support is currently broken");
	cfg_gc_enabled = true;
	return 0;
#else
	log_err("cfg: cannot enable GC, please recompile with GC support");
	return -EINVAL;
#endif
}

static int parse_enable_transparent_hugepages(const char *name, const char *val)
{
  cfg_transparent_hugepages_enabled = true;
  return 0;
}

/*
 * Parsing Infrastructure
 */


static LIST_HEAD(dyn_cfg_handlers);
static unsigned int nr_dyn_cfg_handlers;

void cfg_register(struct cfg_handler *h)
{
	list_add_tail(&dyn_cfg_handlers, &h->link);
	nr_dyn_cfg_handlers++;
}

static const struct cfg_handler cfg_handlers[] = {
	{ "host_addr", parse_host_ip, true },
	{ "host_netmask", parse_host_ip, true },
	{ "host_gateway", parse_host_ip, true },
	{ "host_mac", parse_mac_address, false },
	{ "host_mtu", parse_mtu, false },
	{ "runtime_kthreads", parse_runtime_kthreads, true },
	{ "runtime_spinning_kthreads", parse_runtime_spinning_kthreads, false },
	{ "runtime_guaranteed_kthreads", parse_runtime_guaranteed_kthreads,
			false },
	{ "runtime_priority", parse_runtime_priority, false },
	{ "runtime_ht_punish_us", parse_runtime_ht_punish_us, false },
	{ "runtime_qdelay_us", parse_runtime_qdelay_us, false },
	{ "runtime_quantum_us", parse_runtime_quantum_us, false },
	{ "static_arp", parse_static_arp_entry, false },
	{ "log_level", parse_log_level, false },
	{ "disable_watchdog", parse_watchdog_flag, false },
	{ "preferred_socket", parse_preferred_socket, false },
	{ "enable_storage", parse_enable_storage, false },
	{ "enable_directpath", parse_enable_directpath, false },
	{ "enable_gc", parse_enable_gc, false },
	{ "enable_transparent_hugepages", parse_enable_transparent_hugepages, false},

};

/**
 * cfg_load - loads the configuration file
 * @path: a path to the configuration file
 *
 * Returns 0 if successful, otherwise fail.
 */
int cfg_load(const char *path)
{
	FILE *f;
	char buf[BUFSIZ];
	size_t handler_cnt = ARRAY_SIZE(cfg_handlers) + nr_dyn_cfg_handlers;
	DEFINE_BITMAP(parsed, handler_cnt);
	char *name, *val;
	int i, ret = 0, line = 0;
	size_t len;
	struct list_node *cur;
	const struct cfg_handler *h;

	bitmap_init(parsed, handler_cnt, 0);

	log_info("loading configuration from '%s'", path);

	f = fopen(path, "r");
	if (!f) {
		log_err("Could not find configuation file %s (%s)", path, strerror(errno));
		return -errno;
	}

	while (fgets(buf, sizeof(buf), f)) {
		if (buf[0] == '#' || buf[0] == '\n') {
			line++;
			continue;
		}
		name = strtok(buf, " ");
		if (!name)
			break;
		val = strtok(NULL, " ");

		if (!val) {
			log_err("config option with missing value on line %d", line);
			ret = -EINVAL;
			goto out;
		}

		len = strlen(val);
		if (val[len - 1] == '\n')
			val[len - 1] = '\0';

		cur = dyn_cfg_handlers.n.next;
		for (i = 0; i < handler_cnt; i++) {
			if (i < ARRAY_SIZE(cfg_handlers)) {
				h = &cfg_handlers[i];
			} else {
				h = list_entry(cur, struct cfg_handler, link);
				cur = cur->next;
			}
			if (!strncmp(name, h->name, BUFSIZ)) {
				ret = h->fn(name, val);
				if (ret) {
					log_err("bad config option on line %d",
						line);
					goto out;
				}
				bitmap_set(parsed, i);
				break;
			}
		}

		if (i == handler_cnt) {
			log_warn("unrecognized config option on line %d", line);
			ret = -EINVAL;
			goto out;
		}

		line++;
	}

	cur = dyn_cfg_handlers.n.next;
	for (i = 0; i < handler_cnt; i++) {
		if (i < ARRAY_SIZE(cfg_handlers)) {
			h = &cfg_handlers[i];
		} else {
			h = list_entry(cur, struct cfg_handler, link);
			cur = cur->next;
		}
		if (h->required && !bitmap_test(parsed, i)) {
			log_err("missing required config option '%s'", h->name);
			ret = -EINVAL;
			goto out;
		}
	}

	if (guaranteedks > maxks) {
		log_err("invalid number of guaranteed kthreads requested, '%d'",
				guaranteedks);
		log_err("must be <= %d (number of kthreads)", maxks);
		ret = -EINVAL;
		goto out;
	}

	/* log some relevant config parameters */
	log_info("cfg: provisioned %d cores "
		 "(%d guaranteed, %d burstable, %d spinning)",
		 maxks, guaranteedks, maxks - guaranteedks, spinks);
	log_info("cfg: task is %s",
		 cfg_prio_is_lc ? "latency critical (LC)" : "best effort (BE)");
	log_info("cfg: THRESH_QD: %ld, THRESH_HT: %ld THRESH_QUANTUM: %ld",
		 cfg_qdelay_us, cfg_ht_punish_us, cfg_quantum_us);
	log_info("cfg: storage %s, directpath %s, transparent hugepages %s",
#ifdef DIRECT_STORAGE
		 cfg_storage_enabled ? "enabled" : "disabled",
#else
		"disabled",
#endif
		 cfg_directpath_enabled() ? "enabled" : "disabled",
		 cfg_transparent_hugepages_enabled ? "enabled" : "disabled");

out:
	fclose(f);
	return ret;
}
