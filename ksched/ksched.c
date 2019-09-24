/*
 * ksched.c - an accelerated scheduler interface for the IOKernel
 */

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/local.h>
#include <asm/irq_vectors.h>
#include <asm/set_memory.h>
#include <asm/msr-index.h>
#include <asm/msr.h>
#include <asm/mwait.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <linux/capability.h>
#include <linux/cdev.h>
#include <linux/cpuidle.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/sort.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/smp.h>
#include <linux/uaccess.h>

#include "ksched.h"
#include "../iokernel/pmc.h"

#define KSCHED_PMC_PROBE_DELAY (1)
#define CORE_PERF_GLOBAL_CTRL_ENABLE_PMC_0 (0x1)
#define CORE_PERF_GLOBAL_CTRL_ENABLE_PMC_1 (0x2)

#ifdef ZAIN_VECTOR
#ifndef SUPPRESS_CUSTOMIZED_IPI_HANDLER
#define OS_SUPPORT_CUSTOMIZED_IPI_HANDER
#endif
#endif

#define MSR_APIC_BASE (0x1B)
#define MSR_X2APIC_ICR (0x830)
#define X2APIC_ENABLED(apic_status) (apic_status >> 10)
#define ICR_LOGICAL_MODE (1 << 11)
#define ICR_DEST_FIELD(x) (((unsigned long long)x) << 32)
#define MSR_X2APIC_LDR (0x80D)
#define X2APIC_LDR_MAX_LOGICAL_IDS (16)

#ifdef OS_SUPPORT_CUSTOMIZED_IPI_HANDER

/* so far only use the cores from socket 0 */
#define FOR_ALL_CORES(i) for (i = 0; i < 48; i += 2)

struct ldr_info {
	unsigned int ldr;
	unsigned short id;
};

static unsigned int ldrs[NR_CPUS];
static struct ldr_info ipi_ldrs[NR_CPUS];
int ipi_ldrs_num = 0;

#endif

/* the character device that provides the ksched IOCTL interface */
static struct cdev ksched_cdev;
/* the character device that mmaps the PCI configuration space */
static struct cdev pci_cfg_cdev;

/* shared memory between the IOKernel and the Linux Kernel */
static __read_mostly struct ksched_shm_cpu *shm;
#define SHM_SIZE (NR_CPUS * sizeof(struct ksched_shm_cpu))

struct ksched_percpu {
	unsigned int	last_gen;
	local_t		busy;
	struct task_struct		*running_task;
};

/* per-cpu data to coordinate context switching and signal delivery */
static DEFINE_PER_CPU(struct ksched_percpu, kp);

/**
 * ksched_lookup_task - retreives a task from a pid number
 * @nr: the pid number
 *
 * Returns a task pointer or NULL if none was found.
 */
static struct task_struct *ksched_lookup_task(pid_t nr)
{
	struct pid *pid;

	pid = find_vpid(nr);
	if (unlikely(!pid))
		return NULL;
	return pid_task(pid, PIDTYPE_PID);
}

static void ksched_next_tid(struct ksched_percpu *kp, int cpu, pid_t tid)
{
	struct task_struct *p;
	int ret;

	/* release previous task */
	if (kp->running_task) {
		put_task_struct(kp->running_task);
		kp->running_task = NULL;
	}

	if (unlikely(tid == 0))
		return;

	rcu_read_lock();
	p = ksched_lookup_task(tid);
	if (unlikely(!p)) {
		rcu_read_unlock();
		return;
	}

	if (WARN_ON_ONCE(p->on_cpu || p->state == TASK_WAKING ||
			 p->state == TASK_RUNNING)) {
		rcu_read_unlock();
		return;
	}

	ret = set_cpus_allowed_ptr(p, cpumask_of(cpu));
	if (unlikely(ret)) {
		rcu_read_unlock();
		return;
	}

	get_task_struct(p);
	kp->running_task = p;

	wake_up_process(p);
	rcu_read_unlock();

	return;
}

