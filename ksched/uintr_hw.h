
// UINTR hardware definitions

#pragma once

#define X86_FEATURE_UINTR		(18*32+ 5) /* User Interrupts support */
#define DISABLE_UINTR		(1 << (X86_FEATURE_UINTR & 31))

/* User Interrupt interface */
#define MSR_IA32_UINTR_RR		0x985
#define MSR_IA32_UINTR_HANDLER		0x986
#define MSR_IA32_UINTR_STACKADJUST	0x987
#define MSR_IA32_UINTR_MISC		0x988	/* 39:32-UINV, 31:0-UITTSZ */
#define MSR_IA32_UINTR_PD		0x989
#define MSR_IA32_UINTR_TT		0x98a

#define X86_CR4_UINTR_BIT	25 /* enable User Interrupts support */
#define X86_CR4_UINTR		_BITUL(X86_CR4_UINTR_BIT)

#define UINTR_UPID_STATUS_ON		0x0	/* Outstanding notification */
#define UINTR_UPID_STATUS_SN		0x1	/* Suppressed notification */

#define UINTR_UITT_VALID_BIT        0x0

/*
 * State component 14 is supervisor state used for User Interrupts state.
 * The size of this state is 48 bytes
 */
struct uintr_state {
	__u64 handler;
	__u64 stack_adjust;
	struct {
		__u32	uitt_size;
		__u8	uinv;
		__u8	pad1;
		__u8	pad2;
		__u8	pad3:7;
		__u8	uif:1;
	} __packed misc;
	__u64 upid_addr;
	__u64 uirr;
	__u64 uitt_addr;
} __packed;

/* User Interrupt Target Table Entry (UITTE) */
struct uintr_uitt_entry {
	__u8	valid;			/* bit 0: valid, bit 1-7: reserved */
	__u8	user_vec;
	__u8	reserved[6];
	__u64	target_upid_addr;
} __packed __aligned(16);

#define XFEATURE_UINTR 14
#define XFEATURE_MASK_UINTR		(1 << XFEATURE_UINTR)
