
#include <base/kref.h>
#include <base/mempool.h>
#include <runtime/sync.h>
#include <base/log.h>

#ifdef DIRECTPATH

#include "defs.h"

/* configuration options */
struct pci_addr nic_pci_addr;
int cfg_directpath_mode = DIRECTPATH_MODE_DISABLED;

struct mempool directpath_buf_mp;
struct tcache *directpath_buf_tcache;
DEFINE_PERTHREAD(struct tcache_perthread, directpath_buf_pt);

int directpath_parse_arg(const char *name, const char *val)
{
	if (strncmp(val, "fs", strlen("fs")) == 0)
		cfg_directpath_mode = DIRECTPATH_MODE_FLOW_STEERING;
	else if (strncmp(val, "qs", strlen("qs")) == 0)
		cfg_directpath_mode = DIRECTPATH_MODE_QUEUE_STEERING;
	else if (strncmp(val, "ext", strlen("ext")) == 0)
		cfg_directpath_mode = DIRECTPATH_MODE_EXTERNAL;
	else
		cfg_directpath_mode = DIRECTPATH_MODE_ALLOW_ANY;

	return 0;
}

static int parse_directpath_pci(const char *name, const char *val)
{
	log_warn("WARNING: directpath_pci is ignored, using pci address from the IOKernel");
	return 0;
}

static struct cfg_handler directpath_pci_handler = {
	.name = "directpath_pci",
	.fn = parse_directpath_pci,
	.required = false,
};

REGISTER_CFG(directpath_pci_handler);

size_t directpath_rx_buf_pool_sz(unsigned int nrqs)
{
	size_t buflen = MBUF_DEFAULT_LEN;
	buflen *= MAX(8, guaranteedks) * (16 * RQ_NUM_DESC) * 2UL;
	return align_up(buflen, PGSIZE_2MB);
}

void directpath_rx_completion(struct mbuf *m)
{
	preempt_disable();
	tcache_free(perthread_ptr(directpath_buf_pt), (void *)m);
	preempt_enable();
}

static int rx_memory_init(void)
{
	int ret;

	/* for external mode, memory is provided after init */
	if (cfg_directpath_mode == DIRECTPATH_MODE_EXTERNAL)
		return 0;

	ret = mempool_create(&directpath_buf_mp, iok.rx_buf, iok.rx_len, PGSIZE_2MB,
			     directpath_get_buf_size());
	if (ret)
		return ret;

	directpath_buf_tcache = mempool_create_tcache(&directpath_buf_mp,
		"runtime_rx_bufs", TCACHE_DEFAULT_MAG_SIZE);
	if (!directpath_buf_tcache)
		return -ENOMEM;

	return 0;
}

int directpath_init(void)
{
	int ret;

	if (!cfg_directpath_enabled())
		return 0;

	ret = rx_memory_init();
	if (ret)
		return ret;

	memcpy(&nic_pci_addr, &iok.iok_info->directpath_pci, sizeof(nic_pci_addr));

	log_info("directpath: using pci address from iokernel: %04hx:%02hhx:%02hhx.%hhd",
	         nic_pci_addr.domain, nic_pci_addr.bus,
	         nic_pci_addr.slot, nic_pci_addr.func);

	ret = mlx5_init();
	if (!ret)
		return 0;

	if (getuid() != 0)
		log_err("Could not initialize directpath. Please try again as root.");
	else
		log_err("Could not initialize directpath, ret = %d", ret);

	return ret;
}

int directpath_init_thread(void)
{
	if (!cfg_directpath_enabled())
		return 0;

	if (cfg_directpath_mode != DIRECTPATH_MODE_EXTERNAL) {
		tcache_init_perthread(directpath_buf_tcache,
		                      perthread_ptr(directpath_buf_pt));
	}

	return mlx5_init_thread();
}

static DEFINE_SPINLOCK(flow_worker_lock);
static thread_t *flow_worker_th;
static LIST_HEAD(flow_to_register);
static LIST_HEAD(flow_to_deregister);

static void flow_registration_worker(void *arg)
{
	int ret;
	struct flow_registration *f;

	while (true) {
		spin_lock_np(&flow_worker_lock);
		f = list_pop(&flow_to_register, struct flow_registration, flow_reg_link);
		if (f) {
			spin_unlock_np(&flow_worker_lock);
			ret = net_ops.register_flow(f->kthread_affinity, f->e, &f->hw_flow_handle);
			WARN_ON(ret);
			continue;
		}

		f = list_pop(&flow_to_deregister, struct flow_registration, flow_dereg_link);
		if (f) {
			spin_unlock_np(&flow_worker_lock);
			ret = net_ops.deregister_flow(f->e, f->hw_flow_handle);
			WARN_ON(ret);
			kref_put(f->ref, f->release);
			continue;
		}

		flow_worker_th = thread_self();
		thread_park_and_unlock_np(&flow_worker_lock);
	}
}

void register_flow(struct flow_registration *f)
{
	if (!cfg_directpath_enabled())
		return;

	if (cfg_directpath_mode != DIRECTPATH_MODE_FLOW_STEERING)
		return;

	/* take a reference for the hardware flow table */
	kref_get(f->ref);

	spin_lock_np(&flow_worker_lock);
	list_add(&flow_to_register, &f->flow_reg_link);
	if (flow_worker_th) {
		thread_ready(flow_worker_th);
		flow_worker_th = NULL;
	}
	spin_unlock_np(&flow_worker_lock);

}

void deregister_flow(struct flow_registration *f)
{
	if (!cfg_directpath_enabled())
		return;

	if (cfg_directpath_mode != DIRECTPATH_MODE_FLOW_STEERING)
		return;

	spin_lock_np(&flow_worker_lock);
	list_add(&flow_to_deregister, &f->flow_dereg_link);
	if (flow_worker_th) {
		thread_ready(flow_worker_th);
		flow_worker_th = NULL;
	}
	spin_unlock_np(&flow_worker_lock);
}

int directpath_init_late(void)
{
	if (!cfg_directpath_enabled())
		return 0;

	if (cfg_directpath_mode != DIRECTPATH_MODE_FLOW_STEERING)
		return 0;

	return thread_spawn(flow_registration_worker, NULL);
}

#else

int directpath_init(void)
{
	return 0;
}

int directpath_init_thread(void)
{
	return 0;
}

int directpath_init_late(void)
{
	return 0;
}


#endif
