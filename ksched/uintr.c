
#include <asm/apic.h>
#include <asm/cpufeature.h>
#include <asm/fpu/xcr.h>
#include <asm/fpu/xstate.h>
#include <asm/irq.h>
#include <asm/msr-index.h>
#include <asm/msr.h>
#include <asm/thread_info.h>
#include <asm/tlbflush.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/tracepoint.h>
#include <linux/version.h>

#include "ksched.h"
#include "uintr.h"
#include "defs.h"

#define REX_PREFIX	"0x48, "
#define XSAVES		".byte " REX_PREFIX "0x0f,0xc7,0x2f"
#define XRSTORS		".byte " REX_PREFIX "0x0f,0xc7,0x1f"

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,1,0)
#define PF__HOLE__20000000 0x20000000
#endif

#define PF_IN_SYSCALL PF__HOLE__20000000

#ifndef TIF_NOTIFY_SIGNAL
#define TIF_NOTIFY_SIGNAL TIF_SIGPENDING
#endif

#if __has_include(<asm/fpu/internal.h>)
#include <asm/fpu/internal.h>
#endif
#ifndef XSTATE_OP
#define XSTATE_OP(op, st, lmask, hmask, err)				\
	asm volatile("1:" op "\n\t"					\
		     "xor %[err], %[err]\n"				\
		     "2:\n\t"						\
		     _ASM_EXTABLE_TYPE(1b, 2b, EX_TYPE_FAULT_MCE_SAFE)	\
		     : [err] "=a" (err)					\
		     : "D" (st), "m" (*st), "a" (lmask), "d" (hmask)	\
		     : "memory")
#endif

__ro_after_init bool uintr_enabled;
static __ro_after_init struct uintr_xstate uintr_null_state;
static __ro_after_init struct tracepoint *sched_switch_tp;
static __ro_after_init struct tracepoint *sys_exit_tp;
static __ro_after_init struct tracepoint *sys_enter_tp;
static __ro_after_init int nouintr;

module_param(nouintr, int, 0);
MODULE_PARM_DESC(nouintr, "disable UINTR");

static inline struct uintr_percpu *get_uintr_pcpu(int cpu)
{
	return &per_cpu_ptr(&kp, cpu)->uintr;
}

static inline struct uintr_percpu *get_uintr_this_cpu(void)
{
	return &this_cpu_ptr(&kp)->uintr;
}

static inline void uintr_mark_in_syscall(struct task_struct *tsk)
{
	tsk->flags |= PF_IN_SYSCALL;
}

static inline bool uintr_test_in_syscall(struct task_struct *tsk)
{
	return (tsk->flags & PF_IN_SYSCALL) != 0;
}

static inline void uintr_clear_in_syscall(struct task_struct *tsk)
{
	tsk->flags &= ~PF_IN_SYSCALL;
}

static void uintr_xrstors(struct uintr_xstate *xs)
{
	int err;

	XSTATE_OP(XRSTORS, xs, XFEATURE_MASK_UINTR,
	          ((u64)XFEATURE_MASK_UINTR) >> 32, err);
	WARN_ON_ONCE(err);
}

static void uintr_xsaves(struct uintr_xstate *xs)
{
	int err;

	XSTATE_OP(XSAVES, xs, XFEATURE_MASK_UINTR, ((u64)XFEATURE_MASK_UINTR) >> 32,
	          err);
	WARN_ON_ONCE(err);
}

static inline bool uintr_pending(struct uintr_percpu *p, bool ignore_uif)
{
	int cpu = smp_processor_id();

	/* ignore interrupts if user interrupt flag is 0 */
	if (!ignore_uif && !p->cur_xstate.uintr.misc.uif)
		return false;

	/* check if an interrupt has been recognized */
	if (p->cur_xstate.uintr.uirr)
		return true;

	/* check if software has posted an interrupt */
	if (shm[cpu].upid.puir)
		return true;

	/* check this for good measure, may not be necessary */
	if (test_bit(UINTR_UPID_STATUS_ON, &shm[cpu].upid.word_val))
		return true;

	return false;
}

