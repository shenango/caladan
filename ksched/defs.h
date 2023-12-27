#pragma once

#include <asm/local.h>

#include "ksched.h"
#include "uintr_hw.h"

struct ksched_percpu {
	unsigned int		last_gen;
	local_t			busy;
	u64			last_sel;
	struct task_struct	*running_task;

	struct uintr_percpu	uintr;
};

extern __read_mostly struct ksched_shm_cpu *shm;
DECLARE_PER_CPU(struct ksched_percpu, kp);