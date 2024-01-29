// uintr.h

#pragma once

#include <asm/apic.h>
#include <asm/local.h>
#include <asm/irq_vectors.h>

#include "uintr_hw.h"

/* Use KVM's posted interrupt vector */
#define UIPI_APIC_VECTOR			POSTED_INTR_WAKEUP_VECTOR

struct uintr_ctx {
	unsigned long handler;
	struct kref refcount;
	bool is_admin;

	/* sender UITT table */
	struct uintr_uitt_entry uitt[];
};

struct uintr_xstate {
	struct xregs_state xregs;
	struct uintr_state uintr;
} __packed __aligned(64);

struct uintr_percpu {
	struct task_struct	*assigned_task;
	struct uintr_ctx	*assigned_ctx;

	bool		state_loaded;
	bool		is_admin_ctx;
	struct uintr_xstate cur_xstate;
};

extern void uintr_cleanup_core(struct uintr_percpu *p, int cpu);
extern void uintr_assign_core(struct uintr_ctx *ctx, u64 stack);
extern long uintr_multicast(struct ksched_intr_req __user *ureq);

extern void uintr_deliver_ipi(struct uintr_percpu *p);

extern int uintr_init(void);
extern void uintr_exit(void);
extern long uintr_setup_admin(struct file *filp);
extern long uintr_setup_user(struct file *filp, unsigned long handler);
extern void uintr_file_release(struct file *filp);

extern bool uintr_enabled;

static inline struct uintr_ctx *to_uintr_ctx(struct file *filp)
{
	return (struct uintr_ctx *)filp->private_data;
}

static inline bool uintr_active(struct uintr_percpu *p)
{
	return p->assigned_ctx != NULL;
}

static inline void uintr_signal_self(void)
{
	apic->send_IPI_self(UIPI_APIC_VECTOR);
}