static void uintr_switch_to_kernel(struct uintr_percpu *p)
{
	/* save UINTR state, forces (U)IPIs to land in kernel IPI handler */
	uintr_xsaves(&p->cur_xstate);

	/* detect a UIPI that arrived while we were in the kernel (before xsave) */
	if (uintr_pending(p, false))
		set_tsk_thread_flag(p->assigned_task, TIF_NOTIFY_SIGNAL);
}

/* returns true if an interrupt is pending */
static bool uintr_return_from_kernel(struct uintr_percpu *p)
{
	uintr_xrstors(&p->cur_xstate);
	p->state_loaded = true;

	if (uintr_pending(p, true)) {
		uintr_signal_self();
		return true;
	}

	return false;
}

void uintr_deliver_ipi(struct uintr_percpu *p)
{
	struct task_struct *tsk;

	tsk = smp_load_acquire(&p->assigned_task);
	if (!tsk)
		return;

	set_tsk_thread_flag(tsk, TIF_NOTIFY_SIGNAL);
	wake_up_process(tsk);
}

static void uintr_ipi(void)
{
	uintr_deliver_ipi(get_uintr_this_cpu());
}

static void uintr_ctx_release(struct kref *ref)
{
	struct uintr_ctx *ctx = container_of(ref, struct uintr_ctx, refcount);
	kfree(ctx);
}

void uintr_cleanup_core(struct uintr_percpu *p, int cpu)
{
	p->assigned_task = NULL;
	smp_wmb();

	/* prevent senders from sending to this core */
	p->assigned_ctx->uitt[cpu].valid = 0;

	/* suppress notifications */
	set_bit(UINTR_UPID_STATUS_SN, &shm[cpu].upid.word_val);

	/* load NULL state */
	wrmsrl(MSR_IA32_UINTR_MISC, 0);
	uintr_xrstors(&uintr_null_state);

	/* remove any existing interrupts */
	shm[cpu].upid.puir = 0;
	clear_bit(UINTR_UPID_STATUS_ON, &shm[cpu].upid.word_val);

	/* release ref */
	kref_put(&p->assigned_ctx->refcount, uintr_ctx_release);

	p->assigned_ctx = NULL;
	p->state_loaded = false;
}

void uintr_assign_core(struct uintr_ctx *ctx, u64 stack)
{
	int cpu;
	struct uintr_percpu *p;

	cpu = get_cpu();
	p = get_uintr_this_cpu();

	/* setup new context for uintr */
	p->cur_xstate.uintr.handler = ctx->handler;
	p->cur_xstate.uintr.stack_adjust = stack;
	p->cur_xstate.uintr.misc.uitt_size = nr_cpu_ids - 1;
	p->cur_xstate.uintr.misc.uinv = UIPI_APIC_VECTOR;
	p->cur_xstate.uintr.misc.uif = 1;
	p->cur_xstate.uintr.upid_addr = (u64)&shm[cpu].upid;
	p->cur_xstate.uintr.uirr = 0;
	p->cur_xstate.uintr.uitt_addr = (u64)(&ctx->uitt[0]) | 1UL;

	/* syscall return will restore the context */
	if (!uintr_test_in_syscall(current)) {
		/* the first time a thread attaches the syscall isn't marked */
		uintr_xrstors(&p->cur_xstate);
	}

	/* allow other cores in this ctx to send to this one */
	ctx->uitt[cpu].valid = BIT(UINTR_UITT_VALID_BIT);

	/* allow notifications */
	clear_bit(UINTR_UPID_STATUS_SN, &shm[cpu].upid.word_val);

	/* take a reference */
	kref_get(&ctx->refcount);
	p->assigned_ctx = ctx;
	p->assigned_task = current;
	p->state_loaded = true;
	p->is_admin_ctx = ctx->is_admin;

	put_cpu();
}