static int ksched_mwait_on_addr(const unsigned int *addr, unsigned int hint,
				unsigned int val)
{
	unsigned int cur;

	lockdep_assert_irqs_disabled();

	/* first see if the condition is met without waiting */
	cur = smp_load_acquire(addr);
	if (cur != val)
		return cur;

	/* then arm the monitor address and recheck to avoid a race */
	__monitor(addr, 0, 0);
	cur = smp_load_acquire(addr);
	if (cur != val)
		return cur;

	/* finally, execute mwait, and recheck after waking up */
	__mwait(hint, MWAIT_ECX_INTERRUPT_BREAK);
	return smp_load_acquire(addr);
}

static int __cpuidle ksched_idle(struct cpuidle_device *dev,
				 struct cpuidle_driver *drv, int index)
{
	struct ksched_percpu *p;
	struct ksched_shm_cpu *s;
	unsigned long gen;
	unsigned int hint;
	pid_t tid;
	int cpu;

	lockdep_assert_irqs_disabled();

	cpu = get_cpu();
	p = this_cpu_ptr(&kp);
	s = &shm[cpu];

	/* check if we entered the idle loop with a process still active */
	if (p->running_task) {
			if (unlikely(p->running_task->flags & PF_EXITING)) {
				put_task_struct(p->running_task);
				p->running_task = NULL;
			} else {
				ksched_mwait_on_addr(&s->gen, 0, s->gen);
				put_cpu();
				return index;
			}
	}

	/* mark the core as idle if a new request isn't waiting */
	local_set(&p->busy, false);
	if (s->busy && smp_load_acquire(&s->gen) == p->last_gen)
		WRITE_ONCE(s->busy, false);

	/* use the mwait instruction to efficiently wait for the next request */
	hint = READ_ONCE(s->mwait_hint);
	gen = ksched_mwait_on_addr(&s->gen, hint, p->last_gen);
	if (gen != p->last_gen) {
		tid = READ_ONCE(s->tid);
		p->last_gen = gen;
		ksched_next_tid(p, cpu, tid);
		WRITE_ONCE(s->busy, p->running_task != NULL);
		local_set(&p->busy, true);
		smp_store_release(&s->last_gen, gen);
	}

	put_cpu();

	return index;
}

static long ksched_park(void)
{
	struct ksched_percpu *p;
	struct ksched_shm_cpu *s;
	unsigned long gen;
	pid_t tid;
	int cpu;

	cpu = get_cpu();
	p = this_cpu_ptr(&kp);
	s = &shm[cpu];

	local_set(&p->busy, false);

	/* check if a new request is available yet */
	gen = smp_load_acquire(&s->gen);
	if (gen == p->last_gen) {
		WRITE_ONCE(s->busy, false);
		ksched_next_tid(p, cpu, 0);
		put_cpu();
		goto park;
	}

	/* determine the next task to run */
	tid = READ_ONCE(s->tid);
	p->last_gen = gen;

	/* are we waking the current pid? */
	if (tid == task_pid_vnr(current)) {
		WRITE_ONCE(s->busy, true);
		local_set(&p->busy, true);
		smp_store_release(&s->last_gen, gen);
		put_cpu();
		return smp_processor_id();
	}

	ksched_next_tid(p, cpu, tid);
	WRITE_ONCE(s->busy, p->running_task != NULL);
	local_set(&p->busy, true);
	smp_store_release(&s->last_gen, gen);
	put_cpu();

park:
	/* put this task to sleep and reschedule so the next task can run */
	__set_current_state(TASK_INTERRUPTIBLE);
	schedule();
	__set_current_state(TASK_RUNNING);
	return smp_processor_id();
}

static long ksched_start(void)
{
	/* put this task to sleep and reschedule so the next task can run */
	__set_current_state(TASK_INTERRUPTIBLE);
	schedule();
	__set_current_state(TASK_RUNNING);
	return smp_processor_id();
}

