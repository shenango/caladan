/*
 * ksched.c - an accelerated scheduler interface for the IOKernel
 */

#include <asm/io.h>
#include <asm/local.h>
#include <asm/msr-index.h>
#include <asm/msr.h>
#include <asm/mwait.h>
#include <asm/tlbflush.h>
#include <linux/capability.h>
#include <linux/cdev.h>
#include <linux/cpuidle.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/signal.h>
#include <linux/version.h>

#include "ksched.h"
#include "uintr.h"
#include "defs.h"
#include "../iokernel/pmc.h"

#define CORE_PERF_GLOBAL_CTRL_ENABLE_PMC_0 (0x1)
#define CORE_PERF_GLOBAL_CTRL_ENABLE_PMC_1 (0x2)

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,1,0)
#define PF__HOLE__40000000 0x40000000
#endif

#define PF_KSCHED_PARKED PF__HOLE__40000000

/* the character device that provides the ksched IOCTL interface */
static struct cdev ksched_cdev;

/* shared memory between the IOKernel and the Linux Kernel */
__read_mostly struct ksched_shm_cpu *shm;
#define SHM_SIZE (NR_CPUS * sizeof(struct ksched_shm_cpu))

/* per-cpu data to coordinate context switching and signal delivery */
DEFINE_PER_CPU(struct ksched_percpu, kp);

static void mark_task_parked(struct task_struct *tsk)
{
	tsk->flags |= PF_KSCHED_PARKED;
}

static bool try_mark_task_unparked(struct task_struct *tsk)
{
	if ((tsk->flags & PF_KSCHED_PARKED) > 0) {
		tsk->flags &= ~PF_KSCHED_PARKED;
		return true;
	}

	return false;
}

/**
 * ksched_measure_pmc - read a performance counter
 * @sel: selects an x86 performance counter
 */
static u64 ksched_measure_pmc(u64 sel)
{
	struct ksched_percpu *p = this_cpu_ptr(&kp);
	u64 val;

	if (p->last_sel != sel) {
		wrmsrl(MSR_P6_EVNTSEL0, sel);
		p->last_sel = sel;
	}
	rdmsrl(MSR_P6_PERFCTR0, val);

	return val;
}

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

static void ksched_cleanup_core(struct ksched_percpu *kp, int cpu)
{
	if (uintr_active(&kp->uintr))
		uintr_cleanup_core(&kp->uintr, cpu);

	put_task_struct(kp->running_task);
	kp->running_task = NULL;
}

static void ksched_next_tid(struct ksched_percpu *kp, int cpu, pid_t tid)
{
	struct task_struct *p;
	int ret;
	unsigned long flags;
	bool already_running;

	/* release previous task */
	if (kp->running_task)
		ksched_cleanup_core(kp, cpu);

	if (unlikely(tid == 0))
		return;

	rcu_read_lock();
	p = ksched_lookup_task(tid);
	if (unlikely(!p)) {
		rcu_read_unlock();
		return;
	}

	raw_spin_lock_irqsave(&p->pi_lock, flags);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,14,0)
	already_running = p->on_cpu || p->state == TASK_WAKING ||
			  p->state == TASK_RUNNING || !try_mark_task_unparked(p);
#else
	already_running = p->on_cpu || p->__state == TASK_WAKING ||
			  task_is_running(p) || !try_mark_task_unparked(p);
#endif
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);
	if (unlikely(already_running)) {
		rcu_read_unlock();
		return;
	}

	ret = set_cpus_allowed_ptr(p, cpumask_of(cpu));
	if (unlikely(ret)) {
		mark_task_parked(p);
		rcu_read_unlock();
		return;
	}

	get_task_struct(p);
	kp->running_task = p;

	wake_up_process(p);
	rcu_read_unlock();

	return;
}

static bool has_mwait;

