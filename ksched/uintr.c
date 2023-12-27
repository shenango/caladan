
#include <asm/apic.h>
#include <asm/cpufeature.h>
#include <asm/fpu/xcr.h>
#include <asm/fpu/xstate.h>
#include <asm/msr-index.h>
#include <asm/msr.h>
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
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/signal.h>
#include <linux/tracepoint.h>
#include <linux/version.h>

#include "ksched.h"
#include "uintr.h"
#include "defs.h"

#define REX_PREFIX	"0x48, "
#define XSAVES		".byte " REX_PREFIX "0x0f,0xc7,0x2f"
#define XRSTORS		".byte " REX_PREFIX "0x0f,0xc7,0x1f"

#define XSTATE_OP(op, st, lmask, hmask, err)				\
	asm volatile("1:" op "\n\t"					\
		     "xor %[err], %[err]\n"				\
		     "2:\n\t"						\
		     _ASM_EXTABLE_TYPE(1b, 2b, EX_TYPE_FAULT_MCE_SAFE)	\
		     : [err] "=a" (err)					\
		     : "D" (st), "m" (*st), "a" (lmask), "d" (hmask)	\
		     : "memory")

static __read_mostly bool uintr_enabled;
static __read_mostly struct uintr_xstate uintr_null_state;
static __cold struct tracepoint *hijacked_sched_tp;
static __cold unsigned long long tracepoint_sched_switch;

module_param(tracepoint_sched_switch, ullong, 0);
MODULE_PARM_DESC(tracepoint_sched_switch,
	"kernel address of tracepoint_sched_switch");

static inline struct uintr_percpu *get_uintr_pcpu(int cpu)
{
	return &per_cpu_ptr(&kp, cpu)->uintr;
}

static inline struct uintr_percpu *get_uintr_this_cpu(void)
{
	return &this_cpu_ptr(&kp)->uintr;
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

static void uintr_ctx_release(struct kref *ref)
{
	struct uintr_ctx *ctx = container_of(ref, struct uintr_ctx, refcount);
	kfree(ctx);
}

void uintr_cleanup_core(struct uintr_percpu *p, int cpu)
{
	/* prevent senders from sending to this core */
	p->assigned_ctx->uitt[cpu].valid = 0;

	/* suppress notifications */
	set_bit(UINTR_UPID_STATUS_SN, &shm[cpu].upid.word_val);

	/* load NULL state */
	wrmsrl(MSR_IA32_UINTR_MISC, 0);
	uintr_xrstors(&uintr_null_state);

	/* remove any existing posted interrupts */
	shm[cpu].upid.puir = 0;

	/* release ref */
	kref_put(&p->assigned_ctx->refcount, uintr_ctx_release);

	p->assigned_ctx = NULL;
	p->assigned_task = NULL;
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
	uintr_xrstors(&p->cur_xstate);

	/* allow other cores in this ctx to send to this one */
	ctx->uitt[cpu].valid = BIT(UINTR_UITT_VALID_BIT);

	/* allow notifications */
	clear_bit(UINTR_UPID_STATUS_SN, &shm[cpu].upid.word_val);

	/* take a reference */
	kref_get(&ctx->refcount);
	p->assigned_ctx = ctx;
	p->assigned_task = current;
	p->state_loaded = true;

	put_cpu();
}

/*
 * Some cores may still run other non-managed user tasks. We need to ensure that
 * UINTR notifications are disabled and that those tasks can't send UIPIs. This
 * function is called by the scheduler whenever switching tasks, allowing us to
 * check if UINTR should be temporarily disabled or re-enabled.
 */
static void sched_switch_tracepoint(void *data, bool preempt,
	struct task_struct *prev, struct task_struct *next)
{
	int cpu;
	struct uintr_upid *upid;
	struct uintr_percpu *p;

	/* Do nothing for kernel threads */
	if (next->flags & PF_KTHREAD)
		return;

	cpu = smp_processor_id();
	upid = &shm[cpu].upid;
	p = get_uintr_this_cpu();

	if (next != p->assigned_task) {

		if (!p->state_loaded)
			return;

		/* suppress UIPI notifications */
		set_bit(UINTR_UPID_STATUS_SN, &upid->word_val);

		/* save state (and clear UINV) */
		uintr_xsaves(&p->cur_xstate);

		if (p->cur_xstate.uintr.uirr) {
			// We need two wrmsrs in this case, one to clear UIRR and one
			// to update UITT. It's faster to just load the NULL context.
			uintr_xrstors(&uintr_null_state);
		} else {
			// Just do one wrmsr to disable the UITT
			wrmsrl(MSR_IA32_UINTR_TT, 0);
		}

		p->state_loaded = false;

	} else if (p->assigned_ctx && !p->state_loaded) {

		p->state_loaded = true;

		/* reload saved state */
		uintr_xrstors(&p->cur_xstate);

		/* enable notifications */
		clear_bit(UINTR_UPID_STATUS_SN, &upid->word_val);
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

static void __exit uintr_scheduler_unhijack(void) {
	if (!hijacked_sched_tp)
		return;

	tracepoint_probe_unregister(hijacked_sched_tp, sched_switch_tracepoint,
	                            NULL);
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

static int __init uintr_scheduler_hijack(void)
{
	int ret;
	struct tracepoint *tp;

	if (!tracepoint_sched_switch)
		return -1;

	tp = (struct tracepoint *)tracepoint_sched_switch;

	if (strncmp(tp->name, "sched_switch", strlen("sched_switch"))) {
		printk("bad tracepoint address: got name %s", tp->name);
		return -1;
	}

	ret = tracepoint_probe_register(tp, sched_switch_tracepoint, NULL);
	if (ret) {
		printk(KERN_ERR "Failed to register tracepoint handler (%d)\n", ret);
		return ret;
	}

	hijacked_sched_tp = tp;
	return 0;
}

int __init uintr_init(void)
{
	int ret;
	bool failure = false;

	if (!tracepoint_sched_switch) {
		printk(KERN_INFO "UINTR: not enabled (not requested)");
		return 0;
	}

	if (!cpu_has(&boot_cpu_data, X86_FEATURE_UINTR)) {
		printk(KERN_INFO "UINTR: not enabled (no support)");
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