static void ksched_deliver_signal(struct ksched_percpu *p, unsigned int signum)
{
	/* if core is already idle, don't bother delivering signals */
	if (!local_read(&p->busy))
		return;

	if (p->running_task)
		send_sig(signum, p->running_task, 0);
}

static u64 ksched_measure_pmc(u64 sel)
{
	u64 start, end;

	rdmsrl(MSR_P6_PERFCTR0, start);
	udelay(KSCHED_PMC_PROBE_DELAY);
	rdmsrl(MSR_P6_PERFCTR0, end);
	return end - start;
}

static void ipi_handler(void)
{
	struct ksched_percpu *p;
	struct ksched_shm_cpu *s;
	int cpu, tmp;

	cpu = get_cpu();
	p = this_cpu_ptr(&kp);
	s = &shm[cpu];

	/* check if a signal has been requested */
	tmp = smp_load_acquire(&s->sig);
	if (tmp == p->last_gen) {
		ksched_deliver_signal(p, READ_ONCE(s->signum));
		smp_store_release(&s->sig, 0);
	}

	/* check if a performance counter has been requested */
	tmp = smp_load_acquire(&s->pmc);
	if (tmp != 0) {
		s->pmcval = ksched_measure_pmc(READ_ONCE(s->pmcsel));
		smp_store_release(&s->pmc, 0);
	}

	put_cpu();
}

#ifndef OS_SUPPORT_CUSTOMIZED_IPI_HANDER
static void ksched_ipi(void *unused)
{
	ipi_handler();
}
#else
static inline int get_cluster_id(unsigned int ldr)
{
	return ldr >> X2APIC_LDR_MAX_LOGICAL_IDS;

}

static int compare(const void *lhs, const void *rhs) {
        const struct ldr_info lhs_info = *(const struct ldr_info *)(lhs);
        const struct ldr_info rhs_info = *(const struct ldr_info *)(rhs);

	if (lhs_info.ldr < rhs_info.ldr) return -1;
	if (lhs_info.ldr > rhs_info.ldr) return 1;
	return 0;
}

static inline void write_icr_reg(unsigned int clustered_ldr)
{
	unsigned long long icr;
	if (!clustered_ldr)
		return;
	icr = ZAIN_VECTOR | ICR_LOGICAL_MODE |
		ICR_DEST_FIELD(clustered_ldr);
	wrmsrl(MSR_X2APIC_ICR, icr);
}

static void send_ipi(cpumask_var_t mask)
{		
	unsigned int clustered_ldr = 0;
	unsigned int cur_ldr;
	int i;

	if (cpumask_empty(mask))
		return;

	clustered_ldr = 0;
	for (i = 0; i < ipi_ldrs_num; i++) {
		if (!cpumask_test_cpu(ipi_ldrs[i].id, mask))
			continue;
		cur_ldr = ipi_ldrs[i].ldr;
		if (!clustered_ldr ||
		    get_cluster_id(cur_ldr) ==
		    get_cluster_id(clustered_ldr)) {
			clustered_ldr |= cur_ldr;
		} else {
			write_icr_reg(clustered_ldr);
			clustered_ldr = cur_ldr;
		}
	}
	write_icr_reg(clustered_ldr);
}

static void local_read_ldr(void *unused)
{
	int cpu;
	cpu = get_cpu();
	rdmsrl(MSR_X2APIC_LDR, ldrs[cpu]);
	put_cpu();
}

static void prepare_ldrs(void)
{
	int i;
	on_each_cpu(local_read_ldr, NULL, true);
	FOR_ALL_CORES(i) {
		ipi_ldrs[ipi_ldrs_num].id = i;
		ipi_ldrs[ipi_ldrs_num].ldr = ldrs[i];
		ipi_ldrs_num++;
	}
	sort(ipi_ldrs, ipi_ldrs_num, sizeof(struct ldr_info), &compare, NULL);
}
#endif