/*
 * trace_sys_enter - called when any process on the machine enters a system
 * call. Marks the task as in a system call, and ensures that UIPIs are
 * delivered to the kernel IPI handler.
 */
static void trace_sys_enter(void *data, struct pt_regs *regs, long id)
{
	struct uintr_percpu *p;

	p = get_uintr_this_cpu();

	/* not our UINTR task */
	if (p->assigned_task != current)
		return;

	/* the admin context doesn't receive interrupts */
	if (p->is_admin_ctx)
		return;

	uintr_mark_in_syscall(current);

	WARN_ON_ONCE(!p->state_loaded);

	uintr_switch_to_kernel(p);
}

/* called when any process on the machine returns from a system call */
static void trace_sys_exit(void *data, struct pt_regs *regs, long ret)
{
	struct uintr_percpu *p;
	bool pending;

	p = get_uintr_this_cpu();

	if (p->assigned_task != current || !uintr_test_in_syscall(current))
		return;

	uintr_clear_in_syscall(current);

	/* fully restore uintr context, check if a UIPI is pending */
	pending = uintr_return_from_kernel(p);

	if (!pending)
		return;

	/* a UIPI is pending, break the system call out of a potential restart */
	switch (regs->ax) {
		case -ERESTARTNOHAND:
		case -ERESTARTSYS:
		case -ERESTARTNOINTR:
		case -ERESTART_RESTARTBLOCK:
			regs->ax = -EINTR;
			break;
	}
}


/*
 * Some cores may still run other non-managed user tasks. We need to ensure that
 * UINTR notifications are disabled and that those tasks can't send UIPIs. This
 * function is called by the scheduler whenever switching tasks, allowing us to
 * check if UINTR should be temporarily disabled or re-enabled.
 */
static void trace_sched_switch(void *data, bool preempt,
	struct task_struct *prev, struct task_struct *next)
{
	struct uintr_percpu *p;

	/* Do nothing for kernel threads */
	if (next->flags & PF_KTHREAD)
		return;

	p = get_uintr_this_cpu();

	/* check if this core is currently using UIPI */
	if (!p->assigned_task)
		return;

	/* check if the next task is not our target task */
	if (next != p->assigned_task) {

		if (!p->state_loaded)
			return;

		if (!uintr_test_in_syscall(p->assigned_task))
			uintr_switch_to_kernel(p);

		/* clear all the UINTR state before entering the next task */
		uintr_xrstors(&uintr_null_state);
		p->state_loaded = false;

	} else if (!p->state_loaded) {
		/* we are switching back to the UINTR task, reload the state */
		if (!uintr_test_in_syscall(p->assigned_task))
			uintr_return_from_kernel(p);
	}
}

static struct uintr_ctx *alloc_uintr_ctx(bool admin)
{
	struct uintr_ctx *ctx;
	struct uintr_uitt_entry *uitt;
	int cpu;

	ctx = kzalloc(sizeof(*ctx) + sizeof(*uitt) * nr_cpu_ids, GFP_KERNEL);
	if (!ctx)
		return NULL;

	kref_init(&ctx->refcount);
	ctx->is_admin = admin;

	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		uitt = &ctx->uitt[cpu];
		uitt->target_upid_addr = (u64)&shm[cpu].upid;

		if (admin) {
			uitt->user_vec = SIGUSR1 - 1;

			/* mark as valid only if this is an admin context */
			uitt->valid = BIT(UINTR_UITT_VALID_BIT);
		} else {
			uitt->user_vec = SIGURG - 1;
		}
	}

	return ctx;
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