static int ksched_mwait_on_addr(const unsigned int *addr, unsigned int hint,
				unsigned int val)
{
	unsigned int cur;
	size_t i;

	lockdep_assert_irqs_disabled();

	if (!has_mwait) {
		for (i = 0; i < 10; i++) {
			cur = READ_ONCE(*addr);
			if (cur != val) return cur;
			udelay(1);
		}
		return cur;
	}

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
	if (unlikely(p->running_task)) {
		if (p->running_task->flags & PF_EXITING) {
			ksched_cleanup_core(p, cpu);
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

		/* HACK: calling wake_up_process might have enabled interrupts. */
		if (!irqs_disabled())
			raw_local_irq_disable();
	}

	put_cpu();

	return index;
}

static __always_inline long finish_wake(struct uintr_ctx *ctx, u64 next_stack)
{
	int cpu;
	struct ksched_percpu *p;
	struct ksched_shm_cpu *s;

	cpu = get_cpu();
	p = this_cpu_ptr(&kp);
	s = &shm[cpu];

	if (unlikely(p->running_task != current)) {
		/* The thread is waken up by a user-sent signal instead of
		 * the iokernel. In this case no core was granted. We should
		 * put the thread back into sleep immediately after handling
		 * the signal. */
		put_cpu();
		return -ERESTARTSYS;
	}

	if (ctx) {
		uintr_assign_core(ctx, next_stack);

		/* a UIPI might have arrived before we set up the context */
		if (unlikely(smp_load_acquire(&s->sig) == p->last_gen))
			uintr_signal_self();
	}

	put_cpu();
	return cpu;
}

static long ksched_park(struct uintr_ctx *ctx, u64 next_stack)
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

	if (unlikely(signal_pending(current))) {
		local_set(&p->busy, true);
		put_cpu();
		return -ERESTARTSYS;
	}

	/* may have unparked from a signal */
	if (unlikely(current != p->running_task))
		goto park;

	/* check if a new request is available yet */
	gen = smp_load_acquire(&s->gen);
	if (gen == p->last_gen) {
		WRITE_ONCE(s->busy, false);
		ksched_cleanup_core(p, cpu);
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
	local_set(&p->busy, p->running_task != NULL);
	smp_store_release(&s->last_gen, gen);

park:
	put_cpu();
	/* put this task to sleep and reschedule so the next task can run */
	__set_current_state(TASK_INTERRUPTIBLE);
	mark_task_parked(current);
	schedule();
	__set_current_state(TASK_RUNNING);
	return finish_wake(ctx, next_stack);
}

static long ksched_start(struct uintr_ctx *ctx, u64 next_stack)
{
	/* put this task to sleep and reschedule so the next task can run */
	__set_current_state(TASK_INTERRUPTIBLE);
	mark_task_parked(current);
	schedule();
	__set_current_state(TASK_RUNNING);
	return finish_wake(ctx, next_stack);
}

static void ksched_deliver_signal(struct ksched_percpu *p, unsigned int signum)
{
	/* if core is already idle, don't bother delivering signals */
	if (!local_read(&p->busy))
		return;

	if (uintr_enabled) {
		uintr_deliver_ipi(&p->uintr);
		uintr_signal_self();
	} else if (p->running_task) {
		send_sig(signum, p->running_task, 0);
	}
}

static void ksched_ipi(void *unused)
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
		s->pmctsc = rdtsc();
		smp_store_release(&s->pmc, 0);
	}

	put_cpu();
}

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

	smp_call_function_many(mask, ksched_ipi, NULL, false);
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
		return ksched_start(to_uintr_ctx(filp), arg);
	case KSCHED_IOC_PARK:
		return ksched_park(to_uintr_ctx(filp), arg);
	case KSCHED_IOC_INTR:
		return ksched_intr((void __user *)arg);
	case KSCHED_IOC_UINTR_MULTICAST:
		return uintr_multicast((void __user *)arg);
	case KSCHED_IOC_UINTR_SETUP_USER:
		return uintr_setup_user(filp, arg);
	case KSCHED_IOC_UINTR_SETUP_ADMIN:
		return uintr_setup_admin(filp);
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
	uintr_file_release(filp);
	return 0;
}

static struct file_operations ksched_ops = {
	.owner		= THIS_MODULE,
	.mmap		= ksched_mmap,
	.unlocked_ioctl	= ksched_ioctl,
	.open		= ksched_open,
	.release	= ksched_release,
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
	if (drv->state_count <= 0)
		return -EINVAL;

	cpuidle_pause_and_lock();
	backup_state = drv->states[0];
	backup_state_count = drv->state_count;
	drv->states[0].enter = ksched_idle;
	drv->states[0].flags = CPUIDLE_FLAG_NONE;
	drv->state_count = 1;
	try_module_get(drv->owner);
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
	module_put(drv->owner);
	cpuidle_resume_and_unlock();
}

static void __init ksched_init_pmc(void *arg)
{
	wrmsrl(MSR_CORE_PERF_FIXED_CTR_CTRL, 0x333);
	wrmsrl(MSR_CORE_PERF_GLOBAL_CTRL,
	       CORE_PERF_GLOBAL_CTRL_ENABLE_PMC_0 |
	       CORE_PERF_GLOBAL_CTRL_ENABLE_PMC_1 |
	       (1UL << 32) | (1UL << 33) | (1UL << 34));
}

static int __init ksched_init(void)
{
	dev_t devno_ksched = MKDEV(KSCHED_MAJOR, KSCHED_MINOR);
	int ret;

	has_mwait = cpu_has(&boot_cpu_data, X86_FEATURE_MWAIT);
	if (!has_mwait)
		printk(KERN_ERR "ksched: mwait support is missing");

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

	ret = uintr_init();
	if (ret)
		goto fail_uintr;

	ret = ksched_cpuidle_hijack();
	if (ret)
		goto fail_hijack;

	smp_call_function(ksched_init_pmc, NULL, 1);
	ksched_init_pmc(NULL);
	printk(KERN_INFO "ksched: API V2 enabled");
	return 0;

fail_uintr:
	vfree(shm);
fail_hijack:
	uintr_exit();
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

	uintr_exit();

	ksched_cpuidle_unhijack();
	vfree(shm);
	cdev_del(&ksched_cdev);
	unregister_chrdev_region(devno_ksched, 1);

	for_each_online_cpu(cpu) {
		p = per_cpu_ptr(&kp, cpu);
		if (p->running_task)
			put_task_struct(p->running_task);
	}
}

module_init(ksched_init);
module_exit(ksched_exit);

MODULE_LICENSE("GPL");