static int get_user_cpu_mask(const unsigned long __user *user_mask_ptr,
			     unsigned len, struct cpumask *new_mask)
{
	if (len < cpumask_size())
		cpumask_clear(new_mask);
	else if (len > cpumask_size())
		len = cpumask_size();

	return copy_from_user(new_mask, user_mask_ptr, len) ? -EFAULT : 0;
}

static long ksched_intr(struct ksched_intr_req __user *ureq)
{
	cpumask_var_t mask;
	struct ksched_intr_req req;

	/* only the IOKernel can send interrupts (privileged) */
	if (unlikely(!capable(CAP_SYS_ADMIN)))
		return -EACCES;

	/* validate inputs */
	if (unlikely(copy_from_user(&req, ureq, sizeof(req))))
		return -EFAULT;
	if (unlikely(!alloc_cpumask_var(&mask, GFP_KERNEL)))
		return -ENOMEM;
	if (unlikely(get_user_cpu_mask((const unsigned long __user *)req.mask,
				       req.len, mask))) {
		free_cpumask_var(mask);
		return -EFAULT;
	}

#ifdef OS_SUPPORT_CUSTOMIZED_IPI_HANDER
        send_ipi(mask);
#else
	smp_call_function_many(mask, ksched_ipi, NULL, false);
#endif
	free_cpumask_var(mask);
	return 0;
}

static long
ksched_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	/* validate input */
	if (unlikely(_IOC_TYPE(cmd) != KSCHED_MAGIC))
		return -ENOTTY;
	if (unlikely(_IOC_NR(cmd) > KSCHED_IOC_MAXNR))
		return -ENOTTY;

	switch (cmd) {
	case KSCHED_IOC_START:
		return ksched_start();
	case KSCHED_IOC_PARK:
		return ksched_park();
	case KSCHED_IOC_INTR:
		return ksched_intr((void __user *)arg);
	default:
		break;
	}

	return -ENOTTY;
}

static int ksched_mmap(struct file *file, struct vm_area_struct *vma)
{
	/* only the IOKernel can access the shared region (privileged) */
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	return remap_vmalloc_range(vma, (void *)shm, vma->vm_pgoff);
}

static int ksched_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int ksched_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static struct file_operations ksched_ops = {
	.owner		= THIS_MODULE,
	.mmap		= ksched_mmap,
	.unlocked_ioctl	= ksched_ioctl,
	.open		= ksched_open,
	.release	= ksched_release,
};