long uintr_multicast(struct ksched_intr_req __user *ureq)
{
	cpumask_var_t mask;
	struct ksched_intr_req req;

	/* only the IOKernel can send interrupts (privileged) */
	if (unlikely(!capable(CAP_SYS_ADMIN)))
		return -EACCES;

	if (unlikely(!uintr_enabled))
		return -ENODEV;

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

	apic->send_IPI_mask(mask, UIPI_APIC_VECTOR);
	free_cpumask_var(mask);
	return 0;
}

void uintr_file_release(struct file *filp)
{
	struct uintr_ctx *ctx = to_uintr_ctx(filp);
	if (ctx)
		kref_put(&ctx->refcount, uintr_ctx_release);
}

/* setup an admin context for UINTR */
long uintr_setup_admin(struct file *filp)
{
	int cpu;
	struct uintr_ctx *ctx;
	struct ksched_percpu *p;

	/* check permissions */
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	/* ensure UINTR is available */
	if (!uintr_enabled)
		return -ENODEV;

	/* check if this process has already set up a context */
	if (filp->private_data)
		return -EINVAL;

	ctx = alloc_uintr_ctx(true);
	if (!ctx)
		return -ENOMEM;

	cpu = get_cpu();
	p = this_cpu_ptr(&kp);
	get_task_struct(current);
	p->running_task = current;
	uintr_assign_core(ctx, 0);
	put_cpu();

	filp->private_data = ctx;
	return 0;
}

long uintr_setup_user(struct file *filp, unsigned long handler)
{
	struct uintr_ctx *ctx;

	if (!uintr_enabled)
		return -ENODEV;

	if (filp->private_data)
		return -EINVAL;

	ctx = alloc_uintr_ctx(false);
	if (!ctx)
		return -ENOMEM;

	ctx->handler = handler;
	filp->private_data = ctx;
	return 0;
}

static void __cold uintr_scheduler_unhijack(void) {
	if (sched_switch_tp)
		tracepoint_probe_unregister(sched_switch_tp, trace_sched_switch, NULL);
	if (sys_exit_tp)
		tracepoint_probe_unregister(sys_exit_tp, trace_sys_exit, NULL);
	if (sys_enter_tp)
		tracepoint_probe_unregister(sys_enter_tp, trace_sys_enter, NULL);
}

static void uintr_teardown(void *info)
{
	u64 xss;

	/* Clear UINTR state */
	wrmsrl(MSR_IA32_UINTR_PD, 0);
	wrmsrl(MSR_IA32_UINTR_TT, 0);
	wrmsrl(MSR_IA32_UINTR_MISC, 0);
	wrmsrl(MSR_IA32_UINTR_HANDLER, 0);
	wrmsrl(MSR_IA32_UINTR_STACKADJUST, 0);

	/* Remove UINTR from supervisor XSTATE */
	rdmsrl(MSR_IA32_XSS, xss);
	xss &= ~BIT_ULL(XFEATURE_UINTR);
	wrmsrl(MSR_IA32_XSS, xss);

	/* Disable UINTR feature entirely */
	cr4_clear_bits(X86_CR4_UINTR);
}

static void __init init_compact_xstate(struct uintr_xstate *xs)
{
	xs->xregs.header.xfeatures = XFEATURE_MASK_UINTR;
	xs->xregs.header.xcomp_bv = XCOMP_BV_COMPACTED_FORMAT | XFEATURE_MASK_UINTR;
}

static __init void uintr_init_cpu(void *info)
{
	bool *failure = (bool *)info;
	struct uintr_upid *upid;
	struct uintr_percpu *p;
	int apicid, cpu;
	u64 xss;

	cpu = smp_processor_id();
	upid = &shm[cpu].upid;
	p = get_uintr_this_cpu();

	/* suppress notifications */
	set_bit(UINTR_UPID_STATUS_SN, &upid->word_val);

	/* set normal interrupt vector to use */
	upid->nc.nv = UIPI_APIC_VECTOR;

	apicid = apic->cpu_present_to_apicid(cpu);
	if (apicid == BAD_APICID) {
		*failure = true;
		return;
	}

	if (!x2apic_enabled())
		apicid = ((u32)apicid << 8) & 0xFF00;

	upid->nc.ndst = apicid;

	/* init UINTR */
	cr4_set_bits(X86_CR4_UINTR);

	init_compact_xstate(&p->cur_xstate);

	/* enable xsave component */
	rdmsrl(MSR_IA32_XSS, xss);
	xss |= BIT_ULL(XFEATURE_UINTR);
	wrmsrl(MSR_IA32_XSS, xss);
}

