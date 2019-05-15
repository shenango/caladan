/*
 * ksched.c - an accelerated scheduler interface for the IOKernel
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/smp.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/cpuidle.h>
#include <asm/local.h>
#include <asm/mwait.h>
#include <asm/local.h>

#include "ksched.h"

MODULE_LICENSE("GPL");

/* the character device that provides the ksched IOCTL interface */
static struct cdev ksched_cdev;

/* shared memory between the IOKernel and the Linux Kernel */
static __read_mostly struct ksched_shm_cpu *shm;
#define SHM_SIZE (NR_CPUS * sizeof(struct ksched_shm_cpu))

struct ksched_percpu {
	unsigned int	last_gen;
	pid_t		tid;
	local_t		busy;
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
	return pid_task(find_vpid(nr), PIDTYPE_PID);
}

static int ksched_wakeup_pid(int cpu, pid_t pid)
{
	struct task_struct *p;
	int ret;

	rcu_read_lock();
	p = ksched_lookup_task(pid);
	if (!p) {
		rcu_read_unlock();
		return -ESRCH;
	}
	get_task_struct(p);
	rcu_read_unlock();

	ret = set_cpus_allowed_ptr(p, cpumask_of(cpu));
	if (ret) {
		put_task_struct(p);
		return ret;
	}

	wake_up_process(p);
	put_task_struct(p);

	return 0;
}

static int ksched_mwait_on_addr(const unsigned int *addr, unsigned int val)
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
	__mwait(0, MWAIT_ECX_INTERRUPT_BREAK);
	return smp_load_acquire(addr);
}

static int __cpuidle ksched_idle(struct cpuidle_device *dev,
				 struct cpuidle_driver *drv, int index)
{
	struct ksched_percpu *p;
	struct ksched_shm_cpu *s;
	unsigned long gen;
	pid_t tid;
	int cpu;

	lockdep_assert_irqs_disabled();

	cpu = get_cpu();
	p = this_cpu_ptr(&kp);
	s = &shm[cpu];

	local_set(&p->busy, false);
	WRITE_ONCE(s->busy, false);

	while (true) {
		/* use the mwait instruction to efficiently poll memory */
		gen = ksched_mwait_on_addr(&s->gen, p->last_gen);
		if (gen != p->last_gen) {
			tid = READ_ONCE(s->tid);
			WRITE_ONCE(s->tid, 0);
			p->last_gen = gen;
			ksched_wakeup_pid(cpu, tid);
			WRITE_ONCE(s->busy, true);
			local_set(&p->busy, true);
			break;
		}
		if (need_resched())
			break;
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

	local_irq_disable();
	if (unlikely(signal_pending(current))) {
		local_irq_enable();
		put_cpu();
		return -ERESTARTSYS;
	}
	local_set(&p->busy, false);
	WRITE_ONCE(s->busy, false);

	while (true) {
		/* use the mwait instruction to efficiently poll memory */
		gen = ksched_mwait_on_addr(&s->gen, p->last_gen);

		/* find out why we woke up */
		local_irq_enable();
		if (unlikely(signal_pending(current))) {
			smp_store_release(&s->busy, true);
			local_set(&p->busy, true);
			put_cpu();
			return -ERESTARTSYS;
		}
		if (gen != p->last_gen)
			break;
		put_cpu();

		/* run another task if needed */
		if (need_resched())
			schedule();

		cpu = get_cpu();
		p = this_cpu_ptr(&kp);
		s = &shm[cpu];
		local_irq_disable();
	}

	/* the pid was set before the generation number (x86 is TSO) */
	tid = READ_ONCE(s->tid);
	p->last_gen = gen;
	p->tid = tid;
	WRITE_ONCE(s->tid, 0);
	WRITE_ONCE(s->busy, true);
	local_set(&p->busy, true);

	/* are we waking the current pid? */
	if (tid == current->pid) {
		put_cpu();
		return 0;
	}
	ksched_wakeup_pid(cpu, tid);
	put_cpu();

	/* put this task to sleep and reschedule so the next task can run */
	__set_current_state(TASK_INTERRUPTIBLE);
	schedule();
	__set_current_state(TASK_RUNNING);
	return 0;
}

static long ksched_start(void)
{
	/* put this task to sleep and reschedule so the next task can run */
	__set_current_state(TASK_INTERRUPTIBLE);
	schedule();
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void ksched_ipi(void *unused)
{
	struct ksched_percpu *p;
	struct ksched_shm_cpu *s;
	struct task_struct *t;
	int cpu, gen;

	cpu = get_cpu();
	p = this_cpu_ptr(&kp);
	s = &shm[cpu];

	/* if core is already idle, don't bother delivering signals */
	if (!local_read(&p->busy)) {
		put_cpu();
		return;
	}

	/* lookup the current task assigned to this core */
	t = ksched_lookup_task(p->tid);
	if (!t) {
		put_cpu();
		return;
	}

	/* check if yield has been requested (detecting race conditions) */
	gen = smp_load_acquire(&s->yield);
	if (gen == p->last_gen) {
		send_sig(SIGUSR2, t, 0);
		smp_store_release(&s->yield, 0);
	}

	/* check if cede has been requested (detecting race conditions) */
	gen = smp_load_acquire(&s->cede);
	if (gen == p->last_gen) {
		send_sig(SIGUSR1, t, 0);
		smp_store_release(&s->cede, 0);
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
	if (unlikely(copy_from_user(&req, &ureq, sizeof(req))))
		return -EFAULT;
	if (unlikely(!alloc_cpumask_var(&mask, GFP_KERNEL)))
		return -ENOMEM;
	if (unlikely(get_user_cpu_mask((const unsigned long __user *)req.mask,
				       req.len, mask))) {
		free_cpumask_var(mask);
		return -EFAULT;
	}

	/* send interrupts */
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

static int __init ksched_init(void)
{
	dev_t devno = MKDEV(KSCHED_MAJOR, KSCHED_MINOR);
	int ret;

	ret = register_chrdev_region(devno, 1, "ksched");
	if (ret)
		return ret;

	cdev_init(&ksched_cdev, &ksched_ops);
	ret = cdev_add(&ksched_cdev, devno, 1);
	if (ret)
		goto fail_cdev_add;

	shm = vmalloc(SHM_SIZE);
	if (!shm) {
		ret = -ENOMEM;
		goto fail_shm;
	}
	memset(shm, 0, SHM_SIZE);

	ret = ksched_cpuidle_hijack();
	if (ret)
		goto fail_hijack;

	printk(KERN_INFO "ksched: API V2 enabled");
	return 0;

fail_hijack:
	vfree(shm);
fail_shm:
	cdev_del(&ksched_cdev);
fail_cdev_add:
	unregister_chrdev_region(devno, 1);
	return ret;
}

static void __exit ksched_exit(void)
{
	dev_t devno = MKDEV(KSCHED_MAJOR, KSCHED_MINOR);

	ksched_cpuidle_unhijack();
	vfree(shm);
	cdev_del(&ksched_cdev);
	unregister_chrdev_region(devno, 1);
}

module_init(ksched_init);
module_exit(ksched_exit);