static int pci_cfg_mmap(struct file *file, struct vm_area_struct *vma)
{
        if (!capable(CAP_SYS_ADMIN))
                return -EACCES;
        vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
        if (io_remap_pfn_range(vma, vma->vm_start, PCI_CFG_ADDRESS >> PAGE_SHIFT,
			    vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
	        return -EAGAIN;
	}
	return 0;
}

static struct file_operations pci_cfg_ops = {
	.owner		= THIS_MODULE,
	.mmap		= pci_cfg_mmap,
};

/* TODO: This is a total hack to make ksched work as a module */
static struct cpuidle_state backup_state;
static int backup_state_count;

static int __init ksched_cpuidle_hijack(void)
{
	struct cpuidle_driver *drv;

	drv = cpuidle_get_driver();
	if (!drv)
		return -ENOENT;
	if (drv->state_count <= 0 || drv->states[0].disabled)
		return -EINVAL;

	cpuidle_pause_and_lock();
	backup_state = drv->states[0];
	backup_state_count = drv->state_count;
	drv->states[0].enter = ksched_idle;
	drv->state_count = 1;
	cpuidle_resume_and_unlock();

	return 0;
}

static void __exit ksched_cpuidle_unhijack(void)
{
	struct cpuidle_driver *drv;

	drv = cpuidle_get_driver();
	if (!drv)
		return;

	cpuidle_pause_and_lock();
	drv->states[0] = backup_state;
	drv->state_count = backup_state_count;
	cpuidle_resume_and_unlock();
}

static void __init ksched_init_pmc(void *arg)
{
        wrmsrl(MSR_P6_EVNTSEL0, PMC_LLC_MISSES);
	wrmsrl(MSR_CORE_PERF_FIXED_CTR_CTRL, 0x333);
        wrmsrl(MSR_CORE_PERF_GLOBAL_CTRL,
	       CORE_PERF_GLOBAL_CTRL_ENABLE_PMC_0 |
	       CORE_PERF_GLOBAL_CTRL_ENABLE_PMC_1 |
	       (1UL << 32) | (1UL << 33) | (1UL << 34));
}

static int __init ksched_init(void)
{
	dev_t devno_ksched = MKDEV(KSCHED_MAJOR, KSCHED_MINOR);
	dev_t devno_pci_cfg;
	int ret;

#ifdef OS_SUPPORT_CUSTOMIZED_IPI_HANDER
	int apic_status;
	rdmsrl(MSR_APIC_BASE, apic_status);
	if (!X2APIC_ENABLED(apic_status)) {
		printk(KERN_ERR "ksched: X2APIC is not enabled!");
		return -ENOTSUPP;
	}
#endif

	if (!cpu_has(&boot_cpu_data, X86_FEATURE_MWAIT)) {
		printk(KERN_ERR "ksched: mwait support is required");
		return -ENOTSUPP;
	}

	ret = register_chrdev_region(devno_ksched, 1, "ksched");
	if (ret)
		return ret;

	cdev_init(&ksched_cdev, &ksched_ops);
	ret = cdev_add(&ksched_cdev, devno_ksched, 1);
	if (ret)
		goto fail_ksched_cdev_add;

	shm = vmalloc_user(SHM_SIZE);
	if (!shm) {
		ret = -ENOMEM;
		goto fail_shm;
	}
	memset(shm, 0, SHM_SIZE);

	ret = ksched_cpuidle_hijack();
	if (ret)
		goto fail_hijack;

	smp_call_function(ksched_init_pmc, NULL, 1);
	printk(KERN_INFO "ksched: API V2 enabled");

        devno_pci_cfg = MKDEV(PCI_CFG_MAJOR, PCI_CFG_MINOR);
	ret = register_chrdev_region(devno_pci_cfg, 1, "pcicfg");
	if (ret) {
	  goto fail_pci_cfg_reg_cdev_region;
	}
	cdev_init(&pci_cfg_cdev, &pci_cfg_ops);
	ret = cdev_add(&pci_cfg_cdev, devno_pci_cfg, 1);
	if (ret) {
	  goto fail_pci_cfg_cdev_add;
	}

#ifdef OS_SUPPORT_CUSTOMIZED_IPI_HANDER
	set_customized_ipi_handler(ipi_handler);
	prepare_ldrs();	
#endif
	
	return 0;
	
fail_pci_cfg_cdev_add:
	unregister_chrdev_region(devno_pci_cfg, 1);
fail_pci_cfg_reg_cdev_region:
fail_hijack:
	vfree(shm);
fail_shm:
	cdev_del(&ksched_cdev);
fail_ksched_cdev_add:
	unregister_chrdev_region(devno_ksched, 1);
	return ret;
}

static void __exit ksched_exit(void)
{
	int cpu;
	struct ksched_percpu *p;

	dev_t devno_ksched = MKDEV(KSCHED_MAJOR, KSCHED_MINOR);
	dev_t devno_pci_cfg = MKDEV(PCI_CFG_MAJOR, PCI_CFG_MINOR);

	ksched_cpuidle_unhijack();
	vfree(shm);
	cdev_del(&ksched_cdev);
	unregister_chrdev_region(devno_ksched, 1);
	cdev_del(&pci_cfg_cdev);
	unregister_chrdev_region(devno_pci_cfg, 1);

	for_each_online_cpu(cpu) {
		p = per_cpu_ptr(&kp, cpu);
		if (p->running_task)
			put_task_struct(p->running_task);
	}
}

module_init(ksched_init);
module_exit(ksched_exit);

MODULE_LICENSE("GPL");