static void __init tracepoint_finder(struct tracepoint *tp, void *priv)
{
	if (!strncmp(tp->name, "sched_switch", strlen("sched_switch"))) {
		sched_switch_tp = tp;
		return;
	}

	if (!strncmp(tp->name, "sys_exit", strlen("sys_exit"))) {
		sys_exit_tp = tp;
		return;
	}
	if (!strncmp(tp->name, "sys_enter", strlen("sys_enter"))) {
		sys_enter_tp = tp;
		return;
	}
}

static int __init uintr_scheduler_hijack(void)
{
	int ret;

	for_each_kernel_tracepoint(tracepoint_finder, NULL);

	if (!sys_enter_tp || !sched_switch_tp || !sys_exit_tp)
		return -1;

	ret = tracepoint_probe_register(sched_switch_tp, trace_sched_switch, NULL);
	if (ret) {
		printk(KERN_ERR "Failed to register tracepoint handler (%d)\n", ret);
		return ret;
	}

	ret = tracepoint_probe_register(sys_exit_tp, trace_sys_exit, NULL);
	if (ret) {
		printk(KERN_ERR "Failed to register tracepoint handler (%d)\n", ret);
		tracepoint_probe_unregister(sched_switch_tp, trace_sched_switch, NULL);
		return ret;
	}

	ret = tracepoint_probe_register(sys_enter_tp, trace_sys_enter, NULL);
	if (ret) {
		printk(KERN_ERR "Failed to register tracepoint handler (%d)\n", ret);
		tracepoint_probe_unregister(sched_switch_tp, trace_sched_switch, NULL);
		tracepoint_probe_unregister(sys_exit_tp, trace_sys_exit, NULL);
		return ret;
	}

	return 0;
}

int __init uintr_init(void)
{
	int ret;
	bool failure = false;

	if (!cpu_has(&boot_cpu_data, X86_FEATURE_UINTR)) {
		printk(KERN_INFO "UINTR: not enabled (no support)");
		return 0;
	}

	if (nouintr) {
		printk(KERN_INFO "UINTR: not enabled (disabled by module param)");
		return 0;
	}

	ret = uintr_scheduler_hijack();
	if (ret)
		return ret;

	smp_call_function(uintr_init_cpu, &failure, 1);
	uintr_init_cpu(&failure);
	mb();
	if (READ_ONCE(failure)) {
		uintr_scheduler_unhijack();
		smp_call_function(uintr_teardown, NULL, 1);
		uintr_teardown(NULL);
		return -1;
	}

	init_compact_xstate(&uintr_null_state);

	/* setup callback for the UINTR interrupt vector (borrowed from KVM) */
	kvm_set_posted_intr_wakeup_handler(uintr_ipi);

	uintr_enabled = true;
	printk(KERN_INFO "UINTR: enabled");
	return 0;
}

void uintr_exit(void)
{
	int cpu;
	struct uintr_percpu *p;

	if (!uintr_enabled)
		return;

	uintr_scheduler_unhijack();

	smp_call_function(uintr_teardown, NULL, 1);
	uintr_teardown(NULL);

	for_each_online_cpu(cpu) {
		p = get_uintr_pcpu(cpu);
		if (p->assigned_ctx)
			kref_put(&p->assigned_ctx->refcount, uintr_ctx_release);
	}
}
